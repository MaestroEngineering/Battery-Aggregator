#pragma once
// GET /api/agg/status body, written by the streaming JsonOut (no cJSON tree: the 0.1.7 tree
// needed ~49 KB of heap per request and failed on the device). Pure C++, host-tested by
// tools/test_status_json.cpp. Reporting only: nothing here feeds the inverter output.
#include <stddef.h>
#include <stdint.h>

#include "agg/agg.h"
#include "json_stream.h"

namespace webserver {

// Values from outside the aggregator, filled by the handler.
struct AggStatusExtras {
    bool tx_enabled = false, inv_enabled = false, sending = false;
    const char* inv_state = "";
    uint64_t tx_frames = 0;
    uint32_t tx_failed = 0, tec = 0, blocked_cmds = 0;
    int64_t epoch_offset_us = 0;  // 0 = not synced
    const char* mqtt_state = "";
    uint32_t mqtt_published = 0, mqtt_events_sent = 0;
    int64_t mqtt_last_publish_age_s = -1;
};

// Everything one request needs from the aggregator, copied under its lock (~5 KB, heap).
struct AggStatusData {
    agg::Snapshot snap;
    agg::Event ev[agg::kEventRing];  // oldest first
    size_t nev = 0;
};

// Writes the whole object (begin .. end). Returns false if the sink failed.
bool write_agg_status(JsonOut& j, const AggStatusData& d, const AggStatusExtras& x, uint64_t now_ms);

}  // namespace webserver
