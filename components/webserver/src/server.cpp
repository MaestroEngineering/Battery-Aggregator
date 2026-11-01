// HTTP server core: routing with auth/CSRF checks, sessions, login rate
// limiting, static files.
#include <string.h>

#include <vector>

#include "config/config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.h"
#include "lwip/sockets.h"
#include "net/net.h"
#include "webserver/webserver.h"

namespace webserver {
namespace {

const char* TAG = "web";
httpd_handle_t s_server = nullptr;

// Every POST must carry this header. Browsers cannot add custom headers to a
// cross-site request without a CORS preflight, which we never grant, so this
// blocks CSRF from other sites.
constexpr const char* kCsrfHeader = "X-BMS-Request";

// ---- Sessions -----------------------------------------------------------------
constexpr int kMaxSessions = 4;
constexpr int64_t kIdleUs = 30LL * 60 * 1000000;        // 30 min idle
constexpr int64_t kAbsoluteUs = 12LL * 3600 * 1000000;  // 12 h absolute
constexpr const char* kCookie = "bms_sid";

struct Session {
    char token[65] = {};
    int64_t created = 0, last = 0;
};
Session s_sessions[kMaxSessions];
portMUX_TYPE s_sess_mux = portMUX_INITIALIZER_UNLOCKED;

bool cookie_token(httpd_req_t* req, char out[65]) {
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 1000) return false;
    std::string c(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Cookie", c.data(), len + 1) != ESP_OK) return false;
    std::string key = std::string(kCookie) + "=";
    size_t p = 0;
    while ((p = c.find(key, p)) != std::string::npos) {
        if (p == 0 || c[p - 1] == ' ' || c[p - 1] == ';') {
            p += key.size();
            size_t e = c.find(';', p);
            std::string v = c.substr(p, e == std::string::npos ? std::string::npos : e - p);
            v.resize(strnlen(v.c_str(), v.size()));
            if (v.size() != 64) return false;
            memcpy(out, v.data(), 64);
            out[64] = 0;
            return true;
        }
        p += key.size();
    }
    return false;
}

bool tokens_equal(const char* a, const char* b) {
    uint8_t d = 0;
    for (int i = 0; i < 64; i++) d |= a[i] ^ b[i];
    return d == 0;
}

// ---- Login rate limiting ----------------------------------------------------------
// Per client IP: 5 free attempts, then a lockout that doubles with every further
// failure (30 s, 60 s, ... up to 15 min). Plus a global brake against many IPs.
constexpr int kFreeAttempts = 5;
constexpr int64_t kBaseLockUs = 30LL * 1000000;
constexpr int64_t kMaxLockUs = 15LL * 60 * 1000000;
constexpr int kGlobalMaxFails = 20;  // per 10 min window
constexpr int64_t kGlobalWindowUs = 10LL * 60 * 1000000;

struct Attempt {
    uint32_t ip = 0;
    int fails = 0;
    int64_t locked_until = 0, last = 0;
};
Attempt s_attempts[8];
int s_global_fails = 0;
int64_t s_global_window_start = 0;

Attempt* attempt_for(uint32_t ip, bool create) {
    Attempt* oldest = &s_attempts[0];
    for (auto& a : s_attempts) {
        if (a.ip == ip && a.last) return &a;
        if (a.last < oldest->last) oldest = &a;
    }
    if (!create) return nullptr;
    *oldest = Attempt{};
    oldest->ip = ip;
    return oldest;
}

// ---- Routing ------------------------------------------------------------------------
struct Route {
    esp_err_t (*fn)(httpd_req_t*);
    unsigned flags;
};
std::vector<Route*> s_routes;

esp_err_t dispatch(httpd_req_t* req) {
    const Route* r = static_cast<const Route*>(req->user_ctx);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (req->method == HTTP_POST) {
        char v[8] = {};
        if (httpd_req_get_hdr_value_str(req, kCsrfHeader, v, sizeof(v)) != ESP_OK ||
            strcmp(v, "1") != 0)
            return send_error(req, "403 Forbidden", "missing X-BMS-Request header");
    }
    if (r->flags & R_SETUP_ONLY) {
        if (cfg::is_provisioned()) return send_error(req, "403 Forbidden", "device is already set up");
    } else if ((r->flags & R_AUTH) && !session_valid(req)) {
        return send_error(req, "401 Unauthorized", "login required");
    }
    return r->fn(req);
}

int s_restart_delay_ms = 1500;

void restart_task(void* arg) {
    bool factory = arg != nullptr;
    vTaskDelay(pdMS_TO_TICKS(s_restart_delay_ms));
    if (factory) cfg::factory_reset();
    net::stop();
    esp_restart();
}

// ---- Static files ---------------------------------------------------------------------
esp_err_t static_handler(httpd_req_t* req) {
    std::string path(req->uri);
    size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    bool setup = !cfg::is_provisioned();

    if (path == "/") path = setup ? "/setup.html" : "/index.html";
    if (!setup && path == "/setup.html") path = "/index.html";

    if (!asset_exists(path.c_str())) {
        if (setup) {
            // Captive portal: send any unknown URL (phone connectivity checks
            // etc.) to the setup page.
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
            return httpd_resp_send(req, nullptr, 0);
        }
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "not found");
    }
    return serve_asset(req, path.c_str());
}

}  // namespace

