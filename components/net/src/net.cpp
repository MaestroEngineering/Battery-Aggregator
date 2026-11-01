#include "net/net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mutex>

#include "captive_dns.h"
#include "config/config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"

namespace net {
namespace {

const char* TAG = "net";

// Fallback AP trigger: this many consecutive failures, or this long without an
// IP, whichever comes first. Kept well below the 60 s OTA self-test window so a
// fresh image on a site with WiFi trouble still passes via the AP.
constexpr int kFailuresForAp = 5;
constexpr int64_t kNoIpForApUs = 30LL * 1000 * 1000;
constexpr int64_t kFallbackRetryIdleUs = 60LL * 1000 * 1000;      // no AP clients
constexpr int64_t kFallbackRetryBusyUs = 5 * 60LL * 1000 * 1000;  // clients on AP
constexpr int64_t kSetupTimeoutUs = 25LL * 1000 * 1000;
constexpr int64_t kSetupApGraceUs = 20LL * 1000 * 1000;  // AP stays up after success

enum MsgType : uint8_t { M_STA_START, M_STA_DISCONNECTED, M_GOT_IP, M_LOST_IP, M_AP_JOIN,
                         M_AP_LEAVE, M_RECONNECT, M_PROVISION };
struct Msg {
    MsgType type;
    int arg;
};

QueueHandle_t s_q;
std::recursive_mutex s_mtx;  // guards strings and s_prov; held by the net task while it works

volatile Mode s_mode = Mode::Setup;
volatile bool s_has_ip = false;
volatile bool s_ap_on = false;
volatile bool s_connecting = false;
volatile bool s_time_synced = false;
int s_ap_clients = 0;
int s_failures = 0;
int64_t s_no_ip_since = 0;
int64_t s_next_retry = 0;
int64_t s_ap_off_at = 0;
std::string s_ip, s_ap_ssid, s_sta_ssid;

esp_netif_t* s_sta_netif = nullptr;
esp_netif_t* s_ap_netif = nullptr;

struct Provisioning {
    bool active = false;
    std::string ssid, pass, ap_pass, admin;
    int64_t started = 0;
    const char* state = "idle";
    std::string detail, ip;
    int last_reason = 0;
} s_prov;

int64_t now_us() { return esp_timer_get_time(); }

void post(MsgType t, int arg = 0) {
    Msg m{t, arg};
    xQueueSend(s_q, &m, 0);
}

const char* reason_text(int r) {
    switch (r) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "wrong password";
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found";
        case WIFI_REASON_ASSOC_FAIL:
            return "association failed";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "signal lost";
        default:
            return "connection failed";
    }
}

// ---- WiFi configuration helpers -------------------------------------------------

void configure_ap(bool open) {
    wifi_config_t ap = {};
    std::string ssid;
    {
        std::lock_guard<std::recursive_mutex> l(s_mtx);
        ssid = s_ap_ssid;
    }
    memcpy(ap.ap.ssid, ssid.data(), ssid.size());
    ap.ap.ssid_len = ssid.size();
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    std::string pw = open ? "" : cfg::ap_password();
    if (pw.size() >= cfg::kMinApPassword) {
        memcpy(ap.ap.password, pw.data(), pw.size());
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (!open) ESP_LOGW(TAG, "no recovery AP password stored, fallback AP is open");
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ap.ap.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

void configure_sta(const std::string& ssid, const std::string& pass) {
    wifi_config_t sta = {};
    memcpy(sta.sta.ssid, ssid.data(), std::min(ssid.size(), sizeof(sta.sta.ssid)));
    memcpy(sta.sta.password, pass.data(), std::min(pass.size(), sizeof(sta.sta.password)));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    s_sta_ssid = ssid;
}

void sta_connect() {
    if (s_connecting || s_has_ip) return;
    if (esp_wifi_connect() == ESP_OK) s_connecting = true;
}

void ap_enable(bool open) {
    if (s_ap_on) return;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    configure_ap(open);
    s_ap_on = true;
    s_ap_clients = 0;
    ESP_LOGI(TAG, "AP '%s' up (%s)", s_ap_ssid.c_str(), open ? "open, setup" : "WPA2, recovery");
}

void ap_disable() {
    if (!s_ap_on) return;
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
    s_ap_clients = 0;
    s_ap_off_at = 0;
    ESP_LOGI(TAG, "AP off");
}

void start_station_mode() {
    std::string ssid, pass;
    if (!cfg::wifi_credentials(ssid, pass)) {
        ESP_LOGE(TAG, "provisioned but no WiFi credentials");
        return;
    }
    configure_sta(ssid, pass);
    s_failures = 0;
    s_no_ip_since = now_us();
    s_next_retry = 0;
    ESP_LOGI(TAG, "joining WiFi '%s'", ssid.c_str());
}

// ---- Time --------------------------------------------------------------------------

void time_sync_cb(struct timeval*) {
    if (!s_time_synced) ESP_LOGI(TAG, "time synced by NTP");
    s_time_synced = true;
}

void start_sntp() {
    cfg::Settings s = cfg::get();
    setenv("TZ", s.timezone.c_str(), 1);
    tzset();
    static std::string server;  // must outlive the SNTP config
    server = s.ntp_server;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(server.c_str());
    c.sync_cb = time_sync_cb;
    esp_netif_sntp_init(&c);
}

// ---- State machine (runs only in the net task) ------------------------------------

void provisioning_fail(const char* why) {
    ESP_LOGW(TAG, "setup WiFi test failed: %s", why);
    s_prov.state = "failed";
    s_prov.detail = why;
    s_prov.active = false;
    s_prov.pass.clear();
    s_prov.admin.clear();
    s_prov.ap_pass.clear();
    esp_wifi_disconnect();
    s_connecting = false;
}

void handle(const Msg& m) {
    switch (m.type) {
        case M_STA_START:
            break;
        case M_STA_DISCONNECTED:
            s_connecting = false;
            if (s_has_ip) s_no_ip_since = now_us();
            s_has_ip = false;
            if (s_mode == Mode::Setup) {
                s_prov.last_reason = m.arg;
                if (s_prov.active) {
                    if (m.arg == WIFI_REASON_AUTH_FAIL || m.arg == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                        m.arg == WIFI_REASON_HANDSHAKE_TIMEOUT)
                        provisioning_fail("wrong password");
                    else
                        s_next_retry = now_us() + 2000000;
                }
                break;
            }
            s_failures++;
            ESP_LOGW(TAG, "WiFi disconnected (%s, reason %d), failure %d", reason_text(m.arg), m.arg,
                     s_failures);
            {
                int backoff_s = s_failures < 5 ? (1 << s_failures) : 30;
                s_next_retry = now_us() + backoff_s * 1000000LL;
            }
            break;
        case M_GOT_IP:
            s_connecting = false;
            s_has_ip = true;
            s_failures = 0;
            if (s_mode == Mode::Setup && s_prov.active) {
                esp_err_t err = cfg::provision(s_prov.ssid, s_prov.pass, s_prov.ap_pass, s_prov.admin);
                s_prov.pass.clear();
                s_prov.admin.clear();
                s_prov.ap_pass.clear();
                s_prov.active = false;
                if (err != ESP_OK) {
                    s_prov.state = "failed";
                    s_prov.detail = std::string("could not save settings: ") + esp_err_to_name(err);
                    esp_wifi_disconnect();
                    break;
                }
                {
                    std::lock_guard<std::recursive_mutex> l(s_mtx);
                    s_prov.ip = s_ip;
                }
                s_prov.state = "connected";
                s_prov.detail = "Setup complete. The setup network turns off in 20 s.";
                ESP_LOGI(TAG, "provisioning complete");
                s_mode = Mode::Station;
                s_ap_off_at = now_us() + kSetupApGraceUs;
            } else if (s_mode == Mode::FallbackAP) {
                ESP_LOGI(TAG, "WiFi back, leaving fallback AP mode");
                s_mode = Mode::Station;
                s_ap_off_at = now_us() + (s_ap_clients ? 30000000LL : 2000000LL);
            }
            break;
        case M_LOST_IP:
            s_has_ip = false;
            s_no_ip_since = now_us();
            break;
        case M_AP_JOIN:
            s_ap_clients++;
            break;
        case M_AP_LEAVE:
            if (s_ap_clients > 0) s_ap_clients--;
            break;
        case M_RECONNECT:
            if (s_mode == Mode::Setup) break;
            esp_wifi_disconnect();
            s_has_ip = false;
            s_connecting = false;
            start_station_mode();
            break;
        case M_PROVISION:
            if (s_mode != Mode::Setup) break;
            esp_wifi_disconnect();
            s_connecting = false;
            s_has_ip = false;
            configure_sta(s_prov.ssid, s_prov.pass);
            s_prov.started = now_us();
            s_next_retry = 0;
            ESP_LOGI(TAG, "setup: trying WiFi '%s'", s_prov.ssid.c_str());
            break;
    }
}

void tick() {
    int64_t t = now_us();
    if (s_ap_off_at && t >= s_ap_off_at && s_mode == Mode::Station) ap_disable();

    switch (s_mode) {
        case Mode::Setup:
            if (s_prov.active) {
                if (t - s_prov.started > kSetupTimeoutUs) {
                    provisioning_fail(s_prov.last_reason ? reason_text(s_prov.last_reason)
                                                         : "timed out");
                } else if (!s_connecting && !s_has_ip && t >= s_next_retry) {
                    sta_connect();
                }
            }
            break;
        case Mode::Station:
            if (!s_has_ip && !s_connecting && t >= s_next_retry) sta_connect();
            if (!s_has_ip && !s_ap_on &&
                (s_failures >= kFailuresForAp || t - s_no_ip_since >= kNoIpForApUs)) {
                ESP_LOGW(TAG, "WiFi failing (%d failures), starting recovery AP", s_failures);
                s_mode = Mode::FallbackAP;
                ap_enable(false);
                s_next_retry = t + kFallbackRetryIdleUs;
            }
            break;
        case Mode::FallbackAP:
            // Retrying makes the AP hop channels, so retry rarely while someone
            // is connected to the AP (probably fixing the WiFi settings).
            if (!s_has_ip && !s_connecting && t >= s_next_retry) {
                sta_connect();
                s_next_retry = t + (s_ap_clients ? kFallbackRetryBusyUs : kFallbackRetryIdleUs);
            }
            break;
    }
}

void net_task(void*) {
    for (;;) {
        Msg m;
        bool got = xQueueReceive(s_q, &m, pdMS_TO_TICKS(250)) == pdTRUE;
        std::lock_guard<std::recursive_mutex> l(s_mtx);
        if (got) handle(m);
        tick();
    }
}

void on_wifi(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                post(M_STA_START);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                post(M_STA_DISCONNECTED, ((wifi_event_sta_disconnected_t*)data)->reason);
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                post(M_AP_JOIN);
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                post(M_AP_LEAVE);
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            auto* e = (ip_event_got_ip_t*)data;
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&e->ip_info.ip));
            {
                std::lock_guard<std::recursive_mutex> l(s_mtx);
                s_ip = buf;
            }
            ESP_LOGI(TAG, "got IP %s", buf);
            post(M_GOT_IP);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            post(M_LOST_IP);
        }
    }
}

