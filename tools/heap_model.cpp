// Host heap model: free heap and largest free block over 10 minutes with the Dashboard,
// Aggregator and CAN pages open in three browser tabs, MQTT (discovery + state every 10 s) and
// Verbose Diagnostics uploading. Compares 0.1.7 (cJSON tree for /api/agg/status) with 0.1.8
// (streamed replies), and the Verbose Diagnostics batch-buffer options.
//
//   g++ -std=c++17 -O2 -I tools/hoststub -I components/agg/include -I components/can/include
//       -I components/config/include -I components/webserver/src -I components/hamqtt/src
//       -I managed_components/espressif__cjson/cJSON tools/heap_model.cpp
//       components/webserver/src/agg_status_json.cpp components/webserver/src/can_status_json.cpp
//       components/hamqtt/src/discovery_json.cpp components/agg/src/engine.cpp
//       managed_components/espressif__cjson/cJSON/cJSON.c -o /tmp/hm
//   /tmp/hm [frames.csv]   (engine input from tools/test_replay_exports.py --write-input; synthetic otherwise)
//
// MEASURED here by running the firmware's own code (allocation sizes recorded):
//   /api/agg/status 0.1.8 (one AggStatusData, 32-bit size) and 0.1.7 (the cJSON tree, from the
//   figures measured with the real cJSON on the 13/14 Nov exports: 49 KB normal, 66-72 KB with
//   32 events), /api/can/status writer (no heap), the Settings copy behind cfg::get() in
//   /api/status, MQTT discovery payloads (real discovery_payload + cJSON) and topic strings.
// ASSUMED (not measurable on a PC; parameters below, conservative):
//   WiFi dynamic RX/TX buffers in flight, lwIP TCP send buffers (until ACK), esp_http_server
//   per-request memory, the Verbose Diagnostics HTTP client, MQTT TCP pbufs.
// The allocator is best-fit with coalescing over the free blocks the device reported (0.1.7:
// ~36 KB free, largest ~34 KB, with Verbose Diagnostics' 36 KB buffers allocated). The 0.1.8
// static RAM is 288 B smaller than 0.1.7's (not credited).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "agg_status_json.h"
#include "can_status_json.h"
#include "cJSON.h"
#include "config/config.h"
#include "discovery_json.h"

namespace can {
const char* state_name(State) { return "listen-only"; }
const char* intmap_result_name(IntMapResult) { return "ok"; }
}  // namespace can

// ---- assumptions (bytes, ms) ----
constexpr int kWifiBuf = 1700;          // one dynamic WiFi RX/TX buffer incl. overhead
constexpr int kWifiSteady = 2;          // buffers in flight at any time
constexpr int kWifiBurst = 4;           // extra while a reply/upload is on the air
constexpr int kTcpSnd = 5760;           // CONFIG_LWIP_TCP_SND_BUF_DEFAULT: unACKed data per socket
constexpr int kHttpdReq = 1400;         // esp_http_server per request (scratch, aux, headers)
constexpr int kDiagClient = 4500;       // esp_http_client + socket for one upload
constexpr int kPbufOver = 64;           // pbuf header per TCP segment
constexpr int kAggData32 = 5100;        // sizeof(webserver::AggStatusData) on the ESP32 (g++ -m32)

// ---- allocator ----
struct Heap {
    std::map<long, long> freeb;  // addr -> size
    std::map<long, long> used;   // addr -> size
    long fails = 0;
    explicit Heap(const std::vector<long>& blocks) {
        long a = 0;
        for (long b : blocks) freeb[a] = b, a += b + 100000;  // separate regions never merge
    }
    static long rnd(long n) { return std::max<long>(16, (n + 3) & ~3L) + 4; }
    long alloc(long n) {
        long need = rnd(n), best = -1, bs = 0;
        for (auto& f : freeb)
            if (f.second >= need && (best < 0 || f.second < bs)) best = f.first, bs = f.second;
        if (best < 0) {
            fails++;
            return -1;
        }
        freeb.erase(best);
        if (bs - need >= 16) freeb[best + need] = bs - need;
        else need = bs;
        used[best] = need;
        return best;
    }
    void release(long a) {
        if (a < 0) return;
        auto it = used.find(a);
        if (it == used.end()) return;
        long s = it->second;
        used.erase(it);
        auto nx = freeb.lower_bound(a);
        if (nx != freeb.end() && nx->first == a + s) s += nx->second, freeb.erase(nx);
        auto pv = freeb.lower_bound(a);
        if (pv != freeb.begin()) {
            --pv;
            if (pv->first + pv->second == a) {
                pv->second += s;
                return;
            }
        }
        freeb[a] = s;
    }
    long total() const {
        long t = 0;
        for (auto& f : freeb) t += f.second;
        return t;
    }
    long largest() const {
        long m = 0;
        for (auto& f : freeb) m = std::max(m, f.second);
        return m;
    }
};

