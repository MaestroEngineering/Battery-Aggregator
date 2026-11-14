// Host simulator tests for the aggregation engine (components/agg).
//   g++ -std=c++17 -Wall -Wextra -Werror -I components/agg/include tools/test_agg.cpp
//       components/agg/src/engine.cpp -o /tmp/ta && /tmp/ta
// Simulates a Blue Nova (master + pack 2) on source A and a KNY (Pylontech V1.3) on
// source B in 100 ms steps, and checks the inverter output for every fail-safe path.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "agg/engine.h"

using namespace agg;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (cond) g_pass++;                                               \
        else {                                                            \
            g_fail++;                                                     \
            printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);      \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)
static bool near(float a, float b, float tol = 0.051f) { return fabsf(a - b) <= tol; }

struct Bn {
    float cvl = 55.2f, ccl = 100, dcl = 200, dvl = 48, soc = 60, soh = 99, v = 52.5f, i = 0, t = 25;
    uint16_t ah = 200;
    float p2_ccl = 50, p2_dcl = 100, p2_i = 0;
    bool send = true, send_p2 = true, send_35a = true;
    uint8_t a35[8] = {0xA8, 0xAA, 0x82, 0x02, 0xA8, 0xAA, 0x02, 0x02};
};
struct Kny {
    float cvl = 56.0f, ccl = 150, dcl = 150, dvl = 46, soc = 40, soh = 100, v = 52.4f, i = 0, t = 24;
    uint8_t b359[4] = {0, 0, 0, 0};
    uint8_t flags = 0xC0;  // charge + discharge enable
    bool send = true;
};

static void put16(uint8_t* d, int k, int v) { d[k] = (uint8_t)(v & 0xFF), d[k + 1] = (uint8_t)((v >> 8) & 0xFF); }
static void putbe16(uint8_t* d, int k, int v) { d[k] = (uint8_t)((v >> 8) & 0xFF), d[k + 1] = (uint8_t)(v & 0xFF); }

struct Sim {
    Engine e;
    uint64_t t = 1000;  // ms
    Bn bn;
    Kny kny;
    Config c;
    // Sources: (both = true) A + B; Sim(false) BN only; Sim(true, false) KNY only.
    Sim(bool kny_on = true, bool bn_on = true) {
        c.enabled[0] = bn_on;
        c.enabled[1] = kny_on;
        c.failsafe_cvl = 53.0f;
        c.failsafe_dvl = 47.0f;
        c.ccl_max = 400;
        c.dcl_max = 400;
        // Tests written before the per-source caps (0.1.7) assume uncapped sources; the cap tests
        // set the defaults explicitly (Sim::caps_default()).
        c.cap_a = c.cap_a_p2 = c.cap_b = 1000;
        e.configure(c, t);
    }
    void caps_default() {
        Config d;
        c.cap_a = d.cap_a, c.cap_a_p2 = d.cap_a_p2, c.cap_b = d.cap_b;
        reconf();
    }
    void reconf() { e.configure(c, t); }
    void f(int src, uint32_t id, const uint8_t* d, uint8_t dlc) { e.on_frame(src, id, d, dlc, t); }
    void bn_1hz() {
        if (!bn.send) return;
        uint8_t d[8] = {};
        put16(d, 0, (int)lrintf(bn.cvl * 10)), put16(d, 2, (int)lrintf(bn.ccl * 10)),
            put16(d, 4, (int)lrintf(bn.dcl * 10)), put16(d, 6, (int)lrintf(bn.dvl * 10));
        f(0, 0x351, d, 8);
        memset(d, 0, 8);
        put16(d, 0, (int)bn.soc), put16(d, 2, (int)bn.soh), put16(d, 4, (int)lrintf(bn.soc * 100));
        f(0, 0x355, d, 8);
        memset(d, 0, 8);
        put16(d, 0, (int)lrintf(bn.v * 100)), put16(d, 2, (int)lrintf(bn.i * 10)), put16(d, 4, (int)lrintf(bn.t * 10));
        f(0, 0x356, d, 8);
        if (bn.send_35a) f(0, 0x35A, bn.a35, 8);
        uint8_t z[8] = {0, 0, 0, 7, 0, 0, 0, 0};
        f(0, 0x35F, z, 4);
        memset(d, 0, 8);
        put16(d, 0, 3300), put16(d, 2, 3310), put16(d, 4, 298), put16(d, 6, 299);
        f(0, 0x373, d, 8);
        uint8_t loc[8] = {'0', '1', '-', '-', '0', '9', 0, 0};
        for (uint32_t id = 0x374; id <= 0x377; id++) f(0, id, loc, 8);
        memset(d, 0, 8);
        put16(d, 0, 30000), put16(d, 4, 29000);
        f(0, 0x378, d, 8);
        memset(d, 0, 8);
        put16(d, 0, bn.ah);
        f(0, 0x379, d, 8);
    }
    void bn_pack2() {
        if (!bn.send || !bn.send_p2) return;
        uint8_t d[8] = {0x02};
        putbe16(d, 1, (int)lrintf(bn.v * 100)), putbe16(d, 3, (int)lrintf(bn.p2_i * 100)), d[5] = (uint8_t)bn.soc,
                                                d[6] = 99;
        f(0, 0x211, d, 8);
        for (uint32_t id = 0x212; id <= 0x217; id++) {
            uint8_t x[8] = {0x02, 0, 0, 0, 0, 0, 0, 0};
            if (id == 0x215) putbe16(x, 5, (int)lrintf(bn.p2_ccl * 10));
            if (id == 0x216) putbe16(x, 5, (int)lrintf(bn.p2_dcl * 10));
            f(0, id, x, 8);
        }
    }
    void kny_1hz() {
        if (!kny.send || !c.enabled[1]) return;
        uint8_t d[8] = {};
        put16(d, 0, (int)lrintf(kny.cvl * 10)), put16(d, 2, (int)lrintf(kny.ccl * 10)),
            put16(d, 4, (int)lrintf(kny.dcl * 10)), put16(d, 6, (int)lrintf(kny.dvl * 10));
        f(1, 0x351, d, 8);
        memset(d, 0, 8);
        put16(d, 0, (int)kny.soc), put16(d, 2, (int)kny.soh);
        f(1, 0x355, d, 4);
        memset(d, 0, 8);
        put16(d, 0, (int)lrintf(kny.v * 100)), put16(d, 2, (int)lrintf(kny.i * 10)), put16(d, 4, (int)lrintf(kny.t * 10));
        f(1, 0x356, d, 6);
        uint8_t a[7] = {kny.b359[0], kny.b359[1], kny.b359[2], kny.b359[3], 1, 'P', 'N'};
        f(1, 0x359, a, 7);
        uint8_t fl[2] = {kny.flags, 0};
        f(1, 0x35C, fl, 2);
    }
    // Advance `ms` in 100 ms steps; 1 Hz frames on whole seconds, pack 2 every 2 s.
    void run(uint64_t ms) {
        for (uint64_t k = 0; k < ms; k += 100) {
            t += 100;
            if (t % 1000 == 0) {
                bn_1hz();
                kny_1hz();
            }
            if (t % 2000 == 0) bn_pack2();
            e.tick(t);
        }
    }
    // Runs to the next whole second, so the last 1 Hz frame was sent exactly "now".
    void align() { run((1000 - t % 1000) % 1000); }
    const Output& o() const { return e.output(); }
    bool has_event(EvType ty, const char* sub = nullptr) const {
        Event ev[kEventRing];
        size_t n = e.events_since(0, ev, kEventRing);
        for (size_t i = 0; i < n; i++)
            if (ev[i].type == ty && (!sub || strstr(ev[i].text, sub))) return true;
        return false;
    }
    void dump_events() const {
        Event ev[kEventRing];
        size_t n = e.events_since(0, ev, kEventRing);
        for (size_t i = 0; i < n; i++)
            printf("    %8.1f s %-12s %2d %s\n", ev[i].t_ms / 1000.0, ev_type_name(ev[i].type), ev[i].source, ev[i].text);
    }
};

