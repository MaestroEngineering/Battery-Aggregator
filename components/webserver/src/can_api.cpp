// CAN tap and Verbose Diagnostics API, plus the live capture WebSocket.
//
// Binary capture format (WebSocket messages and GET /api/can/buffer):
//   header 24 bytes: "BMSF", u32 count (0xFFFFFFFF = until end of body),
//                    u32 dropped (frames this client missed), u32 0,
//                    i64 epoch_offset_us (unix_us = ts_us + offset; 0 = not synced)
//   then can::Record (24 bytes each, little-endian; see can/frame.h).
#include <stdio.h>
#include <string.h>

#include "can/can.h"
#include "config/config.h"
#include "diag/diag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "internal.h"
#include "net/net.h"

namespace webserver {
namespace {

const char* TAG = "can_api";
constexpr int kMaxWsClients = 3;
constexpr size_t kWsBatch = 256;
constexpr size_t kHdr = 24;

httpd_handle_t s_h = nullptr;
struct WsClient {
    int fd;  // -1 = free
    uint32_t seq;
    uint32_t dropped;
};
WsClient s_ws[kMaxWsClients] = {{-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}};
esp_timer_handle_t s_ws_timer = nullptr;
volatile bool s_ws_work_queued = false;
uint8_t s_msg[kHdr + kWsBatch * sizeof(can::Record)];  // used only in the httpd task
can::IdStats s_table[16];  // the table is streamed in chunks

void put_header(uint8_t* p, uint32_t count, uint32_t dropped) {
    memcpy(p, "BMSF", 4);
    memcpy(p + 4, &count, 4);
    memcpy(p + 8, &dropped, 4);
    memset(p + 12, 0, 4);
    int64_t off = net::epoch_offset_us();
    memcpy(p + 16, &off, 8);
}

void hex(char* out, const uint8_t* d, int n) {
    static const char* H = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}

// ---- WebSocket ------------------------------------------------------------------

void ws_push(void*) {  // runs in the httpd task
    s_ws_work_queued = false;
    for (auto& c : s_ws) {
        if (c.fd < 0) continue;
        if (httpd_ws_get_fd_info(s_h, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            c.fd = -1;
            continue;
        }
        for (int rounds = 0; rounds < 4; rounds++) {  // up to 1024 frames per tick
            can::Record* recs = (can::Record*)(s_msg + kHdr);
            size_t n = can::ring_read(c.seq, recs, kWsBatch, &c.dropped);
            if (n == 0) break;
            put_header(s_msg, n, c.dropped);
            httpd_ws_frame_t f = {};
            f.type = HTTPD_WS_TYPE_BINARY;
            f.payload = s_msg;
            f.len = kHdr + n * sizeof(can::Record);
            f.final = true;
            if (httpd_ws_send_frame_async(s_h, c.fd, &f) != ESP_OK) {
                ESP_LOGI(TAG, "capture client fd %d gone", c.fd);
                c.fd = -1;
                break;
            }
            if (n < kWsBatch) break;
        }
    }
}

void ws_timer_cb(void*) {
    bool any = false;
    for (auto& c : s_ws) any |= c.fd >= 0;
    if (!any || s_ws_work_queued) return;
    s_ws_work_queued = true;
    if (httpd_queue_work(s_h, ws_push, nullptr) != ESP_OK) s_ws_work_queued = false;
}

esp_err_t ws_pre_handshake(httpd_req_t* req) {
    // Same session check as every other authenticated endpoint.
    if (!session_valid(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "login required");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {  // handshake done
        for (auto& c : s_ws) {
            if (c.fd < 0 || c.fd == fd) {
                c.fd = fd;
                c.seq = can::ring_head();  // live from now
                c.dropped = 0;
                ESP_LOGI(TAG, "capture client fd %d connected", fd);
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "too many capture clients");
        return ESP_FAIL;
    }
    // Incoming message (the browser sends nothing meaningful): read and drop.
    httpd_ws_frame_t f = {};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len) {
        uint8_t buf[64];
        f.payload = buf;
        if (f.len > sizeof(buf)) return ESP_FAIL;
        if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

// ---- CAN --------------------------------------------------------------------------

esp_err_t h_can_status(httpd_req_t* req) {
    can::Status s = can::status();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s.enabled);
    cJSON_AddStringToObject(o, "state", can::state_name(s.state));
    cJSON_AddStringToObject(o, "error", s.error.c_str());
    cJSON_AddNumberToObject(o, "bitrate", s.bitrate);
    cJSON_AddNumberToObject(o, "frames", (double)s.frames_total);
    cJSON_AddNumberToObject(o, "fps", s.fps);
    cJSON_AddNumberToObject(o, "bus_errors", s.bus_errors);
    cJSON_AddNumberToObject(o, "rx_overflows", s.rx_overflows);
    cJSON_AddNumberToObject(o, "rec", s.rec);
    cJSON_AddNumberToObject(o, "ids", s.ids);
    cJSON_AddNumberToObject(o, "ids_dropped", s.ids_dropped);
    cJSON_AddNumberToObject(o, "blocked_cmds", s.blocked_cmds);
    cJSON_AddNumberToObject(o, "last_frame_age_ms", (double)s.last_frame_age_ms);
    cJSON_AddNumberToObject(o, "ring_size", can::kRingSize);
    uint32_t used = can::ring_head() - can::ring_tail();
    cJSON_AddNumberToObject(o, "ring_used", used);
    cJSON* d = cJSON_AddObjectToObject(o, "detect");
    cJSON_AddBoolToObject(d, "running", s.detect_running);
    cJSON* arr = cJSON_AddArrayToObject(d, "results");
    for (int i = 0; i < s.detect_count; i++) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "bitrate", s.detect[i].bitrate);
        cJSON_AddNumberToObject(r, "frames", s.detect[i].frames);
        cJSON_AddNumberToObject(r, "errors", s.detect[i].errors);
        cJSON_AddItemToArray(arr, r);
    }
    cJSON_AddNumberToObject(d, "suggestion", s.detect_suggestion);
    cJSON_AddStringToObject(d, "note", s.detect_note.c_str());
    return send_json(req, o);
}

esp_err_t h_can_table(httpd_req_t* req) {
    int64_t now = esp_timer_get_time();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "[", 1);
    char buf[200], d[17], mn[17], mx[17];
    size_t i = 0;
    for (size_t start = 0;; start += 16) {
      size_t n = can::stats_copy(s_table, start, 16);
      for (size_t k = 0; k < n; k++, i++) {
        const can::IdStats& s = s_table[k];
        hex(d, s.data, s.dlc);
        hex(mn, s.min, s.dlc);
        hex(mx, s.max, s.dlc);
        int len = snprintf(buf, sizeof(buf),
                           "%s{\"id\":%lu,\"x\":%d,\"r\":%d,\"n\":%lu,\"p\":%lu,\"a\":%lld,\"c\":%lld,"
                           "\"l\":%u,\"d\":\"%s\",\"mn\":\"%s\",\"mx\":\"%s\"}",
                           i ? "," : "", (unsigned long)(s.id & can::kIdMask),
                           (s.id & can::kIdExtended) ? 1 : 0, (s.id & can::kIdRtr) ? 1 : 0,
                           (unsigned long)s.count, (unsigned long)(s.period_us / 1000),
                           (long long)((now - (int64_t)s.last_ts_us) / 1000),
                           (long long)((now - (int64_t)s.last_change_us) / 1000), s.dlc, d, mn, mx);
        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) return ESP_FAIL;
      }
      if (n < 16) break;
    }
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t h_can_table_reset(httpd_req_t* req) {
    can::stats_reset();
    return send_ok(req);
}

esp_err_t h_can_detect(httpd_req_t* req) {
    if (!cfg::get().can_enabled)
        return send_error(req, "409 Conflict", "enable the CAN tap first");
    if (can::start_detect() != ESP_OK) return send_error(req, "409 Conflict", "detection already running");
    return send_ok(req);
}

// Whole ring buffer (recent past) in the binary capture format.
esp_err_t h_can_buffer(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"bms-agg-buffer.bin\"");
    uint8_t hdr[kHdr];
    uint32_t seq = can::ring_tail(), dropped = 0;
    put_header(hdr, 0xFFFFFFFF, 0);
    if (httpd_resp_send_chunk(req, (const char*)hdr, kHdr) != ESP_OK) return ESP_FAIL;
    can::Record chunk[64];
    uint32_t end = can::ring_head();
    while (seq < end) {
        size_t n = can::ring_read(seq, chunk, std::min<uint32_t>(64, end - seq), &dropped);
        if (!n) break;
        if (httpd_resp_send_chunk(req, (const char*)chunk, n * sizeof(can::Record)) != ESP_OK)
            return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---- Verbose Diagnostics -------------------------------------------------------------

esp_err_t h_diag_status(httpd_req_t* req) {
    diag::Status s = diag::status();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", s.enabled);
    cJSON_AddBoolToObject(o, "active", s.active);
    cJSON_AddStringToObject(o, "state", s.state);
    cJSON_AddNumberToObject(o, "batches_ok", s.batches_ok);
    cJSON_AddNumberToObject(o, "batches_failed", s.batches_failed);
    cJSON_AddNumberToObject(o, "last_http_status", s.last_http_status);
    cJSON_AddStringToObject(o, "last_error", s.last_error.c_str());
    cJSON_AddNumberToObject(o, "last_ok_age_s", (double)s.last_ok_age_s);
    cJSON_AddNumberToObject(o, "pending_batches", s.pending_batches);
    cJSON_AddNumberToObject(o, "pending_frames", s.pending_frames);
    cJSON_AddNumberToObject(o, "dropped_total", s.dropped_total);
    cJSON_AddNumberToObject(o, "events_total", s.events_total);
    cJSON_AddNumberToObject(o, "next_seq", s.next_seq);
    cJSON_AddNumberToObject(o, "boot_id", s.boot_id);
    cJSON_AddBoolToObject(o, "trigger_latched", s.trigger_latched);
    cJSON_AddNumberToObject(o, "last_event_age_s", (double)s.last_event_age_s);
    char id[12], d[17];
    snprintf(id, sizeof(id), "0x%lX", (unsigned long)s.last_event_id);
    hex(d, s.last_event_data, s.last_event_dlc);
    cJSON_AddStringToObject(o, "last_event_id", id);
    cJSON_AddStringToObject(o, "last_event_data", d);
    return send_json(req, o);
}

esp_err_t h_diag_token(httpd_req_t* req) {
    cJSON* j = read_json(req, 512);
    if (!j) return ESP_OK;
    std::string t = json_str(j, "token");
    cJSON_Delete(j);
    if (cfg::set_diag_token(t) != ESP_OK)
        return send_error(req, "400 Bad Request", "token: printable characters, max 128");
    ESP_LOGI(TAG, "diagnostics upload token %s", t.empty() ? "cleared" : "updated");
    return send_ok(req);
}

esp_err_t h_diag_test_event(httpd_req_t* req) {
    diag::test_event();
    return send_ok(req);
}

esp_err_t h_diag_clear(httpd_req_t* req) {
    diag::clear_trigger();
    return send_ok(req);
}

}  // namespace

void register_can_api(httpd_handle_t h) {
    s_h = h;
    add_route(h, "/api/can/status", HTTP_GET, h_can_status, R_AUTH);
    add_route(h, "/api/can/table", HTTP_GET, h_can_table, R_AUTH);
    add_route(h, "/api/can/table/reset", HTTP_POST, h_can_table_reset, R_AUTH);
    add_route(h, "/api/can/detect", HTTP_POST, h_can_detect, R_AUTH);
    add_route(h, "/api/can/buffer", HTTP_GET, h_can_buffer, R_AUTH);
    add_route(h, "/api/diag/status", HTTP_GET, h_diag_status, R_AUTH);
    add_route(h, "/api/diag/token", HTTP_POST, h_diag_token, R_AUTH);
    add_route(h, "/api/diag/test-event", HTTP_POST, h_diag_test_event, R_AUTH);
    add_route(h, "/api/diag/clear-trigger", HTTP_POST, h_diag_clear, R_AUTH);

    httpd_uri_t ws = {};
    ws.uri = "/ws/can";
    ws.method = HTTP_GET;
    ws.handler = ws_handler;
    ws.is_websocket = true;
    ws.ws_pre_handshake_cb = ws_pre_handshake;
    httpd_register_uri_handler(h, &ws);

    esp_timer_create_args_t ta = {};
    ta.callback = ws_timer_cb;
    ta.name = "ws_push";
    esp_timer_create(&ta, &s_ws_timer);
    esp_timer_start_periodic(s_ws_timer, 200000);  // 5 pushes per second
}

}  // namespace webserver
