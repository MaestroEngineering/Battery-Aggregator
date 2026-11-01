#include "config/config.h"

#include <ctype.h>
#include <string.h>

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
    // Fall back to defaults for anything that no longer validates.
    Settings d;
    if (!valid_hostname(s.hostname)) s.hostname = d.hostname;
    if (!valid_ntp_server(s.ntp_server)) s.ntp_server = d.ntp_server;
    if (!valid_timezone(s.timezone)) s.timezone = d.timezone;
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

bool valid_ssid(const std::string& v) { return !v.empty() && v.size() <= 32; }

bool valid_wifi_password(const std::string& v) {
    return v.empty() || (v.size() >= 8 && v.size() <= 63);
}

}  // namespace cfg
