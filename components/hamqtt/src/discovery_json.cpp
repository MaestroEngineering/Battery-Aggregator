#include "discovery_json.h"

#include "cJSON.h"

namespace hamqtt {

char* discovery_payload(const DiscoveryInfo& d) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", d.name);
    cJSON_AddStringToObject(o, "unique_id", d.unique_id);
    cJSON_AddStringToObject(o, "state_topic", d.state_topic);
    cJSON_AddStringToObject(o, "availability_topic", d.availability_topic);
    if (d.unit) cJSON_AddStringToObject(o, "unit_of_measurement", d.unit);
    if (d.dev_class) cJSON_AddStringToObject(o, "device_class", d.dev_class);
    if (d.state_class) cJSON_AddStringToObject(o, "state_class", d.state_class);
    cJSON* dev = cJSON_AddObjectToObject(o, "device");
    cJSON* ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(d.node));
    cJSON_AddStringToObject(dev, "name", "BMS Aggregator");
    cJSON_AddStringToObject(dev, "model", "bms-agg");
    cJSON_AddStringToObject(dev, "sw_version", d.sw_version);
    char* p = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return p;
}

}  // namespace hamqtt