void start_mdns() {
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed");
        return;
    }
    apply_hostname();
    mdns_instance_name_set("BMS Aggregator");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
}

}  // namespace

// ---- Public API ---------------------------------------------------------------------

esp_err_t start() {
    s_q = xQueueCreate(16, sizeof(Msg));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // our own NVS keys are the only copy
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_wifi, nullptr);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[20];
    snprintf(ssid, sizeof(ssid), "BMS-Agg-%02X%02X", mac[4], mac[5]);
    s_ap_ssid = ssid;

    esp_netif_set_hostname(s_sta_netif, cfg::get().hostname.c_str());

    if (cfg::is_provisioned()) {
        s_mode = Mode::Station;
        esp_wifi_set_mode(WIFI_MODE_STA);
        start_station_mode();
    } else {
        s_mode = Mode::Setup;
        ESP_LOGW(TAG, "not provisioned: starting setup AP '%s' (open) - browse to http://192.168.4.1",
                 ssid);
        ap_enable(true);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (s_mode == Mode::Setup) captive_dns_start();

    start_mdns();
    start_sntp();
    xTaskCreate(net_task, "net", 4096, nullptr, 5, nullptr);
    return ESP_OK;
}

Mode mode() { return s_mode; }

const char* mode_name() {
    switch (s_mode) {
        case Mode::Setup:
            return "setup";
        case Mode::Station:
            return "station";
        case Mode::FallbackAP:
            return "fallback-ap";
    }
    return "?";
}