// ---- Public / internal API ------------------------------------------------------------

void add_route(httpd_handle_t h, const char* uri, httpd_method_t method,
               esp_err_t (*fn)(httpd_req_t*), unsigned flags) {
    Route* r = new Route{fn, flags};
    s_routes.push_back(r);
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = dispatch;
    u.user_ctx = r;
    if (httpd_register_uri_handler(h, &u) != ESP_OK) ESP_LOGE(TAG, "cannot register %s", uri);
}

esp_err_t start() {
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 40;
    c.stack_size = 10240;
    c.lru_purge_enable = true;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_open_sockets = 7;
    c.recv_wait_timeout = 10;
    c.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_server, &c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return err;
    }
    register_api(s_server);
    httpd_uri_t st = {};
    st.uri = "/*";
    st.method = HTTP_GET;
    st.handler = static_handler;
    httpd_register_uri_handler(s_server, &st);
    ESP_LOGI(TAG, "HTTP server on port 80");
    return ESP_OK;
}

bool running() { return s_server != nullptr; }

void schedule_restart(int delay_ms, bool factory_reset) {
    s_restart_delay_ms = delay_ms;
    xTaskCreate(restart_task, "restart", 4096, factory_reset ? (void*)1 : nullptr, 5, nullptr);
}

esp_err_t send_json(httpd_req_t* req, cJSON* root, const char* status) {
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return e;
}

esp_err_t send_error(httpd_req_t* req, const char* status, const std::string& msg) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg.c_str());
    return send_json(req, o, status);
}

esp_err_t send_ok(httpd_req_t* req) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

cJSON* read_json(httpd_req_t* req, size_t limit) {
    if (req->content_len == 0 || req->content_len > limit) {
        send_error(req, "400 Bad Request", "missing or oversized body");
        return nullptr;
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body.data() + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) {
            send_error(req, "400 Bad Request", "body read failed");
            return nullptr;
        }
        got += n;
    }
    cJSON* j = cJSON_ParseWithLength(body.data(), body.size());
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        send_error(req, "400 Bad Request", "body is not a JSON object");
        return nullptr;
    }
    return j;
}

std::string json_str(cJSON* obj, const char* key, bool* present) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    bool ok = cJSON_IsString(v) && v->valuestring;
    if (present) *present = ok;
    return ok ? std::string(v->valuestring) : std::string();
}

uint32_t client_ip(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (sockaddr*)&addr, &len) != 0) return 0;
    if (addr.ss_family == AF_INET) return ((sockaddr_in*)&addr)->sin_addr.s_addr;