static bool zero_limits(const Output& o) {
    const uint8_t* d = o.frames[0].data;
    return o.ccl == 0 && o.dcl == 0 && d[2] == 0 && d[3] == 0 && d[4] == 0 && d[5] == 0 &&
           (o.frames[4].data[0] & 0xC0) == 0;
}
static int frame_cvl_dV(const Output& o) { return o.frames[0].data[0] | o.frames[0].data[1] << 8; }
static uint32_t frame_alarm(const Output& o) {
    const uint8_t* d = o.frames[3].data;
    return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}

// Brings both sources to Normal (start-up hold 10 s + CVL rise hold 60 s settled).
static void settle(Sim& s) { s.run(75000); }

static void test_decode() {
    printf("decode: Blue Nova frames from the 30 Oct capture, KNY Pylontech layout\n");
    Sim s;
    // Real 0x351 from analysis/data: 22 02 E8 03 D0 07 E0 01 = 54.6 V, 100 A, 200 A, 48.0 V.
    s.bn.cvl = 54.6f;
    settle(s);
    const uint8_t real351[8] = {0x22, 0x02, 0xE8, 0x03, 0xD0, 0x07, 0xE0, 0x01};
    s.e.on_frame(0, 0x351, real351, 8, s.t);
    const Source& b = s.e.source(0);
    CHECK(near(b.cvl, 54.6f) && near(b.ccl, 100) && near(b.dcl, 200) && near(b.dvl, 48), "cvl %.1f ccl %.1f dcl %.1f dvl %.1f",
          b.cvl, b.ccl, b.dcl, b.dvl);
    // Real pack-2 0x216: 02 00 00 34 83 03 E8 00 -> DCL 100.0 A, energy 0x3483 / 10 kWh.
    const uint8_t real216[8] = {0x02, 0x00, 0x00, 0x34, 0x83, 0x03, 0xE8, 0x00};
    s.e.on_frame(0, 0x216, real216, 8, s.t);
    CHECK(near(b.p2_dcl, 100) && near((float)b.p2_e_dis_kwh, 1344.3f), "p2 dcl %.1f e %.1f", b.p2_dcl, b.p2_e_dis_kwh);
    CHECK(near(b.capacity_ah, 200) && near(b.t_min, 24.85f) && near(b.cell_max_mv, 3310), "cap/temps/cells");
    const Source& k = s.e.source(1);
    CHECK(near(k.cvl, 56.0f) && near(k.ccl, 150) && near(k.soc, 40) && k.charge_enable && k.discharge_enable, "kny");
    // Capacity-weighted SOC: (60*200 + 40*300) / 500 = 48; voltage mean; current sum; T max; SOH min.
    s.bn.i = 20, s.kny.i = 10;
    s.run(1000);
    CHECK(near(s.o().soc, 48.0f) && near(s.o().voltage, 52.45f) && near(s.o().current, 30) &&
              near(s.o().temperature, 25) && near(s.o().soh, 99) && near(s.o().capacity_ah, 500),
          "soc %.2f v %.2f i %.1f t %.1f soh %.0f cap %.0f", s.o().soc, s.o().voltage, s.o().current,
          s.o().temperature, s.o().soh, s.o().capacity_ah);
}

static void test_boot_missing_source() {
    printf("boot with a missing source: limits 0 until every source is seen and held 10 s\n");
    Sim s;
    s.kny.send = false;
    s.run(30000);
    CHECK(s.o().state == OutState::StartupHold, "state %s", out_state_name(s.o().state));
    CHECK(s.o().send && zero_limits(s.o()), "frames still sent, CCL/DCL 0");
    CHECK(frame_cvl_dV(s.o()) == 530, "fail-safe CVL in frame: %d", frame_cvl_dV(s.o()));
    CHECK(frame_alarm(s.o()) & kAlarmCommFail, "comm-fail alarm bit set");
    CHECK(s.e.source(1).state == SrcState::NotSeen, "KNY not seen");
    CHECK(s.e.source(0).state == SrcState::Ok, "BN ok");
    s.kny.send = true;
    s.run(9000);
    CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "still holding after 9 s: %s",
          out_state_name(s.o().state));
    s.run(2000);
    CHECK(s.o().state == OutState::Normal && s.o().ccl > 0, "normal after the hold: %s ccl %.1f",
          out_state_name(s.o().state), s.o().ccl);
    // Boot with both present: hold first.
    Sim s2;
    s2.run(9500);
    CHECK(s2.o().state == OutState::StartupHold && zero_limits(s2.o()), "both present, 9.5 s: hold");
    s2.run(1500);
    CHECK(s2.o().state == OutState::Normal, "both present, 11 s: normal");
    // A source that never appears at all keeps the output at zero limits indefinitely.
    Sim s3;
    s3.bn.send = false;
    s3.run(600000);
    CHECK(s3.o().state == OutState::StartupHold && zero_limits(s3.o()) && s3.o().send, "BN never seen, 10 min");
}

