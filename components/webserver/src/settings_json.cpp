#include "settings_json.h"

#include <stdio.h>

namespace webserver {

cJSON* settings_json_build(const cfg::Settings& s, const SettingsExtras& x, bool for_export) {
    cJSON* o = cJSON_CreateObject();
    if (for_export) {
        cJSON_AddStringToObject(o, "schema", "bms-agg-config");
        cJSON_AddNumberToObject(o, "schema_version", 1);
        cJSON_AddStringToObject(o, "firmware", x.firmware);
        cJSON_AddStringToObject(o, "exported_at", x.exported_at);
    }
    cJSON* sys = cJSON_AddObjectToObject(o, "system");
    cJSON_AddStringToObject(sys, "hostname", s.hostname.c_str());
    cJSON_AddStringToObject(sys, "ntp_server", s.ntp_server.c_str());
    cJSON_AddStringToObject(sys, "timezone", s.timezone.c_str());
    cJSON* ota = cJSON_AddObjectToObject(o, "ota");
    cJSON_AddBoolToObject(ota, "require_signed_images", s.require_signed_images);
    cJSON* can = cJSON_AddObjectToObject(o, "can");
    cJSON_AddBoolToObject(can, "enabled", s.can_enabled);
    cJSON_AddNumberToObject(can, "bitrate", s.can_bitrate);
    cJSON* ci = cJSON_AddObjectToObject(o, "can_inv");
    cJSON_AddBoolToObject(ci, "enabled", s.can_inv_enabled);
    cJSON_AddNumberToObject(ci, "bitrate", s.can_inv_bitrate);
    cJSON* cb = cJSON_AddObjectToObject(o, "can_b");
    cJSON_AddBoolToObject(cb, "enabled", s.can_b_enabled);
    cJSON_AddNumberToObject(cb, "bitrate", s.can_b_bitrate);
    cJSON_AddBoolToObject(cb, "ack", s.can_b_ack);
    cJSON* ag = cJSON_AddObjectToObject(o, "agg");
    cJSON_AddBoolToObject(ag, "src_a", s.agg_src_a);
    cJSON_AddBoolToObject(ag, "src_b", s.agg_src_b);
    cJSON_AddNumberToObject(ag, "kny_capacity_ah", s.agg_kny_cap_ah);
    cJSON_AddNumberToObject(ag, "failsafe_cvl", s.agg_fs_cvl_dv / 10.0);
    cJSON_AddNumberToObject(ag, "failsafe_dvl", s.agg_fs_dvl_dv / 10.0);
    cJSON_AddNumberToObject(ag, "ccl_max", s.agg_ccl_max);
    cJSON_AddNumberToObject(ag, "dcl_max", s.agg_dcl_max);
    cJSON_AddBoolToObject(ag, "silent", s.agg_silent);
    cJSON_AddNumberToObject(ag, "startup_hold_s", s.agg_hold_s);
    cJSON_AddNumberToObject(ag, "cvl_hold_s", s.agg_cvl_hold_s);
    cJSON_AddNumberToObject(ag, "cap_a", s.agg_cap_a);
    cJSON_AddNumberToObject(ag, "cap_a_pack2", s.agg_cap_a_p2);
    cJSON_AddNumberToObject(ag, "cap_b", s.agg_cap_b);
    cJSON* mq = cJSON_AddObjectToObject(o, "mqtt");
    cJSON_AddBoolToObject(mq, "enabled", s.mqtt_enabled);
    cJSON_AddStringToObject(mq, "host", s.mqtt_host.c_str());
    cJSON_AddNumberToObject(mq, "port", s.mqtt_port);
    cJSON_AddStringToObject(mq, "user", s.mqtt_user.c_str());
    cJSON_AddStringToObject(mq, "base", s.mqtt_base.c_str());
    cJSON_AddBoolToObject(mq, "discovery", s.mqtt_discovery);
    cJSON_AddNumberToObject(mq, "interval_s", s.mqtt_interval_s);
    cJSON_AddBoolToObject(mq, "retain", s.mqtt_retain);
    cJSON* dg = cJSON_AddObjectToObject(o, "diag");
    cJSON_AddBoolToObject(dg, "enabled", s.diag_enabled);
    cJSON_AddStringToObject(dg, "url", s.diag_url.c_str());
    cJSON_AddNumberToObject(dg, "batch", s.diag_batch);
    cJSON_AddNumberToObject(dg, "flush_s", s.diag_flush_s);
    cJSON_AddBoolToObject(dg, "change_only", s.diag_change_only);
    cJSON_AddNumberToObject(dg, "pretrigger_s", s.diag_pretrig_s);
    cJSON* tr = cJSON_AddObjectToObject(o, "trigger");
    cJSON_AddBoolToObject(tr, "enabled", s.trig_enabled);
    char id[16];
    snprintf(id, sizeof(id), "0x%lX", (unsigned long)s.trig_id);
    cJSON_AddStringToObject(tr, "id", id);
    cJSON_AddStringToObject(tr, "baseline", s.trig_baseline.c_str());
    if (!for_export) {
        // Never exported: the token is a secret. Only whether one is set.
        cJSON_AddBoolToObject(dg, "token_set", x.token_set);
        cJSON_AddBoolToObject(mq, "password_set", x.password_set);
        cJSON* w = cJSON_AddObjectToObject(o, "wifi");
        cJSON_AddStringToObject(w, "ssid", x.wifi_ssid);
    }
    return o;
}

}  // namespace webserver
