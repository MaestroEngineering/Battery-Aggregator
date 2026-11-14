// Host test of GET /api/agg/status as the firmware writes it (components/webserver/src/
// agg_status_json.cpp + json_stream.h): valid JSON, the fields the Aggregator page reads,
// events newest first, escaping of BMS-supplied strings, 256-byte chunks, a client that goes
// away mid-reply, and the heap one request needs.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I tools/hoststub -I components/agg/include
//       -I components/can/include -I components/webserver/src -I managed_components/espressif__cjson/cJSON
//       -I components/config/include tools/test_status_json.cpp components/webserver/src/agg_status_json.cpp
//       components/webserver/src/can_status_json.cpp components/webserver/src/settings_json.cpp components/agg/src/engine.cpp
//       managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/tsj && /tmp/tsj
//
// 0.1.7 built a cJSON tree for this reply: ~49 KB of heap per request in the normal state with
// real data (measured with the real cJSON on the 13/14 Nov exports), more than the device had
// (~36 KB free, largest block ~34 KB, with Verbose Diagnostics' 36 KB of batch buffers allocated),
// so every request failed with HTTP 500 once the start-up hold ended.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <new>
#include <string>

#include "agg_status_json.h"
#include "cJSON.h"
#include "can_status_json.h"
#include "settings_json.h"

// can.h declares these; the firmware defines them in tap.cpp.
namespace can {
const char* state_name(State) { return "listen-only"; }
const char* intmap_result_name(IntMapResult) { return "ok"; }
}  // namespace can

// cJSON heap model for the settings tree (with custom hooks cJSON has no realloc, so printing
// grows by malloc + copy + free: the worst case for the peak).
static size_t h_cur = 0, h_peak = 0, h_big = 0;
static std::string g_sizes;
static void* h_malloc(size_t n) {
    size_t* p = (size_t*)malloc(n + sizeof(size_t));
    *p = n;
    h_cur += n + 8;  // + allocator overhead per block (TLSF header and alignment, rounded up)
    if (h_cur > h_peak) h_peak = h_cur;
    if (n > h_big) h_big = n;
    return p + 1;
}
static void h_free(void* q) {
    if (!q) return;
    size_t* p = (size_t*)q - 1;
    h_cur -= *p + 8;
    free(p);
}

static long g_allocs = 0;
static size_t g_bytes = 0;
void* operator new(size_t n) {
    g_allocs++, g_bytes += n;
    return malloc(n ? n : 1);
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    g_allocs++, g_bytes += n;
    return malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static int g_fail = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        printf("  %s ", (c) ? "PASS" : "FAIL"); \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
        if (!(c)) g_fail++;                   \
    } while (0)

static size_t g_max_chunk = 0;
static bool sink(void* ctx, const char* d, size_t n) {
    if (n > g_max_chunk) g_max_chunk = n;
    ((std::string*)ctx)->append(d, n);
    return true;
}
static long g_budget = 0, g_calls_after_fail = 0;
static bool g_failed = false;
static bool sink_fail(void*, const char*, size_t n) {
    if (g_failed) g_calls_after_fail++;
    g_budget -= (long)n;
    if (g_budget <= 0) g_failed = true;
    return !g_failed;
}

static void put16(uint8_t* d, int k, int v) { d[k] = (uint8_t)(v & 0xFF), d[k + 1] = (uint8_t)((v >> 8) & 0xFF); }