static void test_stale() {
    printf("stale source: 5 s (1 Hz frames), 6 s (pack 2), still transmitting fail-safe values\n");
    Sim s;
    settle(s);
    CHECK(s.o().state == OutState::Normal, "normal");
    s.align();
    s.kny.send = false;
    s.run(4800);  // last frame at the start; 4.8 s later
    CHECK(s.o().state == OutState::Normal, "KNY silent 4.8 s: still normal (%s)", out_state_name(s.o().state));
    s.run(1300);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()) && s.o().send, "KNY silent 6.1 s: fail-safe, sending");
    CHECK(frame_alarm(s.o()) & kAlarmCommFail, "alarm flag");
    CHECK(s.has_event(EvType::Stale), "stale event");
    s.kny.send = true;
    s.run(9000);
    CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "recovering: held");
    s.run(2000);
    CHECK(s.o().state == OutState::Normal, "recovered after the hold");
    // Pack-2 frames: 6 s timeout.
    s.run(65000);
    while (s.t % 2000) s.run(100);  // last pack-2 frames sent now
    s.bn.send_p2 = false;
    s.run(5800);
    CHECK(s.o().state == OutState::Normal, "pack 2 silent 5.8 s: normal under the 6 s timeout (%s)",
          out_state_name(s.o().state));
    s.run(400);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "pack 2 silent > 6 s: fail-safe");
    CHECK(strstr(s.e.source(0).reason, "pack 2") != nullptr, "reason names pack 2: %s", s.e.source(0).reason);
    s.bn.send_p2 = true;
    s.run(15000);
    // A single 1 Hz frame missing (0x35A) is enough.
    s.run(65000);
    s.align();
    s.bn.send_35a = false;
    s.run(4500);
    CHECK(s.o().state == OutState::Normal, "0x35A missing 4.5 s: normal (%s: %s)", out_state_name(s.o().state), s.o().reason);
    s.run(1000);
    CHECK(s.o().state == OutState::FailSafe, "0x35A missing 5.5 s: fail-safe");
    s.bn.send_35a = true;
    // 0x305 is not watched: there is no 0x305 in the simulation and the output is normal.
    s.run(15000);
    CHECK(s.o().state == OutState::Normal, "no 0x305 needed");
}

static void test_cvl_hold() {
    printf("CVL: min of sources, falls immediate, rises after a 60 s hold\n");
    Sim s;
    s.run(11000);  // normal; CVL output starts at the fail-safe 53.0 V and must rise through the hold
    CHECK(near(s.o().cvl, 53.0f) && near(s.o().cvl_raw, 55.2f), "after start-up: %.1f (raw %.1f)", s.o().cvl,
          s.o().cvl_raw);
    s.run(59000);
    CHECK(near(s.o().cvl, 53.0f), "59 s: held at 53.0 (%.1f)", s.o().cvl);
    s.run(2000);
    CHECK(near(s.o().cvl, 55.2f), "61 s: 55.2 (%.1f)", s.o().cvl);
    s.bn.cvl = 54.6f;
    s.run(1000);
    CHECK(near(s.o().cvl, 54.6f), "fall immediate: %.1f", s.o().cvl);
    s.bn.cvl = 56.0f;  // raw = min(56.0 BN, 56.0 KNY)
    s.run(58000);
    CHECK(near(s.o().cvl, 54.6f), "rise held 58 s: %.1f", s.o().cvl);
    s.run(3000);
    CHECK(near(s.o().cvl, 56.0f), "rise after 60 s: %.1f", s.o().cvl);
    // Chatter: a rise that falls back inside the hold never reaches the output.
    s.bn.cvl = 54.6f;
    s.run(1000);
    for (int k = 0; k < 10; k++) {
        s.bn.cvl = 55.2f;
        s.run(20000);
        s.bn.cvl = 54.6f;
        s.run(3000);
    }
    CHECK(near(s.o().cvl, 54.6f), "chatter 55.2/54.6 every 23 s: output stays 54.6 (%.1f)", s.o().cvl);
    // A rise in two steps inside one hold window goes to the lower value first.
    s.bn.cvl = 55.2f;
    s.run(20000);
    s.bn.cvl = 56.0f;
    s.run(41000);
    CHECK(near(s.o().cvl, 55.2f), "two-step rise: first to the window minimum 55.2 (%.1f)", s.o().cvl);
    s.run(61000);
    CHECK(near(s.o().cvl, 56.0f), "then 56.0 after another hold (%.1f)", s.o().cvl);
    // KNY lower than BN: min wins.
    s.kny.cvl = 55.0f;
    s.run(1000);
    CHECK(near(s.o().cvl, 55.0f), "min across sources: %.1f", s.o().cvl);
    CHECK(s.has_event(EvType::CvlChange), "CVL change events");
}

