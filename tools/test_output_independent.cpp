// Proves that the inverter output (agg::tx_frames, what port INV sends) does not depend on the
// reporting snapshot used by the web UI and MQTT, and survives the reporting path failing.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I tools/hoststub -I components/agg/include
//       -I components/can/include -I components/config/include -I components/webserver/src
//       tools/test_output_independent.cpp components/agg/src/glue.cpp components/agg/src/engine.cpp
//       components/webserver/src/agg_status_json.cpp -o /tmp/toi
//   /tmp/toi [frames.csv ...]       (inputs: tools/test_replay_exports.py --write-input DIR)
//
// The REAL firmware glue (components/agg/src/glue.cpp, with host stubs for FreeRTOS/NVS/timer)
// is fed the frames and ticked every 100 ms. A second, independent agg::Engine gets exactly the
// same frames and ticks and is never touched by any reporting code. Every 2 s (the web page's
// poll) the reporting path runs in one of these ways:
//   normal status reply, allocation failure (no snapshot), empty (zeroed) snapshot rendered,
//   client gone mid-reply, MQTT-style persistent copy taken and then overwritten with junk.
// Checks, at every 100 ms tick: tx_frames() == the reference engine's frames (byte for byte,
// same silent/sending decision). Also: the control path (on_can_frame, tick, tx_frames) makes
// no heap allocation at all (operator new is armed to fail and counted while it runs), so heap
// exhaustion cannot reach it. Without input files a built-in synthetic two-battery scenario
// (incl. an over-current guard episode and a stale source) is used.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>
#include <string>
#include <vector>

#include "agg/agg.h"
#include "can/pins.h"
#include "agg_status_json.h"
#include "config/config.h"

int64_t g_hoststub_now_us = 0;
int g_hoststub_log = 0;

namespace cfg {
Settings get() {
    Settings s;
    s.agg_src_a = s.agg_src_b = true;
    s.agg_fs_cvl_dv = 530;
    s.agg_fs_dvl_dv = 470;
    s.agg_ccl_max = 100;
    s.agg_dcl_max = 100;
    return s;  // caps 100/100/100, hold 10 s, CVL rise hold 60 s, KNY 300 Ah (defaults)
}
}  // namespace cfg

