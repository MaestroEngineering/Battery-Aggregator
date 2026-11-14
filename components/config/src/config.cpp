#include "config/config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "password.h"

namespace cfg {
namespace {

const char* TAG = "config";
constexpr const char* kNs = "bms";
constexpr uint32_t kPbkdf2Iterations = 20000;  // ~0.3-0.6 s on an ESP32

// NVS keys (max 15 chars).
constexpr const char* K_HOST = "host";
constexpr const char* K_NTP = "ntp";
constexpr const char* K_TZ = "tz";
constexpr const char* K_SIGREQ = "sigreq";
constexpr const char* K_SSID = "wssid";
constexpr const char* K_WPASS = "wpass";
constexpr const char* K_APPASS = "appass";
constexpr const char* K_SALT = "adm_salt";
constexpr const char* K_HASH = "adm_hash";
constexpr const char* K_ITER = "adm_iter";
constexpr const char* K_PROV = "prov";
constexpr const char* K_CAN_EN = "can_en";
constexpr const char* K_CAN_BR = "can_br";
constexpr const char* K_DG_EN = "dg_en";
constexpr const char* K_DG_URL = "dg_url";
constexpr const char* K_DG_TOK = "dg_tok";
constexpr const char* K_DG_BATCH = "dg_batch";
constexpr const char* K_DG_FLUSH = "dg_flush";
constexpr const char* K_DG_CHG = "dg_chg";
constexpr const char* K_DG_PRE = "dg_pre";
constexpr const char* K_TR_EN = "tr_en";
constexpr const char* K_TR_ID = "tr_id";
constexpr const char* K_TR_BASE = "tr_base";
constexpr const char* K_INV_EN = "inv_en";
constexpr const char* K_INV_BR = "inv_br";
constexpr const char* K_B_EN = "b_en";
constexpr const char* K_B_BR = "b_br";
constexpr const char* K_B_ACK = "b_ack";
constexpr const char* K_AG_TX = "ag_tx";
constexpr const char* K_AG_A = "ag_a";
constexpr const char* K_AG_B = "ag_b";
constexpr const char* K_AG_KCAP = "ag_kcap";
constexpr const char* K_AG_FCVL = "ag_fcvl";
constexpr const char* K_AG_FDVL = "ag_fdvl";
constexpr const char* K_AG_CCLMX = "ag_cclmx";
constexpr const char* K_AG_DCLMX = "ag_dclmx";
constexpr const char* K_AG_SIL = "ag_sil";
constexpr const char* K_AG_HOLD = "ag_hold";
constexpr const char* K_AG_CVLH = "ag_cvlh";
constexpr const char* K_AG_CAPA = "ag_capa";
constexpr const char* K_AG_CAPP2 = "ag_capp2";
constexpr const char* K_AG_CAPB = "ag_capb";
constexpr const char* K_MQ_EN = "mq_en";
constexpr const char* K_MQ_HOST = "mq_host";
constexpr const char* K_MQ_PORT = "mq_port";
constexpr const char* K_MQ_USER = "mq_user";
constexpr const char* K_MQ_PASS = "mq_pass";
constexpr const char* K_MQ_BASE = "mq_base";
constexpr const char* K_MQ_DISC = "mq_disc";
constexpr const char* K_MQ_INT = "mq_int";
constexpr const char* K_MQ_RET = "mq_ret";

SemaphoreHandle_t s_lock;
Settings s_settings;
bool s_loaded = false;
bool s_fs = false;
bool s_provisioned = false;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

bool read_str(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 || len > 256) return false;
    std::string v(len, '\0');
    if (nvs_get_str(h, key, v.data(), &len) != ESP_OK) return false;
    v.resize(len - 1);
    out = v;
    return true;
}

esp_err_t write_strs(std::initializer_list<std::pair<const char*, const std::string*>> kv) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (auto& p : kv) {
        err = nvs_set_str(h, p.first, p.second->c_str());
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t store_admin(nvs_handle_t h, const std::string& pw) {
    uint8_t salt[16], hash[32];
    esp_fill_random(salt, sizeof(salt));
    if (!pbkdf2_sha256((const uint8_t*)pw.data(), pw.size(), salt, sizeof(salt), kPbkdf2Iterations,
                       hash))
        return ESP_FAIL;
    esp_err_t err = nvs_set_blob(h, K_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, K_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_set_u32(h, K_ITER, kPbkdf2Iterations);
    memset(hash, 0, sizeof(hash));
    return err;
}

esp_err_t load() {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);  // RW creates the namespace
    if (err != ESP_OK) return err;
    Settings s;
    read_str(h, K_HOST, s.hostname);
    read_str(h, K_NTP, s.ntp_server);
    read_str(h, K_TZ, s.timezone);
    uint8_t u8 = 0;
    if (nvs_get_u8(h, K_SIGREQ, &u8) == ESP_OK) s.require_signed_images = u8 != 0;
    auto get_bool = [&](const char* k, bool& v) {
        uint8_t b;
        if (nvs_get_u8(h, k, &b) == ESP_OK) v = b != 0;
    };
    auto get_u32 = [&](const char* k, uint32_t& v) {
        uint32_t x;
        if (nvs_get_u32(h, k, &x) == ESP_OK) v = x;
    };
    get_bool(K_CAN_EN, s.can_enabled);
    get_u32(K_CAN_BR, s.can_bitrate);
    get_bool(K_DG_EN, s.diag_enabled);
    read_str(h, K_DG_URL, s.diag_url);
    get_u32(K_DG_BATCH, s.diag_batch);
    get_u32(K_DG_FLUSH, s.diag_flush_s);
    get_bool(K_DG_CHG, s.diag_change_only);
    get_u32(K_DG_PRE, s.diag_pretrig_s);
    get_bool(K_TR_EN, s.trig_enabled);
    get_u32(K_TR_ID, s.trig_id);
    read_str(h, K_TR_BASE, s.trig_baseline);
    get_bool(K_INV_EN, s.can_inv_enabled);
    get_u32(K_INV_BR, s.can_inv_bitrate);
    get_bool(K_B_EN, s.can_b_enabled);
    get_u32(K_B_BR, s.can_b_bitrate);
    get_bool(K_B_ACK, s.can_b_ack);
    get_bool(K_AG_TX, s.agg_tx_enabled);
    get_bool(K_AG_A, s.agg_src_a);
    get_bool(K_AG_B, s.agg_src_b);
    get_u32(K_AG_KCAP, s.agg_kny_cap_ah);
    get_u32(K_AG_FCVL, s.agg_fs_cvl_dv);
    get_u32(K_AG_FDVL, s.agg_fs_dvl_dv);
    get_u32(K_AG_CCLMX, s.agg_ccl_max);
    get_u32(K_AG_DCLMX, s.agg_dcl_max);
    get_bool(K_AG_SIL, s.agg_silent);
    get_u32(K_AG_HOLD, s.agg_hold_s);
    get_u32(K_AG_CVLH, s.agg_cvl_hold_s);
    get_u32(K_AG_CAPA, s.agg_cap_a);
    get_u32(K_AG_CAPP2, s.agg_cap_a_p2);
    get_u32(K_AG_CAPB, s.agg_cap_b);
    get_bool(K_MQ_EN, s.mqtt_enabled);
    read_str(h, K_MQ_HOST, s.mqtt_host);
    get_u32(K_MQ_PORT, s.mqtt_port);
    read_str(h, K_MQ_USER, s.mqtt_user);
    read_str(h, K_MQ_BASE, s.mqtt_base);
    get_bool(K_MQ_DISC, s.mqtt_discovery);
    get_u32(K_MQ_INT, s.mqtt_interval_s);
    get_bool(K_MQ_RET, s.mqtt_retain);
    // Fall back to defaults for anything that no longer validates.
    Settings d;
    if (!valid_hostname(s.hostname)) s.hostname = d.hostname;
    if (!valid_ntp_server(s.ntp_server)) s.ntp_server = d.ntp_server;
    if (!valid_timezone(s.timezone)) s.timezone = d.timezone;
    std::string why;
    if (!validate(s, why)) {
        // Everything except the system settings goes back to defaults: CAN ports, aggregator
        // and MQTT off. That is the safe state (nothing enabled, output refused).
        ESP_LOGW(TAG, "stored CAN/aggregator/MQTT/diagnostics settings invalid (%s), using defaults", why.c_str());
        Settings fixed = d;
        fixed.hostname = s.hostname;
        fixed.ntp_server = s.ntp_server;
        fixed.timezone = s.timezone;
        fixed.require_signed_images = s.require_signed_images;
        s = fixed;
    }
    s_settings = s;

    uint8_t prov = 0;
    size_t hl = 0;
    std::string ssid;
    s_provisioned = nvs_get_u8(h, K_PROV, &prov) == ESP_OK && prov == 1 &&
                    nvs_get_blob(h, K_HASH, nullptr, &hl) == ESP_OK && hl == 32 &&
                    read_str(h, K_SSID, ssid) && !ssid.empty();
    nvs_close(h);
    return ESP_OK;
}

esp_err_t mount_fs() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kFsMount;
    conf.partition_label = kFsLabel;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(kFsLabel, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used", kFsMount, (unsigned)used,
             (unsigned)total);
    return ESP_OK;
}

}  // namespace