static void test_limits() {
    printf("CCL/DCL: sum of own limits, user clamp, blocking source, pack-share cap\n");
    Sim s;
    settle(s);
    CHECK(near(s.o().ccl, 250) && near(s.o().dcl, 350), "sum: ccl %.1f dcl %.1f", s.o().ccl, s.o().dcl);
    s.c.ccl_max = 120;
    s.c.dcl_max = 300;
    s.reconf();
    s.run(1000);
    CHECK(near(s.o().ccl, 120) && near(s.o().dcl, 300) && s.o().ccl_clamped, "user clamp: %.1f %.1f", s.o().ccl,
          s.o().dcl);
    s.run(10000);  // clamp events are debounced 10 s
    CHECK(s.has_event(EvType::LimitClamp, "user maximum"), "limit clamp event");
    s.c.ccl_max = 400;
    s.c.dcl_max = 400;
    s.reconf();
    s.run(1000);
    // KNY CCL 0 (e.g. full) blocks charging of the whole bank; discharge unaffected.
    s.kny.ccl = 0;
    s.run(1000);
    CHECK(s.o().ccl == 0 && near(s.o().dcl, 350), "KNY CCL 0 -> bank CCL 0: %.1f / %.1f", s.o().ccl, s.o().dcl);
    s.kny.ccl = 150;
    s.kny.flags = 0x40;  // charge enable cleared
    s.run(1000);
    CHECK(s.o().ccl == 0 && (s.o().frames[4].data[0] & 0x80) == 0 && near(s.o().dcl, 350),
          "KNY charge disabled -> CCL 0: %.1f", s.o().ccl);
    s.kny.flags = 0x80;  // discharge enable cleared
    s.run(1000);
    CHECK(s.o().dcl == 0 && near(s.o().ccl, 250), "KNY discharge disabled -> DCL 0: %.1f", s.o().dcl);
    s.kny.flags = 0xC0;
    s.run(1000);
    // Share cap: charging 60 A total; pack 2 takes 26 A (43 %) with a 30 A own limit.
    s.bn.ccl = 60, s.bn.p2_ccl = 30;  // master 60 = pack1 30 + pack2 30
    s.bn.i = 40, s.bn.p2_i = 26, s.kny.i = 20;
    s.run(30000);
    float cap = 30.0f / (26.0f / 60.0f);
    CHECK(near(s.o().ccl, floorf(cap * 10) / 10, 0.2f) && s.o().ccl_clamped, "share cap %.1f (expect %.1f; sum %.1f)",
          s.o().ccl, cap, s.o().ccl_sum);
    float p2_at_limit = s.o().ccl * 26.0f / 60.0f;
    CHECK(p2_at_limit <= 30.05f, "pack 2 at the bank limit: %.2f A <= its own 30 A", p2_at_limit);
    CHECK(s.has_event(EvType::LimitClamp, "pack share"), "share clamp event");
    // Every source's own limit: at the output CCL, no pack exceeds its own limit at the measured shares.
    float shares[3] = {14.0f / 60, 26.0f / 60, 20.0f / 60}, lims[3] = {30, 30, 150};
    for (int k = 0; k < 3; k++) CHECK(s.o().ccl * shares[k] <= lims[k] + 0.05f, "pack %d over its own limit", k);
    // Discharge: KNY carries 70 % of -100 A with a 150 A limit -> cap 214 A.
    s.bn.i = -30, s.bn.p2_i = -15, s.kny.i = -70;
    s.run(30000);
    CHECK(s.o().dcl <= 150.0f / 0.7f + 0.1f && s.o().dcl_clamped, "discharge share cap: %.1f", s.o().dcl);
}

static void test_failure_modes() {
    printf("failure modes at the inverter output\n");
    {
        Sim s;
        s.c.failsafe_cvl = 0;  // not set
        s.reconf();
        s.run(20000);
        CHECK(s.o().state == OutState::Refused && !s.o().send, "no fail-safe CVL: refused, silent (%s)", s.o().reason);
        s.c.failsafe_cvl = 53;
        s.c.ccl_max = 0;
        s.reconf();
        s.run(1000);
        CHECK(s.o().state == OutState::Refused && !s.o().send, "no CCL maximum: refused");
    }
    {
        Sim s;
        settle(s);
        s.e.set_ota(true, s.t);
        s.run(1000);
        CHECK(s.o().state == OutState::Silent && !s.o().send, "OTA: silent");
        s.e.set_ota(false, s.t);
        s.run(1000);
        CHECK(s.o().state == OutState::Normal && s.o().send, "after OTA: normal");
    }
    {
        Sim s;
        s.c.silent_failsafe = true;
        s.reconf();
        s.run(5000);
        CHECK(s.o().state == OutState::Silent && !s.o().send, "silent action: silent during the start-up hold");
        settle(s);
        CHECK(s.o().state == OutState::Normal && s.o().send, "silent action: normal sends");
        s.kny.send = false;
        s.run(7000);
        CHECK(s.o().state == OutState::Silent && !s.o().send, "silent action: stale -> silent");
    }
    {
        Sim s;
        settle(s);
        s.align();
        s.bn.a35[2] = 0x83;  // 0x35A leaves its baseline
        s.run(2000);         // two identical deviating frames: not yet (one corrupt frame never latches)
        CHECK(s.o().state == OutState::Normal && !s.e.source(0).latched, "0x35A deviating twice: not latched yet");
        s.run(1000);
        CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()) && s.e.source(0).latched, "0x35A change x3: trip");
        CHECK(frame_alarm(s.o()) & kProtSystemError, "system-error flag");
        s.bn.a35[2] = 0x82;  // back to baseline: stays latched
        s.run(30000);
        CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "latched after baseline returns");
        s.e.clear_trips(s.t);
        s.run(9000);
        CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "cleared: held");
        s.run(2000);
        CHECK(s.o().state == OutState::Normal, "cleared: normal after the hold");
    }
    {
        Sim s;
        settle(s);
        s.bn.ah = 100;  // slave pack lost
        s.align();
        s.run(1000);
        CHECK(s.o().state == OutState::Normal, "one 0x379 frame with a new value: no effect yet");
        s.run(1000);
        CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()) && !s.e.source(0).latched,
              "0x379 200 -> 100 Ah: recovery hold, not latched (%s)", out_state_name(s.o().state));
        CHECK(s.has_event(EvType::CapacityStep, "200 -> 100"), "capacity step warning event");
        s.run(10000);
        CHECK(s.o().state == OutState::Normal && near(s.o().capacity_ah, 400), "after the hold: normal with 100 Ah (+300 KNY = %.0f)",
              s.o().capacity_ah);
    }
    {
        Sim s;
        settle(s);
        s.kny.b359[0] = 0x02;  // cell over-voltage protection
        s.run(1000);
        CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "KNY protection bit: fail-safe");
        CHECK(frame_alarm(s.o()) & 0x02, "KNY flag passed to the output 0x359");
        CHECK(s.has_event(EvType::Alarm), "alarm event");
        s.kny.b359[0] = 0;
        s.run(12000);
        CHECK(s.o().state == OutState::Normal && !s.e.source(1).latched, "defined flag clears (not latched)");
        s.kny.b359[2] = 0x08;  // high-temperature warning (alarm byte)
        s.run(1000);
        CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "KNY warning bit: fail-safe");
        s.kny.b359[2] = 0;
        s.run(12000);
        s.kny.b359[0] = 0x01;  // undefined bit, one frame only (a corrupt frame): never latches
        s.run(1000);
        s.kny.b359[0] = 0;
        s.run(20000);
        CHECK(s.o().state == OutState::Normal && !s.e.source(1).latched, "undefined bit in one frame: not latched");
        s.kny.b359[0] = 0x01;  // persistent: latches after 3 identical frames
        s.run(4000);
        s.kny.b359[0] = 0;
        s.run(20000);
        CHECK(s.o().state == OutState::FailSafe && s.e.source(1).latched, "undefined bit x3: latched");
    }
    {
        Sim s;
        settle(s);
        s.bn.cvl = 70;  // implausible limit
        s.run(1000);
        CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()) && s.e.source(0).state == SrcState::Invalid,
              "implausible CVL: fail-safe at once (%s)", s.e.source(0).reason);
        s.bn.cvl = 55.2f;
        s.run(11000);
        CHECK(s.o().state == OutState::Normal, "valid again: normal after the hold");
        s.kny.v = 20;  // implausible measurement: counts as missing -> stale after 5 s
        s.run(3000);
        CHECK(s.o().state == OutState::Normal, "implausible voltage 3 s: last value kept");
        s.run(3000);
        CHECK(s.o().state == OutState::FailSafe, "implausible voltage > 5 s: stale");
        CHECK(s.e.source(1).plausibility_rejects >= 5, "rejects counted");
    }
    {
        // Disabled source: ignored completely.
        Sim s(false);
        s.kny.send = true;
        settle(s);
        CHECK(s.o().state == OutState::Normal && near(s.o().ccl, 100) && near(s.o().capacity_ah, 200),
              "BN only: normal, ccl %.1f", s.o().ccl);
    }
}

