#pragma once
// Aggregation engine: decodes the two battery sources, combines them into one
// Pylontech-style battery and applies the always-on fail-safe rules (SPEC.md
// "Fail-safe behaviour"). Pure C++ with no ESP-IDF dependency: time is passed
// in, so the same code runs in the firmware and in the host simulator
// (tools/test_agg.cpp, tools/sim_agg.cpp).
//
// Sources (fixed in this build; the YAML profile engine is a later phase):
//   source 0 = port A, Blue Nova BN52V-100-5.2K master (+ pack 2 frames), 250k
//   source 1 = port B, KNY-P51300, Pylontech V1.3-style frames, 500k
//
// This engine only COMPUTES the inverter output (frames are built every 100 ms tick).
// Whether anything is sent is decided elsewhere (port INV, agg_tx_enabled, SPI guard).
#include <stddef.h>
#include <stdint.h>

namespace agg {

constexpr int kSources = 2;
constexpr int kSrcBN = 0;   // port A
constexpr int kSrcKNY = 1;  // port B

struct Config {
    bool enabled[kSources] = {false, false};
    float kny_capacity_ah = 300.0f;   // KNY sends no capacity frame (assumption: P51300 = 51.2 V 300 Ah)
    float failsafe_cvl = 0.0f;        // V. No factory default: 0 = not set -> output refused (silent)
    float failsafe_dvl = 0.0f;        // V. Same.
    float ccl_max = 0.0f;             // A, user clamp on the summed CCL. 0 = not set -> refused
    float dcl_max = 0.0f;             // A, same for DCL
    bool silent_failsafe = false;     // fail-safe action: false = fail-safe values (default), true = silent
    uint32_t startup_hold_ms = 10000; // start-up / recovery hold: every source valid this long
    uint32_t cvl_rise_hold_ms = 60000;
    uint32_t timeout_1hz_ms = 5000;   // 1 Hz BMS frames
    uint32_t timeout_pack2_ms = 6000; // Blue Nova pack-2 frames (0x211-0x217, 2 s period)
    // Per-source user caps (A), both directions: limit each source's contribution to the summed
    // CCL/DCL and set the over-current guard limit = min(own reported limit, cap).
    float cap_a = 100.0f;     // Blue Nova bank (port A)
    float cap_a_p2 = 100.0f;  // Blue Nova pack 2 (its own limits are 50/100 A, 30 A derated)
    float cap_b = 100.0f;     // KNY (port B)
};
// Over-current guard (per source and direction). Fixed parameters, not settable.
constexpr float kGuardOver = 1.05f;        // |I| above 105 % of the limit ...
constexpr uint32_t kGuardOverMs = 3000;    // ... for more than 3 s -> that direction at 50 %
constexpr float kGuardHard = 1.20f;        // above 120 % at any time -> that direction 0
constexpr uint32_t kGuardEscalateMs = 15000;  // still over 15 s after the derate -> 0
constexpr uint32_t kGuardHoldMs = 30000;   // a derate is held at least 30 s
constexpr float kGuardRelease = 0.90f;     // released after |I| < 90 % of the limit ...
constexpr uint32_t kGuardReleaseMs = 30000;  // ... for 30 s
constexpr float kGuardMinLimit = 1.0f;     // below 1 A the output for that direction is already 0
constexpr int kGuards = 3;                 // 0 = A (bank), 1 = A pack 2, 2 = B (KNY)
enum class GuardState : uint8_t { Ok, Derated, Zero };
const char* guard_state_name(GuardState g);
const char* guard_name(int g);
// Clamps of the timing settings (they may only be made safer, never switched off).
constexpr uint32_t kMinTimeoutMs = 2000, kMaxTimeout1HzMs = 5000, kMaxTimeoutPack2Ms = 6000;
constexpr uint32_t kMinStartupHoldMs = 5000, kMaxStartupHoldMs = 300000;
constexpr uint32_t kMinCvlHoldMs = 0, kMaxCvlHoldMs = 600000;
// Validity of the user settings (fail-safe CVL/DVL and the CCL/DCL clamps).
bool config_valid(const Config& c, const char** why);

enum class SrcState : uint8_t { Disabled, NotSeen, Holding, Ok, Stale, Invalid, Tripped };
const char* src_state_name(SrcState s);

enum class OutState : uint8_t {
    Refused,    // configuration incomplete: nothing can be sent (silent)
    Silent,     // OTA in progress, or fail-safe action "silent" while not Normal
    StartupHold,// fail-safe values until every source has been valid for the hold time
    FailSafe,   // a source is stale/invalid/tripped: fail-safe values
    Normal
};
const char* out_state_name(OutState s);

struct Pack {  // per-pack limits and current for the share cap
    bool valid;
    float ccl, dcl, current;  // A; current positive = charging
};

struct Source {
    SrcState state = SrcState::Disabled;
    uint64_t ok_since_ms = 0;     // start of the current uninterrupted valid run
    uint64_t last_frame_ms = 0;   // any frame (for the UI)
    char reason[80] = "";         // why not Ok
    char name[17] = "";           // 0x35E / 0x370 manufacturer string
    uint32_t rejected_frames = 0; // IDs/DLCs this source does not send (per-source validation)
    uint32_t frames_ok = 0;       // frames accepted by the validation
    // Last valid values (the "last valid value" of SPEC.md fail-safe table).
    bool have_limits = false, have_soc = false, have_meas = false, have_cap = false;
    float cvl = 0, ccl = 0, dcl = 0, dvl = 0;
    float soc = 0, soh = 0;
    float voltage = 0, current = 0, temperature = 0;
    float capacity_ah = 0;
    // Pylontech request flags (0x35C). Blue Nova sends none: treated as enabled.
    bool charge_enable = true, discharge_enable = true, force_charge = false;
    // Alarms.
    uint32_t alarm_bits = 0;       // Pylontech 0x359 bytes 0-3 (little-endian into one word)
    bool latched = false;          // unknown-flag trip (0x35A deviation, 0x379 change, undefined bits)
    char latch_reason[80] = "";
    bool invalid = false;          // a limit field failed its plausibility check
    char invalid_reason[64] = "";
    // Telemetry (not used for the output).
    bool have_cells = false;
    float cell_min_mv = 0, cell_max_mv = 0, t_min = 0, t_max = 0;
    float last_good_v = 0;         // last plausible pack voltage (fail-safe CVL/DVL follow it)
    bool have_energy = false;
    double e_chg_kwh = 0, e_dis_kwh = 0;
    uint64_t hold_until_ms = 0;    // capacity step: recovery hold (limits 0) until then
    uint16_t cap_pending = 0;      // a new 0x379 value counts only when it arrives twice in a row
    uint8_t cap_pending_n = 0;
    uint8_t confirm_n = 0;         // consecutive identical deviating frames (0x35A / 0x359 latch)
    uint32_t confirm_id = 0;
    uint8_t confirm_data[8] = {};
    // Energy integrated from the BMS voltage x current (kWh), in = charge, out = discharge.
    double e_in_kwh = 0, e_out_kwh = 0;           // whole source (BN bank / KNY)
    double p1_e_in_kwh = 0, p1_e_out_kwh = 0;     // Blue Nova pack 1 (bank - pack 2)
    double p2_e_in_kwh = 0, p2_e_out_kwh = 0;     // Blue Nova pack 2
    uint64_t e_last_ms = 0, p_last_ms = 0;
    // Blue Nova pack 1 (derived: bank minus pack 2; SOC assumes two equal packs).
    bool have_p1 = false, have_p1_soc = false;
    float p1_i = 0, p1_soc = 0, p1_ccl = 0, p1_dcl = 0;
    // Blue Nova pack 2.
    bool have_p2 = false;
    float p2_v = 0, p2_i = 0, p2_soc = 0, p2_soh = 0, p2_ccl = 0, p2_dcl = 0, p2_t = 0;
    float p2_cell_max = 0, p2_cell_min = 0;
    bool p2_balancing = false;
    uint8_t p2_state = 0;
    bool have_p2_energy = false;
    double p2_e_chg_kwh = 0, p2_e_dis_kwh = 0;
    // Required frames and their age.
    struct Req {
        uint16_t id;
        bool pack2;
        uint64_t last_ms;
        bool seen;
    } req[20];
    int nreq = 0;
    uint32_t plausibility_rejects = 0;
};

// Pylontech V1.3 0x359 bits used in the output.
constexpr uint32_t kAlarmCommFail = 1u << (3 * 8 + 3);  // byte 3 bit 3: internal communication fail
constexpr uint32_t kProtSystemError = 1u << (1 * 8 + 3);  // byte 1 bit 3: system error

struct Frame {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
};
constexpr int kOutFrames = 6;  // 0x351 0x355 0x356 0x359 0x35C 0x35E

struct Output {
    OutState state = OutState::Refused;
    char reason[96] = "";
    bool send = false;            // frames below are valid to send (false = silent)
    float cvl = 0, ccl = 0, dcl = 0, dvl = 0;
    float cvl_raw = 0;            // min of the sources before the rise hold
    float ccl_sum = 0, dcl_sum = 0;   // before clamps
    float ccl_share_cap = -1, dcl_share_cap = -1;  // -1 = no share data
    bool ccl_clamped = false, dcl_clamped = false;
    float soc = 0, soh = 0, voltage = 0, current = 0, temperature = 0, capacity_ah = 0;
    bool have_energy = false;
    double e_chg_kwh = 0, e_dis_kwh = 0;
    uint32_t alarm_bits = 0;
    bool charge_enable = false, discharge_enable = false, force_charge = false;
    GuardState guard[kGuards][2] = {};   // [guard][0 = charge, 1 = discharge]
    float guard_limit[kGuards][2] = {};  // min(own limit, user cap); 0 = not available
    float guard_current[kGuards] = {};
    float chg_factor = 1, dis_factor = 1;  // applied by the guard (1, 0.5 or 0)
    float failsafe_cvl = 0, failsafe_dvl = 0;  // values used in fail-safe (after following the pack voltage)
    Frame frames[kOutFrames] = {};
    uint64_t computed_ms = 0;
};

enum class EvType : uint8_t {
    State, Stale, Recovered, NotSeen, Trip, TripCleared, Alarm, LimitClamp, CvlChange, Plausibility, Config,
    CapacityStep, FrameRejected, OvercurrentDerate
};
const char* ev_type_name(EvType t);
struct Event {
    uint32_t seq;
    uint64_t t_ms;
    EvType type;
    int8_t source;  // -1 = gateway
    char text[76];
};
constexpr int kEventRing = 32;

class Engine {
public:
    Engine();
    void configure(const Config& c, uint64_t now_ms);
    const Config& config() const { return cfg_; }
    // One received frame from source `src` (standard 11-bit IDs only; RTR/extended ignored).
    void on_frame(int src, uint32_t id, const uint8_t* data, uint8_t dlc, uint64_t now_ms);
    // Re-evaluates staleness and the output. Call often (e.g. every 100 ms).
    void tick(uint64_t now_ms);
    void set_ota(bool active, uint64_t now_ms);
    void clear_trips(uint64_t now_ms);  // web UI "clear trips" (also cleared by a power cycle)
    const Output& output() const { return out_; }
    // Energy counters (restored from NVS by the firmware after a reboot).
    void set_energy(int src, double in, double out, double p1_in, double p1_out, double p2_in, double p2_out);
    const Source& source(int i) const { return src_[i]; }
    // Events with seq > `after`, oldest first. Returns count copied.
    size_t events_since(uint32_t after, Event* out, size_t max) const;
    uint32_t event_seq() const { return ev_seq_; }

private:
    void reset_source(int i);
    void decode_bn(Source& s, uint32_t id, const uint8_t* d, uint8_t dlc, uint64_t now);
    void decode_kny(Source& s, uint32_t id, const uint8_t* d, uint8_t dlc, uint64_t now);
    void mark(Source& s, uint16_t id, bool pack2, uint64_t now);
    void reject(int src, const char* what, uint64_t now);
    void latch(int src, const char* why, uint64_t now);
    // Latch only after `n` consecutive identical deviating frames (one corrupt frame never latches).
    void latch_confirmed(int src, uint32_t id, const uint8_t* d, const char* why, uint64_t now);
    void deviation_clear(int src, uint32_t id);
    bool frame_allowed(int src, uint32_t id, uint8_t dlc) const;
    void capacity(int src, uint16_t ah, uint64_t now);
    void integrate(Source& s, uint64_t now);
    void guard_update(uint64_t now);
    void eval_source(int i, uint64_t now);
    void compute(uint64_t now);
    void build_frames();
    void event(EvType t, int src, uint64_t now, const char* fmt, ...) __attribute__((format(printf, 5, 6)));

