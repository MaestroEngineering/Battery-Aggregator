// Verbose Diagnostics: buffer CAN frames into batches and POST them to the
// user's server (SPEC.md "Verbose Diagnostics: data logging and upload").
//
//  - Frames are written into fixed RAM batch buffers (kSlots x kMaxBatch). The
//    CAN task only ever appends under a short spinlock: it never waits on the
//    network.
//  - A buffer is sealed when it holds `diag_batch` frames or `diag_flush_s`
//    has passed, and at once on an event. It is freed only after the server
//    answers HTTP 200.
//  - Server down: sealed buffers stay in RAM; when a new one is needed the
//    oldest waiting buffer is dropped and its frames are counted in `dropped`,
//    which the next batch reports.
//  - Change-only mode: only frames whose data changed, plus a full snapshot of
//    every ID once a minute.
//  - Event: a frame with the trigger ID that differs from its baseline (or a
//    manual test). The last `diag_pretrig_s` seconds of frames are taken from
//    the CAN ring buffer and uploaded first, as an event batch.
//  - Runs in its own low-priority task.
#include "diag/diag.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "can/can.h"
#include "config/config.h"
#include "diag/batch_format.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/net.h"

namespace diag {
namespace {

const char* TAG = "diag";

enum SlotState : uint8_t { S_FREE, S_FILLING, S_SEALED, S_UPLOADING };
struct Slot {
    SlotState state;
    uint16_t flags;
    uint32_t n;
    uint32_t seq;
    uint32_t dropped;
    int64_t opened_us;
    uint32_t order;  // seal order, to find the oldest
    can::Record* recs;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_task = nullptr;
Slot s_slots[kSlots];
can::Record* s_mem = nullptr;
uint32_t s_seal_order = 0;

// Settings cache (read by the CAN task without touching NVS or the heap).
volatile bool s_active = false;
bool s_enabled = false;
uint32_t s_batch = 500, s_flush_s = 30, s_pretrig_s = 60;
bool s_change_only = false;
bool s_trig_enabled = false;
uint32_t s_trig_id = 0;
uint8_t s_trig_base[8];
int s_trig_len = 0;

// Counters.
uint32_t s_boot_id = 0, s_next_seq = 1;
uint32_t s_dropped_pending = 0, s_dropped_total = 0;
uint32_t s_ok = 0, s_failed = 0, s_events = 0;
int s_last_http = 0;
char s_last_error[96] = "";
int64_t s_last_ok_us = 0;
const char* s_state_txt = "off";

// Events.
bool s_latched = false;
volatile bool s_event_pending = false;
can::Record s_event_rec;
int64_t s_event_us = 0, s_last_event_us = 0;

uint8_t s_mac[6];

// Event batch waiting to be (re)sent.
can::Record* s_ev_buf = nullptr;
uint32_t s_ev_n = 0;
bool s_ev_ready = false;  // an event batch (possibly with 0 frames) waits to be sent
BatchHeader s_ev_hdr;

// ---- Slots (callers hold s_mux) ---------------------------------------------------

Slot* find(SlotState st) {
    for (auto& s : s_slots)
        if (s.state == st) return &s;
    return nullptr;
}

Slot* oldest_sealed() {
    Slot* best = nullptr;
    for (auto& s : s_slots)
        if (s.state == S_SEALED && (!best || s.order < best->order)) best = &s;
    return best;
}

void seal(Slot* s) {
    s->state = S_SEALED;
    s->seq = s_next_seq++;
    s->dropped = s_dropped_pending;
    s_dropped_pending = 0;
    s->order = s_seal_order++;
}

Slot* filling_slot(int64_t now) {
    Slot* s = find(S_FILLING);
    if (s) return s;
    s = find(S_FREE);
    if (!s) {
        // All buffers wait for the server: drop the oldest waiting batch.
        s = oldest_sealed();
        if (!s) return nullptr;  // (cannot happen with kSlots >= 3)
        s_dropped_pending += s->n;
        s_dropped_total += s->n;
    }
    s->state = S_FILLING;
    s->n = 0;
    s->flags = s_change_only ? kFlagChangeOnly : 0;
    s->opened_us = now;
    return s;
}

void push(const can::Record& r) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    Slot* s = filling_slot(now);
    if (s) {
        s->recs[s->n++] = r;
        if (s->n >= s_batch) seal(s);
    } else {
        s_dropped_pending++;
        s_dropped_total++;
    }
    portEXIT_CRITICAL(&s_mux);
}

// ---- Upload ------------------------------------------------------------------------

void fill_header(BatchHeader& h, uint16_t flags, uint32_t seq, uint32_t n, uint32_t dropped) {
    memset(&h, 0, sizeof(h));
    memcpy(h.mac, s_mac, 6);
    h.flags = flags;
    h.boot_id = s_boot_id;
    h.seq = seq;
    h.n_frames = n;
    h.dropped = dropped;
    h.bitrate = cfg::get().can_bitrate;
    strncpy(h.fw, esp_app_get_description()->version, sizeof(h.fw));
}

void set_err(const char* e) {
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_last_error, e, sizeof(s_last_error));
    portEXIT_CRITICAL(&s_mux);
}

// Returns true on HTTP 200.
bool post(BatchHeader& h, const can::Record* recs, uint32_t n) {
    cfg::Settings s = cfg::get();
    std::string token = cfg::diag_token();
    h.epoch_offset_us = net::epoch_offset_us();
    if (h.epoch_offset_us) h.flags |= kFlagTimeSynced;
    uint8_t hdr[kHeaderLen];
    encode_header(h, hdr);

    esp_http_client_config_t c = {};
    c.url = s.diag_url.c_str();
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = 10000;
    c.crt_bundle_attach = esp_crt_bundle_attach;  // https:// with public CAs
    esp_http_client_handle_t cl = esp_http_client_init(&c);
    if (!cl) {
        set_err("http client init failed");
        return false;
    }
    esp_http_client_set_header(cl, "Content-Type", "application/vnd.bms-agg.batch");
    if (!token.empty()) {
        std::string auth = "Bearer " + token;
        esp_http_client_set_header(cl, "Authorization", auth.c_str());
    }
    int total = kHeaderLen + (int)n * kRecordLen;
    bool ok = false;
    int status = 0;
    esp_err_t e = esp_http_client_open(cl, total);
    if (e == ESP_OK) {
        int w = esp_http_client_write(cl, (const char*)hdr, kHeaderLen);
        // can::Record is the wire layout (little-endian, 24 bytes, no padding).
        if (w == kHeaderLen && n) w = esp_http_client_write(cl, (const char*)recs, n * kRecordLen);
        if (w >= 0) {
            esp_http_client_fetch_headers(cl);
            status = esp_http_client_get_status_code(cl);
            char sink[128];
            while (esp_http_client_read(cl, sink, sizeof(sink)) > 0) {
            }
            ok = status == 200;
            if (!ok) {
                char m[64];
                snprintf(m, sizeof(m), "server answered HTTP %d", status);
                set_err(m);
            }
        } else {
            set_err("send failed");
        }
    } else {
        char m[96];
        snprintf(m, sizeof(m), "cannot connect: %s", esp_err_to_name(e));
        set_err(m);
    }
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    s_last_http = status;
    if (ok) {
        s_ok++;
        s_last_ok_us = esp_timer_get_time();
        set_err("");
    } else {
        s_failed++;
    }
    return ok;
}

// Builds the event batch from the CAN ring: frames in [event - pretrig, event].
void build_event_batch(const can::Record& ev, int64_t ev_us) {
    if (s_ev_ready)
        ESP_LOGW(TAG, "previous event batch seq %lu was not uploaded yet; replacing it",
                 (unsigned long)s_ev_hdr.seq);
    free(s_ev_buf);
    s_ev_buf = nullptr;
    s_ev_n = 0;
    uint64_t from = (uint64_t)std::max<int64_t>(0, ev_us - (int64_t)s_pretrig_s * 1000000);
    can::Record chunk[32];
    // Pass 1: count.
    uint32_t seq = can::ring_tail(), count = 0, dropped = 0;
    uint64_t oldest = UINT64_MAX;
    for (;;) {
        size_t n = can::ring_read(seq, chunk, 32, &dropped);
        if (!n) break;
        for (size_t i = 0; i < n; i++) {
            if (chunk[i].ts_us > (uint64_t)ev_us) continue;
            if (chunk[i].ts_us < oldest) oldest = chunk[i].ts_us;
            if (chunk[i].ts_us >= from) count++;
        }
    }
    uint16_t flags = kFlagEvent;
    if (s_pretrig_s && (oldest == UINT64_MAX || oldest > from + 1000000)) flags |= kFlagTruncated;
    // Allocate (halve on failure; the newest frames matter most).
    uint32_t want = count;
    while (want && !(s_ev_buf = (can::Record*)malloc(want * sizeof(can::Record)))) want /= 2;
    if (want < count) flags |= kFlagTruncated;
    // Pass 2: copy the newest `want` frames in the window.
    uint32_t skip = count - want, seen = 0;
    seq = can::ring_tail();
    for (;;) {
        size_t n = can::ring_read(seq, chunk, 32, &dropped);
        if (!n) break;
        for (size_t i = 0; i < n && s_ev_n < want; i++) {
            if (chunk[i].ts_us > (uint64_t)ev_us || chunk[i].ts_us < from) continue;
            if (seen++ < skip) continue;
            s_ev_buf[s_ev_n++] = chunk[i];
        }
    }
    portENTER_CRITICAL(&s_mux);
    uint32_t evseq = s_next_seq++;
    portEXIT_CRITICAL(&s_mux);
    fill_header(s_ev_hdr, flags, evseq, s_ev_n, 0);
    s_ev_hdr.event_id = ev.id;
    memcpy(s_ev_hdr.event_data, ev.data, 8);
    s_ev_ready = true;
    ESP_LOGW(TAG, "event batch seq %lu: %lu pre-trigger frames (%lu s window)%s",
             (unsigned long)evseq, (unsigned long)s_ev_n, (unsigned long)s_pretrig_s,
             (flags & kFlagTruncated) ? ", truncated" : "");
}

void apply() {
    cfg::Settings s = cfg::get();
    uint8_t base[8] = {};
    int blen = cfg::parse_hex_bytes(s.trig_baseline, base);
    bool want_active = s.diag_enabled && !s.diag_url.empty();
    if (want_active && !s_mem) {
        s_mem = (can::Record*)heap_caps_malloc(kSlots * kMaxBatch * sizeof(can::Record), MALLOC_CAP_8BIT);
        if (!s_mem) {
            ESP_LOGE(TAG, "no RAM for the batch buffers (%u bytes)",
                     (unsigned)(kSlots * kMaxBatch * sizeof(can::Record)));
            set_err("not enough RAM for the batch buffers");
            want_active = false;
        } else {
            for (int i = 0; i < kSlots; i++) s_slots[i].recs = s_mem + i * kMaxBatch;
        }
    }
    portENTER_CRITICAL(&s_mux);
    s_enabled = s.diag_enabled;
    s_batch = s.diag_batch;
    s_flush_s = s.diag_flush_s;
    s_pretrig_s = s.diag_pretrig_s;
    s_change_only = s.diag_change_only;
    s_trig_enabled = s.trig_enabled;
    s_trig_id = s.trig_id;
    s_trig_len = blen < 0 ? 0 : blen;
    memcpy(s_trig_base, base, 8);
    Slot* f = find(S_FILLING);
    if (f && f->n >= s_batch) seal(f);
    s_active = want_active;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Verbose Diagnostics %s%s; trigger %s (id 0x%lX, baseline %s)",
             want_active ? "ON -> " : "off", want_active ? s.diag_url.c_str() : "",
             s.trig_enabled ? "armed" : "off", (unsigned long)s.trig_id, s.trig_baseline.c_str());
}