static void test_frames() {
    printf("output frames: Pylontech V1.3 layout\n");
    Sim s;
    settle(s);
    s.bn.i = 10, s.kny.i = 5;
    s.run(3000);
    const Output& o = s.o();
    const Frame& f = o.frames[0];
    CHECK(f.id == 0x351 && f.dlc == 8 && frame_cvl_dV(o) == 552, "0x351 CVL 552: %d", frame_cvl_dV(o));
    int ccl = (int16_t)(f.data[2] | f.data[3] << 8), dcl = (int16_t)(f.data[4] | f.data[5] << 8),
        dvl = f.data[6] | f.data[7] << 8;
    CHECK(ccl == lrintf(o.ccl * 10) && dcl == lrintf(o.dcl * 10) && dvl == 480, "ccl %d dcl %d dvl %d", ccl, dcl, dvl);
    CHECK(o.frames[1].id == 0x355 && o.frames[2].id == 0x356 && o.frames[3].id == 0x359 && o.frames[4].id == 0x35C &&
              o.frames[5].id == 0x35E,
          "ids");
    int cur = (int16_t)(o.frames[2].data[2] | o.frames[2].data[3] << 8);
    CHECK(cur == 150, "0x356 current 15.0 A: %d", cur);
    CHECK(o.frames[4].data[0] == 0xC0, "0x35C charge+discharge enable: %02X", o.frames[4].data[0]);
    CHECK(o.frames[3].data[4] == 2 && o.frames[3].data[5] == 'P', "0x359 module count");
}

// ---- Single-source operation: KNY only (source A disabled) ---------------------------
static void test_kny_only() {
    printf("KNY only (A disabled): output = KNY values + user maximums + fail-safe rules\n");
    // Boot: KNY missing -> start-up hold with zero limits; BN frames on port A are ignored.
    Sim s(true, false);
    s.kny.send = false;
    s.run(30000);
    CHECK(s.e.source(0).state == SrcState::Disabled, "A disabled: %s", src_state_name(s.e.source(0).state));
    CHECK(s.o().state == OutState::StartupHold && s.o().send && zero_limits(s.o()), "boot, KNY not seen: hold, zero (%s)",
          out_state_name(s.o().state));
    CHECK(frame_cvl_dV(s.o()) == 530 && (frame_alarm(s.o()) & kAlarmCommFail), "fail-safe CVL and comm-fail bit");
    s.kny.send = true;
    s.run(9000);
    CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "KNY seen 9 s: still holding");
    s.run(2000);
    CHECK(s.o().state == OutState::Normal, "KNY seen 11 s: normal (%s: %s)", out_state_name(s.o().state), s.o().reason);
    // Values: exactly the KNY's (CVL still rising through the hold from the fail-safe 53.0 V).
    s.kny.i = 12.5f;
    s.run(1000);
    const Output& o = s.o();
    CHECK(near(o.ccl, 150) && near(o.dcl, 150) && near(o.dvl, 46), "limits = KNY: %.1f %.1f %.1f", o.ccl, o.dcl, o.dvl);
    CHECK(near(o.soc, 40) && near(o.soh, 100) && near(o.voltage, 52.4f) && near(o.current, 12.5f) &&
              near(o.temperature, 24) && near(o.capacity_ah, 300),
          "meas = KNY: soc %.1f soh %.0f v %.2f i %.1f t %.1f cap %.0f", o.soc, o.soh, o.voltage, o.current,
          o.temperature, o.capacity_ah);
    CHECK(o.frames[3].data[4] == 1, "0x359 module count 1: %d", o.frames[3].data[4]);
    CHECK(near(o.cvl, 53.0f) && near(o.cvl_raw, 56.0f), "CVL held at 53.0 (raw %.1f)", o.cvl_raw);
    CHECK(o.ccl_share_cap < 0 || o.ccl_share_cap >= 149.9f, "single pack: share cap never below its own limit (%.1f)",
          o.ccl_share_cap);
    // BN frames arriving on port A change nothing (with BN values that would otherwise win).
    s.bn.cvl = 50.0f, s.bn.ccl = 5, s.bn.soc = 5;
    s.run(5000);
    CHECK(near(s.o().ccl, 150) && near(s.o().soc, 40) && s.o().state == OutState::Normal, "BN frames ignored");
    // CVL hold: rise after 60 s, falls at once.
    s.run(56000);
    CHECK(near(s.o().cvl, 56.0f), "CVL rose to the KNY's 56.0 after the hold (%.1f)", s.o().cvl);
    s.kny.cvl = 55.0f;
    s.run(1000);
    CHECK(near(s.o().cvl, 55.0f), "fall immediate: %.1f", s.o().cvl);
    s.kny.cvl = 56.5f;
    s.run(58000);
    CHECK(near(s.o().cvl, 55.0f), "rise held 58 s: %.1f", s.o().cvl);
    s.run(3000);
    CHECK(near(s.o().cvl, 56.5f), "rise after 60 s: %.1f", s.o().cvl);
    // User maximums.
    s.c.ccl_max = 100, s.c.dcl_max = 120;
    s.reconf();
    s.run(1000);
    CHECK(near(s.o().ccl, 100) && near(s.o().dcl, 120) && s.o().ccl_clamped && s.o().dcl_clamped, "clamped: %.1f %.1f",
          s.o().ccl, s.o().dcl);
    s.run(11000);
    CHECK(s.has_event(EvType::LimitClamp, "user maximum"), "clamp event");
    s.c.ccl_max = 400, s.c.dcl_max = 400;
    s.reconf();
    s.run(1000);
    CHECK(near(s.o().ccl, 150) && !s.o().ccl_clamped, "unclamped again: %.1f", s.o().ccl);
    // KNY's own zero / disable flags.
    s.kny.ccl = 0;
    s.run(1000);
    CHECK(s.o().ccl == 0 && near(s.o().dcl, 150), "KNY CCL 0 -> 0");
    s.kny.ccl = 150, s.kny.flags = 0x80;
    s.run(1000);
    CHECK(s.o().dcl == 0 && near(s.o().ccl, 150), "KNY discharge disabled -> DCL 0");
    s.kny.flags = 0xC0;
    s.run(1000);
    // Stale: 5 s.
    s.align();
    s.kny.send = false;
    s.run(4800);
    CHECK(s.o().state == OutState::Normal, "silent 4.8 s: normal");
    s.run(1300);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()) && s.o().send && (frame_alarm(s.o()) & kAlarmCommFail),
          "silent 6.1 s: fail-safe values, comm-fail (%s)", out_state_name(s.o().state));
    CHECK(frame_cvl_dV(s.o()) == 530, "fail-safe CVL %d", frame_cvl_dV(s.o()));
    s.kny.send = true;
    s.run(9000);
    CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "recovery hold");
    s.run(2000);
    CHECK(s.o().state == OutState::Normal && near(s.o().ccl, 150), "recovered");
    // One required frame missing (0x35C) is enough.
    s.align();
    s.kny.send = false;
    for (int k = 0; k < 7; k++) {  // send everything but 0x35C by hand
        s.run(1000);
        uint8_t d[8] = {};
        put16(d, 0, 565), put16(d, 2, 1500), put16(d, 4, 1500), put16(d, 6, 460);
        s.f(1, 0x351, d, 8);
    }
    CHECK(s.o().state == OutState::FailSafe, "0x35C and others missing: fail-safe");
    s.kny.send = true;
    s.run(12000);
    // Protection flag: fail-safe while set.
    s.kny.b359[0] = 0x02;  // over-voltage protection
    s.run(1000);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "KNY protection bit: fail-safe");
    s.kny.b359[0] = 0;
    s.run(12000);
    CHECK(s.o().state == OutState::Normal, "protection cleared: normal after the hold (%s)", out_state_name(s.o().state));
    // Silent fail-safe action and OTA work the same with one source.
    s.e.set_ota(true, s.t);
    s.run(100);
    CHECK(s.o().state == OutState::Silent && !s.o().send, "OTA: silent");
    s.e.set_ota(false, s.t);
    s.run(11000);
    CHECK(s.o().state == OutState::Normal, "after OTA: normal");
}

