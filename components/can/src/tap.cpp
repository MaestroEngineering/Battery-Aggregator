#include <string.h>

#include <algorithm>

#include "can/can.h"
#include "can/mcp2515_regs.h"
#include "can/pins.h"
#include "config/config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mcp2515.h"

// FAIL-SAFE RULE 7 (SPEC.md): CAN output is silent during OTA. In Phase 2 the
// only controller is a listen-only tap that cannot transmit, so there is no
// output to silence; the flag is kept for the output ports of Phase 3.
static volatile bool s_ota_active = false;
extern "C" void bms_ota_activity(bool active) {
    s_ota_active = active;
    ESP_LOGI("can", "OTA %s (tap is listen-only: nothing to silence)", active ? "started" : "ended");
}

namespace can {
namespace {

const char* TAG = "can";
using namespace mcp2515;

enum Cmd : uint8_t { CMD_APPLY, CMD_DETECT };

Mcp2515 s_mcp;
bool s_spi_ready = false;
TaskHandle_t s_task = nullptr;
QueueHandle_t s_cmd = nullptr;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // ring, stats, status
Sink s_sink = nullptr;

// Status (guarded by s_mux for the string; scalars are word-sized).
volatile State s_state = State::Off;
char s_error[96] = "";  // fixed buffers: no allocation inside critical sections
bool s_enabled = false;
uint32_t s_bitrate = 250000;
uint64_t s_frames = 0;
uint32_t s_fps = 0, s_fps_count = 0;
uint32_t s_merr = 0, s_ovr = 0, s_ids_dropped = 0;
uint8_t s_rec = 0;
uint64_t s_last_frame_us = 0;

bool s_detect_running = false;
int s_detect_count = 0;
DetectResult s_detect[3];
uint32_t s_detect_suggestion = 0;
char s_detect_note[112] = "";

Record s_ring[kRingSize];
uint32_t s_head = 0;

IdStats s_stats[kMaxIds];
size_t s_nstats = 0;

void IRAM_ATTR int_isr(void*) {
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void set_error(const char* why) {
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_error, why ? why : "", sizeof(s_error));
    portEXIT_CRITICAL(&s_mux);
}

bool ensure_spi() {
    if (s_spi_ready) return true;
    spi_bus_config_t bc = {};
    bc.sclk_io_num = BMS_CAN_SPI_SCK;
    bc.mosi_io_num = BMS_CAN_SPI_MOSI;
    bc.miso_io_num = BMS_CAN_SPI_MISO;
    bc.quadwp_io_num = -1;
    bc.quadhd_io_num = -1;
    bc.max_transfer_sz = 32;
    if (spi_bus_initialize(SPI3_HOST, &bc, SPI_DMA_DISABLED) != ESP_OK) return false;
    if (s_mcp.attach(SPI3_HOST, BMS_CAN_A_CS, BMS_CAN_SPI_HZ) != ESP_OK) return false;
    // Unused modules: keep their CS lines high (deselected).
    for (int cs : {BMS_CAN_INV_CS, BMS_CAN_B_CS}) {
        gpio_set_direction((gpio_num_t)cs, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)cs, 1);
    }
    gpio_config_t ic = {};
    ic.pin_bit_mask = 1ULL << BMS_CAN_A_INT;
    ic.mode = GPIO_MODE_INPUT;
    ic.intr_type = GPIO_INTR_NEGEDGE;  // input-only pin: no internal pull-up, MCP2515 drives it
    gpio_config(&ic);
    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)BMS_CAN_A_INT, int_isr, nullptr);
    s_spi_ready = true;
    return true;
}

bool bring_up(uint32_t bitrate) {
    if (!ensure_spi()) {
        set_error("SPI bus init failed");
        s_state = State::Error;
        return false;
    }
    const char* why = "";
    if (s_mcp.start_listen_only(bitrate, &why) != ESP_OK) {
        ESP_LOGE(TAG, "tap not started: %s", why);
        set_error(why);
        s_state = State::Error;
        return false;
    }
    set_error(nullptr);
    s_state = State::ListenOnly;
    return true;
}