    Config cfg_;
    Source src_[kSources];
    Output out_;
    bool ota_ = false;
    // CVL rise hold.
    bool cvl_init_ = false;
    float cvl_out_ = 0;
    bool rising_ = false;
    uint64_t rise_start_ = 0;
    float rise_min_ = 0;
    // Share estimate per pack (index: BN pack 1, BN pack 2, KNY).
    float share_[3] = {0, 0, 0};
    bool share_known_ = false;
    float share_dis_[3] = {0, 0, 0};
    bool share_dis_known_ = false;
    bool share_sample_ = false;  // a fresh current sample arrived
    OutState last_state_ = OutState::Refused;
    bool ccl_hyst_ = false, dcl_hyst_ = false;   // clamp flag with hysteresis
    uint8_t ccl_code_ = 0, dcl_code_ = 0;         // reported clamp reason (debounced)
    uint8_t ccl_cand_ = 0, dcl_cand_ = 0;         // current reason, before the debounce
    uint64_t ccl_cand_since_ = 0, dcl_cand_since_ = 0;
    uint64_t last_plaus_ev_[kSources] = {0, 0};
    uint64_t last_reject_ev_[kSources] = {0, 0};
    struct GuardRt {
        GuardState st = GuardState::Ok;
        uint64_t over_since = 0, under_since = 0, since = 0;
        bool over = false, under = false, zero_logged = false;
    } grd_[kGuards][2];
    float guard_fac_[2] = {1, 1};
    float glim_[kGuards][2] = {};
    float gcur_[kGuards] = {};
    bool cfg_logged_ = false;
    const char* last_why_ = nullptr;  // config_valid() verdict last logged (static strings)
    // Events.
    Event ev_[kEventRing];
    uint32_t ev_seq_ = 0;
};

}  // namespace agg
