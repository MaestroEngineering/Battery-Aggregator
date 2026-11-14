// Replays a Vector ASCII log (.asc, e.g. exported by a CAN viewer) of the KNY bus through the
// aggregation engine with the KNY as the ONLY source (port B; source A disabled), and checks the
// owner's expectations for the 20 h KNY capture (2026-11-13):
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I components/agg/include tools/replay_asc.cpp
//       components/agg/src/engine.cpp -o /tmp/ra     (one line)
//   /tmp/ra <file.asc> --kny-checks            # the owner's checks (first 300 s, Fri 13:14-13:44)
//   /tmp/ra <file.asc> [--cap A] [--from T] [--to T] [-v]   # free replay; T = seconds from the
//                                              # start, or [Mon..Sun ]HH:MM[:SS] wall clock
//
// Every frame of the file is fed in order (Rx and Tx), standard IDs only. The engine is ticked
// every 100 ms of log time. Output: an ID inventory (DLCs, count, max gap), the engine events,
// and the decoded KNY values at chosen moments. Nothing here transmits anything.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "agg/engine.h"

using namespace agg;

namespace {

struct Row {
    double t;  // s from the log start
    uint32_t id;
    uint8_t dlc;
    uint8_t d[8];
};

struct Log {
    std::vector<Row> rows;
    int start_wday = -1;     // 0 = Sun
    double start_tod = -1;   // seconds since local midnight of the "date" header
    bool hex = true;
    bool relative = false;   // "timestamps relative": each line is the delta to the previous
};

const char* kDays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

int day_index(const char* s) {
    for (int i = 0; i < 7; i++)
        if (!strncasecmp(s, kDays[i], 3)) return i;
    return -1;
}

// "date Fri Nov 7 01:23:45.678 pm 2026" (am/pm optional, ms optional).
void parse_date(Log& L, const char* line) {
    char wd[16] = "", mon[16] = "", tm[32] = "", ap[8] = "";
    int day = 0;
    int n = sscanf(line, "date %15s %15s %d %31s %7s", wd, mon, &day, tm, ap);
    if (n < 4) return;
    L.start_wday = day_index(wd);
    int h = 0, m = 0;
    double sec = 0;
    if (sscanf(tm, "%d:%d:%lf", &h, &m, &sec) < 2) return;
    if (n >= 5 && !strncasecmp(ap, "pm", 2) && h < 12) h += 12;
    if (n >= 5 && !strncasecmp(ap, "am", 2) && h == 12) h = 0;
    L.start_tod = h * 3600 + m * 60 + sec;
}

bool load(const char* path, Log& L, std::map<std::string, int>& skipped) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    double prev = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "date ", 5)) {
            parse_date(L, p);
            continue;
        }
        if (!strncmp(p, "base ", 5)) {
            L.hex = strstr(p, "hex") != nullptr;
            L.relative = strstr(p, "relative") != nullptr;
            continue;
        }
        if (!isdigit((unsigned char)*p)) continue;
        // "<time> <chan> <id>[x] Rx|Tx d <dlc> <bytes...>"
        char* e;
        double t = strtod(p, &e);
        if (e == p) continue;
        p = e;
        long chan = strtol(p, &e, 10);
        if (e == p || chan < 0) {  // "ErrorFrame", "CANFD" lines etc.
            skipped["non-classic line"]++;
            continue;
        }
        p = e;
        while (*p == ' ') p++;
        char idtok[32];
        int used = 0;
        if (sscanf(p, "%31s%n", idtok, &used) != 1) continue;
        p += used;
        size_t il = strlen(idtok);
        if (!isxdigit((unsigned char)idtok[0])) {
            skipped[std::string("line: ") + idtok]++;
            continue;
        }
        if (idtok[il - 1] == 'x' || idtok[il - 1] == 'X') {
            skipped["extended id"]++;
            continue;
        }
        uint32_t id = (uint32_t)strtoul(idtok, nullptr, L.hex ? 16 : 10);
        char dir[8], kind[8];
        int dlc;
        if (sscanf(p, "%7s %7s %d%n", dir, kind, &dlc, &used) != 3) continue;
        p += used;
        if (strcmp(kind, "d")) {
            skipped["remote frame"]++;
            continue;
        }
        Row r = {};
        r.t = L.relative ? prev + t : t;
        prev = r.t;
        r.id = id;
        r.dlc = (uint8_t)(dlc > 8 ? 8 : dlc);
        for (int k = 0; k < r.dlc; k++) {
            unsigned v;
            if (sscanf(p, "%x%n", &v, &used) != 1) break;
            r.d[k] = (uint8_t)v;
            p += used;
        }
        L.rows.push_back(r);
    }
    fclose(f);
    return true;
}