#if LWIP_IPV6
    if (addr.ss_family == AF_INET6) {
        // IPv4-mapped (::ffff:a.b.c.d) or hash of the v6 address.
        auto* a6 = (sockaddr_in6*)&addr;
        const uint32_t* w = (const uint32_t*)&a6->sin6_addr;
        return w[3] ^ (w[0] == 0 && w[1] == 0 ? 0 : w[0] ^ w[1] ^ w[2]);
    }
#endif
    return 0;
}

std::string ip_to_str(uint32_t ip) {
    char b[16];
    const uint8_t* p = (const uint8_t*)&ip;
    snprintf(b, sizeof(b), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

bool session_valid(httpd_req_t* req) {
    char tok[65];
    if (!cookie_token(req, tok)) return false;
    int64_t now = esp_timer_get_time();
    bool ok = false;
    portENTER_CRITICAL(&s_sess_mux);
    for (auto& s : s_sessions) {
        if (!s.created) continue;
        if (now - s.last > kIdleUs || now - s.created > kAbsoluteUs) {
            s = Session{};
            continue;
        }
        if (tokens_equal(s.token, tok)) {
            s.last = now;
            ok = true;
        }
    }
    portEXIT_CRITICAL(&s_sess_mux);
    return ok;
}

std::string session_create(uint32_t ip) {
    (void)ip;
    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    char tok[65];
    for (int i = 0; i < 32; i++) snprintf(tok + 2 * i, 3, "%02x", rnd[i]);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_sess_mux);
    Session* slot = &s_sessions[0];
    for (auto& s : s_sessions) {
        if (!s.created) {
            slot = &s;
            break;
        }
        if (s.last < slot->last) slot = &s;  // evict least recently used
    }
    memcpy(slot->token, tok, 65);
    slot->created = slot->last = now;
    portEXIT_CRITICAL(&s_sess_mux);
    return tok;
}

void session_drop(httpd_req_t* req) {
    char tok[65];
    if (!cookie_token(req, tok)) return;
    portENTER_CRITICAL(&s_sess_mux);
    for (auto& s : s_sessions)
        if (s.created && tokens_equal(s.token, tok)) s = Session{};
    portEXIT_CRITICAL(&s_sess_mux);
}

void sessions_drop_all_except(httpd_req_t* req) {
    char tok[65] = {};
    bool have = cookie_token(req, tok);
    portENTER_CRITICAL(&s_sess_mux);
    for (auto& s : s_sessions)
        if (s.created && !(have && tokens_equal(s.token, tok))) s = Session{};
    portEXIT_CRITICAL(&s_sess_mux);
}

int login_wait_s(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    if (now - s_global_window_start > kGlobalWindowUs) {
        s_global_window_start = now;
        s_global_fails = 0;
    }
    if (s_global_fails >= kGlobalMaxFails)
        return (int)((s_global_window_start + kGlobalWindowUs - now) / 1000000) + 1;
    Attempt* a = attempt_for(ip, false);
    if (a && a->locked_until > now) return (int)((a->locked_until - now) / 1000000) + 1;
    return 0;
}

void login_result(uint32_t ip, bool ok) {
    int64_t now = esp_timer_get_time();
    if (ok) {
        Attempt* a = attempt_for(ip, false);
        if (a) *a = Attempt{};
        return;
    }
    s_global_fails++;
    Attempt* a = attempt_for(ip, true);
    a->fails++;
    a->last = now;
    if (a->fails >= kFreeAttempts) {
        int shift = a->fails - kFreeAttempts;
        int64_t lock = shift > 10 ? kMaxLockUs : std::min(kMaxLockUs, kBaseLockUs << shift);
        a->locked_until = now + lock;
        ESP_LOGW(TAG, "login locked for %s for %d s after %d failures", ip_to_str(ip).c_str(),
                 (int)(lock / 1000000), a->fails);
    }
}

}  // namespace webserver
