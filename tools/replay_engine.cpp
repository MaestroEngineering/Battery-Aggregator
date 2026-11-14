// Feeds a frame list (receiver-export replay, tools/test_replay_exports.py) through the firmware's aggregation engine (components/agg/src/engine.cpp)
// and writes the reconstructed inverter output once per second plus the engine events.
//
//   g++ -std=c++17 -O2 -I components/agg/include tools/replay_engine.cpp
//       components/agg/src/engine.cpp -o /tmp/re
//   /tmp/re <frames.csv> <out_prefix> [ccl_max] [dcl_max] [fs_cvl] [fs_dvl] [cap_a] [cap_a_p2] [cap_b]
//
// frames.csv: t_ms,src,id,dlc,hex,seg   (src 0 = BN/port A, 1 = KNY/port B; sorted by t_ms;
// a new seg value restarts the engine, as a device reboot would). Writes <out>_out.csv and
// <out>_events.csv. Nothing here talks to any device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/engine.h"

using namespace agg;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* in = fopen(argv[1], "r");
    if (!in) return 2;
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s_out.csv", argv[2]);
    snprintf(p2, sizeof(p2), "%s_events.csv", argv[2]);
    FILE* fo = fopen(p1, "w");
    FILE* fe = fopen(p2, "w");
    Config c;
    c.enabled[kSrcBN] = c.enabled[kSrcKNY] = true;
    c.ccl_max = argc > 3 ? (float)atof(argv[3]) : 100;
    c.dcl_max = argc > 4 ? (float)atof(argv[4]) : 100;
    c.failsafe_cvl = argc > 5 ? (float)atof(argv[5]) : 53.0f;
    c.failsafe_dvl = argc > 6 ? (float)atof(argv[6]) : 47.0f;
    c.kny_capacity_ah = 300;
    if (argc > 7) c.cap_a = (float)atof(argv[7]);
    if (argc > 8) c.cap_a_p2 = (float)atof(argv[8]);
    if (argc > 9) c.cap_b = (float)atof(argv[9]);
    fprintf(fo, "t_ms,seg,state,cvl,ccl,dcl,dvl,soc,soh,v,i,cap,cvl_raw,ccl_sum,dcl_sum,ccl_cap,dcl_cap,alarm,"
                "bn_state,kny_state,bn_cap,g_a_c,g_a_d,g_p2_c,g_p2_d,g_b_c,g_b_d,bn_rej,kny_rej,reason\n");
    fprintf(fe, "t_ms,seg,type,source,text\n");
    Engine* e = nullptr;
    long seg = -1;
    uint64_t last_tick = 0, next_out = 0;
    uint32_t seen = 0;
    char line[256];
    auto flush_events = [&]() {
        Event ev[kEventRing];
        size_t n;
        while ((n = e->events_since(seen, ev, kEventRing)) > 0)
            for (size_t k = 0; k < n; k++) {
                seen = ev[k].seq;
                fprintf(fe, "%llu,%ld,%s,%d,\"%s\"\n", (unsigned long long)ev[k].t_ms, seg, ev_type_name(ev[k].type),
                        ev[k].source, ev[k].text);
            }
    };
    auto tick_to = [&](uint64_t t) {
        while (last_tick + 100 <= t) {
            last_tick += 100;
            e->tick(last_tick);
            if (last_tick >= next_out) {
                next_out = last_tick + 1000;
                const Output& o = e->output();
                fprintf(fo, "%llu,%ld,%s,%.1f,%.1f,%.1f,%.1f,%.2f,%.0f,%.2f,%.1f,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%lu,%s,%s,%.0f,"
                            "%d,%d,%d,%d,%d,%d,%lu,%lu,\"%s\"\n",
                        (unsigned long long)last_tick, seg, out_state_name(o.state), o.cvl, o.ccl, o.dcl, o.dvl, o.soc,
                        o.soh, o.voltage, o.current, o.capacity_ah, o.cvl_raw, o.ccl_sum, o.dcl_sum, o.ccl_share_cap,
                        o.dcl_share_cap, (unsigned long)o.alarm_bits, src_state_name(e->source(0).state),
                        src_state_name(e->source(1).state), e->source(0).capacity_ah, (int)o.guard[0][0],
                        (int)o.guard[0][1], (int)o.guard[1][0], (int)o.guard[1][1], (int)o.guard[2][0], (int)o.guard[2][1],
                        (unsigned long)e->source(0).rejected_frames, (unsigned long)e->source(1).rejected_frames, o.reason);
            }
            flush_events();
        }
    };
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == 't') continue;
        unsigned long long t;
        int src, dlc;
        unsigned id;
        char hex[32] = "";
        long sg;
        if (sscanf(line, "%llu,%d,%x,%d,%31[^,],%ld", &t, &src, &id, &dlc, hex, &sg) != 6) continue;
        if (sg != seg) {
            if (e) flush_events();
            delete e;
            e = new Engine();
            seg = sg;
            seen = 0;
            e->configure(c, t);
            last_tick = t - t % 100;
            next_out = last_tick;
        }
        tick_to(t);
        uint8_t d[8] = {};
        for (int k = 0; k < dlc && k < 8; k++) {
            unsigned v;
            if (sscanf(hex + 2 * k, "%2x", &v) == 1) d[k] = (uint8_t)v;
        }
        e->on_frame(src, id, d, (uint8_t)dlc, t);
    }
    if (e) flush_events();
    fclose(fo);
    fclose(fe);
    return 0;
}
