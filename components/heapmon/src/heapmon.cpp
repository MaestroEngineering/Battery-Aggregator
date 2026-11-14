#include "heapmon/heapmon.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace heapmon {
void mark(const char* what);
namespace {

const char* TAG = "heap";
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t s_timer = nullptr;
uint32_t s_min_largest = UINT32_MAX;
uint32_t s_episodes = 0;
int64_t s_last_low_us = 0;
bool s_low = false, s_crit = false;
uint32_t s_logged_low = UINT32_MAX;  // last largest-block low written to the log
int64_t s_last_warn_us = -60000000;  // low/recovered lines at most once a minute (log ring)
int s_nmarks = 0;
Mark s_marks[kMaxMarks];

uint32_t largest_now() { return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

void sample(void*) {
    uint32_t fr = (uint32_t)esp_get_free_heap_size(), lg = largest_now();
    int64_t now = esp_timer_get_time();
    bool was_low, was_crit, newlow = false;
    portENTER_CRITICAL(&s_mux);
    if (lg < s_min_largest) s_min_largest = lg;
    was_low = s_low, was_crit = s_crit;
    // Hysteresis: low below kLowLargest, back to normal above it + 4 KB.
    if (!s_low && lg < kLowLargest) s_low = true, s_episodes++;
    if (s_low && lg >= kLowLargest + 4096) s_low = false;
    s_crit = lg < kCriticalLargest;
    if (s_low) s_last_low_us = now;
    if (lg + 2048 <= s_logged_low && lg < 20 * 1024) s_logged_low = lg, newlow = true;
    portEXIT_CRITICAL(&s_mux);
    int64_t up_s = now / 1000000;
    if (up_s == 60) mark("uptime 60 s");
    if (up_s == 600) mark("uptime 10 min");
    // Short dips (an upload and a page reply at the same moment) are counted in low_episodes;
    // the log gets at most one low/recovered line a minute, critical and new lows always.
    bool may = now - s_last_warn_us >= 60000000;
    if (s_crit && !was_crit) {
        ESP_LOGE(TAG, "CRITICAL: largest free block %lu bytes (free %lu)", (unsigned long)lg, (unsigned long)fr);
    } else if (s_low && !was_low && may) {
        s_last_warn_us = now;
        ESP_LOGW(TAG, "low: largest free block %lu bytes (free %lu, %lu low episodes); optional work waits",
                 (unsigned long)lg, (unsigned long)fr, (unsigned long)s_episodes);
    } else if (!s_low && was_low && may) {
        s_last_warn_us = now;
        ESP_LOGI(TAG, "recovered: largest free block %lu bytes (free %lu)", (unsigned long)lg, (unsigned long)fr);
    } else if (newlow)
        ESP_LOGW(TAG, "new low: largest free block %lu bytes (free %lu, lowest free ever %lu)", (unsigned long)lg,
                 (unsigned long)fr, (unsigned long)esp_get_minimum_free_heap_size());
}

}  // namespace

void start() {
    if (s_timer) return;
    esp_timer_create_args_t a = {};
    a.callback = sample;
    a.name = "heapmon";
    if (esp_timer_create(&a, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, 1000000);
    mark("start");
}

void mark(const char* what) {
    uint32_t fr = (uint32_t)esp_get_free_heap_size(), lg = largest_now();
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    portENTER_CRITICAL(&s_mux);
    if (s_nmarks < kMaxMarks) s_marks[s_nmarks++] = Mark{what, up, fr, lg};
    if (lg < s_min_largest) s_min_largest = lg;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%s: free %lu, largest block %lu, lowest free ever %lu (uptime %lu s)", what, (unsigned long)fr,
             (unsigned long)lg, (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)up);
}

bool low() {
    if (largest_now() < kLowLargest) return true;
    portENTER_CRITICAL(&s_mux);
    bool l = s_low;
    portEXIT_CRITICAL(&s_mux);
    return l;
}

bool critical() { return largest_now() < kCriticalLargest; }

Stats stats() {
    Stats s = {};
    s.free = (uint32_t)esp_get_free_heap_size();
    s.largest = largest_now();
    s.min_free = (uint32_t)esp_get_minimum_free_heap_size();
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s.min_largest = s_min_largest == UINT32_MAX ? s.largest : s_min_largest;
    s.low_episodes = s_episodes;
    s.last_low_age_s = s_last_low_us ? (now - s_last_low_us) / 1000000 : -1;
    s.nmarks = s_nmarks;
    for (int i = 0; i < s_nmarks; i++) s.marks[i] = s_marks[i];
    portEXIT_CRITICAL(&s_mux);
    return s;
}

}  // namespace heapmon