// ---- measured allocation patterns from the firmware code ----
static std::vector<long> g_rec;
static bool g_recording = false;
static void* rec_malloc(size_t n) {
    if (g_recording) g_rec.push_back((long)n);
    return malloc(n);
}
static void rec_free(void* p) { free(p); }

// Sizes (in order) of the allocations one MQTT discovery payload makes (cJSON tree + print).
static std::vector<std::vector<long>> discovery_allocs(int nsensors) {
    std::vector<std::vector<long>> out;
    cJSON_Hooks h = {rec_malloc, rec_free};
    cJSON_InitHooks(&h);
    for (int i = 0; i < nsensors; i++) {
        char uid[64], st[96];
        snprintf(uid, sizeof(uid), "bmsagg_06a1b2_battery_a_pack2_energy_out_%02d", i);
        snprintf(st, sizeof(st), "bmsagg/battery_a/pack2_energy_out_%02d", i);
        hamqtt::DiscoveryInfo d = {"A pack 2 energy out (V x I)", uid, st, "bmsagg/availability", "kWh", "energy",
                                   "total", "bmsagg_06a1b2", "0.1.8"};
        g_rec.clear();
        g_recording = true;
        char* p = hamqtt::discovery_payload(d);
        g_recording = false;
        cJSON_free(p);
        out.push_back(g_rec);
    }
    cJSON_Hooks def = {malloc, free};
    cJSON_InitHooks(&def);
    return out;
}

