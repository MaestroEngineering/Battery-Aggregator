// JSON API under /api/... used by the web UI.
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "applog/applog.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "internal.h"
#include "net/net.h"
#include "ota/ota.h"
#include "webserver/webserver.h"

// Fixed at compile time: the build never queries git.
#define BMS_GIT_HASH "unknown"

namespace webserver {
namespace {

const char* TAG = "api";
char s_boot_id[17];

const char* reset_reason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "panic / exception";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown";
    }
}

std::string now_str() {
    time_t t = time(nullptr);
    if (t < 1600000000) return "";
    tm lt;
    localtime_r(&t, &lt);
    char b[40];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %Z", &lt);
    return b;
}

void drain(httpd_req_t* req, size_t remaining) {
    char buf[512];
    int timeouts = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, std::min(remaining, sizeof(buf)));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) return;
        remaining -= n;
    }
}

cJSON* slot_json(const ota::SlotInfo& s) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", s.label.c_str());
    char addr[12];
    snprintf(addr, sizeof(addr), "0x%06lx", (unsigned long)s.address);
    cJSON_AddStringToObject(o, "address", addr);
    cJSON_AddNumberToObject(o, "size", s.size);
    cJSON_AddBoolToObject(o, "has_app", s.has_app);
    cJSON_AddStringToObject(o, "version", s.version.c_str());
    cJSON_AddStringToObject(o, "project", s.project.c_str());
    cJSON_AddStringToObject(o, "build", (s.date + " " + s.time).c_str());
    cJSON_AddStringToObject(o, "state", s.state.c_str());
    return o;
}

// ---- Public ---------------------------------------------------------------------

esp_err_t h_health(httpd_req_t* req) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "boot_id", s_boot_id);
    return send_json(req, o);
}

esp_err_t h_session(httpd_req_t* req) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "setup", !cfg::is_provisioned());
    cJSON_AddBoolToObject(o, "authenticated", cfg::is_provisioned() && session_valid(req));
    return send_json(req, o);
}

esp_err_t h_login(httpd_req_t* req) {
    if (!cfg::is_provisioned()) return send_error(req, "403 Forbidden", "device is not set up yet");
    uint32_t ip = client_ip(req);
    int wait = login_wait_s(ip);
    if (wait > 0) {
        char m[80];
        snprintf(m, sizeof(m), "too many failed attempts, try again in %d s", wait);
        char ra[12];
        snprintf(ra, sizeof(ra), "%d", wait);
        httpd_resp_set_hdr(req, "Retry-After", ra);
        return send_error(req, "429 Too Many Requests", m);
    }
    cJSON* j = read_json(req, 512);
    if (!j) return ESP_OK;
    std::string pw = json_str(j, "password");
    cJSON_Delete(j);
    bool ok = !pw.empty() && cfg::check_admin_password(pw);
    login_result(ip, ok);
    if (!ok) {
        ESP_LOGW(TAG, "failed login from %s", ip_to_str(ip).c_str());
        return send_error(req, "401 Unauthorized", "wrong password");
    }
    std::string tok = session_create(ip);
    std::string cookie = "bms_sid=" + tok + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    ESP_LOGI(TAG, "login from %s", ip_to_str(ip).c_str());
    return send_ok(req);
}

