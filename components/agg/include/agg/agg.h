#pragma once
// Firmware side of the aggregator (thread-safe wrapper around agg::Engine).
// Source A = port A (Blue Nova), source B = port B (KNY). Either source alone, or both,
// can be enabled. The inverter output is computed every 100 ms; port INV sends it
// (about once a second) only while the confirmed agg_tx_enabled setting is on.
#include <stddef.h>
#include <stdint.h>

#include "agg/engine.h"
#include "can/can.h"
#include "can/frame.h"

namespace agg {

void start();           // creates the engine and reads the settings
void apply_settings();  // after the aggregator settings changed

// Hooks for the CAN task (can::set_agg_hooks).
void on_can_frame(const can::Record& r);
void tick();
void set_ota(bool active);
// can::TxSource: the current output frames; false = silent (nothing may be sent).
bool tx_frames(can::TxFrame* out, int max, int* n);

struct Snapshot {
    Output out;
    Source src[kSources];
    Config cfg;
    uint32_t event_seq;
    uint32_t malformed[kSources];  // extended/remote frames on port A / B (dropped before the engine)
};
void snapshot(Snapshot& s);
size_t events_since(uint32_t after, Event* out, size_t max);
void clear_trips();

}  // namespace agg
