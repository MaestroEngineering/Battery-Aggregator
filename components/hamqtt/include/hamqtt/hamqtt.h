#pragma once
// MQTT publisher with Home Assistant discovery. Topics (SPEC.md proposal):
//   <base>/availability             online / offline (last will), retained
//   <base>/state, <base>/reason     gateway output state
//   <base>/merged/<field>           combined values
//   <base>/battery_a/<field>        Blue Nova (port A), incl. pack2_* fields
//   <base>/battery_b/<field>        KNY (port B)
//   <base>/events                   aggregator events (JSON, not retained)
// Energy counters use state_class "total" (the Blue Nova's charged counter steps back
// at full SOC: total_increasing would read that as a meter reset).
#include <stdint.h>

namespace hamqtt {

void start();
void apply_settings();  // reconnects with the new settings

struct Status {
    const char* state;  // "off", "waiting for WiFi", "connecting", "connected", "error"
    uint32_t published;
    uint32_t events_sent;
    int64_t last_publish_age_s;  // -1 = never
};
Status status();

}  // namespace hamqtt
