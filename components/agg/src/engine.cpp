#include "agg/engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace agg {
namespace {

uint16_t le16(const uint8_t* d, int k) { return (uint16_t)(d[k] | d[k + 1] << 8); }
int16_t sle16(const uint8_t* d, int k) { return (int16_t)le16(d, k); }
uint16_t be16(const uint8_t* d, int k) { return (uint16_t)(d[k] << 8 | d[k + 1]); }
int16_t sbe16(const uint8_t* d, int k) { return (int16_t)be16(d, k); }
uint32_t le32(const uint8_t* d, int k) {
    return (uint32_t)d[k] | (uint32_t)d[k + 1] << 8 | (uint32_t)d[k + 2] << 16 | (uint32_t)d[k + 3] << 24;
}
uint32_t be32(const uint8_t* d, int k) {
    return (uint32_t)d[k] << 24 | (uint32_t)d[k + 1] << 16 | (uint32_t)d[k + 2] << 8 | (uint32_t)d[k + 3];
}
bool in(float v, float lo, float hi) { return v >= lo && v <= hi; }

// Blue Nova 0x35A: constant across 5.4 days of capture (bit meanings unknown).
constexpr uint8_t kBn35ABaseline[8] = {0xA8, 0xAA, 0x82, 0x02, 0xA8, 0xAA, 0x02, 0x02};

// Pylontech V1.3 0x359 bits that have a defined meaning. Any other bit set is an
// unknown flag: it trips and latches (SPEC.md rule 3).
constexpr uint32_t kPylonDefined = (0x02u | 0x04u | 0x08u | 0x10u | 0x80u) |  // byte 0 protection
                                   (0x01u | 0x08u) << 8 |                      // byte 1 protection
                                   (0x02u | 0x04u | 0x08u | 0x10u | 0x80u) << 16 |  // byte 2 alarm
                                   (0x01u | 0x08u) << 24;                      // byte 3 alarm

// Plausibility ranges (SPEC.md rule 4: outside = missing). 16S LFP, 48 V class.
constexpr float kVmin = 40.0f, kVmax = 60.0f;      // pack voltage and CVL
constexpr float kDvlMin = 36.0f;                    // DVL
constexpr float kImax = 1000.0f;                    // current, limits
constexpr float kTmin = -40.0f, kTmax = 90.0f;
constexpr float kShareMinA = 20.0f;          // pack shares are learned at >= 20 A bank current
constexpr uint64_t kClampEventMs = 10000;    // clamp events are debounced
constexpr uint8_t kLatchConfirm = 3;          // identical deviating frames needed to latch a trip
constexpr uint64_t kRejectEventMs = 60000;    // rejected-frame events: at most one per minute per source

// Per-source frame validation: the IDs (and DLCs) each source is known to send. Anything else is
// counted and ignored (never decoded, never a trip). Verified captures: analysis/2026-11-05 (BN,
// 6 days) and analysis/2026-11-13/14 (BN + KNY). 0x305 is the inverter's keep-alive, which can be on
// either battery bus. KNY: DLC 8 (seen) or the Pylontech V1.3 lengths.
struct AllowedId {
    uint16_t id;
    uint16_t dlc_mask;  // bit n = DLC n allowed
};
constexpr uint16_t D(int a) { return (uint16_t)(1u << a); }
constexpr AllowedId kBnIds[] = {
    {0x351, D(8)}, {0x355, D(6) | D(8)}, {0x356, D(6) | D(8)}, {0x35A, D(8)}, {0x35E, D(8)}, {0x35F, D(4)},
    {0x370, D(8)}, {0x372, D(8)}, {0x373, D(8)}, {0x374, D(8)}, {0x375, D(8)}, {0x376, D(8)}, {0x377, D(8)},
    {0x378, D(8)}, {0x379, D(2) | D(8)}, {0x210, D(8)}, {0x211, D(8)}, {0x212, D(8)}, {0x213, D(8)},
    {0x214, D(8)}, {0x215, D(8)}, {0x216, D(8)}, {0x217, D(8)}, {0x305, D(8)}};
constexpr AllowedId kKnyIds[] = {
    {0x351, D(8)}, {0x355, D(4) | D(8)}, {0x356, D(6) | D(8)}, {0x359, D(7) | D(8)}, {0x35C, D(2) | D(8)},
    {0x35E, D(8)}, {0x373, D(8)}, {0x379, D(2) | D(8)}, {0x305, D(8)}};

int16_t clamp16(float v) {
    long r = lrintf(v);
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}
uint16_t clampu16(float v) {
    long r = lrintf(v);
    if (r > 65535) r = 65535;
    if (r < 0) r = 0;
    return (uint16_t)r;
}
void put16(uint8_t* d, int k, uint16_t v) {
    d[k] = (uint8_t)(v & 0xFF);
    d[k + 1] = (uint8_t)(v >> 8);
}

}  // namespace

const char* src_state_name(SrcState s) {
    switch (s) {
        case SrcState::Disabled: return "disabled";
        case SrcState::NotSeen: return "not seen";
        case SrcState::Holding: return "holding";
        case SrcState::Ok: return "ok";
        case SrcState::Stale: return "stale";
        case SrcState::Invalid: return "invalid";
        case SrcState::Tripped: return "tripped";
    }
    return "?";
}

const char* out_state_name(OutState s) {
    switch (s) {
        case OutState::Refused: return "refused";
        case OutState::Silent: return "silent";
        case OutState::StartupHold: return "start-up hold";
        case OutState::FailSafe: return "fail-safe";
        case OutState::Normal: return "normal";
    }
    return "?";
}

const char* guard_state_name(GuardState g) {
    switch (g) {
        case GuardState::Ok: return "ok";
        case GuardState::Derated: return "derated 50%";
        case GuardState::Zero: return "zero";
    }
    return "?";
}

const char* guard_name(int g) {
    return g == 0 ? "A (BN bank)" : g == 1 ? "A pack 2" : g == 2 ? "B (KNY)" : "?";
}

const char* ev_type_name(EvType t) {
    switch (t) {
        case EvType::State: return "state";
        case EvType::Stale: return "stale";
        case EvType::Recovered: return "recovered";
        case EvType::NotSeen: return "not_seen";
        case EvType::Trip: return "trip";
        case EvType::TripCleared: return "trip_cleared";
        case EvType::Alarm: return "alarm";
        case EvType::LimitClamp: return "limit_clamp";
        case EvType::CvlChange: return "cvl_change";
        case EvType::Plausibility: return "plausibility";
        case EvType::Config: return "config";
        case EvType::CapacityStep: return "capacity_step";
        case EvType::FrameRejected: return "frame_rejected";
        case EvType::OvercurrentDerate: return "overcurrent_derate";
    }
    return "?";
}

