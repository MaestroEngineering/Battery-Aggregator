#pragma once
// Phase 2 CAN tap: one MCP2515 (port A) in listen-only mode.
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

enum class State { Off, ListenOnly, Error, Detecting };
const char* state_name(State s);

struct DetectResult {
    uint32_t bitrate;
    uint32_t frames;
    uint32_t errors;
};

struct Status {
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
    uint32_t blocked_cmds;   // SPI commands refused by the no-transmit guard (should be 0)
    int64_t last_frame_age_ms;  // -1 if none
    // Bitrate detection.
    bool detect_running;
    int detect_count;
    DetectResult detect[3];
    uint32_t detect_suggestion;  // 0 = none
    std::string detect_note;
};

void start();           // creates the CAN task; reads settings
void apply_settings();  // after can_enabled / can_bitrate changed
Status status();
// For the post-OTA self-test: true if the tap is disabled or listen-only is up.
bool selftest_ok(const char** why);
esp_err_t start_detect();  // listen-only bitrate detection (async)

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

}  // namespace can