// A running engine: both sources normal, BN with pack 2, KNY with a name full of odd bytes.
static void feed(agg::Engine& e, uint64_t& t, int seconds, const char* kny_name) {
    for (int s = 0; s < seconds; s++) {
        t += 1000;
        uint8_t d[8];
        memset(d, 0, 8), put16(d, 0, 552), put16(d, 2, 1000), put16(d, 4, 2000), put16(d, 6, 480);
        e.on_frame(0, 0x351, d, 8, t);
        memset(d, 0, 8), put16(d, 0, 60), put16(d, 2, 99), put16(d, 4, 6000);
        e.on_frame(0, 0x355, d, 8, t);
        memset(d, 0, 8), put16(d, 0, 5250), put16(d, 2, -300), put16(d, 4, 250);
        e.on_frame(0, 0x356, d, 8, t);
        const uint8_t a35[8] = {0xA8, 0xAA, 0x82, 0x02, 0xA8, 0xAA, 0x02, 0x02};
        e.on_frame(0, 0x35A, a35, 8, t);
        memset(d, 0, 8), put16(d, 0, 200);
        e.on_frame(0, 0x379, d, 8, t);
        const uint8_t z[4] = {0, 0, 0, 7};
        e.on_frame(0, 0x35F, z, 4, t);
        memset(d, 0, 8), put16(d, 0, 3300), put16(d, 2, 3310), put16(d, 4, 298), put16(d, 6, 299);
        e.on_frame(0, 0x373, d, 8, t);
        const uint8_t loc[8] = {'0', '1', '-', '-', '0', '9', 0, 0};
        for (uint32_t id = 0x374; id <= 0x377; id++) e.on_frame(0, id, loc, 8, t);
        memset(d, 0, 8), put16(d, 0, 30000), put16(d, 4, 29000);
        e.on_frame(0, 0x378, d, 8, t);
        if (s % 2 == 0) {
            uint8_t q[8] = {0x02, 0x14, 0x82, 0xFA, 0x24, 60, 99, 0};
            e.on_frame(0, 0x211, q, 8, t);
            for (uint32_t id = 0x212; id <= 0x217; id++) {
                uint8_t x[8] = {0x02, 0, 0, 0, 0, 0x01, 0xF4, 0};
                e.on_frame(0, id, x, 8, t);
            }
        }
        memset(d, 0, 8), put16(d, 0, 560), put16(d, 2, 1500), put16(d, 4, 1500), put16(d, 6, 460);
        e.on_frame(1, 0x351, d, 8, t);
        memset(d, 0, 8), put16(d, 0, 40), put16(d, 2, 100);
        e.on_frame(1, 0x355, d, 4, t);
        memset(d, 0, 8), put16(d, 0, 5240), put16(d, 2, -300), put16(d, 4, 240);
        e.on_frame(1, 0x356, d, 6, t);
        const uint8_t f359[7] = {0, 0, 0, 0, 1, 'P', 'N'};
        e.on_frame(1, 0x359, f359, 7, t);
        const uint8_t fl[2] = {0xC0, 0};
        e.on_frame(1, 0x35C, fl, 2, t);
        uint8_t nm[8] = {};
        memcpy(nm, kny_name, strnlen(kny_name, 8));
        e.on_frame(1, 0x35E, nm, 8, t);
        for (int k = 0; k < 10; k++) e.tick(t + k * 100);
    }
}