// Heap allocations of one cfg::get() copy with realistic live settings (libstdc++ SSO: 15 chars).
static long g_new_count = 0, g_new_bytes = 0;
static bool g_count_new = false;
void* operator new(size_t n) {
    if (g_count_new) g_new_count++, g_new_bytes += (long)n;
    void* p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static std::vector<long> settings_copy_allocs() {
    cfg::Settings s;
    s.hostname = "bms-agg";
    s.mqtt_host = "192.168.0.10";
    s.mqtt_user = "homeassistant";
    s.mqtt_base = "bmsagg";
    s.diag_url = "http://192.168.0.7:8088/ingest";
    s.trig_baseline = "";
    g_new_count = g_new_bytes = 0;
    g_count_new = true;
    cfg::Settings c = s;
    g_count_new = false;
    (void)c;
    std::vector<long> v;
    if (g_new_count) v.assign((size_t)g_new_count, g_new_bytes / g_new_count);
    return v;
}

static bool null_sink(void* ctx, const char*, size_t n) {
    *(size_t*)ctx += n;
    return true;
}

// ---- simulation ----
struct Ev {
    long t_ms, dur_ms;
    std::vector<long> sizes;  // allocated at t (in order), all freed at t + dur
    int kind;                 // for failure attribution
};
enum Kind { K_AGG, K_CAN, K_DASH, K_MQTT_DISC, K_MQTT_STATE, K_DIAG, K_WIFI, K_N };
static const char* kKindName[K_N] = {"/api/agg/status", "/api/can/status+table", "/api/status", "MQTT discovery",
                                     "MQTT state", "diag upload", "WiFi buffers"};

struct Result {
    long min_free, min_largest, fails[K_N];
    double avg_free;
};

static Result simulate(bool v017, long base_large, long base_small, size_t agg_reply, size_t can_reply,
                       size_t table_reply, const std::vector<std::vector<long>>& disc, const std::vector<long>& setc,
                       int agg_017_bytes, bool print_series) {
    std::vector<Ev> ev;
    const long T = 600000;
    auto pb = [&](size_t bytes) {  // TCP pbufs for a reply (up to the send buffer)
        std::vector<long> v;
        long left = (long)std::min<size_t>(bytes, kTcpSnd);
        while (left > 0) v.push_back(std::min<long>(left, 1436) + kPbufOver), left -= 1436;
        return v;
    };
    for (long t = 0; t < T; t += 2000) {  // Aggregator tab
        Ev e{t, 40, {kHttpdReq}, K_AGG};
        if (v017) {
            // As measured with the real cJSON: ~770 tree nodes/keys/strings (normal: 49 KB peak incl.
            // printing; 1115 allocations / 72 KB with 32 events), then the print buffer, which
            // cJSON grows by doubling up to ~2x the reply.
            long print = agg_017_bytes > 60000 ? 17800 : 11600, nodes = agg_017_bytes > 60000 ? 1115 : 770;
            long each = (agg_017_bytes - print) / nodes;
            for (long k = 0; k < nodes; k++) e.sizes.push_back(each > 16 ? each - 8 : 8);
            e.sizes.push_back(print);
        } else {
            e.sizes.push_back(kAggData32);
        }
        auto p = pb(agg_reply);
        e.sizes.insert(e.sizes.end(), p.begin(), p.end());
        for (int k = 0; k < kWifiBurst; k++) e.sizes.push_back(kWifiBuf);
        ev.push_back(e);
    }
    for (long t = 0; t < T; t += 2000) {  // Dashboard tab
        Ev e{t + 60, 30, {kHttpdReq}, K_DASH};
        e.sizes.insert(e.sizes.end(), setc.begin(), setc.end());
        e.sizes.push_back(40);  // now_str()
        if (v017) e.sizes.push_back(3200);  // its cJSON tree (36 keys + 10 marks) + print
        auto p = pb(1400);
        e.sizes.insert(e.sizes.end(), p.begin(), p.end());
        ev.push_back(e);
    }
    for (long t = 0; t < T; t += 1000) {  // CAN tab: status + table
        Ev e{t + 120, 40, {kHttpdReq}, K_CAN};
        if (v017) e.sizes.push_back(6500);  // its cJSON tree + print
        auto p = pb(can_reply + table_reply);
        e.sizes.insert(e.sizes.end(), p.begin(), p.end());
        ev.push_back(e);
    }
    {  // MQTT discovery after connecting (t = 5 s), one payload every 20 ms
        long t = 5000;
        for (auto& d : disc) {
            Ev e{t, 15, {64, 48, 48, 80}, K_MQTT_DISC};  // uid, state topic, obj, config topic strings
            for (long s : d) e.sizes.push_back(s);
            e.sizes.push_back(700 + kPbufOver);  // the message in TCP until ACK
            ev.push_back(e);
            t += 20;
        }
    }
    for (long t = 7000; t < T; t += 10000) {  // MQTT state every 10 s: ~65 small publishes in ~0.3 s
        for (int k = 0; k < 65; k++) ev.push_back(Ev{t + k * 5, 4, {40, 48, 120}, K_MQTT_STATE});
    }
    for (long t = 15000; t < T; t += 30000) {  // Verbose Diagnostics upload (change-only, every 30 s)
        Ev e{t, 300, {kDiagClient}, K_DIAG};
        auto p = pb(500 * 24);
        e.sizes.insert(e.sizes.end(), p.begin(), p.end());
        for (int k = 0; k < kWifiBurst; k++) e.sizes.push_back(kWifiBuf);
        ev.push_back(e);
    }
    std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t_ms < b.t_ms; });

    Heap h({base_large, base_small});
    std::vector<long> steady;
    for (int k = 0; k < kWifiSteady; k++) steady.push_back(h.alloc(kWifiBuf));
    std::multimap<long, long> frees;  // time -> addr
    Result r = {};
    r.min_free = r.min_largest = 1L << 40;
    double sum = 0;
    long nsamp = 0;
    size_t i = 0;
    long next_print = 60000, mf = 1L << 40, ml = 1L << 40;
    for (long t = 0; t < T; t++) {
        while (!frees.empty() && frees.begin()->first <= t) h.release(frees.begin()->second), frees.erase(frees.begin());
        while (i < ev.size() && ev[i].t_ms <= t) {
            for (long s : ev[i].sizes) {
                long a = h.alloc(s);
                if (a < 0) r.fails[ev[i].kind]++;
                else frees.insert({t + ev[i].dur_ms, a});
            }
            i++;
        }
        long fr = h.total(), lg = h.largest();
        r.min_free = std::min(r.min_free, fr);
        r.min_largest = std::min(r.min_largest, lg);
        sum += fr, nsamp++;
        mf = std::min(mf, fr), ml = std::min(ml, lg);
        if (print_series && t + 1 >= next_print) {
            printf("      minute %2ld: lowest free %5.1f KB, lowest largest block %5.1f KB, idle free %5.1f KB\n",
                   next_print / 60000, mf / 1024.0, ml / 1024.0, fr / 1024.0);
            next_print += 60000, mf = ml = 1L << 40;
        }
    }
    r.avg_free = sum / nsamp;
    return r;
}

