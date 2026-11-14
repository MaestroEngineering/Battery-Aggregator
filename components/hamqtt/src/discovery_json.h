#pragma once
// One Home Assistant discovery payload (cJSON, printed and freed at once: ~1.5 KB peak, measured
// by tools/heap_model.cpp). Pure C++.
namespace hamqtt {
struct DiscoveryInfo {
    const char *name, *unique_id, *state_topic, *availability_topic;
    const char *unit, *dev_class, *state_class;  // nullptr = omitted
    const char *node, *sw_version;
};
// Returns the printed JSON (free with cJSON_free) or nullptr when out of memory.
char* discovery_payload(const DiscoveryInfo& d);
}  // namespace hamqtt
