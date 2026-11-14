#pragma once
// GET /api/settings and the config export: a cJSON tree. Not polled (page load / export only);
// its heap use is measured by tools/test_status_json.cpp with maximum-length strings.
#include "cJSON.h"
#include "config/config.h"

namespace webserver {

struct SettingsExtras {
    const char* firmware = "";
    const char* exported_at = "";
    bool token_set = false, password_set = false;  // secrets: only whether one is set
    const char* wifi_ssid = "";
};

cJSON* settings_json_build(const cfg::Settings& s, const SettingsExtras& x, bool for_export);

}  // namespace webserver