esp_err_t h_logout(httpd_req_t* req) {
    session_drop(req);
    httpd_resp_set_hdr(req, "Set-Cookie", "bms_sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_ok(req);
}

// ---- Setup (only while not provisioned) ------------------------------------------------

esp_err_t h_setup(httpd_req_t* req) {
    cJSON* j = read_json(req, 1024);
    if (!j) return ESP_OK;
    std::string ssid = json_str(j, "ssid"), pass = json_str(j, "password"),
                ap = json_str(j, "ap_password"), admin = json_str(j, "admin_password");
    cJSON_Delete(j);
    if (!cfg::valid_ssid(ssid)) return send_error(req, "400 Bad Request", "WiFi name is required (max 32)");
    if (!cfg::valid_wifi_password(pass))
        return send_error(req, "400 Bad Request", "WiFi password must be empty (open) or 8-63 characters");
    if (ap.size() < cfg::kMinApPassword || ap.size() > 63)
        return send_error(req, "400 Bad Request", "recovery AP password must be 8-63 characters");
    if (admin.size() < cfg::kMinAdminPassword || admin.size() > 64)
        return send_error(req, "400 Bad Request", "admin password must be 8-64 characters");
    esp_err_t e = net::begin_provisioning(ssid, pass, ap, admin);
    if (e != ESP_OK) return send_error(req, "409 Conflict", "setup already in progress");
    return send_ok(req);
}

esp_err_t h_setup_status(httpd_req_t* req) {
    net::SetupStatus st = net::setup_status();
    // Reachable while in setup and during the grace period right after it.
    if (cfg::is_provisioned() && strcmp(st.state, "connected") != 0)
        return send_error(req, "403 Forbidden", "device is already set up");
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", st.state);
    cJSON_AddStringToObject(o, "detail", st.detail.c_str());
    cJSON_AddStringToObject(o, "ip", st.ip.c_str());
    cJSON_AddStringToObject(o, "hostname", cfg::get().hostname.c_str());
    return send_json(req, o);
}

esp_err_t send_scan(httpd_req_t* req) {
    std::string s = net::scan_json();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, s.c_str());
}

// ---- Status / dashboard -----------------------------------------------------------------

esp_err_t h_status(httpd_req_t* req) {
    const esp_app_desc_t* d = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "project", d->project_name);
    cJSON_AddStringToObject(o, "version", d->version);
    cJSON_AddStringToObject(o, "git", BMS_GIT_HASH);
    cJSON_AddStringToObject(o, "build_date", d->date);
    cJSON_AddStringToObject(o, "build_time", d->time);
    cJSON_AddStringToObject(o, "idf", d->idf_ver);
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "chip_rev", chip.revision);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(o, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddStringToObject(o, "reset_reason", reset_reason());
    cJSON_AddStringToObject(o, "wifi_mode", net::mode_name());
    cJSON_AddStringToObject(o, "ssid", net::connected_ssid().c_str());
    cJSON_AddStringToObject(o, "ip", net::sta_ip().c_str());
    cJSON_AddNumberToObject(o, "rssi", net::rssi());
    cJSON_AddBoolToObject(o, "ap_up", net::ap_up());
    cJSON_AddStringToObject(o, "ap_ssid", net::ap_ssid().c_str());
    cJSON_AddStringToObject(o, "hostname", cfg::get().hostname.c_str());
    cJSON_AddStringToObject(o, "time", now_str().c_str());
    cJSON_AddBoolToObject(o, "time_synced", net::time_synced());
    cJSON_AddStringToObject(o, "selftest", ota::selftest_state());
    cJSON_AddStringToObject(o, "selftest_detail", ota::selftest_detail());
    return send_json(req, o);
}

esp_err_t h_log(httpd_req_t* req) {
    std::string s = applog::dump();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, s.data(), s.size());
}

// ---- System -----------------------------------------------------------------------------

cJSON* settings_json(bool for_export) {
    cfg::Settings s = cfg::get();
    cJSON* o = cJSON_CreateObject();
    if (for_export) {
        cJSON_AddStringToObject(o, "schema", "bms-agg-config");
        cJSON_AddNumberToObject(o, "schema_version", 1);
        cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
        cJSON_AddStringToObject(o, "exported_at", now_str().c_str());
    }
    cJSON* sys = cJSON_AddObjectToObject(o, "system");
    cJSON_AddStringToObject(sys, "hostname", s.hostname.c_str());
    cJSON_AddStringToObject(sys, "ntp_server", s.ntp_server.c_str());
    cJSON_AddStringToObject(sys, "timezone", s.timezone.c_str());
    cJSON* ota = cJSON_AddObjectToObject(o, "ota");
    cJSON_AddBoolToObject(ota, "require_signed_images", s.require_signed_images);
    if (!for_export) {
        cJSON* w = cJSON_AddObjectToObject(o, "wifi");
        cJSON_AddStringToObject(w, "ssid", cfg::wifi_ssid().c_str());
    }
    return o;
}