// ---- heap guard: the control path must not allocate ----
static bool g_control = false;
static long g_control_allocs = 0, g_report_allocs = 0;
static bool g_fail_report_alloc = false;
void* operator new(size_t n) {
    if (g_control) g_control_allocs++;
    else g_report_allocs++;
    void* p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    if (g_control) {
        g_control_allocs++;
        return nullptr;
    }
    if (g_fail_report_alloc) return nullptr;
    g_report_allocs++;
    return malloc(n ? n : 1);
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static int g_fail = 0;
static void check(bool c, const char* m) {
    printf("  %s %s\n", c ? "PASS" : "FAIL", m);
    if (!c) g_fail++;
}

struct In {
    uint64_t t_ms;
    int src;
    uint32_t id;
    uint8_t dlc, d[8];
};

static agg::Config ref_config() {
    agg::Config c;  // mirrors cfg::get() above through glue.cpp's from_settings()
    c.enabled[agg::kSrcBN] = c.enabled[agg::kSrcKNY] = true;
    c.kny_capacity_ah = 300;
    c.failsafe_cvl = 53.0f;
    c.failsafe_dvl = 47.0f;
    c.ccl_max = c.dcl_max = 100;
    c.silent_failsafe = false;
    c.startup_hold_ms = 10000;
    c.cvl_rise_hold_ms = 60000;
    c.cap_a = c.cap_a_p2 = c.cap_b = 100;
    return c;
}

// Reporting sinks.
static bool sink_str(void* ctx, const char* d, size_t n) {
    ((std::string*)ctx)->append(d, n);
    return true;
}
static int g_sink_budget = 0;
static bool sink_fail_after(void*, const char*, size_t n) {
    g_sink_budget -= (int)n;
    return g_sink_budget > 0;
}

static agg::Snapshot* g_mqtt_copy = nullptr;  // like hamqtt's persistent s_snap
static long g_mode_count[5] = {};

static void report(int mode) {
    g_mode_count[mode]++;
    webserver::AggStatusExtras x;
    switch (mode) {
        case 0: {  // normal web reply
            std::unique_ptr<webserver::AggStatusData> d(new (std::nothrow) webserver::AggStatusData);
            if (!d) abort();
            agg::snapshot(d->snap);
            d->nev = agg::events_since(0, d->ev, agg::kEventRing);
            std::string s;
            JsonOut j(sink_str, &s);
            if (!webserver::write_agg_status(j, *d, x, (uint64_t)(g_hoststub_now_us / 1000))) abort();
            break;
        }
        case 1: {  // allocation failure: the handler answers 503 without a snapshot
            g_fail_report_alloc = true;
            std::unique_ptr<webserver::AggStatusData> d(new (std::nothrow) webserver::AggStatusData);
            g_fail_report_alloc = false;
            if (d) abort();
            break;
        }
        case 2: {  // empty snapshot rendered (all zero)
            std::unique_ptr<webserver::AggStatusData> d(new (std::nothrow) webserver::AggStatusData);
            memset((void*)&d->snap, 0, sizeof(d->snap));
            d->nev = 0;
            std::string s;
            JsonOut j(sink_str, &s);
            webserver::write_agg_status(j, *d, x, 0);
            break;
        }
        case 3: {  // client gone after ~1 KB
            std::unique_ptr<webserver::AggStatusData> d(new (std::nothrow) webserver::AggStatusData);
            agg::snapshot(d->snap);
            g_sink_budget = 1000;
            JsonOut j(sink_fail_after, nullptr);
            if (webserver::write_agg_status(j, *d, x, (uint64_t)(g_hoststub_now_us / 1000))) abort();
            break;
        }
        case 4: {  // MQTT copy, then the copy is overwritten (as a corrupted reporter would)
            if (!g_mqtt_copy) g_mqtt_copy = new agg::Snapshot;
            agg::snapshot(*g_mqtt_copy);
            memset((void*)g_mqtt_copy, 0xA5, sizeof(*g_mqtt_copy));
            break;
        }
    }
}

struct Result {
    long ticks = 0, compared_send = 0, compared_silent = 0, mismatches = 0, nonzero_ccl = 0, guard_ticks = 0;
};

static Result run(const std::vector<In>& in, bool with_reports) {
    Result r;
    uint64_t t0 = in.front().t_ms;
    g_hoststub_now_us = (int64_t)t0 * 1000;
    agg::start();  // glue: configure at now
    agg::Engine* ref = new agg::Engine;
    ref->configure(ref_config(), t0);
    uint64_t next_tick = t0 + 100, next_report = t0 + 2000;
    int mode = 0;
    size_t i = 0;
    uint64_t t_end = in.back().t_ms + 20000;  // run on 20 s after the last frame (stale path)
    while (next_tick <= t_end) {
        // frames up to this tick
        while (i < in.size() && in[i].t_ms < next_tick) {
            const In& f = in[i++];
            can::Record rec = {};
            rec.ts_us = f.t_ms * 1000;
            rec.id = f.id;
            rec.dlc = f.dlc;
            rec.port = f.src == 0 ? BMS_CAN_PORT_A : BMS_CAN_PORT_B;
            memcpy(rec.data, f.d, 8);
            g_hoststub_now_us = (int64_t)f.t_ms * 1000;
            g_control = true;
            agg::on_can_frame(rec);
            g_control = false;
            ref->on_frame(f.src, f.id, f.d, f.dlc, f.t_ms);
        }
        g_hoststub_now_us = (int64_t)next_tick * 1000;
        g_control = true;
        agg::tick();
        g_control = false;
        ref->tick(next_tick);
        // what port INV would send now
        can::TxFrame tx[8];
        int n = 0;
        g_control = true;
        bool send = agg::tx_frames(tx, 8, &n);
        g_control = false;
        const agg::Output& o = ref->output();
        bool ok = send == o.send;
        if (ok && send) {
            ok = n == agg::kOutFrames;
            for (int k = 0; ok && k < n; k++)
                ok = tx[k].id == o.frames[k].id && tx[k].dlc == o.frames[k].dlc &&
                     memcmp(tx[k].data, o.frames[k].data, 8) == 0;
        }
        if (!ok && r.mismatches++ < 5)
            printf("    mismatch at t=%llu ms: glue send=%d n=%d, reference send=%d\n",
                   (unsigned long long)(next_tick - t0), send, n, o.send);
        r.ticks++;
        if (send) r.compared_send++;
        else r.compared_silent++;
        if (send && o.ccl > 0) r.nonzero_ccl++;
        if (getenv("TOI_DEBUG") && (next_tick - t0) % 30000 == 0)
            printf("    t=%llus %s (%s) ccl %.1f dcl %.1f | A %s (%s) | B %s (%s)\n", (unsigned long long)(next_tick - t0) / 1000,
                   agg::out_state_name(o.state), o.reason, o.ccl, o.dcl, agg::src_state_name(ref->source(0).state),
                   ref->source(0).reason, agg::src_state_name(ref->source(1).state), ref->source(1).reason);
        if (o.chg_factor < 1 || o.dis_factor < 1) r.guard_ticks++;
        if (with_reports && next_tick >= next_report) {
            next_report += 2000;
            report(mode);
            mode = (mode + 1) % 5;
        }
        next_tick += 100;
    }
    delete ref;
    return r;
}

static bool load(const char* path, std::vector<In>& v) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    if (!fgets(line, sizeof(line), f)) return false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long t;
        int src, dlc;
        unsigned id;
        char hx[40];
        long seg;
        if (sscanf(line, "%llu,%d,%X,%d,%39[^,],%ld", &t, &src, &id, &dlc, hx, &seg) != 6) continue;
        In x = {};
        x.t_ms = t;
        x.src = src;
        x.id = id;
        x.dlc = (uint8_t)(dlc > 8 ? 8 : dlc);
        for (int k = 0; k < x.dlc && hx[2 * k] && hx[2 * k + 1]; k++) {
            unsigned b;
            sscanf(hx + 2 * k, "%2X", &b);
            x.d[k] = (uint8_t)b;
        }
        v.push_back(x);
    }
    fclose(f);
    return !v.empty();
}