void task(void*) {
    int64_t next_try = 0, backoff_us = 5000000, next_snapshot = 0;
    apply();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        int64_t now = esp_timer_get_time();
        if (!s_active) {
            s_state_txt = s_enabled ? "not configured" : "off";
            continue;
        }

        // Event: freeze the pre-trigger window and flush what is buffered.
        if (s_event_pending) {
            can::Record ev;
            int64_t ev_us;
            portENTER_CRITICAL(&s_mux);
            ev = s_event_rec;
            ev_us = s_event_us;
            s_event_pending = false;
            Slot* f = find(S_FILLING);
            if (f && f->n) seal(f);
            portEXIT_CRITICAL(&s_mux);
            build_event_batch(ev, ev_us);
            next_try = 0;  // upload at once
        }

        // Change-only: full snapshot of every ID once a minute.
        if (s_change_only && now >= next_snapshot) {
            next_snapshot = now + 60000000;
            can::Record snap[16];
            for (size_t start = 0;; start += 16) {
                size_t n = can::stats_latest(snap, start, 16);
                for (size_t i = 0; i < n; i++) push(snap[i]);
                if (n < 16) break;
            }
        }

        // Seal by age.
        portENTER_CRITICAL(&s_mux);
        Slot* f = find(S_FILLING);
        if (f && f->n && now - f->opened_us >= (int64_t)s_flush_s * 1000000) seal(f);
        portEXIT_CRITICAL(&s_mux);

        if (now < next_try) {
            s_state_txt = "retrying";
            continue;
        }

        // The event batch goes first.
        if (s_ev_ready) {
            s_state_txt = "uploading";
            if (post(s_ev_hdr, s_ev_buf, s_ev_n)) {
                ESP_LOGI(TAG, "event batch seq %lu uploaded", (unsigned long)s_ev_hdr.seq);
                free(s_ev_buf);
                s_ev_buf = nullptr;
                s_ev_n = 0;
                s_ev_ready = false;
                backoff_us = 5000000;
            } else {
                ESP_LOGW(TAG, "event batch upload failed: %s (retry in %d s)", s_last_error,
                         (int)(backoff_us / 1000000));
                next_try = now + backoff_us;
                backoff_us = std::min<int64_t>(backoff_us * 2, 60000000);
                continue;
            }
        }

        portENTER_CRITICAL(&s_mux);
        Slot* s = oldest_sealed();
        if (s) s->state = S_UPLOADING;
        portEXIT_CRITICAL(&s_mux);
        if (!s) {
            s_state_txt = "idle";
            continue;
        }
        s_state_txt = "uploading";
        BatchHeader h;
        fill_header(h, s->flags, s->seq, s->n, s->dropped);
        bool ok = post(h, s->recs, s->n);
        portENTER_CRITICAL(&s_mux);
        s->state = ok ? S_FREE : S_SEALED;
        portEXIT_CRITICAL(&s_mux);
        if (ok) {
            if (h.dropped)
                ESP_LOGW(TAG, "batch seq %lu uploaded; reported %lu dropped frames",
                         (unsigned long)h.seq, (unsigned long)h.dropped);
            backoff_us = 5000000;
            xTaskNotifyGive(s_task);  // more may be waiting
        } else {
            ESP_LOGW(TAG, "batch seq %lu upload failed: %s (retry in %d s)", (unsigned long)h.seq,
                     s_last_error, (int)(backoff_us / 1000000));
            next_try = now + backoff_us;
            backoff_us = std::min<int64_t>(backoff_us * 2, 60000000);
        }
    }
}