void shut_down() {
    if (s_spi_ready) s_mcp.enter_config();  // configuration mode: receives and sends nothing
    s_state = State::Off;
    set_error(nullptr);
}

// Updates the ID table; returns true if the frame differs from the last one
// with this ID. Caller holds s_mux.
bool stats_update(const Record& r) {
    IdStats* st = nullptr;
    for (size_t i = 0; i < s_nstats; i++)
        if (s_stats[i].id == r.id) {
            st = &s_stats[i];
            break;
        }
    if (!st) {
        if (s_nstats >= kMaxIds) {
            s_ids_dropped++;
            return true;
        }
        st = &s_stats[s_nstats++];
        memset(st, 0, sizeof(*st));
        st->id = r.id;
        memcpy(st->data, r.data, 8);
        memcpy(st->min, r.data, 8);
        memcpy(st->max, r.data, 8);
        st->dlc = r.dlc;
        st->count = 1;
        st->last_ts_us = st->last_change_us = r.ts_us;
        return true;
    }
    uint64_t dt = r.ts_us - st->last_ts_us;
    st->period_us = st->period_us ? (uint32_t)((st->period_us * 7ULL + dt) / 8) : (uint32_t)dt;
    bool changed = st->dlc != r.dlc || memcmp(st->data, r.data, r.dlc) != 0;
    for (int i = 0; i < r.dlc; i++) {
        if (r.data[i] < st->min[i]) st->min[i] = r.data[i];
        if (r.data[i] > st->max[i]) st->max[i] = r.data[i];
    }
    memcpy(st->data, r.data, 8);
    st->dlc = r.dlc;
    st->count++;
    st->last_ts_us = r.ts_us;
    if (changed) st->last_change_us = r.ts_us;
    return changed;
}

void deliver(Record& r) {
    r.port = BMS_CAN_PORT_A;
    r.ts_us = esp_timer_get_time();
    bool changed;
    portENTER_CRITICAL(&s_mux);
    s_ring[s_head % kRingSize] = r;
    s_head++;
    changed = stats_update(r);
    s_frames++;
    s_fps_count++;
    s_last_frame_us = r.ts_us;
    portEXIT_CRITICAL(&s_mux);
    if (s_sink) s_sink(r, changed);
}

