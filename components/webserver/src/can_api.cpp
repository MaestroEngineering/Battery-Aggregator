// CAN tap and Verbose Diagnostics API, plus the live capture WebSocket.
//
// Binary capture format (WebSocket messages and GET /api/can/buffer):
//   header 24 bytes: "BMSF", u32 count (0xFFFFFFFF = until end of body),
//                    u32 dropped (frames this client missed), u32 0,
//                    i64 epoch_offset_us (unix_us = ts_us + offset; 0 = not synced)
//   then can::Record (24 bytes each, little-endian; see can/frame.h).
#include <stdio.h>
#include <string.h>

#include <memory>
#include <new>

#include "can/can.h"
#include "can/pins.h"
#include "config/config.h"
#include "agg/agg.h"
#include "agg_status_json.h"
#include "can_status_json.h"
#include "heapmon/heapmon.h"
#include "diag/diag.h"
#include "hamqtt/hamqtt.h"
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

// Large replies are streamed through this sink (JsonOut, 256-byte chunks): no cJSON tree.
bool chunk_sink(void* ctx, const char* data, size_t len) {
    return httpd_resp_send_chunk((httpd_req_t*)ctx, data, len) == ESP_OK;
}

esp_err_t h_can_status(httpd_req_t* req) {
    can::Status s = can::status();  // on the httpd stack, as before (0.1.6/0.1.7)
    httpd_resp_set_type(req, "application/json");
    JsonOut j(chunk_sink, req);
    if (!write_can_status(j, s, can::kRingSize, can::ring_head() - can::ring_tail())) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
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
                           "%s{\"pt\":%u,\"id\":%lu,\"x\":%d,\"r\":%d,\"n\":%lu,\"p\":%lu,\"a\":%lld,\"c\":%lld,"
                           "\"l\":%u,\"d\":\"%s\",\"mn\":\"%s\",\"mx\":\"%s\"}",
                           i ? "," : "", (unsigned)s.port, (unsigned long)(s.id & can::kIdMask),
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

esp_err_t h_can_probe(httpd_req_t* req) {
    if (can::start_probe() != ESP_OK) return send_error(req, "409 Conflict", "a check or detection is already running");
    return send_ok(req);
}

// BENCH ONLY: needs tools/bench_can_check.py --bench --tests 4 sending test frames.
esp_err_t h_can_intmap(httpd_req_t* req) {
    if (can::start_intmap() != ESP_OK)
        return send_error(req, "409 Conflict", "a check, detection or INT mapping is already running");
    return send_ok(req);
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

// ---- Aggregator ------------------------------------------------------------------------

// GET /api/agg/status. Streamed in 256-byte chunks (JsonOut): the request needs ~5 KB of heap
// (snapshot + events) instead of the ~49 KB cJSON tree of 0.1.7, which failed on the device.

esp_err_t h_agg_status(httpd_req_t* req) {
    std::unique_ptr<AggStatusData> d(new (std::nothrow) AggStatusData);
    if (!d) return send_error(req, "503 Service Unavailable", "out of memory");
    agg::snapshot(d->snap);
    d->nev = agg::events_since(0, d->ev, agg::kEventRing);
    AggStatusExtras x;
    {
        can::Status cs = can::status();
        const can::PortStatus& inv = cs.ports[BMS_CAN_PORT_INV];
        x.tx_enabled = cs.tx_enabled;
        x.inv_enabled = inv.enabled;
        x.inv_state = can::state_name(inv.state);
        x.sending = cs.tx_enabled && inv.state == can::State::Transmitting && d->snap.out.send;
        x.tx_frames = inv.tx_frames;
        x.tx_failed = inv.tx_failed;
        x.tec = inv.tec;
        x.blocked_cmds = cs.blocked_cmds;
    }
    x.epoch_offset_us = net::epoch_offset_us();
    hamqtt::Status ms = hamqtt::status();
    x.mqtt_state = ms.state;
    x.mqtt_published = ms.published;
    x.mqtt_events_sent = ms.events_sent;
    x.mqtt_last_publish_age_s = ms.last_publish_age_s;
    httpd_resp_set_type(req, "application/json");
    JsonOut j(chunk_sink, req);
    if (!write_agg_status(j, *d, x, (uint64_t)(esp_timer_get_time() / 1000))) return ESP_FAIL;  // client gone
    static bool s_marked = false;
    if (!s_marked) s_marked = true, heapmon::mark("first aggregator status");
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t h_agg_clear(httpd_req_t* req) {
    cJSON* j = read_json(req, 256);
    if (!j) return ESP_OK;
    std::string c = json_str(j, "confirm");
    cJSON_Delete(j);
    if (c != "CLEAR") return send_error(req, "400 Bad Request", "type CLEAR to confirm");
    ESP_LOGW(TAG, "latched trips cleared from the web UI (limits return after the hold time)");
    agg::clear_trips();
    return send_ok(req);
}

// Inverter transmit on port INV. Enabling needs {"enable":true,"confirm":"TRANSMIT"},
// port INV enabled and a complete aggregator configuration. Disabling needs nothing.
esp_err_t h_agg_transmit(httpd_req_t* req) {
    cJSON* j = read_json(req, 256);
    if (!j) return ESP_OK;
    cJSON* en = cJSON_GetObjectItemCaseSensitive(j, "enable");
    std::string c = json_str(j, "confirm");
    bool valid = cJSON_IsBool(en), enable = cJSON_IsTrue(en);
    cJSON_Delete(j);
    if (!valid) return send_error(req, "400 Bad Request", "enable must be true or false");
    cfg::Settings s = cfg::get();
    if (enable) {
        if (c != "TRANSMIT") return send_error(req, "400 Bad Request", "type TRANSMIT to confirm");
        if (!s.can_inv_enabled) return send_error(req, "409 Conflict", "enable port INV (and set its bitrate) first");
        if (!s.agg_src_a && !s.agg_src_b) return send_error(req, "409 Conflict", "enable at least one aggregator source first");
        agg::Snapshot* snap = new (std::nothrow) agg::Snapshot;
        if (!snap) return send_error(req, "503 Service Unavailable", "out of memory");
        agg::snapshot(*snap);
        const char* why = "";
        bool ok = agg::config_valid(snap->cfg, &why);
        delete snap;
        if (!ok) return send_error(req, "409 Conflict", std::string("aggregator not configured: ") + why);
    }
    if (s.agg_tx_enabled != enable) {
        s.agg_tx_enabled = enable;
        std::string err;
        if (cfg::save(s, err) != ESP_OK) return send_error(req, "400 Bad Request", err);
        ESP_LOGW(TAG, "inverter transmit on port INV %s from the web UI", enable ? "ENABLED" : "disabled");
        can::apply_settings();
    }
    return send_ok(req);
}

// ---- Verbose Diagnostics -------------------------------------------------------------

esp_err_t h_diag_status(httpd_req_t* req) {  // streamed (JsonOut)
    diag::Status s = diag::status();
    httpd_resp_set_type(req, "application/json");
    JsonOut j(chunk_sink, req);
    j.begin();
    j.boolean("enabled", s.enabled);
    j.boolean("active", s.active);
    j.str("state", s.state);
    j.num("batches_ok", s.batches_ok);
    j.num("batches_failed", s.batches_failed);
    j.num("last_http_status", s.last_http_status);
    j.str("last_error", s.last_error.c_str());
    j.num("last_ok_age_s", (double)s.last_ok_age_s);
    j.num("pending_batches", s.pending_batches);
    j.num("pending_frames", s.pending_frames);
    j.num("dropped_total", s.dropped_total);
    j.num("events_total", s.events_total);
    j.num("next_seq", s.next_seq);
    j.num("boot_id", s.boot_id);
    j.boolean("trigger_latched", s.trigger_latched);
    j.num("last_event_age_s", (double)s.last_event_age_s);
    char id[12], d[17];
    snprintf(id, sizeof(id), "0x%lX", (unsigned long)s.last_event_id);
    hex(d, s.last_event_data, s.last_event_dlc);
    j.str("last_event_id", id);
    j.str("last_event_data", d);
    if (!j.end()) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
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
    add_route(h, "/api/can/probe", HTTP_POST, h_can_probe, R_AUTH);
    add_route(h, "/api/can/intmap", HTTP_POST, h_can_intmap, R_AUTH);
    add_route(h, "/api/can/buffer", HTTP_GET, h_can_buffer, R_AUTH);
    add_route(h, "/api/agg/status", HTTP_GET, h_agg_status, R_AUTH);
    add_route(h, "/api/agg/clear-trips", HTTP_POST, h_agg_clear, R_AUTH);
    add_route(h, "/api/agg/transmit", HTTP_POST, h_agg_transmit, R_AUTH);
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