static void test_kny_dlc8_telemetry() {
    printf("KNY: DLC 8 on 0x355/0x356/0x359/0x35C, 0x373 cells/temps, 0x379 capacity\n");
    Sim s(true, false);
    s.kny.send = false;
    for (int k = 0; k < 15; k++) {
        s.run(1000);
        uint8_t d[8] = {};
        put16(d, 0, 576), put16(d, 2, 1950), put16(d, 4, 1950), put16(d, 6, 480);
        s.f(1, 0x351, d, 8);
        uint8_t a[8] = {};
        put16(a, 0, 87), put16(a, 2, 100);
        s.f(1, 0x355, a, 8);
        uint8_t m[8] = {};
        put16(m, 0, 5532), put16(m, 2, -125), put16(m, 4, 251);
        s.f(1, 0x356, m, 8);
        uint8_t al[8] = {0, 0, 0, 0, 1, 'P', 'N', 0};
        s.f(1, 0x359, al, 8);
        uint8_t fl[8] = {0xC0};
        s.f(1, 0x35C, fl, 8);
        uint8_t c[8] = {};
        put16(c, 0, 3400), put16(c, 2, 3460), put16(c, 4, 297), put16(c, 6, 299);
        s.f(1, 0x373, c, 8);
        uint8_t cap[8] = {};
        put16(cap, 0, 280);
        s.f(1, 0x379, cap, 8);
    }
    const Source& k = s.e.source(1);
    CHECK(s.o().state == OutState::Normal && near(s.o().ccl, 195) && near(k.soc, 87) && near(k.voltage, 55.32f) &&
              near(k.current, -12.5f) && k.charge_enable,
          "DLC 8 frames decoded: %s ccl %.1f soc %.0f v %.2f i %.1f", out_state_name(s.o().state), s.o().ccl, k.soc,
          k.voltage, k.current);
    CHECK(k.have_cells && near(k.cell_min_mv, 3400) && near(k.cell_max_mv, 3460) && near(k.t_min, 23.85f) &&
              near(k.t_max, 25.85f),
          "0x373: %.0f-%.0f mV, %.2f-%.2f C", k.cell_min_mv, k.cell_max_mv, k.t_min, k.t_max);
    CHECK(k.have_cap && near(k.capacity_ah, 280) && near(s.o().capacity_ah, 280), "0x379 capacity %.0f replaces the setting (out %.0f)",
          k.capacity_ah, s.o().capacity_ah);
    CHECK(k.plausibility_rejects == 0, "no rejects");
}

static void test_bn_only() {
    printf("BN only (B disabled): output = BN values; KNY frames ignored\n");
    Sim s(false);
    s.c.enabled[1] = false;
    s.reconf();
    settle(s);
    CHECK(s.o().state == OutState::Normal, "normal (%s)", out_state_name(s.o().state));
    CHECK(near(s.o().ccl, 100) && near(s.o().dcl, 200) && near(s.o().cvl, 55.2f) && near(s.o().soc, 60) &&
              near(s.o().capacity_ah, 200),
          "BN values: ccl %.1f dcl %.1f cvl %.1f soc %.1f cap %.0f", s.o().ccl, s.o().dcl, s.o().cvl, s.o().soc,
          s.o().capacity_ah);
    // KNY frames on port B with a lower CVL change nothing.
    uint8_t d[8] = {};
    put16(d, 0, 500), put16(d, 2, 10), put16(d, 4, 10), put16(d, 6, 400);
    s.f(1, 0x351, d, 8);
    s.run(1000);
    CHECK(near(s.o().cvl, 55.2f) && near(s.o().ccl, 100), "KNY frames ignored");
    CHECK(s.o().frames[3].data[4] == 1, "module count 1");
    s.align();
    s.bn.send = false;
    s.run(6100);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "BN stale: fail-safe");
}

static void test_no_source() {
    printf("no source enabled: output refused (silent)\n");
    Sim s(false, false);
    s.run(20000);
    CHECK(s.o().state == OutState::Refused && !s.o().send, "refused: %s (%s)", out_state_name(s.o().state), s.o().reason);
}