esp_err_t init() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) err = load();
    s_loaded = err == ESP_OK;
    // Apply the timezone straight away so every log timestamp after this point
    // (and after a soft reboot, where the clock keeps running) is local time.
    setenv("TZ", s_settings.timezone.c_str(), 1);
    tzset();
    if (!s_loaded) ESP_LOGE(TAG, "config load failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "config loaded: host=%s ntp=%s tz=%s provisioned=%d",
                 s_settings.hostname.c_str(), s_settings.ntp_server.c_str(),
                 s_settings.timezone.c_str(), s_provisioned);
    s_fs = mount_fs() == ESP_OK;
    return err;
}

bool loaded_ok() { return s_loaded; }
bool fs_mounted() { return s_fs; }
bool is_provisioned() { return s_provisioned; }

Settings get() {
    Lock l;
    return s_settings;
}

bool validate(const Settings& s, std::string& err) {
    uint8_t tmp[8];
    if (!valid_hostname(s.hostname)) err = "hostname: 1-32 letters, digits or '-'";
    else if (!valid_ntp_server(s.ntp_server)) err = "invalid NTP server";
    else if (!valid_timezone(s.timezone)) err = "invalid timezone string";
    else if (!valid_can_bitrate(s.can_bitrate))
        err = "port A bitrate must be 125000, 250000 or 500000 (8 MHz crystal: 1 Mbit/s is impossible)";
    else if (!valid_can_bitrate(s.can_inv_bitrate))
        err = "port INV bitrate must be 125000, 250000 or 500000 (8 MHz crystal: 1 Mbit/s is impossible)";
    else if (!valid_can_bitrate(s.can_b_bitrate))
        err = "port B bitrate must be 125000, 250000 or 500000 (8 MHz crystal: 1 Mbit/s is impossible)";
    else if (s.agg_src_a && !s.can_enabled) err = "aggregator source A needs port A enabled";
    else if (s.agg_src_b && !s.can_b_enabled) err = "aggregator source B needs port B enabled";
    else if (s.agg_tx_enabled && !s.can_inv_enabled) err = "inverter transmit needs port INV enabled";
    else if (s.agg_kny_cap_ah < 1 || s.agg_kny_cap_ah > 2000) err = "KNY capacity must be 1-2000 Ah";
    else if (s.agg_fs_cvl_dv != 0 && (s.agg_fs_cvl_dv < 400 || s.agg_fs_cvl_dv > 600))
        err = "fail-safe CVL must be 40.0-60.0 V";
    else if (s.agg_fs_dvl_dv != 0 && (s.agg_fs_dvl_dv < 360 || s.agg_fs_dvl_dv > 600))
        err = "fail-safe DVL must be 36.0-60.0 V";
    else if (s.agg_fs_cvl_dv && s.agg_fs_dvl_dv && s.agg_fs_dvl_dv >= s.agg_fs_cvl_dv)
        err = "fail-safe DVL must be below fail-safe CVL";
    else if (s.agg_ccl_max > 1000 || s.agg_dcl_max > 1000) err = "CCL/DCL maximum must be 0-1000 A";
    else if (s.agg_hold_s < 5 || s.agg_hold_s > 300) err = "start-up hold must be 5-300 s";
    else if (s.agg_cvl_hold_s > 600) err = "CVL rise hold must be 0-600 s";
    else if (s.agg_cap_a < 1 || s.agg_cap_a > 1000 || s.agg_cap_a_p2 < 1 || s.agg_cap_a_p2 > 1000 || s.agg_cap_b < 1 ||
             s.agg_cap_b > 1000)
        err = "per-source caps must be 1-1000 A";
    else if (!valid_mqtt_host(s.mqtt_host)) err = "MQTT host: up to 64 letters, digits, '.', '-' or ':'";
    else if (s.mqtt_enabled && s.mqtt_host.empty()) err = "set an MQTT broker host before enabling MQTT";
    else if (s.mqtt_port < 1 || s.mqtt_port > 65535) err = "MQTT port must be 1-65535";
    else if (s.mqtt_user.size() > 64) err = "MQTT user: max 64 characters";
    else if (!valid_mqtt_base(s.mqtt_base)) err = "MQTT base topic: 1-48 of A-Z a-z 0-9 _ - /, no wildcards";
    else if (s.mqtt_interval_s < 2 || s.mqtt_interval_s > 3600) err = "MQTT interval must be 2-3600 s";
    else if (s.diag_url.size() > 200 ||
             (!s.diag_url.empty() && s.diag_url.rfind("http://", 0) != 0 &&
              s.diag_url.rfind("https://", 0) != 0))
        err = "diagnostics URL must start with http:// or https:// (max 200 characters)";
    else if (s.diag_enabled && s.diag_url.empty()) err = "set a server URL before enabling Verbose Diagnostics";
    else if (s.diag_batch < 50 || s.diag_batch > 500) err = "batch size must be 50-500 frames";
    else if (s.diag_flush_s < 5 || s.diag_flush_s > 600) err = "flush interval must be 5-600 s";
    else if (s.diag_pretrig_s > 120) err = "pre-trigger must be 0-120 s";
    else if (s.trig_id > 0x1FFFFFFF) err = "trigger ID must be at most 0x1FFFFFFF";
    else if (parse_hex_bytes(s.trig_baseline, tmp) < 0)
        err = "trigger baseline must be 0-8 bytes of hex, e.g. A8AA8202A8AA0202";
    else return true;
    return false;
}