int main() {
    agg::Config c;
    c.enabled[0] = c.enabled[1] = true;
    c.failsafe_cvl = 53, c.failsafe_dvl = 47, c.ccl_max = c.dcl_max = 100, c.kny_capacity_ah = 300;
    agg::Engine e;
    uint64_t t = 100000;
    e.configure(c, t);
    const char odd[8] = {'K', '"', '\\', 0x01, (char)0xC3, (char)0xFF, '<', 'Y'};  // quote, backslash, control, high bytes
    feed(e, t, 60, odd);
    const bool name_clean = strcmp(e.source(1).name, "K\"\\   <Y") == 0;

    printf("status reply\n");
    long a0 = g_allocs;
    size_t b0 = g_bytes;
    std::unique_ptr<webserver::AggStatusData> d(new (std::nothrow) webserver::AggStatusData);
    long allocs = g_allocs - a0;
    size_t bytes = g_bytes - b0;
    d->snap.out = e.output();
    for (int i = 0; i < agg::kSources; i++) d->snap.src[i] = e.source(i);
    d->snap.cfg = e.config();
    d->nev = e.events_since(0, d->ev, agg::kEventRing);
    webserver::AggStatusExtras x;
    x.inv_state = "transmitting", x.mqtt_state = "connected", x.epoch_offset_us = 1794500000000000LL;
    // Raw bytes as a corrupted frame or event text could carry them (the engine itself already
    // replaces non-printable name bytes with spaces).
    memcpy(d->snap.src[1].name, odd, 8);
    d->snap.src[1].name[8] = 0;
    std::string out;
    out.reserve(16384);
    a0 = g_allocs;
    JsonOut j(sink, &out);
    bool ok = webserver::write_agg_status(j, *d, x, t + 1000);
    long w_allocs = g_allocs - a0;
    CHECK(ok && out.size() == j.bytes(), "written, %zu bytes", out.size());
    CHECK(allocs == 1 && bytes == sizeof(webserver::AggStatusData) && w_allocs == 0,
          "heap per request: %ld allocation of %zu bytes (snapshot + events; ~5.1 KB on the ESP32), none while "
          "writing (0.1.7: ~49 KB)", allocs, bytes);
    CHECK(g_max_chunk <= 256, "sent in chunks of at most %zu bytes", g_max_chunk);

    cJSON* r = cJSON_Parse(out.c_str());
    CHECK(r != nullptr, "valid JSON");
    if (!r) return 1;
    auto num = [&](cJSON* o, const char* k) { cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsNumber(v) ? v->valuedouble : NAN; };
    auto str = [&](cJSON* o, const char* k) { cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? std::string(v->valuestring) : std::string("<missing>"); };
    cJSON* o = cJSON_GetObjectItemCaseSensitive(r, "out");
    CHECK(str(r, "state") == "normal", "state %s (%s)", str(r, "state").c_str(), str(r, "reason").c_str());
    CHECK(fabs(num(o, "ccl") - e.output().ccl) < 1e-4 && fabs(num(o, "cvl") - e.output().cvl) < 1e-4 &&
              fabs(num(o, "voltage") - e.output().voltage) < 1e-4,
          "out.ccl %.1f / cvl %.2f / voltage %.2f equal the engine", num(o, "ccl"), num(o, "cvl"), num(o, "voltage"));
    cJSON* src = cJSON_GetObjectItemCaseSensitive(r, "sources");
    cJSON* sa = cJSON_GetArrayItem(src, 0);
    cJSON* sb = cJSON_GetArrayItem(src, 1);
    CHECK(cJSON_GetArraySize(src) == 2 && str(sa, "port") == "A" && str(sb, "port") == "B", "sources A and B (column B)");
    CHECK(cJSON_GetObjectItemCaseSensitive(sa, "pack2") && cJSON_GetObjectItemCaseSensitive(sa, "pack1"),
          "A has pack1 and pack2 objects");
    CHECK(!std::isnan(num(sb, "malformed_frames")) && !std::isnan(num(sb, "rejected_frames")) &&
              !std::isnan(num(sb, "soc")),
          "B has soc, rejected_frames, malformed_frames");
    CHECK(name_clean, "engine stores the KNY name with non-printable bytes as spaces ('%s')", e.source(1).name);
    std::string nm = str(sb, "name");
    std::string want = std::string("K\"\\\x01") + "\xC3\x83" + "\xC3\xBF" + "<Y";  // high bytes as U+00C3 / U+00FF
    CHECK(nm == want, "BMS name with quote, backslash, control and high bytes escaped (got %zu bytes)", nm.size());
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "guard")) == agg::kGuards, "guard rows: 3");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r, "frames")) == agg::kOutFrames, "output frames: 6");
    cJSON* ev = cJSON_GetObjectItemCaseSensitive(r, "events");
    int ne = cJSON_GetArraySize(ev);
    bool desc = ne == (int)d->nev && ne > 0;
    for (int k = 1; k < ne; k++) desc &= num(cJSON_GetArrayItem(ev, k - 1), "seq") > num(cJSON_GetArrayItem(ev, k), "seq");
    CHECK(desc, "%d events, newest first", ne);
    CHECK(str(cJSON_GetObjectItemCaseSensitive(r, "mqtt"), "state") == "connected", "mqtt object");
    cJSON_Delete(r);

    printf("empty snapshot (all zero)\n");
    memset((void*)&d->snap, 0, sizeof(d->snap));
    d->nev = 0;
    std::string e0;
    JsonOut j0(sink, &e0);
    CHECK(webserver::write_agg_status(j0, *d, x, 0) && cJSON_Parse(e0.c_str()) != nullptr, "still valid JSON");

    printf("client gone mid-reply\n");
    g_budget = 1000;
    JsonOut j1(sink_fail, nullptr);
    d->snap.out = e.output();
    bool r1 = webserver::write_agg_status(j1, *d, x, t);
    CHECK(!r1 && g_calls_after_fail == 0, "returns false, no write after the failed one");

    printf("numbers\n");
    std::string nb;
    JsonOut jn(sink, &nb);
    jn.begin();
    jn.num("a", 53.4f), jn.num("b", 1e300 * 1e10), jn.num("c", -0.5), jn.num("d", 4294967295.0), jn.num("e", NAN);
    jn.end();
    CHECK(nb == "{\"a\":53.4,\"b\":null,\"c\":-0.5,\"d\":4294967295,\"e\":null}", "%s", nb.c_str());

    printf("CAN status (streamed)\n");
    {
        can::Status cs;
        cs.enabled = true, cs.bitrate = 500000, cs.frames_total = 123456789ULL, cs.error = "x";
        cs.detect_count = 3, cs.detect_note = "running \"quoted\"";
        cs.probe_done = true;
        cs.intmap_done = true, cs.intmap_ncand = 4, cs.intmap_port = 0;
        for (auto& p : cs.intmap) p.rounds = can::kIntMapRounds;
        std::string co;
        co.reserve(16384);
        long ca = g_allocs;
        JsonOut jc(sink, &co);
        bool okc = webserver::write_can_status(jc, cs, 1536, 77);
        long c_allocs = g_allocs - ca;
        cJSON* cr = cJSON_Parse(co.c_str());
        CHECK(okc && cr && c_allocs == 0, "valid JSON, %zu bytes, no heap allocation while writing", co.size());
        cJSON* ports = cJSON_GetObjectItemCaseSensitive(cr, "ports");
        CHECK(cJSON_GetArraySize(ports) == 3 && num(cr, "frames") == 123456789.0 && num(cr, "ring_used") == 77,
              "3 ports, 64-bit frame counter, ring fill");
        cJSON* im = cJSON_GetObjectItemCaseSensitive(cr, "intmap");
        CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(im, "candidates")) == 4 &&
                  cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(im, "ports"), 0), "per_round")) ==
                      can::kIntMapRounds,
              "INT map candidates and per-round arrays");
        cJSON_Delete(cr);
    }

    printf("settings (cJSON tree, page load / export only)\n");
    {
        cfg::Settings st;
        st.hostname = std::string(32, 'h');
        st.ntp_server = std::string(64, 'n');
        st.timezone = std::string(64, 't');
        st.mqtt_host = std::string(64, 'm');
        st.mqtt_user = std::string(64, 'u');
        st.mqtt_base = std::string(48, 'b');
        st.diag_url = "http://" + std::string(193, 'd');
        st.trig_baseline = "0011223344556677";
        webserver::SettingsExtras sx;
        std::string ssid(32, 's');
        sx.firmware = "0.1.8", sx.exported_at = "2026-11-14T16:00:00+02:00", sx.wifi_ssid = ssid.c_str();
        cJSON_Hooks hk = {h_malloc, h_free};
        cJSON_InitHooks(&hk);
        size_t worst = 0, worst_big = 0, worst_len = 0;
        for (int exp = 0; exp < 2; exp++) {
            h_cur = h_peak = h_big = 0;
            cJSON* t = webserver::settings_json_build(st, sx, exp == 1);
            char* txt = cJSON_PrintUnformatted(t);
            size_t len = txt ? strlen(txt) : 0;
            cJSON_Delete(t);
            cJSON_free(txt);
            CHECK(txt && h_cur == 0, "%s: %zu bytes of JSON, peak heap %zu bytes, largest block %zu bytes, all freed",
                  exp ? "export" : "GET /api/settings", len, h_peak, h_big);
            if (h_peak > worst) worst = h_peak, worst_big = h_big, worst_len = len;
        }
        cJSON_Hooks def = {malloc, free};
        cJSON_InitHooks(&def);
        CHECK(worst < 12 * 1024 && worst_big < 4096,
              "settings stay below 12 KB peak / 4 KB block with every string at its maximum (%zu / %zu)", worst,
              worst_big);
        (void)worst_len;
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all status JSON checks passed");
    return g_fail ? 1 : 0;
}