// "123.4" = seconds from the start; "Fri 13:14" / "13:14:30" = wall clock (needs the date header).
bool parse_when(const Log& L, const char* s, double& out) {
    int wd = day_index(s);
    const char* p = s;
    if (wd >= 0) {
        p += 3;
        while (*p == ' ') p++;
    }
    int h, m;
    double sec = 0;
    if (strchr(p, ':') && sscanf(p, "%d:%d:%lf", &h, &m, &sec) >= 2) {
        if (L.start_tod < 0) return false;
        double tod = h * 3600 + m * 60 + sec;
        int days = 0;
        if (wd >= 0 && L.start_wday >= 0) days = (wd - L.start_wday + 7) % 7;
        else if (tod < L.start_tod) days = 1;
        out = days * 86400.0 + tod - L.start_tod;
        return true;
    }
    char* e;
    out = strtod(s, &e);
    return e != s;
}

std::string wall(const Log& L, double t) {
    if (L.start_tod < 0) {
        char b[32];
        snprintf(b, sizeof(b), "+%.3f s", t);
        return b;
    }
    double a = L.start_tod + t;
    int days = (int)(a / 86400);
    a -= days * 86400.0;
    char b[48];
    snprintf(b, sizeof(b), "%s %02d:%02d:%06.3f", L.start_wday >= 0 ? kDays[(L.start_wday + days) % 7] : "",
             (int)(a / 3600), (int)fmod(a / 60, 60), fmod(a, 60));
    return b;
}

struct Run {
    Engine e;
    uint64_t t0_ms = 0, last_tick = 0;
    bool started = false;
    explicit Run(float cap) {
        Config c;
        c.enabled[kSrcBN] = false;
        c.enabled[kSrcKNY] = true;
        c.failsafe_cvl = 53.0f;
        c.failsafe_dvl = 47.0f;
        c.ccl_max = cap;
        c.dcl_max = cap;
        c.cap_a = c.cap_a_p2 = c.cap_b = cap;  // per-source caps (0.1.7) at the same value
        e.configure(c, 1000);
    }
    static uint64_t ms(double t) { return 1000 + (uint64_t)llround(t * 1000.0); }
    void until(double t) {
        uint64_t now = ms(t);
        if (!started) {
            last_tick = now - now % 100;
            started = true;
        }
        while (last_tick + 100 <= now) {
            last_tick += 100;
            e.tick(last_tick);
        }
    }
    void frame(const Row& r) {
        until(r.t);
        e.on_frame(kSrcKNY, r.id, r.d, r.dlc, ms(r.t));
        e.tick(ms(r.t));  // the output reacts at once (the firmware ticks every <= 100 ms)
    }
};

int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            g_pass++;                                       \
            printf("  PASS ");                              \
        } else {                                            \
            g_fail++;                                       \
            printf("  FAIL ");                              \
        }                                                   \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
    } while (0)

