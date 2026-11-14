#include <string.h>

#include "agg/agg.h"
#include "can/pins.h"
#include "config/config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace agg {
namespace {

const char* TAG = "agg";
Engine s_engine;
SemaphoreHandle_t s_lock = nullptr;
uint64_t s_next_tick = 0;
uint32_t s_logged = 0;  // last event written to the log
uint32_t s_malformed[kSources] = {};
uint64_t s_next_save = 0;
double s_saved_sum = -1;
constexpr uint64_t kEnergySaveMs = 15 * 60 * 1000;  // energy counters to NVS every 15 min (flash wear)
constexpr const char* kNvsNs = "aggE";

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

uint64_t now_ms() { return (uint64_t)(esp_timer_get_time() / 1000); }

Config from_settings(const cfg::Settings& s) {
    Config c;
    c.enabled[kSrcBN] = s.agg_src_a;
    c.enabled[kSrcKNY] = s.agg_src_b;
    c.kny_capacity_ah = (float)s.agg_kny_cap_ah;
    c.failsafe_cvl = s.agg_fs_cvl_dv / 10.0f;
    c.failsafe_dvl = s.agg_fs_dvl_dv / 10.0f;
    c.ccl_max = (float)s.agg_ccl_max;
    c.dcl_max = (float)s.agg_dcl_max;
    c.silent_failsafe = s.agg_silent;
    c.startup_hold_ms = s.agg_hold_s * 1000;
    c.cvl_rise_hold_ms = s.agg_cvl_hold_s * 1000;
    c.cap_a = (float)s.agg_cap_a;
    c.cap_a_p2 = (float)s.agg_cap_a_p2;
    c.cap_b = (float)s.agg_cap_b;
    return c;  // frame timeouts stay at 5 s (1 Hz) and 6 s (pack 2)
}

// Writes new engine events to the log (caller holds the lock).
void log_events() {
    Event ev[8];
    size_t n;
    while ((n = s_engine.events_since(s_logged, ev, 8)) > 0) {
        for (size_t i = 0; i < n; i++) {
            const Event& e = ev[i];
            s_logged = e.seq;
            const char* src = e.source == kSrcBN ? "A " : e.source == kSrcKNY ? "B " : "";
            if (e.type == EvType::Trip || e.type == EvType::Stale || e.type == EvType::Plausibility)
                ESP_LOGW(TAG, "%s%s: %s", src, ev_type_name(e.type), e.text);
            else
                ESP_LOGI(TAG, "%s%s: %s", src, ev_type_name(e.type), e.text);
        }
    }
}

// Energy counters (kWh, integrated from V x I) survive a reboot: saved every 15 min, so at most
// 15 min of counting is lost (Home Assistant "total" sensors then step back by that little).
struct EnergyBlob {
    uint32_t version;
    double v[kSources][6];  // in, out, pack-1 in, pack-1 out, pack-2 in, pack-2 out
};

void energy_load() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return;
    EnergyBlob b = {};
    size_t len = sizeof(b);
    if (nvs_get_blob(h, "e", &b, &len) == ESP_OK && len == sizeof(b) && b.version == 1) {
        for (int i = 0; i < kSources; i++)
            s_engine.set_energy(i, b.v[i][0], b.v[i][1], b.v[i][2], b.v[i][3], b.v[i][4], b.v[i][5]);
        ESP_LOGI(TAG, "energy counters restored");
    }
    nvs_close(h);
}

void energy_save_if_due(uint64_t now) {  // caller holds the lock
    if (now < s_next_save) return;
    s_next_save = now + kEnergySaveMs;
    EnergyBlob b = {};
    b.version = 1;
    double sum = 0;
    for (int i = 0; i < kSources; i++) {
        const Source& s = s_engine.source(i);
        double v[6] = {s.e_in_kwh, s.e_out_kwh, s.p1_e_in_kwh, s.p1_e_out_kwh, s.p2_e_in_kwh, s.p2_e_out_kwh};
        for (int k = 0; k < 6; k++) b.v[i][k] = v[k], sum += v[k];
    }
    if (s_saved_sum >= 0 && sum - s_saved_sum < 0.01) return;  // nothing worth a flash write
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "e", &b, sizeof(b)) == ESP_OK && nvs_commit(h) == ESP_OK) s_saved_sum = sum;
    nvs_close(h);
}

}  // namespace

void start() {
    s_lock = xSemaphoreCreateMutex();
    apply_settings();
    Lock l;
    energy_load();
    s_next_save = now_ms() + kEnergySaveMs;
}

void apply_settings() {
    if (!s_lock) return;
    Config c = from_settings(cfg::get());
    Lock l;
    s_engine.configure(c, now_ms());
    log_events();
}

void on_can_frame(const can::Record& r) {
    if (!s_lock) return;
    int src = r.port == BMS_CAN_PORT_A ? kSrcBN : r.port == BMS_CAN_PORT_B ? kSrcKNY : -1;
    if (src < 0) return;  // port INV: the inverter's frames are not battery data
    if (r.id & (can::kIdExtended | can::kIdRtr)) {  // neither battery sends these: corrupt or foreign
        s_malformed[src]++;
        return;
    }
    Lock l;
    s_engine.on_frame(src, r.id & can::kIdMask, r.data, r.dlc, r.ts_us / 1000);
}

void tick() {
    if (!s_lock) return;
    uint64_t now = now_ms();
    if (now < s_next_tick) return;
    s_next_tick = now + 100;
    Lock l;
    s_engine.tick(now);
    log_events();
    energy_save_if_due(now);
}

void set_ota(bool active) {
    if (!s_lock) return;
    Lock l;
    s_engine.set_ota(active, now_ms());
    log_events();
}

bool tx_frames(can::TxFrame* out, int max, int* n) {
    *n = 0;
    if (!s_lock) return false;
    Lock l;
    const Output& o = s_engine.output();
    if (!o.send) return false;
    for (int k = 0; k < kOutFrames && k < max; k++) {
        out[k].id = (uint16_t)o.frames[k].id;
        out[k].dlc = o.frames[k].dlc;
        memcpy(out[k].data, o.frames[k].data, 8);
        (*n)++;
    }
    return *n > 0;
}

void snapshot(Snapshot& s) {
    Lock l;
    s.out = s_engine.output();
    for (int i = 0; i < kSources; i++) s.src[i] = s_engine.source(i);
    s.cfg = s_engine.config();
    s.event_seq = s_engine.event_seq();
    for (int i = 0; i < kSources; i++) s.malformed[i] = s_malformed[i];
}

size_t events_since(uint32_t after, Event* out, size_t max) {
    Lock l;
    return s_engine.events_since(after, out, max);
}

void clear_trips() {
    Lock l;
    s_engine.clear_trips(now_ms());
    log_events();
}

}  // namespace agg