bool config_valid(const Config& c, const char** why) {
    const char* w = nullptr;
    if (!c.enabled[0] && !c.enabled[1]) w = "no battery source enabled";
    else if (!in(c.failsafe_cvl, kVmin, kVmax)) w = "fail-safe CVL not set (40.0-60.0 V)";
    else if (!in(c.failsafe_dvl, kDvlMin, kVmax)) w = "fail-safe DVL not set (36.0-60.0 V)";
    else if (c.failsafe_dvl >= c.failsafe_cvl) w = "fail-safe DVL must be below fail-safe CVL";
    else if (!in(c.ccl_max, 1.0f, kImax)) w = "CCL maximum not set (1-1000 A)";
    else if (!in(c.dcl_max, 1.0f, kImax)) w = "DCL maximum not set (1-1000 A)";
    else if (c.enabled[kSrcKNY] && !in(c.kny_capacity_ah, 1.0f, 2000.0f)) w = "KNY capacity must be 1-2000 Ah";
    else if (c.timeout_1hz_ms < kMinTimeoutMs || c.timeout_1hz_ms > kMaxTimeout1HzMs) w = "1 Hz timeout must be 2-5 s";
    else if (c.timeout_pack2_ms < kMinTimeoutMs || c.timeout_pack2_ms > kMaxTimeoutPack2Ms)
        w = "pack-2 timeout must be 2-6 s";
    else if (c.startup_hold_ms < kMinStartupHoldMs || c.startup_hold_ms > kMaxStartupHoldMs)
        w = "start-up hold must be 5-300 s";
    else if (c.cvl_rise_hold_ms > kMaxCvlHoldMs) w = "CVL rise hold must be 0-600 s";
    else if (!in(c.cap_a, 1.0f, kImax) || !in(c.cap_a_p2, 1.0f, kImax) || !in(c.cap_b, 1.0f, kImax))
        w = "per-source caps must be 1-1000 A";
    if (why) *why = w ? w : "";
    return w == nullptr;
}

Engine::Engine() {
    for (int i = 0; i < kSources; i++) reset_source(i);
    memset(ev_, 0, sizeof(ev_));
}

void Engine::reset_source(int i) {
    Source& s = src_[i];
    s = Source();
    static const uint16_t kBn1Hz[] = {0x351, 0x355, 0x356, 0x35A, 0x35F, 0x373, 0x374,
                                      0x375, 0x376, 0x377, 0x378, 0x379};
    static const uint16_t kBnPack2[] = {0x211, 0x212, 0x213, 0x214, 0x215, 0x216, 0x217};
    static const uint16_t kKny[] = {0x351, 0x355, 0x356, 0x359, 0x35C};
    if (i == kSrcBN) {
        for (uint16_t id : kBn1Hz) s.req[s.nreq++] = {id, false, 0, false};
        for (uint16_t id : kBnPack2) s.req[s.nreq++] = {id, true, 0, false};
    } else {
        for (uint16_t id : kKny) s.req[s.nreq++] = {id, false, 0, false};
    }
}

void Engine::event(EvType t, int src, uint64_t now, const char* fmt, ...) {
    Event& e = ev_[ev_seq_ % kEventRing];
    e.seq = ++ev_seq_;
    e.t_ms = now;
    e.type = t;
    e.source = (int8_t)src;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    va_end(ap);
}

size_t Engine::events_since(uint32_t after, Event* out, size_t max) const {
    uint32_t first = ev_seq_ > (uint32_t)kEventRing ? ev_seq_ - kEventRing + 1 : 1;
    if (after + 1 > first) first = after + 1;
    size_t n = 0;
    for (uint32_t q = first; q <= ev_seq_ && n < max; q++) out[n++] = ev_[(q - 1) % kEventRing];
    return n;
}

void Engine::configure(const Config& c, uint64_t now) {
    bool changed_sources = c.enabled[0] != cfg_.enabled[0] || c.enabled[1] != cfg_.enabled[1];
    cfg_ = c;
    if (changed_sources)
        for (int i = 0; i < kSources; i++)
            if (!c.enabled[i]) reset_source(i);
    const char* why = "";
    bool ok = config_valid(c, &why);
    if (!cfg_logged_ || why != last_why_) {  // log only when the verdict changes
        if (!ok) event(EvType::Config, -1, now, "output refused: %s", why);
        else event(EvType::Config, -1, now, "settings complete");
        cfg_logged_ = true;
        last_why_ = why;
    }
    tick(now);
}

void Engine::set_ota(bool active, uint64_t now) {
    ota_ = active;
    tick(now);
}

void Engine::clear_trips(uint64_t now) {
    for (int i = 0; i < kSources; i++) {
        Source& s = src_[i];
        if (s.latched) event(EvType::TripCleared, i, now, "latched trip cleared: %s", s.latch_reason);
        s.latched = false;
        s.latch_reason[0] = 0;
        s.confirm_n = 0;
    }
    tick(now);
}

void Engine::mark(Source& s, uint16_t id, bool pack2, uint64_t now) {
    for (int k = 0; k < s.nreq; k++)
        if (s.req[k].id == id && s.req[k].pack2 == pack2) {
            s.req[k].last_ms = now;
            s.req[k].seen = true;
        }
}

void Engine::reject(int src, const char* what, uint64_t now) {
    Source& s = src_[src];
    s.plausibility_rejects++;
    if (now - last_plaus_ev_[src] >= 10000 || last_plaus_ev_[src] == 0) {
        last_plaus_ev_[src] = now;
        event(EvType::Plausibility, src, now, "rejected: %s", what);
    }
}

void Engine::latch(int src, const char* why, uint64_t now) {
    Source& s = src_[src];
    if (s.latched) return;
    s.latched = true;
    snprintf(s.latch_reason, sizeof(s.latch_reason), "%s", why);
    event(EvType::Trip, src, now, "TRIP (latched until cleared): %s", why);
}

void Engine::latch_confirmed(int src, uint32_t id, const uint8_t* d, const char* why, uint64_t now) {
    Source& s = src_[src];
    if (s.confirm_n && s.confirm_id == id && memcmp(s.confirm_data, d, 8) == 0) {
        if (s.confirm_n < 255) s.confirm_n++;
    } else {
        s.confirm_n = 1;
        s.confirm_id = id;
        memcpy(s.confirm_data, d, 8);
    }
    if (s.confirm_n >= kLatchConfirm) latch(src, why, now);
}

void Engine::deviation_clear(int src, uint32_t id) {
    Source& s = src_[src];
    if (s.confirm_id == id) s.confirm_n = 0;
}

bool Engine::frame_allowed(int src, uint32_t id, uint8_t dlc) const {
    const AllowedId* t = src == kSrcBN ? kBnIds : kKnyIds;
    size_t n = src == kSrcBN ? sizeof(kBnIds) / sizeof(kBnIds[0]) : sizeof(kKnyIds) / sizeof(kKnyIds[0]);
    for (size_t k = 0; k < n; k++)
        if (t[k].id == id) return dlc <= 8 && (t[k].dlc_mask & D(dlc));
    return false;
}