// Synthetic (same encodings as tools/test_agg.cpp): both batteries at 1 Hz (BN pack 2 every 2 s)
// for 10 min; the KNY discharges 160 A against its DCL 150 / cap 100 for 60 s (over-current
// guard), then goes silent for 30 s (stale -> fail-safe), then returns (recovery hold).
static void synthetic(std::vector<In>& v) {
    auto put = [&](uint64_t t, int src, uint32_t id, const uint8_t* d, uint8_t dlc) {
        In x = {};
        x.t_ms = t, x.src = src, x.id = id, x.dlc = dlc;
        memcpy(x.d, d, dlc);
        v.push_back(x);
    };
    auto p16 = [](uint8_t* d, int k, int x) { d[k] = (uint8_t)(x & 0xFF), d[k + 1] = (uint8_t)((x >> 8) & 0xFF); };
    auto pbe = [](uint8_t* d, int k, int x) { d[k] = (uint8_t)((x >> 8) & 0xFF), d[k + 1] = (uint8_t)(x & 0xFF); };
    for (uint64_t s = 0; s < 600; s++) {
        uint64_t t = 1000000 + s * 1000;
        uint8_t d[8];
        // Blue Nova bank: CVL 55.2 CCL 100 DCL 200 DVL 48, SOC 60, 52.5 V, -30 A
        memset(d, 0, 8), p16(d, 0, 552), p16(d, 2, 1000), p16(d, 4, 2000), p16(d, 6, 480), put(t, 0, 0x351, d, 8);
        memset(d, 0, 8), p16(d, 0, 60), p16(d, 2, 99), p16(d, 4, 6000), put(t, 0, 0x355, d, 8);
        memset(d, 0, 8), p16(d, 0, 5250), p16(d, 2, -300), p16(d, 4, 250), put(t, 0, 0x356, d, 8);
        const uint8_t a35[8] = {0xA8, 0xAA, 0x82, 0x02, 0xA8, 0xAA, 0x02, 0x02};
        put(t, 0, 0x35A, a35, 8);
        const uint8_t z[4] = {0, 0, 0, 7};
        put(t, 0, 0x35F, z, 4);
        memset(d, 0, 8), p16(d, 0, 3300), p16(d, 2, 3310), p16(d, 4, 298), p16(d, 6, 299), put(t, 0, 0x373, d, 8);
        const uint8_t loc[8] = {'0', '1', '-', '-', '0', '9', 0, 0};
        for (uint32_t id = 0x374; id <= 0x377; id++) put(t, 0, id, loc, 8);
        memset(d, 0, 8), p16(d, 0, 30000), p16(d, 4, 29000), put(t, 0, 0x378, d, 8);
        memset(d, 0, 8), p16(d, 0, 200), put(t, 0, 0x379, d, 8);
        if (s % 2 == 0) {
            uint8_t q[8] = {0x02};
            pbe(q, 1, 5250), pbe(q, 3, -1500), q[5] = 60, q[6] = 99, put(t + 5, 0, 0x211, q, 8);
            for (uint32_t id = 0x212; id <= 0x217; id++) {
                uint8_t x[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};
                if (id == 0x215) pbe(x, 5, 500);
                if (id == 0x216) pbe(x, 5, 1000);
                put(t + 5, 0, id, x, 8);
            }
        }
        if (s >= 400 && s < 430) continue;  // KNY silent
        int ib = s >= 200 && s < 260 ? -1600 : -300;
        memset(d, 0, 8), p16(d, 0, 560), p16(d, 2, 1500), p16(d, 4, 1500), p16(d, 6, 460), put(t + 10, 1, 0x351, d, 8);
        memset(d, 0, 8), p16(d, 0, 40), p16(d, 2, 100), put(t + 10, 1, 0x355, d, 4);
        memset(d, 0, 8), p16(d, 0, 5240), p16(d, 2, ib), p16(d, 4, 240), put(t + 10, 1, 0x356, d, 6);
        const uint8_t f359[7] = {0, 0, 0, 0, 1, 'P', 'N'};
        put(t + 10, 1, 0x359, f359, 7);
        const uint8_t fl[2] = {0xC0, 0};
        put(t + 10, 1, 0x35C, fl, 2);
    }
}