esp_err_t save(const Settings& s, std::string& err) {
    if (!validate(s, err)) return ESP_ERR_INVALID_ARG;
    Lock l;
    nvs_handle_t h;
    esp_err_t e = nvs_open(kNs, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        err = esp_err_to_name(e);
        return e;
    }
    auto ok = [&](esp_err_t r) { if (e == ESP_OK) e = r; };
    ok(nvs_set_str(h, K_HOST, s.hostname.c_str()));
    ok(nvs_set_str(h, K_NTP, s.ntp_server.c_str()));
    ok(nvs_set_str(h, K_TZ, s.timezone.c_str()));
    ok(nvs_set_u8(h, K_SIGREQ, s.require_signed_images));
    ok(nvs_set_u8(h, K_CAN_EN, s.can_enabled));
    ok(nvs_set_u32(h, K_CAN_BR, s.can_bitrate));
    ok(nvs_set_u8(h, K_DG_EN, s.diag_enabled));
    ok(nvs_set_str(h, K_DG_URL, s.diag_url.c_str()));
    ok(nvs_set_u32(h, K_DG_BATCH, s.diag_batch));
    ok(nvs_set_u32(h, K_DG_FLUSH, s.diag_flush_s));
    ok(nvs_set_u8(h, K_DG_CHG, s.diag_change_only));
    ok(nvs_set_u32(h, K_DG_PRE, s.diag_pretrig_s));
    ok(nvs_set_u8(h, K_TR_EN, s.trig_enabled));
    ok(nvs_set_u32(h, K_TR_ID, s.trig_id));
    ok(nvs_set_str(h, K_TR_BASE, s.trig_baseline.c_str()));
    ok(nvs_set_u8(h, K_INV_EN, s.can_inv_enabled));
    ok(nvs_set_u32(h, K_INV_BR, s.can_inv_bitrate));
    ok(nvs_set_u8(h, K_B_EN, s.can_b_enabled));
    ok(nvs_set_u32(h, K_B_BR, s.can_b_bitrate));
    ok(nvs_set_u8(h, K_B_ACK, s.can_b_ack));
    ok(nvs_set_u8(h, K_AG_TX, s.agg_tx_enabled));
    ok(nvs_set_u8(h, K_AG_A, s.agg_src_a));
    ok(nvs_set_u8(h, K_AG_B, s.agg_src_b));
    ok(nvs_set_u32(h, K_AG_KCAP, s.agg_kny_cap_ah));
    ok(nvs_set_u32(h, K_AG_FCVL, s.agg_fs_cvl_dv));
    ok(nvs_set_u32(h, K_AG_FDVL, s.agg_fs_dvl_dv));
    ok(nvs_set_u32(h, K_AG_CCLMX, s.agg_ccl_max));
    ok(nvs_set_u32(h, K_AG_DCLMX, s.agg_dcl_max));
    ok(nvs_set_u8(h, K_AG_SIL, s.agg_silent));
    ok(nvs_set_u32(h, K_AG_HOLD, s.agg_hold_s));
    ok(nvs_set_u32(h, K_AG_CVLH, s.agg_cvl_hold_s));
    ok(nvs_set_u32(h, K_AG_CAPA, s.agg_cap_a));
    ok(nvs_set_u32(h, K_AG_CAPP2, s.agg_cap_a_p2));
    ok(nvs_set_u32(h, K_AG_CAPB, s.agg_cap_b));
    ok(nvs_set_u8(h, K_MQ_EN, s.mqtt_enabled));
    ok(nvs_set_str(h, K_MQ_HOST, s.mqtt_host.c_str()));
    ok(nvs_set_u32(h, K_MQ_PORT, s.mqtt_port));
    ok(nvs_set_str(h, K_MQ_USER, s.mqtt_user.c_str()));
    ok(nvs_set_str(h, K_MQ_BASE, s.mqtt_base.c_str()));
    ok(nvs_set_u8(h, K_MQ_DISC, s.mqtt_discovery));
    ok(nvs_set_u32(h, K_MQ_INT, s.mqtt_interval_s));
    ok(nvs_set_u8(h, K_MQ_RET, s.mqtt_retain));
    ok(nvs_commit(h));
    nvs_close(h);
    if (e == ESP_OK) s_settings = s;
    else err = std::string("saving failed: ") + esp_err_to_name(e);
    return e;
}