// 0x379 (both sources). A step (a pack joins or leaves) is a warning plus a recovery hold, then
// the new capacity is used. Zero or implausible: the source is invalid (fail-safe, not latched)
// until a sane value arrives. A different value (step or implausible) counts only when it arrives
// in two consecutive frames: one corrupt frame never causes a hold or a trip.
void Engine::capacity(int src, uint16_t ah, uint64_t now) {
    Source& s = src_[src];
    bool same = s.have_cap && (uint16_t)lrintf(s.capacity_ah) == ah && !(s.invalid && strncmp(s.invalid_reason, "0x379", 5) == 0);
    if (!same && s.have_cap) {
        if (s.cap_pending == ah && s.cap_pending_n) s.cap_pending_n++;
        else s.cap_pending = ah, s.cap_pending_n = 1;
        if (s.cap_pending_n < 2) {
            char why[48];
            snprintf(why, sizeof(why), "0x379 capacity %u Ah (once)", ah);
            return reject(src, why, now);
        }
    }
    s.cap_pending_n = 0;
    if (ah < 1 || ah > 2000) {
        s.invalid = true;
        snprintf(s.invalid_reason, sizeof(s.invalid_reason), "0x379 capacity %u Ah", ah);
        return reject(src, s.invalid_reason, now);
    }
    if (s.invalid && strncmp(s.invalid_reason, "0x379", 5) == 0) s.invalid = false;
    if (s.have_cap && (uint16_t)lrintf(s.capacity_ah) != ah) {
        event(EvType::CapacityStep, src, now, "capacity %u -> %u Ah: recovery hold %.0f s, then the new capacity",
              (unsigned)lrintf(s.capacity_ah), ah, cfg_.startup_hold_ms / 1000.0);
        s.hold_until_ms = now + cfg_.startup_hold_ms;
    }
    s.capacity_ah = ah, s.have_cap = true;
}

// Integrates V x I into the energy counters (called on every fresh current sample). Gaps over 5 s
// are not bridged.
void Engine::integrate(Source& s, uint64_t now) {
    if (s.e_last_ms && now > s.e_last_ms && now - s.e_last_ms <= 5000) {
        double kwh = (double)s.voltage * s.current * (now - s.e_last_ms) / 3.6e9;
        if (kwh > 0) s.e_in_kwh += kwh;
        else s.e_out_kwh -= kwh;
    }
    s.e_last_ms = now;
}

void Engine::set_energy(int src, double in, double out, double p1_in, double p1_out, double p2_in, double p2_out) {
    if (src < 0 || src >= kSources) return;
    Source& s = src_[src];
    s.e_in_kwh = in, s.e_out_kwh = out, s.p1_e_in_kwh = p1_in, s.p1_e_out_kwh = p1_out;
    s.p2_e_in_kwh = p2_in, s.p2_e_out_kwh = p2_out;
}

void Engine::on_frame(int src, uint32_t id, const uint8_t* d, uint8_t dlc, uint64_t now) {
    if (src < 0 || src >= kSources || !cfg_.enabled[src] || id > 0x7FF) return;
    Source& s = src_[src];
    if (!frame_allowed(src, id, dlc)) {
        s.rejected_frames++;
        if (!last_reject_ev_[src] || now - last_reject_ev_[src] >= kRejectEventMs) {
            last_reject_ev_[src] = now;
            event(EvType::FrameRejected, src, now, "ignored 0x%03lX DLC %u (not sent by this battery; %lu so far)",
                  (unsigned long)id, dlc, (unsigned long)s.rejected_frames);
        }
        return;
    }
    s.frames_ok++;
    s.last_frame_ms = now;
    if (id == 0x305) return;  // the inverter's keep-alive on the battery bus: not battery data
    if (src == kSrcBN) decode_bn(s, id, d, dlc, now);
    else decode_kny(s, id, d, dlc, now);
}