void raise_event(const can::Record& r, const char* why) {
    portENTER_CRITICAL(&s_mux);
    s_event_rec = r;
    s_event_us = r.ts_us ? (int64_t)r.ts_us : esp_timer_get_time();
    s_last_event_us = esp_timer_get_time();
    s_event_pending = true;
    s_events++;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "EVENT (%s): id 0x%lX data %02X %02X %02X %02X %02X %02X %02X %02X", why,
             (unsigned long)(r.id & can::kIdMask), r.data[0], r.data[1], r.data[2], r.data[3],
             r.data[4], r.data[5], r.data[6], r.data[7]);
    if (s_task) xTaskNotifyGive(s_task);
}

}  // namespace

void start() {
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    s_boot_id = esp_random();
    xTaskCreate(task, "diag", 8192, nullptr, 2, &s_task);  // 8 KB: room for a TLS handshake  // low priority (SPEC.md)
}

void apply_settings() {
    if (s_task) {
        apply();
        xTaskNotifyGive(s_task);
    }
}

void on_frame(const can::Record& r, bool changed) {
    // Trigger: frame with the trigger ID that differs from the baseline. It
    // latches until the frame returns to the baseline (or is cleared).
    if (s_trig_enabled && !(r.id & can::kIdRtr) && (r.id & can::kIdMask) == s_trig_id) {
        bool differs = r.dlc != s_trig_len || memcmp(r.data, s_trig_base, s_trig_len) != 0;
        if (differs && !s_latched) {
            s_latched = true;
            raise_event(r, "frame left its baseline");
        } else if (!differs) {
            s_latched = false;
        }
    }
    if (!s_active) return;
    if (s_change_only && !changed) return;
    push(r);
}