int main(int argc, char** argv) {
    // Engine state for realistic reply sizes (real data when given).
    agg::Engine e;
    agg::Config c;
    c.enabled[0] = c.enabled[1] = true;
    c.failsafe_cvl = 53, c.failsafe_dvl = 47, c.ccl_max = c.dcl_max = 100, c.kny_capacity_ah = 300;
    size_t agg_reply = 0;
    if (argc > 1) {
        FILE* f = fopen(argv[1], "r");
        if (!f) return 2;
        char line[256];
        if (!fgets(line, sizeof(line), f)) return 2;
        uint64_t base = 0, last = 0;
        long n = 0;
        while (fgets(line, sizeof(line), f) && n < 400000) {
            unsigned long long t;
            int src, dlc;
            unsigned id;
            char hx[40];
            long seg;
            if (sscanf(line, "%llu,%d,%X,%d,%39[^,],%ld", &t, &src, &id, &dlc, hx, &seg) != 6) continue;
            if (!base) base = t, last = t, e.configure(c, t);
            while (last + 100 <= t) last += 100, e.tick(last);
            uint8_t d[8] = {};
            for (int k = 0; k < dlc && k < 8 && hx[2 * k] && hx[2 * k + 1]; k++) {
                unsigned b;
                sscanf(hx + 2 * k, "%2X", &b);
                d[k] = (uint8_t)b;
            }
            e.on_frame(src, id, d, (uint8_t)dlc, t);
            n++;
        }
        fclose(f);
        static webserver::AggStatusData sd;
        sd.snap.out = e.output();
        for (int k = 0; k < agg::kSources; k++) sd.snap.src[k] = e.source(k);
        sd.snap.cfg = e.config();
        sd.nev = e.events_since(0, sd.ev, agg::kEventRing);
        webserver::AggStatusExtras x;
        JsonOut j(null_sink, &agg_reply);
        webserver::write_agg_status(j, sd, x, last);
    } else {
        agg_reply = 5500;
    }
    size_t can_reply = 0;
    {
        can::Status cs;
        JsonOut j(null_sink, &can_reply);
        webserver::write_can_status(j, cs, 1536, 0);
    }
    const size_t table_reply = 40 * 150;  // ~40 IDs on the live buses x ~150 bytes per row
    auto disc = discovery_allocs(72);
    long disc_peak = 0;
    for (auto& d : disc) {
        long s = 0;
        for (long x : d) s += Heap::rnd(x);
        disc_peak = std::max(disc_peak, s);
    }
    auto setc = settings_copy_allocs();

    printf("measured: /api/agg/status reply %zu B (0.1.8 heap: one %d B block); /api/can/status reply %zu B "
           "(no heap); MQTT discovery payload <= %ld B of heap each (72 sensors); cfg::get() copy: %zu "
           "allocations\n\n",
           agg_reply, kAggData32, can_reply, disc_peak, setc.size());

    struct Opt {
        const char* name;
        long extra;  // heap given back by a smaller Verbose Diagnostics buffer
    } opts[] = {{"diag batch 500 (36 KB buffers, as now)", 0},
                {"diag batch 250 (18 KB)", 18000},
                {"diag batch 125 (9 KB)", 27000}};
    const long base_large = 34 * 1024, base_small = 2 * 1024;  // device report for 0.1.7 (with diag on)
    printf("10 min, Dashboard + Aggregator + CAN tabs, MQTT on, Verbose Diagnostics uploading\n");
    printf("(start: free %.1f KB, largest %.1f KB, as reported on the device)\n\n", (base_large + base_small) / 1024.0,
           base_large / 1024.0);
    for (int v = 0; v < 2; v++) {
        for (const Opt& o : opts) {
            if (v == 0 && o.extra) continue;
            for (int agg017 : {49200, 72448}) {
                if (v == 1 && agg017 != 49200) continue;
                printf("  %s, %s%s\n", v ? "0.1.8" : "0.1.7", o.name,
                       v ? "" : (agg017 > 50000 ? ", 32 events stored (72 KB tree)" : ", normal (49 KB tree)"));
                Result r = simulate(v == 0, base_large + o.extra, base_small, agg_reply, can_reply, table_reply, disc,
                                    setc, agg017, v == 1 && o.extra == 0);
                printf("    lowest free %.1f KB, lowest largest block %.1f KB, average free %.1f KB\n",
                       r.min_free / 1024.0, r.min_largest / 1024.0, r.avg_free / 1024.0);
                long tf = 0;
                for (int k = 0; k < K_N; k++) tf += r.fails[k];
                if (!tf) printf("    allocation failures: none\n");
                for (int k = 0; k < K_N; k++)
                    if (r.fails[k]) printf("    allocation failures: %-22s %ld\n", kKindName[k], r.fails[k]);
            }
        }
    }
    return 0;
}