std::string diag_token() {
    Lock l;
    nvs_handle_t h;
    std::string v;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        read_str(h, K_DG_TOK, v);
        nvs_close(h);
    }
    return v;
}

esp_err_t set_diag_token(const std::string& t) {
    if (t.size() > 128) return ESP_ERR_INVALID_ARG;
    for (char c : t)
        if (!isprint((unsigned char)c)) return ESP_ERR_INVALID_ARG;
    Lock l;
    return write_strs({{K_DG_TOK, &t}});
}

std::string mqtt_password() {
    Lock l;
    nvs_handle_t h;
    std::string v;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        read_str(h, K_MQ_PASS, v);
        nvs_close(h);
    }
    return v;
}

esp_err_t set_mqtt_password(const std::string& p) {
    if (p.size() > 128) return ESP_ERR_INVALID_ARG;
    for (char c : p)
        if (!isprint((unsigned char)c)) return ESP_ERR_INVALID_ARG;
    Lock l;
    return write_strs({{K_MQ_PASS, &p}});
}

esp_err_t set_hostname(const std::string& v) {
    if (!valid_hostname(v)) return ESP_ERR_INVALID_ARG;
    Lock l;
    esp_err_t err = write_strs({{K_HOST, &v}});
    if (err == ESP_OK) s_settings.hostname = v;
    return err;
}

