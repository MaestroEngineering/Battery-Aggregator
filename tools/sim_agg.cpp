// Replays a CANable capture (analysis/data/can_*.csv.gz: t_wall,id_hex,dlc,data_hex) through
// the aggregation engine with the Blue Nova as the only source, and reports what the output
// would have done: state time, stale/trip events, CVL output changes per day.
//
//   g++ -std=c++17 -O2 -I components/agg/include tools/sim_agg.cpp components/agg/src/engine.cpp -o /tmp/sa
//   zcat analysis/data/can_*_p*.csv.gz | /tmp/sa [-v]
//
// Rows are processed in file order; rows with a time going backwards are skipped (the
// logger freeze burst). Gaps over 60 s are logger/host gaps, not BMS silence: each is counted
// separately so the stale events they cause can be told apart from real ones.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/engine.h"

using namespace agg;

int main(int argc, char** argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    // --watch <from_unix> <to_unix>: print the CCL computation every 5 s in that window.
    bool watch = argc > 3 && !strcmp(argv[1], "--watch");
    uint64_t watch_from = watch ? (uint64_t)(atof(argv[2]) * 1000) : 0, watch_to = watch ? (uint64_t)(atof(argv[3]) * 1000) : 0,
             next_watch = 0;
    if (watch) verbose = false;
    Engine e;
    Config c;
    c.enabled[kSrcBN] = true;
    c.failsafe_cvl = 53.0f;
    c.failsafe_dvl = 47.0f;
    c.ccl_max = 1000;
    c.dcl_max = 1000;
    char line[256];
    uint64_t t0 = 0, now = 0, last_tick = 0, next_dump = 0;
    uint64_t rows = 0, skipped = 0, gaps = 0, gaps_short = 0;
    uint64_t state_ms[5] = {};
    uint64_t ratio_ms[4] = {};  // output CCL / summed CCL in normal: >=98 %, 90-98, 75-90, <75
    uint32_t ev_seen = 0;
    uint32_t cnt[16] = {};
    uint32_t stale_near_gap = 0, stale_other = 0;
    uint64_t last_gap_end = 0;
    bool configured = false;
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 't') continue;  // header
        char* p = line;
        double tw = strtod(p, &p);
        if (*p != ',') continue;
        unsigned long id = strtoul(p + 1, &p, 16);
        if (*p != ',') continue;
        int dlc = (int)strtol(p + 1, &p, 10);
        if (*p != ',') continue;
        p++;
        uint8_t d[8] = {};
        for (int k = 0; k < dlc && k < 8; k++) {
            unsigned v;
            if (sscanf(p + 2 * k, "%2x", &v) != 1) break;
            d[k] = (uint8_t)v;
        }
        uint64_t t = (uint64_t)(tw * 1000.0);
        if (!configured) {
            t0 = now = last_tick = t;
            e.configure(c, t);
            configured = true;
        }
        if (t < now) {
            skipped++;
            continue;
        }
        if (t - now > 60000) {
            gaps++;
            last_gap_end = t;
        } else if (t - now > 2000) {
            gaps_short++;
        }
        // Tick every 100 ms of capture time up to this frame.
        while (last_tick + 100 <= t) {
            last_tick += 100;
            e.tick(last_tick);
            const Output& oo = e.output();
            state_ms[(int)oo.state] += 100;
            if (oo.state == OutState::Normal && oo.ccl_sum > 0) {
                float r = oo.ccl / oo.ccl_sum;
                ratio_ms[r >= 0.98f ? 0 : r >= 0.9f ? 1 : r >= 0.75f ? 2 : 3] += 100;
            }
        }
        now = t;
        e.on_frame(kSrcBN, (uint32_t)id, d, (uint8_t)dlc, t);
        rows++;
        // Events.
        Event ev[kEventRing];
        size_t n = e.events_since(ev_seen, ev, kEventRing);
        for (size_t k = 0; k < n; k++) {
            ev_seen = ev[k].seq;
            cnt[(int)ev[k].type]++;
            if (ev[k].type == EvType::Stale) {
                if (ev[k].t_ms - last_gap_end < 120000 || last_gap_end > ev[k].t_ms) stale_near_gap++;
                else stale_other++;
            }
            if (verbose || (ev[k].type != EvType::CvlChange && ev[k].type != EvType::State &&
                            ev[k].type != EvType::Recovered))
                printf("%13.3f %-12s %s\n", ev[k].t_ms / 1000.0, ev_type_name(ev[k].type), ev[k].text);
        }
        if (watch && t >= watch_from && t <= watch_to && t >= next_watch) {
            next_watch = t + 5000;
            const Output& o = e.output();
            const Source& b = e.source(kSrcBN);
            printf("%13.3f WATCH CCL out %.1f (sum %.1f, share cap %.1f) bank I %.1f, pack2 I %.2f / own %.1f, pack1 I %.2f / own %.1f\n",
                   t / 1000.0, o.ccl, o.ccl_sum, o.ccl_share_cap, b.current, b.p2_i, b.p2_ccl, b.current - b.p2_i,
                   b.ccl - b.p2_ccl);
        }
        if (verbose && t >= next_dump) {
            next_dump = t + 3600000;
            const Output& o = e.output();
            printf("%13.3f [%s] CVL %.1f (raw %.1f) CCL %.1f DCL %.1f SOC %.1f V %.2f I %.1f\n", t / 1000.0,
                   out_state_name(o.state), o.cvl, o.cvl_raw, o.ccl, o.dcl, o.soc, o.voltage, o.current);
        }
    }
    // Events after the last tick.
    double days = (now - t0) / 86400000.0;
    printf("\nrows %llu (skipped %llu out-of-order), span %.2f days, logger gaps > 60 s: %llu, 2-60 s: %llu\n",
           (unsigned long long)rows, (unsigned long long)skipped, days, (unsigned long long)gaps,
           (unsigned long long)gaps_short);
    for (int s = 0; s < 5; s++)
        if (state_ms[s]) printf("  output %-14s %10.2f h\n", out_state_name((OutState)s), state_ms[s] / 3600000.0);
    for (int k = 0; k <= (int)EvType::Config; k++)
        if (cnt[k]) printf("  events %-14s %u\n", ev_type_name((EvType)k), cnt[k]);
    printf("  stale events within 2 min after a logger gap > 60 s: %u, other stale events: %u\n", stale_near_gap,
           stale_other);
    double normal_days = state_ms[(int)OutState::Normal] / 86400000.0;
    if (normal_days > 0)
        printf("  CVL output changes per day in normal: %.1f\n", cnt[(int)EvType::CvlChange] / normal_days);
    printf("  plausibility rejects: %u\n", e.source(kSrcBN).plausibility_rejects);
    double tot = (ratio_ms[0] + ratio_ms[1] + ratio_ms[2] + ratio_ms[3]) / 100.0;
    if (tot > 0)
        printf("  output CCL / summed CCL in normal: >=98 %% %.1f%%, 90-98 %% %.1f%%, 75-90 %% %.1f%%, <75 %% %.1f%%\n",
               ratio_ms[0] / tot, ratio_ms[1] / tot, ratio_ms[2] / tot, ratio_ms[3] / tot);
    return 0;
}