// ---- 0.1.7 -----------------------------------------------------------------------------
static void test_capacity_step() {
    printf("0.1.7 capacity step: warning + recovery hold, no latch; zero/implausible = invalid\n");
    Sim s;
    s.bn.ah = 100;  // boots with one pack in the bank
    settle(s);
    CHECK(s.o().state == OutState::Normal && near(s.e.source(0).capacity_ah, 100), "normal with 100 Ah");
    s.bn.ah = 200;  // pack 2 joins (13 Nov 15:19:18); counts on the second frame
    s.align();
    s.run(2000);
    CHECK(s.o().state == OutState::StartupHold && zero_limits(s.o()), "100 -> 200: hold (%s)", out_state_name(s.o().state));
    CHECK(s.has_event(EvType::CapacityStep, "100 -> 200") && !s.e.source(0).latched, "warning event, not latched");
    s.run(8000);
    CHECK(s.o().state == OutState::StartupHold, "still holding at 9 s");
    s.run(2500);
    CHECK(s.o().state == OutState::Normal && near(s.e.source(0).capacity_ah, 200) && s.has_event(EvType::Recovered),
          "normal after the hold, 200 Ah (%s)", out_state_name(s.o().state));
    s.bn.ah = 0;  // zero capacity: invalid (fail-safe), not latched
    s.run(2000);
    CHECK(s.o().state == OutState::FailSafe && s.e.source(0).state == SrcState::Invalid && !s.e.source(0).latched,
          "0 Ah: invalid, fail-safe (%s)", src_state_name(s.e.source(0).state));
    s.bn.ah = 200;
    s.run(12000);
    CHECK(s.o().state == OutState::Normal, "sane capacity again: normal after the hold");
    s.bn.ah = 5000;  // implausible
    s.run(2000);
    CHECK(s.e.source(0).state == SrcState::Invalid, "5000 Ah: invalid");
    s.bn.ah = 200;
    s.run(12000);
    // KNY 0x379 step (KNY sends 300 Ah)
    uint8_t c[8] = {0x2C, 0x01};
    s.f(1, 0x379, c, 8);
    s.run(1000);
    s.run(12000);
    CHECK(s.o().state == OutState::Normal && near(s.e.source(1).capacity_ah, 300), "KNY 300 Ah learned");
    c[0] = 0x96, c[1] = 0;  // 150 Ah
    s.f(1, 0x379, c, 8);
    s.run(500);
    CHECK(s.e.source(1).state == SrcState::Ok, "KNY: one 150 Ah frame ignored");
    s.f(1, 0x379, c, 8);
    s.run(500);
    CHECK(s.e.source(1).state == SrcState::Holding && s.has_event(EvType::CapacityStep, "300 -> 150"), "KNY step: hold");
}