esp_err_t set_ntp(const std::string& server, const std::string& tz) {
    if (!valid_ntp_server(server) || !valid_timezone(tz)) return ESP_ERR_INVALID_ARG;
    Lock l;
    esp_err_t err = write_strs({{K_NTP, &server}, {K_TZ, &tz}});
    if (err == ESP_OK) {
        s_settings.ntp_server = server;
        s_settings.timezone = tz;
    }
    return err;
}

esp_err_t set_require_signed_images(bool v) {
    Lock l;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_SIGREQ, v ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_settings.require_signed_images = v;
    return err;
}

std::string wifi_ssid() {
    std::string s, p;
    wifi_credentials(s, p);
    return s;
}

bool wifi_credentials(std::string& ssid, std::string& pass) {
    Lock l;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = read_str(h, K_SSID, ssid);
    if (!read_str(h, K_WPASS, pass)) pass.clear();
    nvs_close(h);
    return ok && !ssid.empty();
}

esp_err_t set_wifi(const std::string& ssid, const std::string& pass) {
    if (!valid_ssid(ssid) || !valid_wifi_password(pass)) return ESP_ERR_INVALID_ARG;
    Lock l;
    return write_strs({{K_SSID, &ssid}, {K_WPASS, &pass}});
}

std::string ap_password() {
    Lock l;
    nvs_handle_t h;
    std::string v;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        read_str(h, K_APPASS, v);
        nvs_close(h);
    }
    return v;
}

