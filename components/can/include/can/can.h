#pragma once
// CAN ports. Port A is the listen-only tap (Blue Nova); it is hardware-silent. Ports INV
// and B are receivers with their own bitrate. Exceptions (SPI guard, mcp2515_regs.h):
//  - B may run in normal mode to acknowledge the KNY (setting can_b_ack); it never
//    sends a data frame (LOAD TX / RTS refused on B).
//  - INV may transmit the aggregator output (setting agg_tx_enabled, set only through
//    the confirmed web action): normal mode with one-shot, allow-listed IDs/DLCs only,
//    at most 1 Hz per ID.
//
// Frames go to (1) a RAM ring buffer (recent past, for the live capture stream
// and the device-buffer download), (2) a per-ID statistics table (live frame
// table) and (3) an optional sink (Verbose Diagnostics).
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "can/frame.h"
#include "esp_err.h"

namespace can {

enum class State { Off, ListenOnly, Error, Detecting, Checking, Mapping, AckOnly, Transmitting };
const char* state_name(State s);

struct DetectResult {
    uint32_t bitrate;
    uint32_t frames;
    uint32_t errors;
};

// Hardware check of all three MCP2515 modules (SPI, registers, CS isolation,
// INT line, and a short listen-only receive). It never transmits: it uses
// only configuration and listen-only modes, through the same SPI guard.
struct ProbePort {
    char name[4];      // "INV", "A", "B"
    int cs, int_gpio;
    bool spi_ok;       // reset values right, register read-back right, own CS only
    bool int_ok;       // INT idles high, goes low on an interrupt, on the right GPIO
    bool listen_ok;    // entered listen-only mode at the configured bitrate
    uint8_t canstat, canctrl;  // read right after reset (expect 0x80 / 0x87)
    uint32_t frames, errors;   // seen during the listen window (polled: a lower bound)
    char note[112];    // "" = all good
};

// INT-line mapping (BENCH ONLY; tools/bench_can_check.py --bench --tests 4 sends the
// frames). Each module in turn is put alone in listen-only mode (catch-all RX mask);
// every other module stays in reset. When a frame with an ID in 0x100-0x1FF has
// arrived, its RX interrupt is masked/unmasked (CANINTE) while every candidate GPIO
// is sampled: the INT pin is the one that is high, then low, then high again. This
// is repeated kIntMapRounds times. It never transmits (same SPI guard) and never
// changes pins.h: it only reports.
constexpr int kIntMapRounds = 5;
constexpr int kIntMapMaxCandidates = 16;
enum class IntMapResult : uint8_t {
    NotRun,      // module skipped (another failure first)
    Ok,          // the same single GPIO matched in every round
    NoSpi,       // no SPI answer / wrong reset values / read-back failed
    NoFrames,    // SPI fine, but no test frame arrived within the wait
    NoInt,       // frames arrived, but no candidate GPIO followed the interrupt
    MultiInt,    // more than one GPIO followed the interrupt (shorted INT lines?)
    Inconsistent // different GPIOs (or none) in different rounds
};
const char* intmap_result_name(IntMapResult r);
struct IntMapPort {
    char name[4];
    int cs, int_pins_h;           // from pins.h
    IntMapResult result;
    int found;                    // GPIO that matched in every round, -1 = none
    int rounds;                   // rounds completed (test frames used)
    int16_t per_round[kIntMapRounds];  // matching GPIO per round; -1 none, -2 several
    uint32_t multi_mask;          // candidate bits that matched in some round, when several did
    uint32_t foreign;             // frames with IDs outside 0x100-0x1FF (not from the bench script)
    uint32_t rx_errors;           // MERRF seen while waiting (bitrate/crystal mismatch?)
    uint32_t first_id;            // ID of the first test frame used
    char note[128];
};

struct PortStatus {  // per port: INV, A (the tap), B
    char name[4];
    bool enabled;
    State state;
    uint32_t bitrate;
    uint64_t frames;
    uint32_t fps, bus_errors, rx_overflows;
    uint8_t rec;
    int64_t last_frame_age_ms;
    char error[96];
    uint8_t tec;          // transmit error counter (INV/B in normal mode)
    uint64_t tx_frames;   // INV: frames sent (acknowledged)
    uint32_t tx_failed;   // INV: frames not acknowledged / aborted (one-shot: not retried)
    bool fallback;        // B: ACK mode dropped back to listen-only after errors
};

struct Status {
    PortStatus ports[3];  // indexed by BMS_CAN_PORT_*
    bool enabled;
    State state;
    std::string error;
    uint32_t bitrate;
    uint64_t frames_total;
    uint32_t fps;            // frames in the last second
    uint32_t bus_errors;     // MERRF: frames the controller could not receive
    uint32_t rx_overflows;   // RX buffer overruns (frames lost in the controller)
    uint8_t rec;             // receive error counter
    uint32_t ids;            // distinct IDs in the table
    uint32_t ids_dropped;    // frames whose ID did not fit in the table
    uint32_t blocked_cmds;   // SPI commands refused by the guards (should be 0)
    bool tx_enabled;         // agg_tx_enabled setting (INV transmit)
    int64_t last_frame_age_ms;  // -1 if none
    // Bitrate detection.
    bool detect_running;
    int detect_count;
    DetectResult detect[3];
    uint32_t detect_suggestion;  // 0 = none
    std::string detect_note;
    // Hardware check (runs at boot and on request).
    bool probe_running;
    bool probe_done;
    uint32_t probe_bitrate;
    int64_t probe_age_s;
    ProbePort probe[3];
    // INT-line mapping.
    bool intmap_running;
    bool intmap_done;
    int intmap_port;              // module being mapped (-1 = none)
    uint32_t intmap_bitrate;
    int64_t intmap_age_s;
    int intmap_ncand;
    int intmap_cand[kIntMapMaxCandidates];
    IntMapPort intmap[3];
};

void start();           // creates the CAN task; reads settings
void apply_settings();  // after can_enabled / can_bitrate changed
Status status();
// For the post-OTA self-test: true if the tap is disabled or listen-only is up.
bool selftest_ok(const char** why);
esp_err_t start_detect();  // listen-only bitrate detection (async)
esp_err_t start_probe();   // hardware check of all three modules (async, ~4 s)
esp_err_t start_intmap();  // INT-line mapping (async; up to ~20 s per module without frames)

// Ring buffer of recent frames. Sequence numbers count every frame ever stored.
constexpr size_t kRingSize = 1536;
uint32_t ring_head();
// Copies frames from `seq` on (at most `max`), advances `seq`. If `seq` is
// older than the ring, skips ahead and adds the skipped count to *dropped.
size_t ring_read(uint32_t& seq, Record* out, size_t max, uint32_t* dropped);
// Oldest sequence still in the ring.
uint32_t ring_tail();

// Per-ID statistics.
struct IdStats {
    uint32_t id;
    uint8_t port;  // BMS_CAN_PORT_*: the table is per (port, ID)
    uint32_t count;
    uint64_t last_ts_us;
    uint64_t last_change_us;
    uint32_t period_us;  // smoothed interval
    uint8_t dlc;
    uint8_t data[8], min[8], max[8];
};
constexpr size_t kMaxIds = 192;
// Copies entries [start, start+max). Returns the number copied (0 at the end).
size_t stats_copy(IdStats* out, size_t start, size_t max);
void stats_reset();
// Latest frame of every ID (for change-only snapshots).
size_t stats_latest(Record* out, size_t start, size_t max);

// Called from the CAN task for every received frame. `changed` is true for a
// new ID or when DLC/data differ from the previous frame with that ID.
// Must be fast and never block.
using Sink = void (*)(const Record& r, bool changed);
void set_sink(Sink s);
// Aggregator hooks, all called from the CAN task: every received frame (any port), a
// tick on every task loop (<= 20 ms), the OTA start/end (fail-safe rule 7), and the
// inverter output: `tx` fills up to `max` frames and returns false when the output is
// silent. It is asked about once a second, only while port INV transmits.
using FrameHook = void (*)(const Record& r);
struct TxFrame {
    uint16_t id;
    uint8_t dlc;
    uint8_t data[8];
};
using TxSource = bool (*)(TxFrame* out, int max, int* n);
void set_agg_hooks(FrameHook on_frame, void (*tick)(), void (*ota)(bool), TxSource tx);

}  // namespace can