// Reads everything pending. `count_only` (bitrate detection) counts frames
// without storing them. Returns frames read.
uint32_t service(bool count_only, uint32_t* errors) {
    uint32_t n = 0;
    for (int loops = 0; loops < 32; loops++) {
        uint8_t intf = s_mcp.read_reg(CANINTF);
        if (intf == 0xFF) break;  // chip gone (MISO floating high)
        if (!(intf & (INT_RX0 | INT_RX1 | INT_MERR | INT_ERR))) break;
        Record r;
        if (intf & INT_RX0 && s_mcp.read_rx(0, r) == ESP_OK) {
            n++;
            if (!count_only) deliver(r);
        }
        if (intf & INT_RX1 && s_mcp.read_rx(1, r) == ESP_OK) {
            n++;
            if (!count_only) deliver(r);
        }
        if (intf & INT_MERR) {
            if (errors) (*errors)++;
            s_merr++;
            s_mcp.bit_modify(CANINTF, INT_MERR, 0);
        }
        if (intf & INT_ERR) {
            uint8_t eflg = s_mcp.read_reg(EFLG);
            if (eflg & (EFLG_RX0OVR | EFLG_RX1OVR)) s_ovr++;
            s_mcp.bit_modify(EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
            s_mcp.bit_modify(CANINTF, INT_ERR, 0);
        }
    }
    return n;
}

void run_detect() {
    ESP_LOGI(TAG, "bitrate detection (listen-only): trying 125k, 250k, 500k");
    State before = s_state;
    portENTER_CRITICAL(&s_mux);
    s_detect_running = true;
    s_detect_count = 0;
    s_detect_suggestion = 0;
    strlcpy(s_detect_note, "running", sizeof(s_detect_note));
    portEXIT_CRITICAL(&s_mux);
    s_state = State::Detecting;
    std::string note;
    int i = 0;
    for (const auto& t : kTimings8MHz) {
        DetectResult res = {t.bitrate, 0, 0};
        const char* why = "";
        if (!ensure_spi() || s_mcp.start_listen_only(t.bitrate, &why) != ESP_OK) {
            note = std::string("controller error: ") + why;
            break;
        }
        int64_t end = esp_timer_get_time() + 1500000;
        while (esp_timer_get_time() < end) {
            esp_task_wdt_reset();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            res.frames += service(true, &res.errors);
        }
        ESP_LOGI(TAG, "  %lu bit/s: %lu frames, %lu errors", (unsigned long)t.bitrate,
                 (unsigned long)res.frames, (unsigned long)res.errors);
        portENTER_CRITICAL(&s_mux);
        s_detect[i] = res;
        s_detect_count = ++i;
        portEXIT_CRITICAL(&s_mux);
    }
    uint32_t best = 0, best_frames = 0;
    bool any_frames = false;
    for (int k = 0; k < i; k++) {
        any_frames |= s_detect[k].frames > 0;
        if (s_detect[k].frames > 0 && s_detect[k].errors == 0 && s_detect[k].frames > best_frames) {
            best = s_detect[k].bitrate;
            best_frames = s_detect[k].frames;
        }
    }
    if (note.empty()) {
        if (best) note = "clean frames at " + std::to_string(best / 1000) + " kbit/s";
        else if (any_frames) note = "frames seen but with errors at every rate; check wiring/termination";
        else note = "no traffic seen at any rate (bus idle, or 1 Mbit/s which an 8 MHz MCP2515 cannot do)";
    }
    ESP_LOGI(TAG, "bitrate detection done: %s", note.c_str());
    portENTER_CRITICAL(&s_mux);
    s_detect_suggestion = best;
    strlcpy(s_detect_note, note.c_str(), sizeof(s_detect_note));
    s_detect_running = false;
    portEXIT_CRITICAL(&s_mux);
    // Restore the configured mode.
    if (before == State::ListenOnly || before == State::Error) {
        if (s_enabled) bring_up(s_bitrate);
        else shut_down();
    } else {
        shut_down();
    }
}

void apply() {
    cfg::Settings s = cfg::get();
    s_enabled = s.can_enabled;
    s_bitrate = s.can_bitrate;
    if (s_enabled) {
        ESP_LOGI(TAG, "tap enabled on port A (CS %d, INT %d) at %lu bit/s", BMS_CAN_A_CS,
                 BMS_CAN_A_INT, (unsigned long)s_bitrate);
        bring_up(s_bitrate);
    } else {
        ESP_LOGI(TAG, "tap disabled");
        shut_down();
    }
}

void task(void*) {
    // FAIL-SAFE RULE 6 (SPEC.md): the task watchdog resets the chip if this
    // task stops running (CONFIG_ESP_TASK_WDT_PANIC).
    esp_task_wdt_add(nullptr);
    apply();
    int64_t next_check = 0, next_retry = 0, next_fps = esp_timer_get_time() + 1000000;
    for (;;) {
        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        Cmd c;
        while (xQueueReceive(s_cmd, &c, 0) == pdTRUE) {
            if (c == CMD_APPLY) apply();
            else if (c == CMD_DETECT) run_detect();
        }
        int64_t now = esp_timer_get_time();
        if (s_state == State::ListenOnly) {
            service(false, nullptr);  // also polls, in case an INT edge was missed
            if (now >= next_check) {
                next_check = now + 1000000;
                uint8_t m = s_mcp.opmode();
                s_rec = s_mcp.read_reg(REC);
                if (m != MODE_LISTEN_ONLY) {
                    // Never continue in any other mode. Force configuration mode.
                    ESP_LOGE(TAG, "controller left listen-only mode (CANSTAT mode 0x%02x): stopping", m);
                    s_mcp.enter_config();
                    set_error("controller left listen-only mode (reset/brown-out?); restarting");
                    s_state = State::Error;
                    next_retry = now + 2000000;
                }
            }
        } else if (s_state == State::Error && s_enabled && now >= next_retry) {
            next_retry = now + 5000000;
            bring_up(s_bitrate);
        }
        if (now >= next_fps) {
            next_fps = now + 1000000;
            s_fps = s_fps_count;
            s_fps_count = 0;
        }
    }
}

}  // namespace