void test_event() {
    can::Record r = {};
    uint32_t seq = can::ring_head() ? can::ring_head() - 1 : 0;
    uint32_t d = 0;
    if (!can::ring_read(seq, &r, 1, &d)) memset(&r, 0, sizeof(r));
    r.ts_us = esp_timer_get_time();
    raise_event(r, "manual test");
}

void clear_trigger() { s_latched = false; }

Status status() {
    Status st = {};
    char err[sizeof(s_last_error)];
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    st.enabled = s_enabled;
    st.active = s_active;
    st.state = s_state_txt;
    st.batches_ok = s_ok;
    st.batches_failed = s_failed;
    st.last_http_status = s_last_http;
    memcpy(err, s_last_error, sizeof(err));
    st.last_ok_age_s = s_last_ok_us ? (now - s_last_ok_us) / 1000000 : -1;
    for (auto& s : s_slots) {
        if (s.state == S_SEALED || s.state == S_UPLOADING) st.pending_batches++;
        if (s.state != S_FREE) st.pending_frames += s.n;
    }
    if (s_ev_ready) st.pending_batches++;
    st.dropped_total = s_dropped_total;
    st.events_total = s_events;
    st.next_seq = s_next_seq;
    st.boot_id = s_boot_id;
    st.trigger_latched = s_latched;
    st.last_event_age_s = s_last_event_us ? (now - s_last_event_us) / 1000000 : -1;
    st.last_event_id = s_event_rec.id & can::kIdMask;
    memcpy(st.last_event_data, s_event_rec.data, 8);
    st.last_event_dlc = s_event_rec.dlc;
    portEXIT_CRITICAL(&s_mux);
    st.last_error = err;
    return st;
}

}  // namespace diag
