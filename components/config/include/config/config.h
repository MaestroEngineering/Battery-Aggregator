#pragma once
// Persistent settings and secrets (NVS) plus the LittleFS mount.
//
// Secrets (WiFi password, recovery-AP password, admin password hash) live only
// in NVS. The admin password is stored as PBKDF2-HMAC-SHA256 with a random
// salt, never in plain text.
#include <stddef.h>
#include <stdint.h>
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

    // CAN listen-only tap (Phase 2). Off by default: with no MCP2515 fitted the
    // post-OTA self-test would otherwise fail ("CAN controllers initialised").
    bool can_enabled = false;
    uint32_t can_bitrate = 250000;  // 125000 | 250000 | 500000 (8 MHz crystal)
    // Ports INV and B. Bitrates and modes are applied on change, without a reboot.
    bool can_inv_enabled = false;
    uint32_t can_inv_bitrate = 250000;  // inverter side (Sunsynk)
    bool can_b_enabled = false;
    uint32_t can_b_bitrate = 500000;    // KNY-P51300
    bool can_b_ack = false;             // B in normal mode: acknowledge the KNY (never sends data frames)

    // Aggregator (source A = Blue Nova on port A, source B = KNY on port B).
    bool agg_src_a = false, agg_src_b = false;
    uint32_t agg_kny_cap_ah = 300;      // the KNY sends no capacity frame
    uint32_t agg_fs_cvl_dv = 0;         // fail-safe CVL in 0.1 V; 0 = not set (output refused)
    uint32_t agg_fs_dvl_dv = 0;         // fail-safe DVL in 0.1 V; 0 = not set
    uint32_t agg_ccl_max = 0;           // A, clamp on the summed CCL; 0 = not set (output refused)
    uint32_t agg_dcl_max = 0;           // A, same for DCL
    bool agg_silent = false;            // fail-safe action: false = fail-safe values, true = silent
    uint32_t agg_hold_s = 10;           // start-up / recovery hold (5-300 s)
    uint32_t agg_cvl_hold_s = 60;       // CVL rise hold (0-600 s)
    // Per-source user caps (A, both directions; 1-1000): cap each source's contribution and set the
    // over-current guard limit = min(own reported limit, cap).
    uint32_t agg_cap_a = 100;           // Blue Nova bank
    uint32_t agg_cap_a_p2 = 100;        // Blue Nova pack 2
    uint32_t agg_cap_b = 100;           // KNY
    // Inverter transmit on port INV. Off by default. NOT part of the settings sections or
    // exports/imports: it is set only through POST /api/agg/transmit with the typed
    // confirmation, and cleared whenever port INV is disabled or its bitrate changes.
    bool agg_tx_enabled = false;

    // MQTT / Home Assistant. The password is a secret (NVS only, never exported).
    bool mqtt_enabled = false;
    std::string mqtt_host;              // broker host name or IP
    uint32_t mqtt_port = 1883;
    std::string mqtt_user;
    std::string mqtt_base = "bms-agg";  // base topic
    bool mqtt_discovery = true;         // Home Assistant discovery
    uint32_t mqtt_interval_s = 10;      // publish interval (2-3600 s)
    bool mqtt_retain = false;           // retain state topics

    // Verbose Diagnostics (SPEC.md): upload CAN traffic to a server you run.
    bool diag_enabled = false;
    std::string diag_url;            // e.g. http://192.168.0.7:8080/api/v1/batches
    uint32_t diag_batch = 500;       // frames per batch (50..500)
    uint32_t diag_flush_s = 30;      // upload at least this often (5..600 s)
    bool diag_change_only = false;   // only changed frames + 1-minute snapshot
    uint32_t diag_pretrig_s = 60;    // pre-trigger window uploaded with an event (0..120 s)
    // Event trigger: a frame with this ID that differs from the baseline.
    bool trig_enabled = false;
    uint32_t trig_id = 0x35A;
    std::string trig_baseline = "A8AA8202A8AA0202";  // hex, Blue Nova 0x35A
};

// Opens NVS (recovering it if its layout is unusable), loads settings and
// mounts LittleFS (formatting it if it cannot be mounted).
esp_err_t init();

bool loaded_ok();     // init() succeeded and settings were read
bool fs_mounted();
bool is_provisioned();  // setup completed (WiFi + admin password stored)

Settings get();
// Validates and stores every field (all-or-nothing validation). On failure
// `err` says why and nothing is written.
esp_err_t save(const Settings& s, std::string& err);
bool validate(const Settings& s, std::string& err);
// Upload token for Verbose Diagnostics: a secret, never exported.
std::string diag_token();
esp_err_t set_diag_token(const std::string& t);
std::string mqtt_password();
esp_err_t set_mqtt_password(const std::string& p);
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
bool valid_can_bitrate(uint32_t v);  // 125k/250k/500k: an 8 MHz MCP2515 cannot do 1 Mbit/s
bool valid_mqtt_host(const std::string& v);
bool valid_mqtt_base(const std::string& v);
// Parses up to 8 bytes of hex ("A8AA..."); returns byte count or -1.
int parse_hex_bytes(const std::string& hex, uint8_t out[8]);

}  // namespace cfg