const char* state_name(State s) {
    switch (s) {
        case State::Off: return "off";
        case State::ListenOnly: return "listen-only";
        case State::Error: return "error";
        case State::Detecting: return "detecting";
    }
    return "?";
}

void start() {
    s_cmd = xQueueCreate(4, sizeof(Cmd));
    // Core 1, high priority: WiFi and the web server run on core 0.
    xTaskCreatePinnedToCore(task, "can", 4096, nullptr, 18, &s_task, 1);
}

void apply_settings() {
    Cmd c = CMD_APPLY;
    if (s_cmd) xQueueSend(s_cmd, &c, 0);
}

esp_err_t start_detect() {
    if (s_detect_running) return ESP_ERR_INVALID_STATE;
    Cmd c = CMD_DETECT;
    return (s_cmd && xQueueSend(s_cmd, &c, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

Status status() {
    Status st;
    char err[sizeof(s_error)], note[sizeof(s_detect_note)];
    portENTER_CRITICAL(&s_mux);
    memcpy(err, s_error, sizeof(err));
    memcpy(note, s_detect_note, sizeof(note));
    st.enabled = s_enabled;
    st.state = s_state;
    st.bitrate = s_bitrate;
    st.frames_total = s_frames;
    st.fps = s_fps;
    st.bus_errors = s_merr;
    st.rx_overflows = s_ovr;
    st.rec = s_rec;
    st.ids = s_nstats;
    st.ids_dropped = s_ids_dropped;
    st.last_frame_age_ms = s_last_frame_us ? (esp_timer_get_time() - (int64_t)s_last_frame_us) / 1000 : -1;
    st.detect_running = s_detect_running;
    st.detect_count = s_detect_count;
    memcpy(st.detect, s_detect, sizeof(s_detect));
    st.detect_suggestion = s_detect_suggestion;
    portEXIT_CRITICAL(&s_mux);
    st.error = err;
    st.detect_note = note;
    st.blocked_cmds = s_mcp.blocked_commands();
    return st;
}

bool selftest_ok(const char** why) {
    if (!cfg::get().can_enabled) return true;
    if (s_state == State::ListenOnly || s_state == State::Detecting) return true;
    *why = "CAN tap enabled but MCP2515 not in listen-only mode";
    return false;
}

uint32_t ring_head() { return s_head; }

uint32_t ring_tail() {
    uint32_t h = s_head;
    return h > kRingSize ? h - kRingSize : 0;
}

size_t ring_read(uint32_t& seq, Record* out, size_t max, uint32_t* dropped) {
    portENTER_CRITICAL(&s_mux);
    uint32_t head = s_head;
    uint32_t tail = head > kRingSize ? head - kRingSize : 0;
    if (seq < tail) {
        if (dropped) *dropped += tail - seq;
        seq = tail;
    }
    if (seq > head) seq = head;
    size_t n = 0;
    while (seq < head && n < max) out[n++] = s_ring[seq++ % kRingSize];
    portEXIT_CRITICAL(&s_mux);
    return n;
}

size_t stats_copy(IdStats* out, size_t start, size_t max) {
    portENTER_CRITICAL(&s_mux);
    size_t n = start < s_nstats ? std::min(max, s_nstats - start) : 0;
    memcpy(out, s_stats + start, n * sizeof(IdStats));
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void stats_reset() {
    portENTER_CRITICAL(&s_mux);
    s_nstats = 0;
    s_ids_dropped = 0;
    portEXIT_CRITICAL(&s_mux);
}

size_t stats_latest(Record* out, size_t start, size_t max) {
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    for (size_t i = start; i < s_nstats && n < max; i++, n++) {
        memset(&out[n], 0, sizeof(Record));
        out[n].ts_us = s_stats[i].last_ts_us;
        out[n].id = s_stats[i].id;
        out[n].dlc = s_stats[i].dlc;
        out[n].port = BMS_CAN_PORT_A;
        out[n].flags = kFlagSnapshot;
        memcpy(out[n].data, s_stats[i].data, 8);
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void set_sink(Sink s) { s_sink = s; }

}  // namespace can