void inventory(const Log& L, double from, double to) {
    struct S {
        int n = 0;
        std::map<int, int> dlc;
        double last = -1, max_gap = 0, first = -1;
    };
    std::map<uint32_t, S> m;
    for (const Row& r : L.rows) {
        if (r.t < from || r.t > to) continue;
        S& s = m[r.id];
        s.n++;
        s.dlc[r.dlc]++;
        if (s.last >= 0 && r.t - s.last > s.max_gap) s.max_gap = r.t - s.last;
        if (s.first < 0) s.first = r.t;
        s.last = r.t;
    }
    printf("  ID     count  period  max gap  DLCs        last data\n");
    for (auto& [id, s] : m) {
        std::string dl;
        for (auto& [d, n] : s.dlc) dl += std::to_string(d) + (s.dlc.size() > 1 ? "x" + std::to_string(n) : "") + " ";
        const Row* last = nullptr;
        for (const Row& r : L.rows)
            if (r.id == id && r.t >= from && r.t <= to) last = &r;
        char data[32] = "";
        for (int k = 0; last && k < last->dlc; k++) snprintf(data + 3 * k, 4, "%02X ", last->d[k]);
        double per = s.n > 1 ? (s.last - s.first) / (s.n - 1) : 0;
        printf("  0x%03X %6d %6.2fs %7.2fs  %-11s %s\n", id, s.n, per, s.max_gap, dl.c_str(), data);
    }
}

void print_events(const Engine& e, uint32_t& seen, const Log& L) {
    Event ev[kEventRing];
    size_t n;
    while ((n = e.events_since(seen, ev, kEventRing)) > 0)
        for (size_t k = 0; k < n; k++) {
            seen = ev[k].seq;
            printf("    %s %-12s %s\n", wall(L, (ev[k].t_ms - 1000) / 1000.0).c_str(), ev_type_name(ev[k].type),
                   ev[k].text);
        }
}

void dump_src(const Engine& e, const Log& L, double t, const char* tag) {
    const Source& s = e.source(kSrcKNY);
    const Output& o = e.output();
    printf("  [%s] %s  KNY %s: CVL %.1f CCL %.1f DCL %.1f DVL %.1f SOC %.0f SOH %.0f V %.2f I %.1f T %.1f "
           "359 %08lX chg %d dis %d fc %d cells %.0f-%.0f mV T %.1f-%.1f cap %.0f%s\n"
           "      output %s: CVL %.1f CCL %.1f DCL %.1f DVL %.1f SOC %.1f (%s)\n",
           tag, wall(L, t).c_str(), src_state_name(s.state), s.cvl, s.ccl, s.dcl, s.dvl, s.soc, s.soh, s.voltage,
           s.current, s.temperature, (unsigned long)s.alarm_bits, s.charge_enable, s.discharge_enable, s.force_charge,
           s.cell_min_mv, s.cell_max_mv, s.t_min, s.t_max, s.capacity_ah, s.have_cap ? "" : " (setting)",
           out_state_name(o.state), o.cvl, o.ccl, o.dcl, o.dvl, o.soc, o.reason);
}

// Replays [from, to]; calls `at(t, run)` after every frame. Returns the run.
template <typename F>
void replay(const Log& L, Run& R, double from, double to, F at) {
    for (const Row& r : L.rows) {
        if (r.t < from) continue;
        if (r.t > to) break;
        R.frame(r);
        at(r);
    }
    R.until(to);
}

