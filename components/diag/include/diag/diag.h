#pragma once
#include <stdint.h>

#include <string>

#include "can/frame.h"

namespace diag {

constexpr int kSlots = 3;           // batch buffers: one filling, the rest waiting/uploading
constexpr int kMaxBatch = 500;      // frames per buffer (SPEC.md)

void start();           // low-priority uploader task
void apply_settings();  // after diag/trigger settings changed
// CAN sink (CAN task context; never blocks).
void on_frame(const can::Record& r, bool changed);
// Raise an event by hand (tests the immediate upload and pre-trigger capture).
void test_event();
// Re-arm the trigger after it fired.
void clear_trigger();

struct Status {
    bool enabled, active;
    const char* state;  // off | idle | uploading | retrying
    uint32_t batches_ok, batches_failed;
    int last_http_status;
    std::string last_error;
    int64_t last_ok_age_s;  // -1 never
    uint32_t pending_batches, pending_frames;
    uint32_t dropped_total, events_total;
    uint32_t next_seq, boot_id;
    bool trigger_latched;
    int64_t last_event_age_s;
    uint32_t last_event_id;
    uint8_t last_event_data[8];
    uint8_t last_event_dlc;
};
Status status();

}  // namespace diag