bool sta_connected() { return s_has_ip; }
bool ap_up() { return s_ap_on; }
bool time_synced() { return s_time_synced; }

int rssi() {
    if (!s_has_ip) return 0;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

std::string sta_ip() {
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    return s_has_ip ? s_ip : std::string();
}

std::string ap_ssid() {
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    return s_ap_ssid;
}

std::string connected_ssid() {
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    return s_has_ip ? s_sta_ssid : std::string();
}

esp_err_t begin_provisioning(const std::string& ssid, const std::string& wifi_pass,
                             const std::string& ap_pass, const std::string& admin_pass) {
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    if (s_mode != Mode::Setup) return ESP_ERR_INVALID_STATE;
    if (s_prov.active) return ESP_ERR_INVALID_STATE;
    s_prov.ssid = ssid;
    s_prov.pass = wifi_pass;
    s_prov.ap_pass = ap_pass;
    s_prov.admin = admin_pass;
    s_prov.state = "connecting";
    s_prov.detail = "Connecting to " + ssid + " ...";
    s_prov.ip.clear();
    s_prov.last_reason = 0;
    s_prov.active = true;
    post(M_PROVISION);
    return ESP_OK;
}

SetupStatus setup_status() {
    std::lock_guard<std::recursive_mutex> l(s_mtx);
    SetupStatus st;
    st.state = s_prov.state;
    st.detail = s_prov.detail;
    st.ip = s_prov.ip;
    return st;
}

void reconnect() { post(M_RECONNECT); }

void apply_hostname() {
    std::string h = cfg::get().hostname;
    mdns_hostname_set(h.c_str());
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, h.c_str());
    ESP_LOGI(TAG, "hostname %s.local", h.c_str());
}

void apply_time_config() {
    esp_netif_sntp_deinit();
    s_time_synced = false;
    start_sntp();
    ESP_LOGI(TAG, "time config applied: server=%s tz=%s", cfg::get().ntp_server.c_str(),
             cfg::get().timezone.c_str());
}

std::string scan_json() {
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return "[]";
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return "[]";
    std::string out = "[";
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid((const char*)recs[i].ssid);
        if (ssid.empty()) continue;
        // Escape for JSON.
        std::string esc;
        for (char c : ssid) {
            if (c == '"' || c == '\\') esc += '\\';
            if ((unsigned char)c < 0x20) continue;
            esc += c;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                 out.size() > 1 ? "," : "", esc.c_str(), recs[i].rssi,
                 recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        out += buf;
    }
    out += "]";
    return out;
}

void stop() {
    esp_wifi_disconnect();
    esp_wifi_stop();
}

}  // namespace net