void Engine::decode_bn(Source& s, uint32_t id, const uint8_t* d, uint8_t dlc, uint64_t now) {
    char why[64];
    switch (id) {
        case 0x351: {
            if (dlc < 8) return reject(kSrcBN, "0x351 short", now);
            float cvl = le16(d, 0) / 10.0f, ccl = sle16(d, 2) / 10.0f, dcl = sle16(d, 4) / 10.0f,
                  dvl = le16(d, 6) / 10.0f;
            if (!in(cvl, kVmin, kVmax) || !in(dvl, kDvlMin, kVmax) || dvl >= cvl || !in(ccl, 0, kImax) ||
                !in(dcl, 0, kImax)) {
                snprintf(why, sizeof(why), "0x351 CVL %.1f CCL %.1f DCL %.1f DVL %.1f", cvl, ccl, dcl, dvl);
                s.invalid = true;
                snprintf(s.invalid_reason, sizeof(s.invalid_reason), "%s", why);
                return reject(kSrcBN, why, now);
            }
            s.cvl = cvl, s.ccl = ccl, s.dcl = dcl, s.dvl = dvl;
            s.have_limits = true;
            s.invalid = false;
            break;
        }
        case 0x355: {
            if (dlc < 4) return reject(kSrcBN, "0x355 short", now);
            float soc = le16(d, 0), soh = le16(d, 2);
            if (dlc >= 6) {  // bytes 4-5: SOC in 0.01 % (verified against HA)
                float hr = le16(d, 4) / 100.0f;
                if (in(hr, 0, 100) && fabsf(hr - soc) <= 1.5f) soc = hr;
            }
            if (!in(soc, 0, 100) || !in(soh, 0, 100)) {
                snprintf(why, sizeof(why), "0x355 SOC %.0f SOH %.0f", soc, soh);
                return reject(kSrcBN, why, now);
            }
            s.soc = soc, s.soh = soh, s.have_soc = true;
            break;
        }
        case 0x356: {
            if (dlc < 6) return reject(kSrcBN, "0x356 short", now);
            float v = le16(d, 0) / 100.0f, i = sle16(d, 2) / 10.0f, t = sle16(d, 4) / 10.0f;
            if (!in(v, kVmin, kVmax) || !in(i, -kImax, kImax) || !in(t, kTmin, kTmax)) {
                snprintf(why, sizeof(why), "0x356 V %.2f I %.1f T %.1f", v, i, t);
                return reject(kSrcBN, why, now);
            }
            s.voltage = v, s.current = i, s.temperature = t, s.have_meas = true, s.last_good_v = v;
            integrate(s, now);
            if (!s.have_p2) share_sample_ = true;  // no pack-2 data: sample on 0x356
            break;
        }
        case 0x35A: {
            if (memcmp(d, kBn35ABaseline, 8) != 0) {
                snprintf(why, sizeof(why), "0x35A left its baseline: %02X %02X %02X %02X %02X %02X %02X %02X",
                         d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
                latch_confirmed(kSrcBN, 0x35A, d, why, now);
            } else {
                deviation_clear(kSrcBN, 0x35A);
            }
            break;
        }
        case 0x35F:
        case 0x374:
        case 0x375:
        case 0x376:
        case 0x377:
            break;  // presence only
        case 0x373: {
            if (dlc < 8) return reject(kSrcBN, "0x373 short", now);
            s.cell_min_mv = le16(d, 0), s.cell_max_mv = le16(d, 2);
            s.t_min = sle16(d, 4) - 273.15f, s.t_max = sle16(d, 6) - 273.15f;
            s.have_cells = true;
            break;
        }
        case 0x378: {
            if (dlc < 8) return reject(kSrcBN, "0x378 short", now);
            s.e_chg_kwh = le32(d, 0) / 10.0, s.e_dis_kwh = le32(d, 4) / 10.0, s.have_energy = true;
            break;
        }
        case 0x379:
            capacity(kSrcBN, le16(d, 0), now);
            break;
        case 0x35E:
        case 0x370: {
            int n = dlc > 8 ? 8 : dlc;
            for (int k = 0; k < n; k++) s.name[k] = (d[k] >= 32 && d[k] < 127) ? (char)d[k] : ' ';
            s.name[n] = 0;
            return;
        }
        case 0x211:
            if (dlc < 7 || d[0] != 0x02) return;  // AA FF.. placeholder stream
            s.p2_v = be16(d, 1) / 100.0f, s.p2_i = sbe16(d, 3) / 100.0f, s.p2_soc = d[5], s.p2_soh = d[6];
            s.have_p2 = true;
            share_sample_ = true;
            // Pack energy on each fresh pack-2 sample: pack 2 at its own voltage, pack 1 = bank - pack 2
            // at the bank voltage.
            if (s.p_last_ms && now > s.p_last_ms && now - s.p_last_ms <= 5000 && s.have_meas) {
                double h = (now - s.p_last_ms) / 3.6e9;
                double e2 = (double)s.p2_v * s.p2_i * h, e1 = (double)s.voltage * (s.current - s.p2_i) * h;
                if (e2 > 0) s.p2_e_in_kwh += e2;
                else s.p2_e_out_kwh -= e2;
                if (e1 > 0) s.p1_e_in_kwh += e1;
                else s.p1_e_out_kwh -= e1;
            }
            s.p_last_ms = now;
            mark(s, 0x211, true, now);
            return;
        case 0x212:
        case 0x213:
        case 0x214:
        case 0x215:
        case 0x216:
        case 0x217: {
            if (dlc < 7 || d[0] != 0x02) return;
            if (id == 0x213) s.p2_t = sbe16(d, 1) / 10.0f, s.p2_state = d[5];
            if (id == 0x214) s.p2_balancing = (d[4] & 0x80) != 0;
            if (id == 0x215 || id == 0x216) {
                float lim = be16(d, 5) / 10.0f;
                if (!in(lim, 0, kImax)) {
                    snprintf(why, sizeof(why), "0x%03X pack-2 limit %.1f A", (unsigned)id, lim);
                    s.invalid = true;
                    snprintf(s.invalid_reason, sizeof(s.invalid_reason), "%s", why);
                    return reject(kSrcBN, why, now);
                }
                if (id == 0x215) s.p2_ccl = lim, s.p2_e_chg_kwh = be32(d, 1) / 10.0;
                else s.p2_dcl = lim, s.p2_e_dis_kwh = be32(d, 1) / 10.0;
                s.have_p2_energy = true;
            }
            if (id == 0x217) s.p2_cell_max = be16(d, 1), s.p2_cell_min = be16(d, 3);
            mark(s, (uint16_t)id, true, now);
            return;
        }
        default:
            return;
    }
    mark(s, (uint16_t)id, false, now);
}

void Engine::decode_kny(Source& s, uint32_t id, const uint8_t* d, uint8_t dlc, uint64_t now) {
    char why[64];
    switch (id) {
        case 0x351: {
            if (dlc < 8) return reject(kSrcKNY, "0x351 short", now);
            float cvl = le16(d, 0) / 10.0f, ccl = sle16(d, 2) / 10.0f, dcl = sle16(d, 4) / 10.0f,
                  dvl = le16(d, 6) / 10.0f;
            if (!in(cvl, kVmin, kVmax) || !in(dvl, kDvlMin, kVmax) || dvl >= cvl || !in(ccl, 0, kImax) ||
                !in(dcl, 0, kImax)) {
                snprintf(why, sizeof(why), "0x351 CVL %.1f CCL %.1f DCL %.1f DVL %.1f", cvl, ccl, dcl, dvl);
                s.invalid = true;
                snprintf(s.invalid_reason, sizeof(s.invalid_reason), "%s", why);
                return reject(kSrcKNY, why, now);
            }
            s.cvl = cvl, s.ccl = ccl, s.dcl = dcl, s.dvl = dvl;
            s.have_limits = true;
            s.invalid = false;
            break;
        }
        case 0x355: {
            if (dlc < 4) return reject(kSrcKNY, "0x355 short", now);
            float soc = le16(d, 0), soh = le16(d, 2);
            if (!in(soc, 0, 100) || !in(soh, 0, 100)) {
                snprintf(why, sizeof(why), "0x355 SOC %.0f SOH %.0f", soc, soh);
                return reject(kSrcKNY, why, now);
            }
            s.soc = soc, s.soh = soh, s.have_soc = true;
            break;
        }
        case 0x356: {
            if (dlc < 6) return reject(kSrcKNY, "0x356 short", now);
            float v = sle16(d, 0) / 100.0f, i = sle16(d, 2) / 10.0f, t = sle16(d, 4) / 10.0f;
            if (!in(v, kVmin, kVmax) || !in(i, -kImax, kImax) || !in(t, kTmin, kTmax)) {
                snprintf(why, sizeof(why), "0x356 V %.2f I %.1f T %.1f", v, i, t);
                return reject(kSrcKNY, why, now);
            }
            s.voltage = v, s.current = i, s.temperature = t, s.have_meas = true, s.last_good_v = v;
            integrate(s, now);
            if (!cfg_.enabled[kSrcBN]) share_sample_ = true;  // KNY alone
            break;
        }
        case 0x359: {
            if (dlc < 4) return reject(kSrcKNY, "0x359 short", now);
            uint32_t bits = le32(d, 0);
            if (bits != s.alarm_bits)
                event(EvType::Alarm, kSrcKNY, now, "0x359 alarm/protection bits 0x%08lX -> 0x%08lX",
                      (unsigned long)s.alarm_bits, (unsigned long)bits);
            s.alarm_bits = bits;
            if (bits & ~kPylonDefined) {
                snprintf(why, sizeof(why), "0x359 undefined bits set: 0x%08lX", (unsigned long)(bits & ~kPylonDefined));
                latch_confirmed(kSrcKNY, 0x359, d, why, now);
            } else {
                deviation_clear(kSrcKNY, 0x359);
            }
            break;
        }
        case 0x35C: {
            if (dlc < 1) return reject(kSrcKNY, "0x35C short", now);
            s.charge_enable = (d[0] & 0x80) != 0;
            s.discharge_enable = (d[0] & 0x40) != 0;
            s.force_charge = (d[0] & 0x30) != 0;
            break;
        }
        case 0x35E: {
            int n = dlc > 8 ? 8 : dlc;
            for (int k = 0; k < n; k++) s.name[k] = (d[k] >= 32 && d[k] < 127) ? (char)d[k] : ' ';
            s.name[n] = 0;
            return;
        }
        // Telemetry frames (not required: no timeout). Same layout as the Blue Nova's.
        case 0x373: {  // min/max cell mV, min/max temperature K
            if (dlc < 8) return reject(kSrcKNY, "0x373 short", now);
            float cmin = le16(d, 0), cmax = le16(d, 2), tmin = le16(d, 4) - 273.15f, tmax = le16(d, 6) - 273.15f;
            if (!in(cmin, 1500, 4500) || !in(cmax, 1500, 4500) || cmin > cmax || !in(tmin, kTmin, kTmax) ||
                !in(tmax, kTmin, kTmax)) {
                snprintf(why, sizeof(why), "0x373 cells %.0f-%.0f mV T %.1f-%.1f", cmin, cmax, tmin, tmax);
                return reject(kSrcKNY, why, now);
            }
            s.cell_min_mv = cmin, s.cell_max_mv = cmax, s.t_min = tmin, s.t_max = tmax, s.have_cells = true;
            return;
        }
        case 0x379:  // installed capacity, Ah: replaces the capacity setting for the SOC weighting
            capacity(kSrcKNY, le16(d, 0), now);
            return;
        default:
            return;
    }
    mark(s, (uint16_t)id, false, now);
}

void Engine::eval_source(int i, uint64_t now) {
    Source& s = src_[i];
    SrcState prev = s.state;
    if (!cfg_.enabled[i]) {
        s.state = SrcState::Disabled;
        s.reason[0] = 0;
        return;
    }
    SrcState bad = SrcState::Ok;
    char why[80] = "";
    if (s.latched) {
        bad = SrcState::Tripped;
        snprintf(why, sizeof(why), "%s", s.latch_reason);
    } else if (s.alarm_bits) {
        bad = SrcState::Tripped;
        snprintf(why, sizeof(why), "alarm/protection flags 0x%08lX", (unsigned long)s.alarm_bits);
    } else if (s.invalid) {
        bad = SrcState::Invalid;
        snprintf(why, sizeof(why), "implausible %s", s.invalid_reason);
    } else {
        for (int k = 0; k < s.nreq; k++) {
            const auto& r = s.req[k];
            uint32_t to = r.pack2 ? cfg_.timeout_pack2_ms : cfg_.timeout_1hz_ms;
            if (!r.seen) {  // never received since boot/enable: start-up hold (stale wins below)
                if (bad == SrcState::Ok) {
                    bad = SrcState::NotSeen;
                    snprintf(why, sizeof(why), "0x%03X%s not seen yet", r.id, r.pack2 ? " (pack 2)" : "");
                }
            } else if (now - r.last_ms > to) {
                if (bad != SrcState::Stale) snprintf(why, sizeof(why), "0x%03X%s silent for %.1f s", r.id,
                                                     r.pack2 ? " (pack 2)" : "", (now - r.last_ms) / 1000.0);
                bad = SrcState::Stale;
            }
        }
    }
    if (bad == SrcState::Ok && now < s.hold_until_ms) {  // capacity step: recovery hold
        s.state = SrcState::Holding;
        s.ok_since_ms = s.hold_until_ms - cfg_.startup_hold_ms;
        snprintf(s.reason, sizeof(s.reason), "capacity step: hold %.1f s", (s.hold_until_ms - now) / 1000.0);
        return;
    }
    if (bad == SrcState::Ok) {
        bool was_ok = prev == SrcState::Ok || prev == SrcState::Holding;
        if (!was_ok) s.ok_since_ms = now;
        if (now - s.ok_since_ms >= cfg_.startup_hold_ms) {
            s.state = SrcState::Ok;
            s.reason[0] = 0;
            if (prev != SrcState::Ok) event(EvType::Recovered, i, now, "valid for %.0f s: limits released",
                                           cfg_.startup_hold_ms / 1000.0);
        } else {
            s.state = SrcState::Holding;
            snprintf(s.reason, sizeof(s.reason), "valid for %.1f of %.0f s", (now - s.ok_since_ms) / 1000.0,
                     cfg_.startup_hold_ms / 1000.0);
        }
        return;
    }
    s.state = bad;
    snprintf(s.reason, sizeof(s.reason), "%s", why);
    if (bad != prev) {
        if (bad == SrcState::Stale) event(EvType::Stale, i, now, "STALE: %s", why);
        else if (bad == SrcState::NotSeen && prev == SrcState::Disabled) event(EvType::NotSeen, i, now, "%s", why);
        else if (bad == SrcState::Tripped && !s.latched) event(EvType::Trip, i, now, "TRIP: %s", why);
        else if (bad == SrcState::Invalid) event(EvType::Plausibility, i, now, "INVALID: %s", why);
    }
}

void Engine::tick(uint64_t now) {
    for (int i = 0; i < kSources; i++) eval_source(i, now);
    // Blue Nova pack 1, derived: bank minus pack 2. SOC only for a two-pack bank (>= 150 Ah), assuming
    // two equal packs: pack 1 = 2 x bank - pack 2.
    Source& b = src_[kSrcBN];
    b.have_p1 = b.have_p2 && b.have_meas;
    if (b.have_p1) {
        b.p1_i = b.current - b.p2_i;
        b.p1_ccl = fmaxf(0, b.ccl - b.p2_ccl);
        b.p1_dcl = fmaxf(0, b.dcl - b.p2_dcl);
    }
    b.have_p1_soc = b.have_p1 && b.have_soc && b.have_cap && b.capacity_ah >= 150;
    if (b.have_p1_soc) b.p1_soc = fminf(100, fmaxf(0, 2 * b.soc - b.p2_soc));
    guard_update(now);
    compute(now);
}

// Per-source over-current guard. For each source and direction: limit = min(own reported limit,
// user cap). Over 105 % for > 3 s -> that direction of the output at 50 % (held >= 30 s, released
// after 30 s under 90 %). Still over 15 s after the derate, or over 120 % at any time -> 0 for the
// recovery hold, then back to the 50 % stage. Below a 1 A limit the output is already 0 for that
// direction (a source at 0 blocks it); the guard then only logs current against the zero limit.
void Engine::guard_update(uint64_t now) {
    const Source& b = src_[kSrcBN];
    const Source& k = src_[kSrcKNY];
    struct In {
        bool ok;
        float i, lc, ld;
    } in3[kGuards] = {
        {cfg_.enabled[kSrcBN] && b.have_meas && b.have_limits, b.current, fminf(b.ccl, cfg_.cap_a), fminf(b.dcl, cfg_.cap_a)},
        {cfg_.enabled[kSrcBN] && b.have_p2 && b.p2_ccl + b.p2_dcl > 0, b.p2_i, fminf(b.p2_ccl, cfg_.cap_a_p2),
         fminf(b.p2_dcl, cfg_.cap_a_p2)},
        {cfg_.enabled[kSrcKNY] && k.have_meas && k.have_limits, k.current, fminf(k.ccl, cfg_.cap_b), fminf(k.dcl, cfg_.cap_b)},
    };
    float fac[2] = {1, 1};
    for (int g = 0; g < kGuards; g++) {
        for (int d = 0; d < 2; d++) {
            GuardRt& r = grd_[g][d];
            const char* dir = d == 0 ? "charge" : "discharge";
            float lim = d == 0 ? in3[g].lc : in3[g].ld;
            float x = d == 0 ? in3[g].i : -in3[g].i;  // current in this direction (positive)
            glim_[g][d] = in3[g].ok ? lim : 0;
            if (!in3[g].ok) {
                if (r.st != GuardState::Ok) event(EvType::OvercurrentDerate, g == 2 ? kSrcKNY : kSrcBN, now,
                                                  "%s %s: released (no data)", guard_name(g), dir);
                r = GuardRt();
                continue;
            }
            if (lim < kGuardMinLimit) {  // output already 0 for this direction: log only
                bool against = x > 2.0f;
                if (against && !r.over) r.over = true, r.over_since = now;
                if (!against) r.over = false, r.zero_logged = false;
                if (against && !r.zero_logged && now - r.over_since > kGuardOverMs) {
                    r.zero_logged = true;
                    event(EvType::OvercurrentDerate, g == 2 ? kSrcKNY : kSrcBN, now,
                          "%s %s %.1f A against its own limit 0 A (output already 0)", guard_name(g), dir, x);
                }
                if (r.st != GuardState::Ok) r.st = GuardState::Ok;
                continue;
            }
            bool over = x > kGuardOver * lim, hard = x > kGuardHard * lim, under = x < kGuardRelease * lim;
            if (over && !r.over) r.over_since = now;
            if (under && !r.under) r.under_since = now;
            r.over = over, r.under = under;
            int src = g == 2 ? kSrcKNY : kSrcBN;
            if (r.st != GuardState::Zero && hard) {
                r.st = GuardState::Zero, r.since = now;
                event(EvType::OvercurrentDerate, src, now, "%s %s %.1f A > 120%% of %.1f A: %s limit 0 for %.0f s",
                      guard_name(g), dir, x, lim, dir, cfg_.startup_hold_ms / 1000.0);
            } else if (r.st == GuardState::Ok) {
                if (over && now - r.over_since > kGuardOverMs) {
                    r.st = GuardState::Derated, r.since = now;
                    event(EvType::OvercurrentDerate, src, now, "%s %s %.1f A > 105%% of %.1f A for 3 s: %s limit 50%%",
                          guard_name(g), dir, x, lim, dir);
                }
            } else if (r.st == GuardState::Derated) {
                if (over && now - r.since > kGuardEscalateMs && now - r.over_since > kGuardEscalateMs) {
                    r.st = GuardState::Zero, r.since = now;
                    event(EvType::OvercurrentDerate, src, now, "%s %s still %.1f A > %.1f A after 15 s at 50%%: %s limit 0",
                          guard_name(g), dir, x, lim, dir);
                } else if (under && now - r.under_since >= kGuardReleaseMs && now - r.since >= kGuardHoldMs) {
                    r.st = GuardState::Ok;
                    event(EvType::OvercurrentDerate, src, now, "%s %s released (%.1f A, limit %.1f A)", guard_name(g),
                          dir, x, lim);
                }
            } else {  // Zero: after the recovery hold, back to the 50 % stage with fresh timers
                if (now - r.since >= cfg_.startup_hold_ms) {
                    r.st = GuardState::Derated, r.since = now;
                    r.over_since = now, r.under_since = now;
                    event(EvType::OvercurrentDerate, src, now, "%s %s: recovery hold over, limit 50%%", guard_name(g), dir);
                }
            }
            float f = r.st == GuardState::Ok ? 1.0f : r.st == GuardState::Derated ? 0.5f : 0.0f;
            fac[d] = fminf(fac[d], f);
        }
        gcur_[g] = in3[g].ok ? in3[g].i : 0;
    }
    guard_fac_[0] = fac[0], guard_fac_[1] = fac[1];
}

void Engine::compute(uint64_t now) {
    Output o;
    o.computed_ms = now;
    const char* why = "";
    bool any_bad = false, all_ok = true;
    char sreason[64] = "";
    for (int i = 0; i < kSources; i++) {
        const Source& s = src_[i];
        if (s.state == SrcState::Disabled) continue;
        if (s.state != SrcState::Ok) all_ok = false;
        if (s.state == SrcState::Stale || s.state == SrcState::Invalid || s.state == SrcState::Tripped)
            any_bad = true;
        if (s.state != SrcState::Ok && !sreason[0])
            snprintf(sreason, sizeof(sreason), "%s %s: %.40s", i == kSrcBN ? "A (BN)" : "B (KNY)",
                     src_state_name(s.state), s.reason);
    }
    if (!config_valid(cfg_, &why)) {
        o.state = OutState::Refused;
        snprintf(o.reason, sizeof(o.reason), "%s", why);
    } else if (ota_) {
        o.state = OutState::Silent;
        snprintf(o.reason, sizeof(o.reason), "firmware update in progress (rule 7)");
    } else if (all_ok) {
        o.state = OutState::Normal;
    } else {
        o.state = any_bad ? OutState::FailSafe : OutState::StartupHold;
        snprintf(o.reason, sizeof(o.reason), "%s", sreason);
        if (cfg_.silent_failsafe) {
            o.state = OutState::Silent;
            snprintf(o.reason, sizeof(o.reason), "fail-safe action 'silent': %s", sreason);
        }
    }

    // Measurements: last valid values of the enabled sources (also in fail-safe).
    float cap_sum = 0, soc_w = 0, v_sum = 0, i_sum = 0, soh_min = 1000, t_max = -1000;
    int n_v = 0, n_soc = 0;
    float soc_plain = 0;
    for (int i = 0; i < kSources; i++) {
        const Source& s = src_[i];
        if (!cfg_.enabled[i]) continue;
        float cap = s.have_cap ? s.capacity_ah : (i == kSrcKNY ? cfg_.kny_capacity_ah : 0);
        o.capacity_ah += cap;
        if (s.have_soc) {
            soc_w += s.soc * cap;
            cap_sum += cap;
            soc_plain += s.soc;
            n_soc++;
            if (s.soh < soh_min) soh_min = s.soh;
        }
        if (s.have_meas) {
            v_sum += s.voltage;
            i_sum += s.current;
            n_v++;
            if (s.temperature > t_max) t_max = s.temperature;
        }
        if (s.have_energy) {
            o.have_energy = true;
            o.e_chg_kwh += s.e_chg_kwh;
            o.e_dis_kwh += s.e_dis_kwh;
        }
        o.alarm_bits |= s.alarm_bits;
        if (s.state == SrcState::Stale || s.state == SrcState::NotSeen) o.alarm_bits |= kAlarmCommFail;
        if (s.latched || s.state == SrcState::Invalid) o.alarm_bits |= kProtSystemError;
        o.force_charge |= s.force_charge;
    }
    o.soc = cap_sum > 0 ? soc_w / cap_sum : (n_soc ? soc_plain / n_soc : 0);
    o.soh = n_soc ? soh_min : 0;
    o.voltage = n_v ? v_sum / n_v : 0;
    o.current = i_sum;
    o.temperature = n_v ? t_max : 0;

    if (o.state == OutState::Normal) {
        // CVL: min across sources; DVL: max.
        float cvl = 1e9f, dvl = 0;
        bool chg_block = false, dis_block = false;
        for (int i = 0; i < kSources; i++) {
            const Source& s = src_[i];
            if (!cfg_.enabled[i]) continue;
            if (s.cvl < cvl) cvl = s.cvl;
            if (s.dvl > dvl) dvl = s.dvl;
            // A source that may not charge (CCL 0 or charge disabled) blocks the bank: on one DC
            // bus the inverter cannot charge the other battery alone.
            chg_block |= s.ccl <= 0 || !s.charge_enable;
            dis_block |= s.dcl <= 0 || !s.discharge_enable;
            // each source contributes its own limit, capped by its per-source user cap
            float cap = i == kSrcBN ? cfg_.cap_a : cfg_.cap_b;
            o.ccl_sum += fminf(s.ccl, cap);
            o.dcl_sum += fminf(s.dcl, cap);
        }
        o.cvl_raw = cvl;
        o.dvl = dvl;
        // Packs for the share cap: BN pack 1 = master minus pack 2 (verified in the capture).
        Pack p[3] = {};
        if (cfg_.enabled[kSrcBN]) {
            const Source& b = src_[kSrcBN];
            if (b.have_p2) {
                p[0] = {true, fmaxf(0, b.ccl - b.p2_ccl), fmaxf(0, b.dcl - b.p2_dcl), b.current - b.p2_i};
                p[1] = {true, fminf(b.p2_ccl, cfg_.cap_a_p2), fminf(b.p2_dcl, cfg_.cap_a_p2), b.p2_i};
            } else {
                p[0] = {true, b.ccl, b.dcl, b.current};
            }
        }
        if (cfg_.enabled[kSrcKNY]) {
            const Source& k = src_[kSrcKNY];
            p[2] = {true, fminf(k.ccl, cfg_.cap_b), fminf(k.dcl, cfg_.cap_b), k.current};
        }
        float itot = 0;
        for (const auto& q : p)
            if (q.valid) itot += q.current;
        // Share of each pack in the bank current. A pack that carries more than its share of a
        // summed limit exceeds its own limit (analysis 2026-11-05: pack 2 at 33 A against 30 A for
        // 104 s). Updated only at >= 20 A (below that the 0.1 A resolution makes it noise). It
        // follows a rising share at once (safe direction) and a falling one slowly (~1 min).
        // Sampled once per fresh current sample (Blue Nova: each new pack-2 0x211, paired with the
        // latest 0x356; mixing a new 0x356 with a 2 s old 0x211 gives false share spikes). A rising
        // share is followed fast (half the step per sample), a falling one slowly (~2 min).
        auto update = [&](float* share, bool& known, float sign) {
            float tot = sign * itot;
            if (tot < kShareMinA) return;
            for (int k = 0; k < 3; k++) {
                float x = p[k].valid ? fmaxf(0, sign * p[k].current) / tot : 0;
                if (!known) share[k] = x;
                else if (x >= share[k]) share[k] += (x - share[k]) * 0.5f;
                else share[k] += (x - share[k]) * 0.017f;
            }
            known = true;
        };
        if (share_sample_) {
            share_sample_ = false;
            update(share_, share_known_, 1.0f);
            update(share_dis_, share_dis_known_, -1.0f);
        }
        auto cap = [&](const float* share, bool known, bool charge) -> float {
            if (!known) return -1;
            float c = 1e9f;
            for (int k = 0; k < 3; k++)
                if (p[k].valid && share[k] > 0.02f) {
                    float lim = charge ? p[k].ccl : p[k].dcl;
                    c = fminf(c, lim / share[k]);
                }
            return c < 1e9f ? c : -1;
        };
        o.ccl_share_cap = cap(share_, share_known_, true);
        o.dcl_share_cap = cap(share_dis_, share_dis_known_, false);
        float ccl = chg_block ? 0 : o.ccl_sum, dcl = dis_block ? 0 : o.dcl_sum;
        if (o.ccl_share_cap >= 0) ccl = fminf(ccl, o.ccl_share_cap);
        if (o.dcl_share_cap >= 0) dcl = fminf(dcl, o.dcl_share_cap);
        ccl = fminf(ccl, cfg_.ccl_max);
        dcl = fminf(dcl, cfg_.dcl_max);
        // Over-current guard: 1, 0.5 or 0 per direction.
        ccl *= guard_fac_[0];
        dcl *= guard_fac_[1];
        // "Clamped" (for the event and the UI) only when the clamp takes off at least 1 A or 2 %,
        // with hysteresis: a 50/50 share cap sits a fraction of an amp under the sum all day.
        auto clamped = [](float v, float sum, bool was) {
            float on = fmaxf(1.0f, 0.02f * sum), off = 0.5f * on;
            return was ? v < sum - off : v < sum - on;
        };
        o.ccl_clamped = clamped(ccl, o.ccl_sum, ccl_hyst_);
        o.dcl_clamped = clamped(dcl, o.dcl_sum, dcl_hyst_);
        ccl_hyst_ = o.ccl_clamped;
        dcl_hyst_ = o.dcl_clamped;
        o.ccl = floorf(ccl * 10) / 10;  // round down to the 0.1 A of the frame
        o.dcl = floorf(dcl * 10) / 10;
        // CVL: falls pass at once; a rise passes only after the raw value has stayed above the
        // output for the hold time, and then only to the lowest raw value seen in that window.
        if (!cvl_init_) {
            cvl_out_ = cvl;
            cvl_init_ = true;
        } else if (cvl <= cvl_out_) {
            if (cvl < cvl_out_ - 0.001f)
                event(EvType::CvlChange, -1, now, "CVL %.1f -> %.1f V (fall, immediate)", cvl_out_, cvl);
            cvl_out_ = cvl;
            rising_ = false;
        } else {
            if (!rising_) {
                rising_ = true;
                rise_start_ = now;
                rise_min_ = cvl;
            }
            if (cvl < rise_min_) rise_min_ = cvl;
            if (now - rise_start_ >= cfg_.cvl_rise_hold_ms) {
                event(EvType::CvlChange, -1, now, "CVL %.1f -> %.1f V (rise after %.0f s hold)", cvl_out_,
                      rise_min_, cfg_.cvl_rise_hold_ms / 1000.0);
                cvl_out_ = rise_min_;
                rising_ = false;
            }
        }
        o.cvl = cvl_out_;
        o.charge_enable = o.ccl > 0;
        o.discharge_enable = o.dcl > 0;
        // Clamp events (debounced 10 s): reported when the clamp starts, ends or changes reason.
        static const char* const kWhy[] = {"", "a source may not charge/discharge", "pack share", "user maximum",
                                           "over-current guard"};
        auto code = [&](bool clamped_now, bool block, float share_cap, float user_max, float gf) -> uint8_t {
            if (!clamped_now) return 0;
            if (block) return 1;
            if (gf < 1) return 4;
            return (share_cap >= 0 && share_cap <= user_max) ? 2 : 3;
        };
        auto settled = [&](uint8_t v, uint8_t& last, uint8_t& cand, uint64_t& since) {
            if (v != cand) cand = v, since = now;
            if (cand != last && now - since >= kClampEventMs) {
                last = cand;
                return true;
            }
            return false;
        };
        uint8_t cc = code(o.ccl_clamped, chg_block, o.ccl_share_cap, cfg_.ccl_max, guard_fac_[0]);
        uint8_t dc = code(o.dcl_clamped, dis_block, o.dcl_share_cap, cfg_.dcl_max, guard_fac_[1]);
        if (settled(cc, ccl_code_, ccl_cand_, ccl_cand_since_)) {
            if (cc) event(EvType::LimitClamp, -1, now, "CCL clamped %.1f -> %.1f A (%s)", o.ccl_sum, o.ccl, kWhy[cc]);
            else event(EvType::LimitClamp, -1, now, "CCL clamp released (%.1f A)", o.ccl);
        }
        if (settled(dc, dcl_code_, dcl_cand_, dcl_cand_since_)) {
            if (dc) event(EvType::LimitClamp, -1, now, "DCL clamped %.1f -> %.1f A (%s)", o.dcl_sum, o.dcl, kWhy[dc]);
            else event(EvType::LimitClamp, -1, now, "DCL clamp released (%.1f A)", o.dcl);
        }
    } else {
        // Fail-safe values (also during the start-up hold): zero limits, user CVL/DVL, but never a
        // CVL below the last good pack voltage or a DVL above it (the inverter must not see the
        // battery as over- or under-voltage just because data is missing).
        float vmax = 0, vmin = 1e9f;
        for (int i = 0; i < kSources; i++) {
            const Source& s = src_[i];
            if (!cfg_.enabled[i] || s.last_good_v <= 0) continue;
            vmax = fmaxf(vmax, s.last_good_v);
            vmin = fminf(vmin, s.last_good_v);
        }
        o.cvl = cfg_.failsafe_cvl;
        o.dvl = cfg_.failsafe_dvl;
        if (vmax > 0 && o.cvl < vmax) o.cvl = ceilf(vmax * 10 - 0.001f) / 10;
        if (vmin < 1e9f && o.dvl > vmin) o.dvl = floorf(vmin * 10 + 0.001f) / 10;
        if (o.dvl >= o.cvl) o.dvl = o.cvl - 0.1f;
        o.ccl = 0;
        o.dcl = 0;
        o.charge_enable = o.discharge_enable = false;
        o.force_charge = false;
        cvl_out_ = o.cvl;  // a later rise to the battery CVL goes through the hold
        cvl_init_ = true;
        rising_ = false;
        ccl_hyst_ = dcl_hyst_ = false;
        ccl_code_ = dcl_code_ = ccl_cand_ = dcl_cand_ = 0;
    }
    o.send = o.state == OutState::Normal || o.state == OutState::FailSafe || o.state == OutState::StartupHold;
    for (int g = 0; g < kGuards; g++) {
        for (int d = 0; d < 2; d++) o.guard[g][d] = grd_[g][d].st, o.guard_limit[g][d] = glim_[g][d];
        o.guard_current[g] = gcur_[g];
    }
    o.chg_factor = guard_fac_[0], o.dis_factor = guard_fac_[1];
    if (o.state != OutState::Normal) o.failsafe_cvl = o.cvl, o.failsafe_dvl = o.dvl;
    if (o.state != last_state_)
        event(EvType::State, -1, now, "output %s -> %s%s%s", out_state_name(last_state_), out_state_name(o.state),
              o.reason[0] ? ": " : "", o.reason);
    last_state_ = o.state;
    out_ = o;
    build_frames();
}

void Engine::build_frames() {
    Output& o = out_;
    Frame* f = o.frames;
    memset(f, 0, sizeof(o.frames));
    // 0x351: CVL, CCL, DCL, DVL (0.1 V / 0.1 A, little-endian; CCL/DCL signed).
    f[0].id = 0x351;
    f[0].dlc = 8;
    put16(f[0].data, 0, clampu16(o.cvl * 10));
    put16(f[0].data, 2, (uint16_t)clamp16(o.ccl * 10));
    put16(f[0].data, 4, (uint16_t)clamp16(o.dcl * 10));
    put16(f[0].data, 6, clampu16(o.dvl * 10));
    // 0x355: SOC, SOH (%).
    f[1].id = 0x355;
    f[1].dlc = 4;
    put16(f[1].data, 0, clampu16(o.soc));
    put16(f[1].data, 2, clampu16(o.soh));
    // 0x356: voltage 0.01 V, current 0.1 A (positive = charging), temperature 0.1 C.
    f[2].id = 0x356;
    f[2].dlc = 6;
    put16(f[2].data, 0, (uint16_t)clamp16(o.voltage * 100));
    put16(f[2].data, 2, (uint16_t)clamp16(o.current * 10));
    put16(f[2].data, 4, (uint16_t)clamp16(o.temperature * 10));
    // 0x359: protection/alarm bytes 0-3, module count, "PN".
    f[3].id = 0x359;
    f[3].dlc = 7;
    for (int k = 0; k < 4; k++) f[3].data[k] = (uint8_t)(o.alarm_bits >> (8 * k));
    f[3].data[4] = (uint8_t)((cfg_.enabled[0] ? 1 : 0) + (cfg_.enabled[1] ? 1 : 0));
    f[3].data[5] = 'P';
    f[3].data[6] = 'N';
    // 0x35C: request flags.
    f[4].id = 0x35C;
    f[4].dlc = 2;
    f[4].data[0] = (uint8_t)((o.charge_enable ? 0x80 : 0) | (o.discharge_enable ? 0x40 : 0) |
                             (o.force_charge && o.charge_enable ? 0x20 : 0));
    // 0x35E: manufacturer name.
    f[5].id = 0x35E;
    f[5].dlc = 8;
    memcpy(f[5].data, "BMS-AGG ", 8);
}

}  // namespace agg