esp_err_t h_settings_get(httpd_req_t* req) { return send_json(req, settings_json(false)); }

// Validates and applies {system:{...}, ota:{...}}. All-or-nothing.
esp_err_t apply_settings(cJSON* j, std::string& err, bool strict_top) {
    static const char* kTop[] = {"schema", "schema_version", "firmware", "exported_at", "system", "ota"};
    if (strict_top) {
        cJSON* schema = cJSON_GetObjectItemCaseSensitive(j, "schema");
        if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "bms-agg-config") != 0) {
            err = "not a bms-agg config file (schema)";
            return ESP_ERR_INVALID_ARG;
        }
        cJSON* ver = cJSON_GetObjectItemCaseSensitive(j, "schema_version");
        if (!cJSON_IsNumber(ver) || ver->valueint != 1) {
            err = "unsupported schema_version";
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (cJSON* it = j->child; it; it = it->next) {
        bool known = false;
        for (const char* k : kTop) known |= strcmp(it->string, k) == 0;
        if (!known) {
            err = std::string("unknown key '") + it->string + "'";
            return ESP_ERR_INVALID_ARG;
        }
    }
    cfg::Settings cur = cfg::get(), next = cur;
    cJSON* sys = cJSON_GetObjectItemCaseSensitive(j, "system");
    if (sys) {
        if (!cJSON_IsObject(sys)) {
            err = "'system' must be an object";
            return ESP_ERR_INVALID_ARG;
        }
        for (cJSON* it = sys->child; it; it = it->next) {
            std::string k = it->string;
            if (!cJSON_IsString(it)) {
                err = "system." + k + " must be a string";
                return ESP_ERR_INVALID_ARG;
            }
            if (k == "hostname") next.hostname = it->valuestring;
            else if (k == "ntp_server") next.ntp_server = it->valuestring;
            else if (k == "timezone") next.timezone = it->valuestring;
            else {
                err = "unknown key 'system." + k + "'";
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    cJSON* ota = cJSON_GetObjectItemCaseSensitive(j, "ota");
    if (ota) {
        if (!cJSON_IsObject(ota)) {
            err = "'ota' must be an object";
            return ESP_ERR_INVALID_ARG;
        }
        for (cJSON* it = ota->child; it; it = it->next) {
            if (strcmp(it->string, "require_signed_images") == 0 && cJSON_IsBool(it))
                next.require_signed_images = cJSON_IsTrue(it);
            else {
                err = std::string("bad key 'ota.") + it->string + "'";
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    if (!cfg::valid_hostname(next.hostname)) {
        err = "hostname: 1-32 letters, digits or '-'";
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg::valid_ntp_server(next.ntp_server)) {
        err = "invalid NTP server";
        return ESP_ERR_INVALID_ARG;
    }
    if (!cfg::valid_timezone(next.timezone)) {
        err = "invalid timezone string";
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = ESP_OK;
    if (next.hostname != cur.hostname) {
        e = cfg::set_hostname(next.hostname);
        if (e == ESP_OK) net::apply_hostname();
    }
    if (e == ESP_OK && (next.ntp_server != cur.ntp_server || next.timezone != cur.timezone)) {
        e = cfg::set_ntp(next.ntp_server, next.timezone);
        if (e == ESP_OK) net::apply_time_config();
    }
    if (e == ESP_OK && next.require_signed_images != cur.require_signed_images)
        e = cfg::set_require_signed_images(next.require_signed_images);
    if (e != ESP_OK) err = std::string("saving failed: ") + esp_err_to_name(e);
    return e;
}

esp_err_t h_settings_post(httpd_req_t* req) {
    cJSON* j = read_json(req, 2048);
    if (!j) return ESP_OK;
    std::string err;
    esp_err_t e = apply_settings(j, err, false);
    cJSON_Delete(j);
    if (e != ESP_OK) return send_error(req, "400 Bad Request", err);
    return send_json(req, settings_json(false));
}

esp_err_t h_export(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"bms-agg-config.json\"");
    cJSON* o = settings_json(true);
    char* s = cJSON_Print(o);
    cJSON_Delete(o);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return e;
}

esp_err_t h_import(httpd_req_t* req) {
    cJSON* j = read_json(req, 4096);
    if (!j) return ESP_OK;
    std::string err;
    esp_err_t e = apply_settings(j, err, true);
    cJSON_Delete(j);
    if (e != ESP_OK) return send_error(req, "400 Bad Request", "import rejected: " + err);
    ESP_LOGI(TAG, "config imported");
    return send_json(req, settings_json(false));
}

esp_err_t h_wifi(httpd_req_t* req) {
    cJSON* j = read_json(req, 512);
    if (!j) return ESP_OK;
    std::string ssid = json_str(j, "ssid"), pass = json_str(j, "password");
    cJSON_Delete(j);
    if (!cfg::valid_ssid(ssid)) return send_error(req, "400 Bad Request", "WiFi name is required (max 32)");
    if (!cfg::valid_wifi_password(pass))
        return send_error(req, "400 Bad Request", "password must be empty (open) or 8-63 characters");
    esp_err_t e = cfg::set_wifi(ssid, pass);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    ESP_LOGI(TAG, "WiFi changed to '%s', reconnecting", ssid.c_str());
    send_ok(req);
    net::reconnect();
    return ESP_OK;
}

esp_err_t h_password(httpd_req_t* req) {
    cJSON* j = read_json(req, 512);
    if (!j) return ESP_OK;
    std::string cur = json_str(j, "current"), next = json_str(j, "new");
    cJSON_Delete(j);
    uint32_t ip = client_ip(req);
    if (login_wait_s(ip) > 0) return send_error(req, "429 Too Many Requests", "too many failed attempts");
    if (!cfg::check_admin_password(cur)) {
        login_result(ip, false);
        return send_error(req, "403 Forbidden", "current password is wrong");
    }
    if (next.size() < cfg::kMinAdminPassword || next.size() > 64)
        return send_error(req, "400 Bad Request", "new password must be 8-64 characters");
    esp_err_t e = cfg::set_admin_password(next);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    sessions_drop_all_except(req);
    ESP_LOGI(TAG, "admin password changed; other sessions logged out");
    return send_ok(req);
}

esp_err_t h_factory_reset(httpd_req_t* req) {
    cJSON* j = read_json(req, 256);
    if (!j) return ESP_OK;
    std::string c = json_str(j, "confirm");
    cJSON_Delete(j);
    if (c != "RESET") return send_error(req, "400 Bad Request", "type RESET to confirm");
    ESP_LOGW(TAG, "factory reset requested from web UI");
    send_ok(req);
    schedule_restart(1500, true);
    return ESP_OK;
}

esp_err_t h_reboot(httpd_req_t* req) {
    ESP_LOGI(TAG, "reboot requested from web UI");
    send_ok(req);
    schedule_restart(1000, false);
    return ESP_OK;
}

// ---- Firmware -----------------------------------------------------------------------

esp_err_t h_ota_info(httpd_req_t* req) {
    ota::Info i = ota::info();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "running", slot_json(i.running));
    cJSON_AddItemToObject(o, "other", slot_json(i.other));
    cJSON_AddStringToObject(o, "boot", i.boot_label.c_str());
    cJSON_AddBoolToObject(o, "rollback_possible", i.rollback_possible);
    cJSON_AddBoolToObject(o, "pending_verify", i.pending_verify);
    cJSON_AddBoolToObject(o, "busy", ota::busy());
    cJSON_AddNumberToObject(o, "max_size", ota::max_image_size());
    cJSON_AddBoolToObject(o, "require_signed_images", cfg::get().require_signed_images);
    cJSON_AddStringToObject(o, "selftest", ota::selftest_state());
    return send_json(req, o);
}

esp_err_t h_ota_upload(httpd_req_t* req) {
    if (!ota::try_lock()) {
        drain(req, req->content_len);
        return send_error(req, "409 Conflict", "another update is already in progress");
    }
    struct Unlock {
        ~Unlock() { ota::unlock(); }
    } unlock;

    size_t total = req->content_len;
    if (total == 0) return send_error(req, "411 Length Required", "empty upload");
    std::string err;
    ota::Upload up;
    esp_err_t e = up.begin(total, err);
    if (e != ESP_OK) {
        drain(req, total);
        return send_error(req, "400 Bad Request", err);
    }
    const size_t kBuf = 4096;
    uint8_t* buf = (uint8_t*)malloc(kBuf);
    if (!buf) {
        drain(req, total);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t remaining = total;
    int timeouts = 0;
    int64_t t0 = esp_timer_get_time();
    while (remaining > 0) {
        int n = httpd_req_recv(req, (char*)buf, std::min(remaining, kBuf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (n <= 0) {
            free(buf);
            up.abort();
            ESP_LOGE(TAG, "upload connection lost with %u bytes left", (unsigned)remaining);
            return ESP_FAIL;  // closes the socket
        }
        timeouts = 0;
        remaining -= n;
        // After a rejection keep reading (and discarding) so the browser gets
        // a clean error response instead of a connection reset.
        if (e == ESP_OK) {
            e = up.write(buf, n, err);
            if (e != ESP_OK) up.abort();
        }
    }
    free(buf);
    if (e == ESP_OK) e = up.finish(err);
    if (e != ESP_OK) return send_error(req, "400 Bad Request", err);

    ESP_LOGI(TAG, "upload ok: %u bytes in %d ms, rebooting into %s", (unsigned)total,
             (int)((esp_timer_get_time() - t0) / 1000), up.facts().version);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "version", up.facts().version);
    cJSON_AddBoolToObject(o, "rebooting", true);
    send_json(req, o);
    schedule_restart(1500, false);
    return ESP_OK;
}

esp_err_t h_ota_rollback(httpd_req_t* req) {
    if (ota::busy()) return send_error(req, "409 Conflict", "an update is in progress");
    std::string err;
    if (ota::select_previous(err) != ESP_OK) return send_error(req, "400 Bad Request", err);
    send_ok(req);
    schedule_restart(1500, false);
    return ESP_OK;
}

}  // namespace

void register_api(httpd_handle_t h) {
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 8; i++) snprintf(s_boot_id + 2 * i, 3, "%02x", r[i]);

    add_route(h, "/api/health", HTTP_GET, h_health, R_PUBLIC);
    add_route(h, "/api/session", HTTP_GET, h_session, R_PUBLIC);
    add_route(h, "/api/login", HTTP_POST, h_login, R_PUBLIC);
    add_route(h, "/api/logout", HTTP_POST, h_logout, R_PUBLIC);

    add_route(h, "/api/setup", HTTP_POST, h_setup, R_SETUP_ONLY);
    add_route(h, "/api/setup/status", HTTP_GET, h_setup_status, R_PUBLIC);  // checks itself
    add_route(h, "/api/setup/scan", HTTP_GET, send_scan, R_SETUP_ONLY);

    add_route(h, "/api/status", HTTP_GET, h_status, R_AUTH);
    add_route(h, "/api/log", HTTP_GET, h_log, R_AUTH);
    add_route(h, "/api/settings", HTTP_GET, h_settings_get, R_AUTH);
    add_route(h, "/api/settings", HTTP_POST, h_settings_post, R_AUTH);
    add_route(h, "/api/config/export", HTTP_GET, h_export, R_AUTH);
    add_route(h, "/api/config/import", HTTP_POST, h_import, R_AUTH);
    add_route(h, "/api/wifi", HTTP_POST, h_wifi, R_AUTH);
    add_route(h, "/api/wifi/scan", HTTP_GET, send_scan, R_AUTH);
    add_route(h, "/api/password", HTTP_POST, h_password, R_AUTH);
    add_route(h, "/api/factory-reset", HTTP_POST, h_factory_reset, R_AUTH);
    add_route(h, "/api/reboot", HTTP_POST, h_reboot, R_AUTH);
    add_route(h, "/api/ota/info", HTTP_GET, h_ota_info, R_AUTH);
    add_route(h, "/api/ota/upload", HTTP_POST, h_ota_upload, R_AUTH);
    add_route(h, "/api/ota/rollback", HTTP_POST, h_ota_rollback, R_AUTH);
}

}  // namespace webserver
