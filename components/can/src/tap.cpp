#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "can/can.h"
#include "can/mcp2515_regs.h"
#include "can/pins.h"
#include "config/config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mcp2515.h"

// FAIL-SAFE RULE 7 (SPEC.md): CAN output is silent during OTA. The aggregator output
// goes silent through the OTA hook, and port INV also checks s_ota_active itself
// before every transmit cycle.
static volatile bool s_ota_active = false;
static void (*s_ota_hook)(bool) = nullptr;
extern "C" void bms_ota_activity(bool active) {
    s_ota_active = active;
    if (s_ota_hook) s_ota_hook(active);
    ESP_LOGI("can", "OTA %s (inverter output %s)", active ? "started" : "ended", active ? "silent" : "resumes");
}

namespace can {
namespace {

const char* TAG = "can";
using namespace mcp2515;

enum Cmd : uint8_t { CMD_APPLY, CMD_DETECT, CMD_PROBE, CMD_INTMAP };

struct PortPins {
    const char* name;
    int cs, irq;
};
constexpr PortPins kPorts[3] = {  // indexed by BMS_CAN_PORT_*
    {"INV", BMS_CAN_INV_CS, BMS_CAN_INV_INT},
    {"A", BMS_CAN_A_CS, BMS_CAN_A_INT},
    {"B", BMS_CAN_B_CS, BMS_CAN_B_INT},
};
Mcp2515 s_chip[3];
Mcp2515& s_mcp = s_chip[BMS_CAN_PORT_A];  // the tap
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

bool s_probe_running = false, s_probe_done = false;
uint32_t s_probe_bitrate = 0;
int64_t s_probe_us = 0;
ProbePort s_probe[3];

int64_t s_next_retry = 0;  // next automatic bring_up() after a failure

// Ports INV and B: receivers (own bitrate and mode, applied on change). Their guards
// (fixed per port) allow B to ACK and INV to send allow-listed frames; nothing else.
struct Aux {
    bool enabled = false;
    uint32_t bitrate = 0;
    uint8_t mode = MODE_LISTEN_ONLY;   // CANCTRL value wanted (listen-only, normal, normal+OSM)
    bool fallback = false;             // B: ACK mode given up after errors (until settings change)
    uint8_t tec = 0;
    uint64_t tx_frames = 0;
    uint32_t tx_failed = 0;
    uint32_t merr_at_check = 0;
    State state = State::Off;
    char error[96] = "";
    uint64_t frames = 0;
    uint32_t fps = 0, fps_count = 0, merr = 0, ovr = 0;
    uint8_t rec = 0;
    uint64_t last_us = 0;
    int64_t next_retry = 0, next_check = 0;
};
Aux s_aux[3];  // indexed by port; [BMS_CAN_PORT_A] is unused (the tap has its own state)
constexpr int kAuxPorts[2] = {BMS_CAN_PORT_INV, BMS_CAN_PORT_B};
FrameHook s_hook = nullptr;  // aggregator
void (*s_tick)() = nullptr;    // aggregator tick
TxSource s_tx = nullptr;       // aggregator inverter output
bool s_tx_enabled = false;     // agg_tx_enabled
int64_t s_next_tx = 0;
constexpr int64_t kTxPeriodUs = 1050000;  // > kInvTxMinIntervalMs: the guard never has to refuse
// B in ACK mode: fall back to listen-only when it disturbs the bus (wrong bitrate?).
constexpr uint8_t kAckMaxErrCount = 96;     // TEC or REC at the error-warning level
constexpr uint32_t kAckMaxMerrPerS = 20;

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
    // All three modules are SPI devices (the driver keeps every CS high while
    // idle). Only the tap (port A) gets an interrupt handler.
    for (int p = 0; p < 3; p++)
        if (s_chip[p].attach(SPI3_HOST, kPorts[p].cs, BMS_CAN_SPI_HZ, p) != ESP_OK) return false;  // fixes the guard policy
    gpio_config_t ic = {};
    ic.pin_bit_mask = (1ULL << BMS_CAN_INV_INT) | (1ULL << BMS_CAN_A_INT) | (1ULL << BMS_CAN_B_INT);
    ic.mode = GPIO_MODE_INPUT;  // input-only pins: no internal pull-up, the MCP2515 drives INT
    ic.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&ic);
    gpio_install_isr_service(0);
    for (const auto& k : kPorts) {
        gpio_set_intr_type((gpio_num_t)k.irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add((gpio_num_t)k.irq, int_isr, nullptr);
    }
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
        s_next_retry = esp_timer_get_time() + 5000000;  // no immediate second attempt from the task loop
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
        if (s_stats[i].id == r.id && s_stats[i].port == r.port) {
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
        st->port = r.port;
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

void deliver(Record& r, int port) {
    r.port = (uint8_t)port;
    r.ts_us = esp_timer_get_time();
    bool changed;
    portENTER_CRITICAL(&s_mux);
    s_ring[s_head % kRingSize] = r;
    s_head++;
    changed = stats_update(r);
    if (port == BMS_CAN_PORT_A) {
        s_frames++;
        s_fps_count++;
        s_last_frame_us = r.ts_us;
    } else {
        s_aux[port].frames++;
        s_aux[port].fps_count++;
        s_aux[port].last_us = r.ts_us;
    }
    portEXIT_CRITICAL(&s_mux);
    if (s_hook) s_hook(r);  // aggregator (fast, never blocks for long)
    if (s_sink) s_sink(r, changed);
}

// A frame this device transmitted on port INV (acknowledged): into the ring (capture, pre-trigger)
// and to the diagnostics sink, tagged kFlagTx. Not to the aggregator hook (it is not battery data)
// and not into the per-ID receive table. Called after the send: what is sent is unchanged.
void deliver_tx(const TxFrame& f) {
    static uint8_t last[8][8];
    static uint16_t last_id[8];
    static uint8_t last_dlc[8];
    Record r = {};
    r.ts_us = esp_timer_get_time();
    r.id = f.id;
    r.dlc = f.dlc;
    r.port = BMS_CAN_PORT_INV;
    r.flags = kFlagTx;
    memcpy(r.data, f.data, f.dlc > 8 ? 8 : f.dlc);
    int slot = -1;
    for (int k = 0; k < kInvTxAllowCount; k++)
        if (kInvTxAllow[k].id == f.id) slot = k;
    bool changed = slot < 0 || last_id[slot] != f.id || last_dlc[slot] != f.dlc || memcmp(last[slot], r.data, 8) != 0;
    if (slot >= 0) last_id[slot] = f.id, last_dlc[slot] = f.dlc, memcpy(last[slot], r.data, 8);
    portENTER_CRITICAL(&s_mux);
    s_ring[s_head % kRingSize] = r;
    s_head++;
    portEXIT_CRITICAL(&s_mux);
    if (s_sink) s_sink(r, changed);
}

// Reads everything pending. `count_only` (bitrate detection) counts frames
// without storing them. Returns frames read.
uint32_t service(int port, bool count_only, uint32_t* errors) {
    Mcp2515& m = s_chip[port];
    uint32_t& merr = port == BMS_CAN_PORT_A ? s_merr : s_aux[port].merr;
    uint32_t& ovr = port == BMS_CAN_PORT_A ? s_ovr : s_aux[port].ovr;
    uint32_t n = 0;
    for (int loops = 0; loops < 32; loops++) {
        uint8_t intf = m.read_reg(CANINTF);
        if (intf == 0xFF) break;  // chip gone (MISO floating high)
        if (!(intf & (INT_RX0 | INT_RX1 | INT_MERR | INT_ERR))) break;
        Record r;
        if (intf & INT_RX0 && m.read_rx(0, r) == ESP_OK) {
            n++;
            if (!count_only) deliver(r, port);
        }
        if (intf & INT_RX1 && m.read_rx(1, r) == ESP_OK) {
            n++;
            if (!count_only) deliver(r, port);
        }
        if (intf & INT_MERR) {
            if (errors) (*errors)++;
            merr++;
            m.bit_modify(CANINTF, INT_MERR, 0);
        }
        if (intf & INT_ERR) {
            uint8_t eflg = m.read_reg(EFLG);
            if (eflg & (EFLG_RX0OVR | EFLG_RX1OVR)) ovr++;
            m.bit_modify(EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
            m.bit_modify(CANINTF, INT_ERR, 0);
        }
    }
    return n;
}

void aux_set_error(int p, const char* why) {
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_aux[p].error, why ? why : "", sizeof(s_aux[p].error));
    portEXIT_CRITICAL(&s_mux);
}

bool aux_up(int p) {
    Aux& a = s_aux[p];
    if (!ensure_spi()) {
        aux_set_error(p, "SPI bus init failed");
        a.state = State::Error;
        return false;
    }
    const char* why = "";
    const uint8_t mode = a.fallback ? MODE_LISTEN_ONLY : a.mode;
    if (s_chip[p].start(a.bitrate, mode, &why) != ESP_OK) {
        ESP_LOGE(TAG, "port %s not started: %s", kPorts[p].name, why);
        aux_set_error(p, why);
        a.state = State::Error;
        a.next_retry = esp_timer_get_time() + 5000000;
        return false;
    }
    if (!a.fallback) aux_set_error(p, nullptr);
    a.state = (mode & MODE_MASK) == MODE_LISTEN_ONLY ? State::ListenOnly
              : p == BMS_CAN_PORT_INV                ? State::Transmitting
                                                     : State::AckOnly;
    a.merr_at_check = a.merr;
    ESP_LOGI(TAG, "port %s %s at %lu bit/s (CS %d, INT %d)", kPorts[p].name, state_name(a.state),
             (unsigned long)a.bitrate, kPorts[p].cs, kPorts[p].irq);
    return true;
}

bool aux_running(const Aux& a) {
    return a.state == State::ListenOnly || a.state == State::AckOnly || a.state == State::Transmitting;
}

// Port INV: one cycle of the inverter output (about once a second). Only TX buffer 0 is
// used, one frame at a time; one-shot mode means a frame that is not acknowledged is not
// retried (the next one follows a second later).
void tx_cycle(int64_t now) {
    Aux& a = s_aux[BMS_CAN_PORT_INV];
    if (a.state != State::Transmitting || !s_tx || !s_tx_enabled || now < s_next_tx) return;
    s_next_tx = now + kTxPeriodUs;
    if (s_ota_active) return;  // rule 7, independent of the aggregator's own silent state
    TxFrame f[8];
    int n = 0;
    if (!s_tx(f, 8, &n)) return;  // output silent (refused, OTA, fail-safe action 'silent')
    Mcp2515& m = s_chip[BMS_CAN_PORT_INV];
    for (int k = 0; k < n; k++) {
        esp_err_t e = m.send(f[k].id, f[k].dlc, f[k].data);
        if (e != ESP_OK) {
            a.tx_failed++;
            if (e == ESP_ERR_INVALID_STATE) {  // buffer 0 still busy: abort it and stop this cycle
                m.write_reg(CANCTRL, (uint8_t)(MODE_NORMAL | CANCTRL_OSM | CANCTRL_ABAT));
                m.write_reg(CANCTRL, (uint8_t)(MODE_NORMAL | CANCTRL_OSM));
                break;
            }
            continue;
        }
        // Wait for the one attempt to finish (an 8-byte frame takes < 1 ms at 125k-500k).
        uint8_t ctrl = TXB_TXREQ;
        for (int w = 0; w < 40 && (ctrl & TXB_TXREQ); w++) {
            esp_rom_delay_us(100);
            ctrl = m.txb0ctrl();
        }
        if (!(ctrl & TXB_TXREQ) && !(ctrl & (TXB_ABTF | TXB_MLOA | TXB_TXERR))) {
            a.tx_frames++;
            deliver_tx(f[k]);  // Verbose Diagnostics / capture: tagged TX
        } else {
            a.tx_failed++;
        }
    }
}

// Health check of a running INV/B port (once a second).
void aux_check(int p, int64_t now) {
    Aux& a = s_aux[p];
    Mcp2515& c = s_chip[p];
    uint8_t m = c.opmode();
    a.rec = c.read_reg(REC);
    a.tec = c.read_reg(TEC);
    const uint8_t want = (a.fallback ? MODE_LISTEN_ONLY : a.mode) & MODE_MASK;
    if (m != want) {
        // Never continue in any other mode. Force configuration mode.
        ESP_LOGE(TAG, "port %s left %s mode (CANSTAT mode 0x%02x): stopping", kPorts[p].name, state_name(a.state), m);
        c.enter_config();
        aux_set_error(p, "controller left its operating mode (reset/brown-out?); restarting");
        a.state = State::Error;
        a.next_retry = now + 2000000;
        return;
    }
    if (a.state == State::AckOnly) {
        uint32_t merr = a.merr - a.merr_at_check;
        if (a.tec >= kAckMaxErrCount || a.rec >= kAckMaxErrCount || merr > kAckMaxMerrPerS) {
            ESP_LOGE(TAG, "port B: errors in ACK mode (TEC %u, REC %u, %lu errors/s): back to listen-only",
                     a.tec, a.rec, (unsigned long)merr);
            a.fallback = true;
            aux_set_error(p, "ACK mode stopped after bus errors (wrong bitrate?); listen-only until settings change");
            aux_up(p);
            return;
        }
    }
    a.merr_at_check = a.merr;
}

void aux_down(int p) {
    if (s_spi_ready) s_chip[p].enter_config();
    s_aux[p].state = State::Off;
    aux_set_error(p, nullptr);
}

// After a check that reset every module: bring the enabled INV/B ports back.
void aux_restore() {
    for (int p : kAuxPorts) {
        if (s_aux[p].enabled) aux_up(p);
        else aux_down(p);
    }
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
            res.frames += service(BMS_CAN_PORT_A, true, &res.errors);
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

// ---- Hardware check ------------------------------------------------------------------
// Uses only RESET, READ, WRITE (CNF1, CANINTE) and BIT MODIFY (CANINTF), plus
// listen-only mode: all through the no-transmit guard. Nothing goes on the bus.

void note(ProbePort& r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(ProbePort& r, const char* fmt, ...) {
    if (r.note[0]) return;  // keep the first problem
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.note, sizeof(r.note), fmt, ap);
    va_end(ap);
}

int int_level(int p) { return gpio_get_level((gpio_num_t)kPorts[p].irq); }

// SPI and registers of one module.
void probe_spi(int p, ProbePort& r) {
    Mcp2515& m = s_chip[p];
    m.reset();
    r.canstat = m.read_reg(CANSTAT);
    r.canctrl = m.read_reg(CANCTRL);
    if (r.canstat == 0xFF && r.canctrl == 0xFF) {
        note(r, "no answer: MISO reads all 1s (module unpowered, CS %d not wired, or MISO open)", r.cs);
        return;
    }
    if (r.canstat == 0x00 && r.canctrl == 0x00) {
        note(r, "no answer: MISO reads all 0s (MISO shorted low or not driven)");
        return;
    }
    if ((r.canstat & MODE_MASK) != MODE_CONFIG || r.canctrl != 0x87) {
        note(r, "wrong reset values CANSTAT 0x%02X CANCTRL 0x%02X (expect 0x80/0x87): noisy SPI or RESET not received",
             r.canstat, r.canctrl);
        return;
    }
    for (uint8_t v : {0x55, 0xAA, 0x0F, 0xF0, 0x00, 0xFF, 0x01, 0x80}) {
        m.write_reg(CNF1, v);
        uint8_t got = m.read_reg(CNF1);
        if (got != v) {
            note(r, "register read-back failed: wrote 0x%02X, read 0x%02X (bad MOSI/MISO bit or SCK)", v, got);
            m.write_reg(CNF1, 0);
            return;
        }
    }
    m.write_reg(CNF1, 0);
    r.spi_ok = true;
}

// INT line: idle high; low while an enabled flag is set; only on its own GPIO.
void probe_int(int p, ProbePort& r) {
    Mcp2515& m = s_chip[p];
    if (int_level(p) != 1) {
        note(r, "INT GPIO%d is low with no interrupt pending (not wired, divider wrong, or shorted)", r.int_gpio);
        return;
    }
    int lv[3];
    m.write_reg(CANINTF, 0);
    m.write_reg(CANINTE, 0x40);  // WAKIE only: harmless in configuration mode
    m.bit_modify(CANINTF, 0x40, 0x40);
    vTaskDelay(pdMS_TO_TICKS(2));
    for (int k = 0; k < 3; k++) lv[k] = int_level(k);
    m.bit_modify(CANINTF, 0x40, 0x00);
    m.write_reg(CANINTE, 0x00);
    vTaskDelay(pdMS_TO_TICKS(2));
    int back = int_level(p);
    if (lv[p] != 0) {
        for (int k = 0; k < 3; k++)
            if (k != p && lv[k] == 0) {
                note(r, "INT shows up on GPIO%d (port %s) instead of GPIO%d: INT wires swapped",
                     kPorts[k].irq, kPorts[k].name, r.int_gpio);
                return;
            }
        note(r, "INT GPIO%d did not go low when the module raised an interrupt (INT not connected?)", r.int_gpio);
        return;
    }
    for (int k = 0; k < 3; k++)
        if (k != p && lv[k] == 0 && int_level(k) == 1) {
            note(r, "INT also pulls GPIO%d (port %s) low: INT lines shorted together", kPorts[k].irq,
                 kPorts[k].name);
            return;
        }
    if (back != 1) {
        note(r, "INT GPIO%d stayed low after the interrupt was cleared", r.int_gpio);
        return;
    }
    r.int_ok = true;
}

// Listen-only receive for a short window. Polled, so on a busy bus some frames
// are missed: the count only shows that the transceiver path delivers frames.
void probe_listen(int p, ProbePort& r, uint32_t bitrate) {
    Mcp2515& m = s_chip[p];
    const char* why = "";
    if (m.start_listen_only(bitrate, &why) != ESP_OK) {
        note(r, "listen-only: %s", why);
        return;
    }
    r.listen_ok = true;
    int64_t end = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < end) {
        esp_task_wdt_reset();
        uint8_t intf = m.read_reg(CANINTF);
        Record rec;
        if (intf & INT_RX0 && m.read_rx(0, rec) == ESP_OK) r.frames++;
        if (intf & INT_RX1 && m.read_rx(1, rec) == ESP_OK) r.frames++;
        if (intf & INT_MERR) {
            r.errors++;
            m.bit_modify(CANINTF, INT_MERR, 0);
        }
        if (intf & INT_ERR) {
            m.bit_modify(EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
            m.bit_modify(CANINTF, INT_ERR, 0);
        }
        if (!(intf & (INT_RX0 | INT_RX1))) vTaskDelay(1);
    }
    m.enter_config();
}

void run_probe() {
    State before = s_state;
    uint32_t bitrate = cfg::get().can_bitrate;
    portENTER_CRITICAL(&s_mux);
    s_probe_running = true;
    portEXIT_CRITICAL(&s_mux);
    s_state = State::Checking;
    ESP_LOGI(TAG, "hardware check of all three MCP2515 modules (no transmit; listen at %lu bit/s)",
             (unsigned long)bitrate);
    ProbePort res[3] = {};
    for (int p = 0; p < 3; p++) {
        strlcpy(res[p].name, kPorts[p].name, sizeof(res[p].name));
        res[p].cs = kPorts[p].cs;
        res[p].int_gpio = kPorts[p].irq;
    }
    if (!ensure_spi()) {
        for (auto& r : res) note(r, "SPI bus init failed");
    } else {
        for (int p = 0; p < 3; p++) probe_spi(p, res[p]);
        // CS isolation: give each answering module its own CNF1 value, then
        // read them all back. Two modules on one CS (or crossed CS) show up here.
        for (int p = 0; p < 3; p++)
            if (res[p].spi_ok) s_chip[p].write_reg(CNF1, (uint8_t)(0x21 + 0x11 * p));
        for (int p = 0; p < 3; p++) {
            if (!res[p].spi_ok) continue;
            uint8_t want = (uint8_t)(0x21 + 0x11 * p), got = s_chip[p].read_reg(CNF1);
            if (got != want) {
                res[p].spi_ok = false;
                note(res[p], "CS isolation failed: read 0x%02X, expected 0x%02X (CS lines crossed or shorted)",
                     got, want);
            }
        }
        for (int p = 0; p < 3; p++)
            if (res[p].spi_ok) {
                s_chip[p].reset();  // back to clean reset state before the INT test
                probe_int(p, res[p]);
            }
        for (int p = 0; p < 3; p++)
            if (res[p].spi_ok) probe_listen(p, res[p], bitrate);
        // Leave every module in reset (configuration mode: it cannot drive the bus).
        for (int p = 0; p < 3; p++) s_chip[p].reset();
    }
    for (const auto& r : res) {
        bool ok = r.spi_ok && r.int_ok && r.listen_ok && !r.note[0];
        if (ok)
            ESP_LOGI(TAG, "  port %-3s CS %d INT %d: OK (CANSTAT 0x%02X, %lu frames, %lu errors in 1 s)", r.name,
                     r.cs, r.int_gpio, r.canstat, (unsigned long)r.frames, (unsigned long)r.errors);
        else
            ESP_LOGE(TAG, "  port %-3s CS %d INT %d: FAIL: %s", r.name, r.cs, r.int_gpio, r.note);
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_probe, res, sizeof(res));
    s_probe_bitrate = bitrate;
    s_probe_us = esp_timer_get_time();
    s_probe_done = true;
    s_probe_running = false;
    portEXIT_CRITICAL(&s_mux);
    // Restore the tap and the INV/B ports.
    s_state = before;
    if (s_enabled && (before == State::ListenOnly || before == State::Error)) bring_up(s_bitrate);
    else if (!s_enabled) shut_down();
    aux_restore();
}

// ---- INT-line mapping (BENCH ONLY) -----------------------------------------------------
// See can.h. Uses the same commands as the hardware check (RESET, READ, WRITE of CNF/
// CANINTE/CANINTF/CANCTRL=listen-only, BIT MODIFY of CANINTF/EFLG, READ RX BUFFER), all
// through the no-transmit guard. Only the frames the bench script sends are used.

// Candidates: the pins.h INT pins first, then every GPIO the INT lines were ever wired or
// planned to: the approved map (34/35/36), GPIO39 (SENSOR_VN, next to 36 on the header),
// and the free pins listed in pins.h and the wiring sheet. Only read, never driven.
constexpr int kIntMapExtra[] = {34, 35, 36, 39, 32, 33, 4, 13, 14, 16, 17, 21, 22};
constexpr int64_t kIntMapWaitUs = 20000000;  // per module, for kIntMapRounds test frames
constexpr uint32_t kTestIdLo = 0x100, kTestIdHi = 0x1FF;  // tools/bench_can_check.py

bool s_intmap_running = false, s_intmap_done = false;
int s_intmap_port = -1;
uint32_t s_intmap_bitrate = 0;
int64_t s_intmap_us = 0;
int s_ncand = 0;
int s_cand[kIntMapMaxCandidates];
IntMapPort s_intmap[3];

bool is_pins_h_int(int g) {
    for (const auto& k : kPorts)
        if (k.irq == g) return true;
    return false;
}

void intmap_build_candidates() {
    s_ncand = 0;
    auto add = [](int g) {
        for (int i = 0; i < s_ncand; i++)
            if (s_cand[i] == g) return;
        if (s_ncand < kIntMapMaxCandidates) s_cand[s_ncand++] = g;
    };
    for (const auto& k : kPorts) add(k.irq);
    for (int g : kIntMapExtra) add(g);
    // The extra pins become plain inputs (no pull: a pull-up would load the INT divider).
    for (int i = 0; i < s_ncand; i++) {
        if (is_pins_h_int(s_cand[i])) continue;  // already inputs (ensure_spi)
        gpio_config_t ic = {};
        ic.pin_bit_mask = 1ULL << s_cand[i];
        ic.mode = GPIO_MODE_INPUT;
        ic.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&ic);
    }
}

void intmap_release_candidates() {
    for (int i = 0; i < s_ncand; i++)
        if (!is_pins_h_int(s_cand[i]) && GPIO_IS_VALID_OUTPUT_GPIO(s_cand[i]))
            gpio_reset_pin((gpio_num_t)s_cand[i]);
}

// Bit i set = candidate i reads low (majority of 5 reads: GPIO36/39 glitch for ~80 ns).
uint32_t intmap_sample() {
    uint32_t low = 0;
    for (int i = 0; i < s_ncand; i++) {
        int lows = 0;
        for (int k = 0; k < 5; k++) {
            lows += gpio_get_level((gpio_num_t)s_cand[i]) == 0;
            esp_rom_delay_us(2);
        }
        if (lows >= 3) low |= 1u << i;
    }
    return low;
}

void intmap_note(IntMapPort& r, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void intmap_note(IntMapPort& r, const char* fmt, ...) {
    if (r.note[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.note, sizeof(r.note), fmt, ap);
    va_end(ap);
}

bool is_test_frame(const Record& rec) {
    if (rec.id & (kIdExtended | kIdRtr)) return false;
    uint32_t id = rec.id & kIdMask;
    return id >= kTestIdLo && id <= kTestIdHi;
}

void intmap_one(int p, IntMapPort& r, uint32_t bitrate) {
    Mcp2515& m = s_chip[p];
    ProbePort pr = {};
    pr.cs = kPorts[p].cs;
    probe_spi(p, pr);  // reset values + register read-back
    if (!pr.spi_ok) {
        r.result = IntMapResult::NoSpi;
        intmap_note(r, "%s", pr.note);
        m.reset();
        return;
    }
    const char* why = "";
    if (m.start_listen_only(bitrate, &why) != ESP_OK) {  // all valid frames (filters open)
        r.result = IntMapResult::NoSpi;
        intmap_note(r, "listen-only: %s", why);
        m.reset();
        return;
    }
    m.write_reg(CANINTE, INT_RX0 | INT_RX1);  // INT follows the RX flags only
    m.write_reg(CANINTF, 0);
    uint32_t seen_mask = 0;     // union of matches over all rounds
    uint32_t inconclusive = 0;  // rounds discarded because another frame arrived mid-round
    int64_t deadline = esp_timer_get_time() + kIntMapWaitUs;
    while (r.rounds < kIntMapRounds && esp_timer_get_time() < deadline) {
        esp_task_wdt_reset();
        // 1. Baseline: no RX flag pending -> INT must be high.
        m.bit_modify(CANINTF, INT_RX0 | INT_RX1 | INT_MERR | INT_ERR, 0);
        m.bit_modify(EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0);
        uint32_t low0 = intmap_sample();
        uint8_t intf = m.read_reg(CANINTF);
        if (intf == 0xFF) {
            intmap_note(r, "module stopped answering over SPI during the mapping");
            break;
        }
        if (intf & (INT_RX0 | INT_RX1)) {  // a frame arrived during the baseline: again
            inconclusive++;
            continue;
        }
        // 2. Wait for a frame (the RX flag sets INT low).
        uint8_t rx = 0;
        while (esp_timer_get_time() < deadline) {
            intf = m.read_reg(CANINTF);
            if (intf & INT_MERR) {
                r.rx_errors++;
                m.bit_modify(CANINTF, INT_MERR, 0);
            }
            rx = intf & (INT_RX0 | INT_RX1);
            if (rx) break;
            esp_task_wdt_reset();
            vTaskDelay(1);
        }
        if (!rx) break;
        esp_rom_delay_us(10);
        uint32_t low1 = intmap_sample();
        // 3. Read the frame(s): clears the flags, so INT must go high again.
        bool test = false;
        for (int b = 0; b < 2; b++) {
            Record rec;
            if (!(rx & (1 << b)) || m.read_rx(b, rec) != ESP_OK) continue;
            if (is_test_frame(rec)) {
                if (!test && r.rounds == 0) r.first_id = rec.id & kIdMask;
                test = true;
            } else {
                r.foreign++;
            }
        }
        uint32_t low2 = intmap_sample();
        if (m.read_reg(CANINTF) & (INT_RX0 | INT_RX1)) {  // next frame already in: no clean "high"
            inconclusive++;
            continue;
        }
        if (!test) continue;  // only the bench script's frames count
        uint32_t match = ~low0 & low1 & ~low2 & ((1u << s_ncand) - 1);
        int16_t g = -1;
        if (match && !(match & (match - 1))) g = (int16_t)s_cand[__builtin_ctz(match)];
        else if (match) g = -2;
        seen_mask |= match;
        r.per_round[r.rounds++] = g;
        vTaskDelay(pdMS_TO_TICKS(20));  // next round on a later frame
    }
    m.reset();  // back to configuration mode: INT released, nothing received
    if (r.note[0]) {
        r.result = IntMapResult::NoSpi;
        return;
    }
    if (r.rounds == 0) {
        r.result = IntMapResult::NoFrames;
        if (r.foreign)
            intmap_note(r, "only frames outside 0x100-0x1FF (%lu): not the bench script's; is this a bench bus?",
                        (unsigned long)r.foreign);
        else if (r.rx_errors)
            intmap_note(r, "no test frame, but %lu receive errors: bitrate/crystal mismatch or bus wiring",
                        (unsigned long)r.rx_errors);
        else
            intmap_note(r, "no test frame in %d s (%lu inconclusive): bench script not sending, or no RX path",
                        (int)(kIntMapWaitUs / 1000000), (unsigned long)inconclusive);
        return;
    }
    bool same = true;
    for (int i = 1; i < r.rounds; i++) same &= r.per_round[i] == r.per_round[0];
    if (seen_mask == 0) {
        r.result = IntMapResult::NoInt;
        intmap_note(r, "%d test frames received, but no candidate GPIO went low and back", r.rounds);
    } else if (same && r.per_round[0] >= 0 && r.rounds == kIntMapRounds) {
        r.result = IntMapResult::Ok;
        r.found = r.per_round[0];
    } else if (same && r.per_round[0] == -2) {
        r.result = IntMapResult::MultiInt;
        r.multi_mask = seen_mask;
        char list[48] = "";
        for (int i = 0; i < s_ncand; i++)
            if (seen_mask & (1u << i)) {
                size_t l = strlen(list);
                snprintf(list + l, sizeof(list) - l, "%s%d", l ? "," : "", s_cand[i]);
            }
        intmap_note(r, "INT pulls several GPIOs low together: %s (lines shorted?)", list);
    } else {
        r.result = IntMapResult::Inconsistent;
        r.multi_mask = seen_mask;
        intmap_note(r, "the matching GPIO differs between rounds%s (loose wire or floating pin?)",
                    r.rounds < kIntMapRounds ? ", and not all rounds completed" : "");
    }
}

const char* intmap_hand_checks(IntMapResult res) {
    switch (res) {
        case IntMapResult::NoSpi:
            return "check by hand: module 5 V power; level shifter OE pin high; CS pull-up (10k to 3.3 V); "
                   "SCK/MOSI/MISO through the shifter; crystal marking (8.000 MHz expected)";
        case IntMapResult::NoFrames:
            return "check by hand: CAN-H/CAN-L to this module; termination (~60 ohm across the pair, power off); "
                   "crystal marking (8.000 MHz expected: another crystal means another bitrate); bench script running";
        case IntMapResult::NoInt:
            return "check by hand: INT pin on the module to its divider (1k over 2k) to the GPIO; divider ground; "
                   "the GPIO it lands on (only the candidate pins were scanned)";
        case IntMapResult::MultiInt:
            return "check by hand: INT lines shorted together or two dividers sharing a node (never wire-OR INT)";
        case IntMapResult::Inconsistent:
            return "check by hand: loose INT wire or divider joint; re-run the mapping";
        default:
            return "";
    }
}

void run_intmap() {
    State before = s_state;
    uint32_t bitrate = cfg::get().can_bitrate;
    IntMapPort res[3] = {};
    for (int p = 0; p < 3; p++) {
        strlcpy(res[p].name, kPorts[p].name, sizeof(res[p].name));
        res[p].cs = kPorts[p].cs;
        res[p].int_pins_h = kPorts[p].irq;
        res[p].found = -1;
        for (auto& g : res[p].per_round) g = -1;
    }
    portENTER_CRITICAL(&s_mux);
    s_intmap_running = true;
    s_intmap_port = -1;
    memcpy(s_intmap, res, sizeof(res));
    portEXIT_CRITICAL(&s_mux);
    s_state = State::Mapping;
    if (!ensure_spi()) {
        for (auto& r : res) {
            r.result = IntMapResult::NoSpi;
            intmap_note(r, "SPI bus init failed");
        }
    } else {
        intmap_build_candidates();
        char cl[80] = "";
        for (int i = 0; i < s_ncand; i++) {
            size_t l = strlen(cl);
            snprintf(cl + l, sizeof(cl) - l, "%s%d", i ? "," : "", s_cand[i]);
        }
        ESP_LOGW(TAG, "INT mapping (BENCH ONLY, no transmit, %lu bit/s): waiting for test frames 0x100-0x1FF "
                      "from tools/bench_can_check.py; candidates GPIO %s", (unsigned long)bitrate, cl);
        for (int p = 0; p < 3; p++) s_chip[p].reset();  // all modules silent and INT released
        for (int p = 0; p < 3; p++) {
            portENTER_CRITICAL(&s_mux);
            s_intmap_port = p;
            portEXIT_CRITICAL(&s_mux);
            intmap_one(p, res[p], bitrate);
            portENTER_CRITICAL(&s_mux);
            s_intmap[p] = res[p];
            portEXIT_CRITICAL(&s_mux);
        }
        for (int p = 0; p < 3; p++) s_chip[p].reset();
        intmap_release_candidates();
    }
    // Report. pins.h is never changed here.
    ESP_LOGI(TAG, "INT map result (module -> CS -> INT found vs INT in pins.h):");
    ESP_LOGI(TAG, "  module  CS  found  pins.h  rounds  result");
    bool differ = false, all_ok = true;
    for (const auto& r : res) {
        char found[12] = "-";
        if (r.found >= 0) snprintf(found, sizeof(found), "%d", r.found);
        char rounds[48] = "";
        for (int i = 0; i < r.rounds; i++) {
            size_t l = strlen(rounds);
            int16_t g = r.per_round[i];
            if (g >= 0) snprintf(rounds + l, sizeof(rounds) - l, "%s%d", i ? "," : "", g);
            else snprintf(rounds + l, sizeof(rounds) - l, "%s%s", i ? "," : "", g == -1 ? "none" : "several");
        }
        bool ok = r.result == IntMapResult::Ok;
        all_ok &= ok;
        differ |= ok && r.found != r.int_pins_h;
        ESP_LOGI(TAG, "  %-6s  %2d  %5s  %6d  %d/%d [%s]  %s%s%s", r.name, r.cs, found, r.int_pins_h, r.rounds,
                 kIntMapRounds, rounds, intmap_result_name(r.result),
                 ok ? (r.found == r.int_pins_h ? " (matches pins.h)" : " (DIFFERS from pins.h)") : ": ",
                 ok ? "" : r.note);
        if (!ok) ESP_LOGW(TAG, "    %s: %s", r.name, intmap_hand_checks(r.result));
    }
    if (differ) {
        ESP_LOGW(TAG, "INT wiring differs from pins.h. Proposed (NOT applied, pins.h unchanged):");
        for (const auto& r : res)
            if (r.result == IntMapResult::Ok)
                ESP_LOGW(TAG, "  #define BMS_CAN_%s_INT %d   (now %d)", r.name, r.found, r.int_pins_h);
    } else if (all_ok) {
        ESP_LOGI(TAG, "INT wiring matches pins.h for all three modules");
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_intmap, res, sizeof(res));
    s_intmap_bitrate = bitrate;
    s_intmap_us = esp_timer_get_time();
    s_intmap_port = -1;
    s_intmap_done = true;
    s_intmap_running = false;
    portEXIT_CRITICAL(&s_mux);
    // Restore the tap and the INV/B ports.
    s_state = before;
    if (s_enabled && (before == State::ListenOnly || before == State::Error)) bring_up(s_bitrate);
    else if (!s_enabled) shut_down();
    aux_restore();
}

void apply() {
    cfg::Settings s = cfg::get();
    // Ports INV and B: (re)start when enabled or when the bitrate changed.
    const bool en[3] = {s.can_inv_enabled, false, s.can_b_enabled};
    const uint32_t br[3] = {s.can_inv_bitrate, 0, s.can_b_bitrate};
    // INV transmits only with agg_tx_enabled (confirmed web action); B ACKs only with can_b_ack.
    const uint8_t md[3] = {(uint8_t)(s.agg_tx_enabled ? (MODE_NORMAL | CANCTRL_OSM) : MODE_LISTEN_ONLY), 0,
                           (uint8_t)(s.can_b_ack ? MODE_NORMAL : MODE_LISTEN_ONLY)};
    s_tx_enabled = s.agg_tx_enabled;
    for (int p : kAuxPorts) {
        Aux& a = s_aux[p];
        bool changed = a.enabled != en[p] || a.bitrate != br[p] || a.mode != md[p];
        if (changed) a.fallback = false;  // a settings change retries ACK mode
        a.enabled = en[p];
        a.bitrate = br[p];
        a.mode = md[p];
        if (!a.enabled) {
            if (a.state != State::Off) ESP_LOGI(TAG, "port %s disabled", kPorts[p].name);
            aux_down(p);
        } else if (changed || !aux_running(a)) {
            aux_up(p);
        }
    }
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
    run_probe();  // boot-time hardware check (log only; the tap does not depend on it)
    apply();
    int64_t next_check = 0, next_fps = esp_timer_get_time() + 1000000;
    for (;;) {
        esp_task_wdt_reset();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        Cmd c;
        while (xQueueReceive(s_cmd, &c, 0) == pdTRUE) {
            if (c == CMD_APPLY) apply();
            else if (c == CMD_DETECT) run_detect();
            else if (c == CMD_PROBE) run_probe();
            else if (c == CMD_INTMAP) run_intmap();
        }
        int64_t now = esp_timer_get_time();
        if (s_state == State::ListenOnly) {
            service(BMS_CAN_PORT_A, false, nullptr);  // also polls, in case an INT edge was missed
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
                    s_next_retry = now + 2000000;
                }
            }
        } else if (s_state == State::Error && s_enabled && now >= s_next_retry) {
            s_next_retry = now + 5000000;
            bring_up(s_bitrate);
        }
        // Ports INV and B (only while no check/detection owns the modules: those run
        // synchronously in this task, so here the modules are always ours).
        for (int p : kAuxPorts) {
            Aux& a = s_aux[p];
            if (aux_running(a)) {
                service(p, false, nullptr);
                if (now >= a.next_check) {
                    a.next_check = now + 1000000;
                    aux_check(p, now);
                }
            } else if (a.state == State::Error && a.enabled && now >= a.next_retry) {
                a.next_retry = now + 5000000;
                aux_up(p);
            }
        }
        if (s_tick) s_tick();  // aggregator: staleness and output, every loop (<= 20 ms)
        tx_cycle(now);         // port INV: inverter output, about once a second
        if (now >= next_fps) {
            next_fps = now + 1000000;
            s_fps = s_fps_count;
            s_fps_count = 0;
            for (int p : kAuxPorts) {
                s_aux[p].fps = s_aux[p].fps_count;
                s_aux[p].fps_count = 0;
            }
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
        case State::Checking: return "checking";
        case State::Mapping: return "mapping INT";
        case State::AckOnly: return "normal (ACK only)";
        case State::Transmitting: return "transmitting";
    }
    return "?";
}

void start() {
    s_cmd = xQueueCreate(4, sizeof(Cmd));
    // Core 1, high priority: WiFi and the web server run on core 0.
    xTaskCreatePinnedToCore(task, "can", 6144, nullptr, 18, &s_task, 1);  // 6 KB: INT-map report
}

void apply_settings() {
    Cmd c = CMD_APPLY;
    if (s_cmd) xQueueSend(s_cmd, &c, 0);
}

const char* intmap_result_name(IntMapResult r) {
    switch (r) {
        case IntMapResult::NotRun: return "not run";
        case IntMapResult::Ok: return "OK";
        case IntMapResult::NoSpi: return "no SPI response";
        case IntMapResult::NoFrames: return "no test frame";
        case IntMapResult::NoInt: return "no INT";
        case IntMapResult::MultiInt: return "several GPIOs";
        case IntMapResult::Inconsistent: return "inconsistent";
    }
    return "?";
}

esp_err_t start_probe() {
    if (s_probe_running || s_detect_running || s_intmap_running) return ESP_ERR_INVALID_STATE;
    Cmd c = CMD_PROBE;
    return (s_cmd && xQueueSend(s_cmd, &c, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t start_intmap() {
    if (s_probe_running || s_detect_running || s_intmap_running) return ESP_ERR_INVALID_STATE;
    Cmd c = CMD_INTMAP;
    return (s_cmd && xQueueSend(s_cmd, &c, 0) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t start_detect() {
    if (s_detect_running || s_probe_running || s_intmap_running) return ESP_ERR_INVALID_STATE;
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
    st.probe_running = s_probe_running;
    st.probe_done = s_probe_done;
    st.probe_bitrate = s_probe_bitrate;
    st.probe_age_s = s_probe_done ? (esp_timer_get_time() - s_probe_us) / 1000000 : -1;
    memcpy(st.probe, s_probe, sizeof(s_probe));
    for (int p = 0; p < 3; p++) {
        PortStatus& ps = st.ports[p];
        strlcpy(ps.name, kPorts[p].name, sizeof(ps.name));
        if (p == BMS_CAN_PORT_A) {
            ps.enabled = s_enabled, ps.state = s_state, ps.bitrate = s_bitrate, ps.frames = s_frames;
            ps.fps = s_fps, ps.bus_errors = s_merr, ps.rx_overflows = s_ovr, ps.rec = s_rec;
            ps.last_frame_age_ms = s_last_frame_us ? (esp_timer_get_time() - (int64_t)s_last_frame_us) / 1000 : -1;
            memcpy(ps.error, s_error, sizeof(ps.error));
            ps.tec = 0, ps.tx_frames = 0, ps.tx_failed = 0, ps.fallback = false;  // listen-only: never transmits
        } else {
            const Aux& a = s_aux[p];
            ps.enabled = a.enabled, ps.state = a.state, ps.bitrate = a.bitrate, ps.frames = a.frames;
            ps.fps = a.fps, ps.bus_errors = a.merr, ps.rx_overflows = a.ovr, ps.rec = a.rec;
            ps.last_frame_age_ms = a.last_us ? (esp_timer_get_time() - (int64_t)a.last_us) / 1000 : -1;
            memcpy(ps.error, a.error, sizeof(ps.error));
            ps.tec = a.tec, ps.tx_frames = a.tx_frames, ps.tx_failed = a.tx_failed, ps.fallback = a.fallback;
        }
    }
    st.intmap_running = s_intmap_running;
    st.intmap_done = s_intmap_done;
    st.intmap_port = s_intmap_port;
    st.intmap_bitrate = s_intmap_bitrate;
    st.intmap_age_s = s_intmap_done ? (esp_timer_get_time() - s_intmap_us) / 1000000 : -1;
    st.intmap_ncand = s_ncand;
    memcpy(st.intmap_cand, s_cand, sizeof(s_cand));
    memcpy(st.intmap, s_intmap, sizeof(s_intmap));
    portEXIT_CRITICAL(&s_mux);
    st.error = err;
    st.detect_note = note;
    st.tx_enabled = s_tx_enabled;
    st.blocked_cmds = 0;
    for (const auto& c : s_chip) st.blocked_cmds += c.blocked_commands();
    return st;
}

bool selftest_ok(const char** why) {
    // Every enabled INV/B port must be up in its operating mode ("CAN controllers initialised").
    for (int p : kAuxPorts)
        if (s_aux[p].enabled && !aux_running(s_aux[p])) {
            *why = p == BMS_CAN_PORT_B ? "CAN port B enabled but not running" : "CAN port INV enabled but not running";
            return false;
        }
    if (!cfg::get().can_enabled) return true;
    // Not while Checking: the self-test waits until the tap is back in listen-only.
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
        out[n].port = s_stats[i].port;
        out[n].flags = kFlagSnapshot;
        memcpy(out[n].data, s_stats[i].data, 8);
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void set_sink(Sink s) { s_sink = s; }
void set_agg_hooks(FrameHook on_frame, void (*tick)(), void (*ota)(bool), TxSource tx) {
    s_tx = tx;
    s_hook = on_frame;
    s_tick = tick;
    s_ota_hook = ota;
}

}  // namespace can