int kny_checks(const Log& L) {
    double w0, w1, t_975, t_zero;
    if (!parse_when(L, "Fri 13:14:00", w0) || !parse_when(L, "Fri 13:44:00", w1) ||
        !parse_when(L, "Fri 13:15:55", t_975) || !parse_when(L, "Fri 13:16:09", t_zero)) {
        printf("no usable 'date' header: wall-clock windows cannot be found\n");
        return 2;
    }
    double end = L.rows.back().t;
    printf("log: %zu frames, %s .. %s\n", L.rows.size(), wall(L, 0).c_str(), wall(L, end).c_str());
    if (w0 < 0 || w0 > end) printf("WARNING: Fri 13:14 is outside the log (offset %.0f s)\n", w0);

    printf("\n== ID inventory, first 300 s\n");
    inventory(L, 0, 300);
    printf("\n== ID inventory, Fri 13:14-13:44\n");
    inventory(L, w0, w1);

    for (float cap : {1000.0f, 100.0f}) {
        printf("\n== First 300 s, KNY only, user maximum %.0f A\n", cap);
        Run R(cap);
        uint32_t seen = 0;
        bool printed = false;
        float min_ccl = 1e9f, max_ccl = -1;
        replay(L, R, 0, 300, [&](const Row& r) {
            const Output& o = R.e.output();
            if (o.state == OutState::Normal && r.t > 75) {  // after the hold and the CVL rise hold
                min_ccl = fminf(min_ccl, o.ccl), max_ccl = fmaxf(max_ccl, o.ccl);
                if (!printed) {
                    dump_src(R.e, L, r.t, "first normal after 75 s");
                    printed = true;
                }
            }
        });
        print_events(R.e, seen, L);
        const Source& s = R.e.source(kSrcKNY);
        const Output& o = R.e.output();
        dump_src(R.e, L, 300, "at 300 s");
        CHECK(fabsf(s.ccl - 195) < 0.05f && fabsf(s.dcl - 195) < 0.05f && fabsf(s.cvl - 57.6f) < 0.05f,
              "decoded KNY CCL %.1f DCL %.1f CVL %.1f (expect 195 / 195 / 57.6)", s.ccl, s.dcl, s.cvl);
        float want = cap < 195 ? cap : 195;
        CHECK(o.state == OutState::Normal && fabsf(o.ccl - want) < 0.05f && fabsf(o.dcl - want) < 0.05f,
              "output %s CCL %.1f DCL %.1f (expect %.0f)", out_state_name(o.state), o.ccl, o.dcl, want);
        CHECK(fabsf(o.cvl - 57.6f) < 0.05f, "output CVL %.1f at 300 s (57.6 after the 60 s rise hold)", o.cvl);
        CHECK(min_ccl <= max_ccl && fabsf(min_ccl - want) < 0.05f && fabsf(max_ccl - want) < 0.05f,
              "output CCL in normal after 75 s: %.1f-%.1f", min_ccl, max_ccl);
        CHECK(s.plausibility_rejects == 0, "plausibility rejects %u", s.plausibility_rejects);
    }

    for (float cap : {1000.0f, 100.0f}) {
        printf("\n== Fri 13:14-13:44, KNY only, user maximum %.0f A\n", cap);
        Run R(cap);
        uint32_t seen = 0;
        bool d975 = false, dzero = false;
        double zero_frame_t = -1, out_zero_t = -1;
        bool nonzero_after = false;
        double nonzero_t = -1;
        float ccl_at_975 = -1;
        replay(L, R, w0, w1, [&](const Row& r) {
            const Source& s = R.e.source(kSrcKNY);
            const Output& o = R.e.output();
            if (!d975 && r.t >= t_975 && r.id == 0x351) {
                d975 = true;
                ccl_at_975 = o.ccl;
                dump_src(R.e, L, r.t, "first 0x351 from 13:15:55");
            }
            if (r.id == 0x351 && s.ccl == 0 && zero_frame_t < 0 && r.t >= t_975) zero_frame_t = r.t;
            if (zero_frame_t >= 0 && out_zero_t < 0 && o.ccl == 0) out_zero_t = r.t;
            if (!dzero && r.t >= t_zero + 1) {
                dzero = true;
                dump_src(R.e, L, r.t, "13:16:10");
            }
            if (zero_frame_t >= 0 && r.t > zero_frame_t && o.ccl != 0 && !nonzero_after) {
                nonzero_after = true;
                nonzero_t = r.t;
            }
        });
        print_events(R.e, seen, L);
        dump_src(R.e, L, w1, "13:44");
        float want975 = cap < 97.5f ? cap : 97.5f;
        CHECK(fabsf(ccl_at_975 - want975) < 0.05f, "13:15:55 output CCL %.1f (expect %.1f)", ccl_at_975, want975);
        CHECK(zero_frame_t >= 0 && fabsf((float)(zero_frame_t - t_zero)) < 2.0f,
              "KNY CCL 0 first at %s (expect about 13:16:09)", zero_frame_t >= 0 ? wall(L, zero_frame_t).c_str() : "never");
        CHECK(out_zero_t >= 0 && out_zero_t - zero_frame_t < 0.001,
              "output CCL 0 at the same frame (%.3f s later)", out_zero_t >= 0 ? out_zero_t - zero_frame_t : -1.0);
        CHECK(!nonzero_after, "output CCL stays 0 to 13:44%s",
              nonzero_after ? (" (non-zero at " + wall(L, nonzero_t) + ")").c_str() : "");
        const Source& s = R.e.source(kSrcKNY);
        CHECK(!s.charge_enable && (s.alarm_bits & 0xFF) == 0x02, "at 13:44: 0x35C charge disabled %d, 0x359 byte0 %02lX",
              !s.charge_enable, (unsigned long)(s.alarm_bits & 0xFF));
        CHECK(s.plausibility_rejects == 0, "plausibility rejects %u", s.plausibility_rejects);
    }

    // Staleness and 0x305 across the whole log.
    printf("\n== Whole log: staleness (5 s) and 0x305 gaps\n");
    {
        Run R(1000);
        uint32_t seen = 0;
        int stale = 0;
        double max305 = 0, last305 = -1;
        std::vector<std::string> stale_at;
        replay(L, R, 0, end, [&](const Row& r) {
            if (r.id == 0x305) {
                if (last305 >= 0) max305 = fmax(max305, r.t - last305);
                last305 = r.t;
            }
            Event ev[kEventRing];
            size_t n;
            while ((n = R.e.events_since(seen, ev, kEventRing)) > 0)
                for (size_t k = 0; k < n; k++) {
                    seen = ev[k].seq;
                    if (ev[k].type == EvType::Stale) {
                        stale++;
                        if (stale_at.size() < 10)
                            stale_at.push_back(wall(L, (ev[k].t_ms - 1000) / 1000.0) + " " + ev[k].text);
                    }
                }
        });
        // Required-frame gaps in the log itself (> 5 s means the engine must go stale there).
        double max_req_gap = 0;
        std::string worst;
        for (uint32_t id : {0x351u, 0x355u, 0x356u, 0x359u, 0x35Cu}) {
            double last = -1;
            for (const Row& r : L.rows)
                if (r.id == id) {
                    if (last >= 0 && r.t - last > max_req_gap) max_req_gap = r.t - last, worst = wall(L, last);
                    last = r.t;
                }
        }
        printf("  0x305: largest gap %.2f s; required frames: largest gap %.2f s (after %s); stale events %d\n",
               max305, max_req_gap, worst.c_str(), stale);
        for (auto& s : stale_at) printf("    stale: %s\n", s.c_str());
        CHECK(max_req_gap > 5.0 || stale == 0, "no stale event unless a required KNY frame really paused > 5 s");
        CHECK(max_req_gap <= 5.0 || stale > 0, "a pause > 5 s of a required frame is caught");
        printf("  0x305 is not watched by the engine: its gaps cannot change the output.\n");
        printf("  plausibility rejects over the whole log: %u\n", R.e.source(kSrcKNY).plausibility_rejects);
    }
    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.asc> --kny-checks | [--cap A] [--from T] [--to T] [-v]\n", argv[0]);
        return 2;
    }
    Log L;
    std::map<std::string, int> skipped;
    if (!load(argv[1], L, skipped) || L.rows.empty()) {
        fprintf(stderr, "cannot read frames from %s\n", argv[1]);
        return 2;
    }
    for (auto& [k, n] : skipped) printf("skipped %d x %s\n", n, k.c_str());
    float cap = 1000;
    double from = 0, to = L.rows.back().t;
    bool verbose = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--kny-checks")) return kny_checks(L);
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) parse_when(L, argv[++i], from);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) parse_when(L, argv[++i], to);
    }
    printf("log: %zu frames, %s .. %s; replay %s .. %s\n", L.rows.size(), wall(L, 0).c_str(),
           wall(L, L.rows.back().t).c_str(), wall(L, from).c_str(), wall(L, to).c_str());
    inventory(L, from, to);
    Run R(cap);
    uint32_t seen = 0;
    double next = from;
    replay(L, R, from, to, [&](const Row& r) {
        if (verbose && r.t >= next) {
            next = r.t + 60;
            dump_src(R.e, L, r.t, "minute");
        }
    });
    print_events(R.e, seen, L);
    dump_src(R.e, L, to, "end");
    return 0;
}