int main(int argc, char** argv) {
    std::vector<std::pair<std::string, std::vector<In>>> sets;
    for (int a = 1; a < argc; a++) {
        std::vector<In> v;
        if (!load(argv[a], v)) {
            printf("cannot read %s\n", argv[a]);
            return 2;
        }
        sets.push_back({argv[a], v});
    }
    if (sets.empty()) {
        std::vector<In> v;
        synthetic(v);
        sets.push_back({"synthetic (2 batteries, guard episode, KNY silent 30 s)", v});
    }
    for (auto& s : sets) {
        printf("%s: %zu frames\n", s.first.c_str(), s.second.size());
        g_control_allocs = 0;
        Result r = run(s.second, true);
        printf("    %ld ticks: %ld sending, %ld silent, %ld with CCL > 0, %ld with the guard active; "
               "reports per mode %ld/%ld/%ld/%ld/%ld\n",
               r.ticks, r.compared_send, r.compared_silent, r.nonzero_ccl, r.guard_ticks, g_mode_count[0],
               g_mode_count[1], g_mode_count[2], g_mode_count[3], g_mode_count[4]);
        check(r.mismatches == 0, "tx_frames() equals the independent reference engine at every 100 ms tick, "
                                 "while the status snapshot succeeds, fails to allocate, is empty, is cut off "
                                 "or its copy is overwritten");
        check(r.compared_send > 0 && r.nonzero_ccl > 0, "the comparison covers real sending output (not only silence)");
        check(g_control_allocs == 0, "the control path (frames, tick, tx_frames) made no heap allocation");
        // Restarting the glue engine for the next set: start() reconfigures the same engine.
    }
    printf("\n%s\n", g_fail ? "FAILED" : "all output-independence checks passed");
    return g_fail ? 1 : 0;
}