static void test_frame_validation() {
    printf("0.1.7 per-source frame validation: wrong IDs/DLCs counted and ignored; a corrupt burst never latches\n");
    Sim s;
    settle(s);
    uint8_t d[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    s.f(1, 0x3DE, d, 8);   // bit-flipped KNY ID (13 Nov)
    s.f(1, 0x355, d, 6);   // KNY 0x355 with a DLC it does not use
    s.f(0, 0x231, d, 8);   // unknown on the Blue Nova bus
    s.f(0, 0x351, d, 5);   // BN 0x351 short
    s.run(1000);
    CHECK(s.e.source(1).rejected_frames == 2 && s.e.source(0).rejected_frames == 2, "rejected: B %u, A %u",
          s.e.source(1).rejected_frames, s.e.source(0).rejected_frames);
    CHECK(s.o().state == OutState::Normal && s.e.source(1).plausibility_rejects == 0, "no effect on the output");
    CHECK(s.has_event(EvType::FrameRejected), "rejected-frame event");
    // 13 Nov-style burst: 2000 frames in 1 s with random data on KNY IDs (well-formed but wrong).
    unsigned seed = 1;
    const uint32_t ids[] = {0x351, 0x355, 0x356, 0x359, 0x35C, 0x35E, 0x373, 0x379};
    for (int k = 0; k < 2000; k++) {
        uint8_t x[8];
        for (auto& b : x) seed = seed * 1103515245u + 12345u, b = (uint8_t)(seed >> 16);
        seed = seed * 1103515245u + 12345u;
        s.e.on_frame(1, ids[(seed >> 16) % 8], x, 8, s.t);
    }
    s.run(1000);
    CHECK(!s.e.source(1).latched && !s.e.source(0).latched, "burst: no latched trip");
    s.run(15000);
    CHECK(s.o().state == OutState::Normal, "burst: back to normal after the hold (%s: %s)", out_state_name(s.o().state),
          s.o().reason);
}

static void test_kny_flags_energy() {
    printf("0.1.7 KNY 0x35C flags, cells, capacity; energy counters (KNY, BN packs)\n");
    Sim s;
    settle(s);
    s.kny.flags = 0x40;  // charge disabled (13 Nov cut-off)
    s.run(1000);
    CHECK(!s.e.source(1).charge_enable && s.e.source(1).discharge_enable && s.o().ccl == 0 && s.o().dcl > 0,
          "0x35C 0x40: charge disabled -> CCL 0, DCL %.1f", s.o().dcl);
    s.kny.flags = 0xC0;
    s.run(1000);
    // one hour: KNY -10 A at 52.4 V, BN bank -20 A at 52.5 V of which pack 2 -8 A
    s.kny.i = -10, s.bn.i = -20, s.bn.p2_i = -8;
    s.run(3600000);
    const Source& k = s.e.source(1);
    const Source& b = s.e.source(0);
    CHECK(fabs(k.e_out_kwh - 0.524) < 0.01 && k.e_in_kwh < 0.001, "KNY out %.3f kWh (0.524)", k.e_out_kwh);
    CHECK(fabs(b.e_out_kwh - 1.05) < 0.02, "BN bank out %.3f kWh (1.05)", b.e_out_kwh);
    CHECK(fabs(b.p2_e_out_kwh - 0.42) < 0.02 && fabs(b.p1_e_out_kwh - 0.63) < 0.02, "pack 2 %.3f (0.42), pack 1 %.3f (0.63)",
          b.p2_e_out_kwh, b.p1_e_out_kwh);
    s.kny.i = 5;
    s.run(60000);
    CHECK(k.e_in_kwh > 0.004 && k.e_in_kwh < 0.0045, "KNY in %.4f kWh after 1 min at +5 A", k.e_in_kwh);
}

static void test_pack1() {
    printf("0.1.7 Blue Nova pack 1 (bank minus pack 2)\n");
    Sim s;
    s.bn.i = -20, s.bn.p2_i = -8, s.bn.soc = 60;
    settle(s);
    const Source& b = s.e.source(0);
    CHECK(b.have_p1 && near(b.p1_i, -12) && near(b.p1_ccl, 50) && near(b.p1_dcl, 100), "pack 1: I %.1f CCL %.1f DCL %.1f",
          b.p1_i, b.p1_ccl, b.p1_dcl);
    CHECK(b.have_p1_soc && near(b.p1_soc, 60), "pack 1 SOC %.1f (bank 60, pack 2 60)", b.p1_soc);
    s.bn.ah = 100;  // one-pack bank: no derived SOC
    s.run(16000);
    CHECK(!s.e.source(0).have_p1_soc, "100 Ah bank: no pack-1 SOC");
}

static void test_failsafe_voltages() {
    printf("0.1.7 fail-safe CVL/DVL follow the last good pack voltage\n");
    Sim s;
    s.bn.v = 53.34f, s.kny.v = 53.30f;  // above the 53.0 V fail-safe CVL
    settle(s);
    s.align();
    s.kny.send = false;
    s.run(6000);
    CHECK(s.o().state == OutState::FailSafe && zero_limits(s.o()), "stale: fail-safe");
    CHECK(near(s.o().cvl, 53.4f) && near(s.o().dvl, 47.0f), "CVL %.1f >= pack 53.34 V, DVL %.1f", s.o().cvl, s.o().dvl);
    s.kny.send = true;
    s.bn.v = 46.5f, s.kny.v = 46.6f;  // below the 47.0 V fail-safe DVL
    s.run(3000);
    s.align();
    s.bn.send = false;
    s.run(6000);
    CHECK(s.o().state == OutState::FailSafe && near(s.o().dvl, 46.5f) && near(s.o().cvl, 53.0f),
          "DVL %.1f <= pack 46.5 V, CVL %.1f", s.o().dvl, s.o().cvl);
    // silent action is still available
    Sim q;
    q.c.silent_failsafe = true;
    q.reconf();
    settle(q);
    q.kny.send = false;
    q.run(7000);
    CHECK(q.o().state == OutState::Silent && !q.o().send, "silent fail-safe action still works");
}

static void test_guard() {
    printf("0.1.7 per-source over-current guard\n");
    Sim s;
    s.caps_default();  // A 100, A pack 2 50, B 100
    settle(s);
    CHECK(near(s.o().ccl_sum, 200) && near(s.o().dcl_sum, 200), "caps limit each source's contribution: %.0f / %.0f",
          s.o().ccl_sum, s.o().dcl_sum);
    // A discharges 108 A against min(200, cap 100): > 105 % for > 3 s -> DCL 50 %
    s.bn.i = -108, s.bn.p2_i = -40;
    s.run(3000);
    CHECK(s.o().guard[0][1] == GuardState::Ok, "3.0 s over: not yet");
    float before = s.o().dcl;  // sum / share cap / user max, before the guard
    s.run(1500);
    CHECK(s.o().guard[0][1] == GuardState::Derated && near(s.o().dis_factor, 0.5f) && fabsf(s.o().dcl - before / 2) < 0.15f,
          "derated: DCL %.1f = 50%% of %.1f (%s)", s.o().dcl, before, guard_state_name(s.o().guard[0][1]));
    CHECK(s.has_event(EvType::OvercurrentDerate, "105%"), "overcurrent_derate event");
    CHECK(s.o().guard[0][0] == GuardState::Ok && s.o().chg_factor == 1, "charge direction unaffected");
    s.bn.i = -85, s.bn.p2_i = -40;  // under 90 % immediately; held at least 30 s
    s.run(25000);
    CHECK(s.o().guard[0][1] == GuardState::Derated, "held (25 s)");
    s.run(10000);
    CHECK(s.o().guard[0][1] == GuardState::Ok && s.o().dis_factor == 1 && s.o().dcl > before / 2 + 1,
          "released after 30 s under 90 %%: DCL %.1f", s.o().dcl);
    // still over 15 s after the derate -> 0
    s.bn.i = -110;
    s.run(5000);
    CHECK(s.o().guard[0][1] == GuardState::Derated, "derated again");
    s.run(16000);
    CHECK(s.o().guard[0][1] == GuardState::Zero && s.o().dcl == 0, "still over after 15 s: DCL 0");
    s.run(11000);
    CHECK(s.o().guard[0][1] == GuardState::Derated, "after the recovery hold: back to 50 %%");
    s.bn.i = -40, s.bn.p2_i = -15;
    s.run(31000);
    CHECK(s.o().guard[0][1] == GuardState::Ok, "released");
    // > 120 % at any moment: 0 at once (pack 2 charge: 61 A > 1.2 x 50)
    s.bn.i = 70, s.bn.p2_i = 61;
    s.run(1000);
    s.run(1000);
    CHECK(s.o().guard[1][0] == GuardState::Zero && s.o().ccl == 0, "pack 2 at 122 %%: CCL 0 (%s)",
          guard_state_name(s.o().guard[1][0]));
    s.bn.i = 10, s.bn.p2_i = 5;
    s.run(45000);
    CHECK(s.o().guard[1][0] == GuardState::Ok, "pack 2 released");
    // KNY charging against its own CCL 0: output already 0, the guard only logs
    Sim k;
    k.caps_default();
    settle(k);
    k.kny.ccl = 0, k.kny.flags = 0x40, k.kny.i = 5.6f;  // 13 Nov 15:13
    k.run(5000);
    CHECK(k.o().ccl == 0 && k.o().guard[2][0] == GuardState::Ok && k.has_event(EvType::OvercurrentDerate, "against its own limit 0"),
          "KNY 5.6 A against CCL 0: logged, no derate");
    // guard state is reported for every source with data, also outside normal
    CHECK(k.o().guard_limit[2][1] > 0 && near(k.o().guard_limit[0][0], 100), "guard limits reported: A charge %.0f",
          k.o().guard_limit[0][0]);
}

int main() {
    test_decode();
    test_boot_missing_source();
    test_stale();
    test_cvl_hold();
    test_limits();
    test_failure_modes();
    test_frames();
    test_kny_only();
    test_kny_dlc8_telemetry();
    test_bn_only();
    test_no_source();
    test_capacity_step();
    test_frame_validation();
    test_kny_flags_energy();
    test_pack1();
    test_failsafe_voltages();
    test_guard();
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
