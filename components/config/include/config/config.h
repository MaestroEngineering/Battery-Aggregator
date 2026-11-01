#pragma once
// Persistent settings and secrets (NVS) plus the LittleFS mount.
//
// Secrets (WiFi password, recovery-AP password, admin password hash) live only
// in NVS. The admin password is stored as PBKDF2-HMAC-SHA256 with a random
// salt, never in plain text.
#include <stddef.h>
#include <string>

#include "esp_err.h"

namespace cfg {

constexpr const char* kFsMount = "/littlefs";
constexpr const char* kFsLabel = "littlefs";

constexpr size_t kMinAdminPassword = 8;
constexpr size_t kMinApPassword = 8;  // WPA2 minimum

struct Settings {
    std::string hostname = "bms-agg";        // mDNS: <hostname>.local
    std::string ntp_server = "pool.ntp.org";
    std::string timezone = "SAST-2";         // POSIX TZ string
    // OTA signature verification (hook only in Phase 1, see ota.cpp).
    // When true, unsigned uploads are rejected; Phase 1 cannot verify
    // signatures yet, so turning it on rejects every upload (fail closed).
    bool require_signed_images = false;
};

// Opens NVS (recovering it if its layout is unusable), loads settings and
// mounts LittleFS (formatting it if it cannot be mounted).
esp_err_t init();

bool loaded_ok();     // init() succeeded and settings were read
bool fs_mounted();
bool is_provisioned();  // setup completed (WiFi + admin password stored)

Settings get();
esp_err_t set_hostname(const std::string& v);
esp_err_t set_ntp(const std::string& server, const std::string& tz);
esp_err_t set_require_signed_images(bool v);

std::string wifi_ssid();
bool wifi_credentials(std::string& ssid, std::string& pass);
esp_err_t set_wifi(const std::string& ssid, const std::string& pass);
std::string ap_password();

// First-boot provisioning: stores everything, then the "provisioned" flag last.
esp_err_t provision(const std::string& ssid, const std::string& wifi_pass,
                    const std::string& ap_pass, const std::string& admin_pass);

bool check_admin_password(const std::string& pw);  // constant-time compare
esp_err_t set_admin_password(const std::string& pw);

// Erases NVS and formats LittleFS. Does not reboot; caller must esp_restart().
esp_err_t factory_reset();

// Validators (also used by the web API).
bool valid_hostname(const std::string& v);
bool valid_ntp_server(const std::string& v);
bool valid_timezone(const std::string& v);
bool valid_ssid(const std::string& v);
bool valid_wifi_password(const std::string& v);  // "" (open) or 8..63

}  // namespace cfg
