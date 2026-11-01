#pragma once
// WiFi, mDNS and NTP.
//
// Modes:
//   Setup      - not provisioned: open AP "BMS-Agg-XXXX" + captive DNS; the
//                setup page is the only thing served without login.
//   Station    - joined the configured WiFi; AP off.
//   FallbackAP - provisioned, but WiFi failed repeatedly (5 failures or 30 s
//                without an IP): WPA2 AP comes back (recovery password) while
//                the station keeps retrying. Login is still required.
#include <stdint.h>
#include <string>

#include "esp_err.h"

namespace net {

enum class Mode { Setup, Station, FallbackAP };

esp_err_t start();

Mode mode();
const char* mode_name();
bool sta_connected();  // associated and has an IPv4 address
bool ap_up();
int rssi();  // dBm, 0 if not connected
std::string sta_ip();
std::string ap_ssid();
std::string connected_ssid();
bool time_synced();
// unix_us = esp_timer_get_time() + offset; 0 while the clock is not NTP-synced.
int64_t epoch_offset_us();

// --- Setup (first boot) -------------------------------------------------------
// Tries the given WiFi while the setup AP stays up. Only when the station gets
// an IP are the credentials and admin password persisted; then the AP turns
// off after a short grace period so the setup page can show the result.
esp_err_t begin_provisioning(const std::string& ssid, const std::string& wifi_pass,
                             const std::string& ap_pass, const std::string& admin_pass);
struct SetupStatus {
    const char* state;  // "idle" | "connecting" | "connected" | "failed"
    std::string detail;
    std::string ip;
};
SetupStatus setup_status();

// --- Runtime changes ------------------------------------------------------------
void reconnect();          // after WiFi credentials changed
void apply_hostname();     // after hostname changed
void apply_time_config();  // after NTP server / timezone changed

// Blocking scan (~2-3 s). Returns a JSON array string of {ssid, rssi, secure}.
std::string scan_json();

// Stop WiFi cleanly (before a reboot).
void stop();

}  // namespace net