esp_err_t provision(const std::string& ssid, const std::string& wifi_pass,
                    const std::string& ap_pass, const std::string& admin_pass) {
    if (!valid_ssid(ssid) || !valid_wifi_password(wifi_pass) || ap_pass.size() < kMinApPassword ||
        ap_pass.size() > 63 || admin_pass.size() < kMinAdminPassword || admin_pass.size() > 64)
        return ESP_ERR_INVALID_ARG;
    Lock l;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_SSID, ssid.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, K_WPASS, wifi_pass.c_str());
    if (err == ESP_OK) err = nvs_set_str(h, K_APPASS, ap_pass.c_str());
    if (err == ESP_OK) err = store_admin(h, admin_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    // The flag goes last so a power cut mid-way leaves the device in setup mode.
    if (err == ESP_OK) err = nvs_set_u8(h, K_PROV, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_provisioned = true;
    return err;
}

bool check_admin_password(const std::string& pw) {
    uint8_t salt[16], hash[32], calc[32];
    uint32_t iter = 0;
    size_t sl = sizeof(salt), hl = sizeof(hash);
    {
        Lock l;
        nvs_handle_t h;
        if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        bool ok = nvs_get_blob(h, K_SALT, salt, &sl) == ESP_OK && sl == sizeof(salt) &&
                  nvs_get_blob(h, K_HASH, hash, &hl) == ESP_OK && hl == sizeof(hash) &&
                  nvs_get_u32(h, K_ITER, &iter) == ESP_OK && iter > 0 && iter <= 1000000;
        nvs_close(h);
        if (!ok) return false;
    }
    if (!pbkdf2_sha256((const uint8_t*)pw.data(), pw.size(), salt, sizeof(salt), iter, calc))
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(hash); i++) diff |= hash[i] ^ calc[i];
    memset(calc, 0, sizeof(calc));
    return diff == 0;
}

esp_err_t set_admin_password(const std::string& pw) {
    if (pw.size() < kMinAdminPassword || pw.size() > 64) return ESP_ERR_INVALID_ARG;
    Lock l;
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = store_admin(h, pw);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t factory_reset() {
    ESP_LOGW(TAG, "FACTORY RESET: erasing NVS and formatting LittleFS");
    Lock l;
    if (s_fs) {
        esp_vfs_littlefs_unregister(kFsLabel);
        s_fs = false;
    }
    esp_err_t e1 = esp_littlefs_format(kFsLabel);
    nvs_flash_deinit();
    esp_err_t e2 = nvs_flash_erase();
    ESP_LOGW(TAG, "factory reset done: littlefs=%s nvs=%s", esp_err_to_name(e1),
             esp_err_to_name(e2));
    s_provisioned = false;
    return e2 != ESP_OK ? e2 : e1;
}

// ---- validators -------------------------------------------------------------

bool valid_hostname(const std::string& v) {
    if (v.empty() || v.size() > 32 || v.front() == '-' || v.back() == '-') return false;
    for (char c : v)
        if (!(isalnum((unsigned char)c) || c == '-')) return false;
    return true;
}

bool valid_ntp_server(const std::string& v) {
    if (v.empty() || v.size() > 64) return false;
    for (char c : v)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == ':')) return false;
    return true;
}

bool valid_timezone(const std::string& v) {
    if (v.empty() || v.size() > 64) return false;
    for (char c : v)
        if (!isprint((unsigned char)c) || c == '"' || c == '\\') return false;
    return true;
}

bool valid_can_bitrate(uint32_t v) { return v == 125000 || v == 250000 || v == 500000; }

int parse_hex_bytes(const std::string& hex, uint8_t out[8]) {
    if (hex.size() % 2 || hex.size() > 16) return -1;
    for (size_t i = 0; i < hex.size(); i += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i + k];
            int d = isdigit((unsigned char)c) ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i / 2] = (uint8_t)v;
    }
    return (int)hex.size() / 2;
}

bool valid_mqtt_host(const std::string& v) {
    if (v.size() > 64) return false;
    for (char c : v)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == ':')) return false;
    return true;
}

bool valid_mqtt_base(const std::string& v) {
    if (v.empty() || v.size() > 48 || v.front() == '/' || v.back() == '/') return false;
    for (char c : v)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '/')) return false;
    return true;
}

bool valid_ssid(const std::string& v) { return !v.empty() && v.size() <= 32; }

bool valid_wifi_password(const std::string& v) {
    return v.empty() || (v.size() >= 8 && v.size() <= 63);
}

}  // namespace cfg
