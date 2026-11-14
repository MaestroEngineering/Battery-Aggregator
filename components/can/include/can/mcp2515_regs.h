#pragma once
// MCP2515 registers, bit timing for an 8 MHz crystal, and the SPI GUARD.
// Pure C++ with no ESP-IDF dependencies so it is unit-tested on a PC
// (tools/test_mcp2515.cpp).
//
// SPI GUARD. Every SPI transaction the driver sends is checked first, by a
// per-module Guard whose policy is fixed by the port (kPortPolicy, never a
// setting). The baseline (all ports) rejects:
//   - the LOAD TX BUFFER (0x40-0x45) and RTS (0x80-0x87) instructions,
//   - any write or bit-modify of the TX buffer registers (0x30-0x3D,
//     0x40-0x4D, 0x50-0x5D, which include TXREQ),
//   - TXRTSCTRL values that would let the TXnRTS pins request a transmit,
//   - any bit-modify of CANCTRL or TXRTSCTRL (mode changes go through WRITE),
//   - CANCTRL modes other than configuration, listen-only and sleep
//     (loopback is refused on every port).
// The ONLY exceptions (approved by the owner, 0.1.6):
//   - Port A (Blue Nova): none. Listen-only, hardware-silent.
//   - Port B (KNY): CANCTRL normal mode is allowed, so the controller
//     acknowledges frames. LOAD TX and RTS stay refused, so B never sends a
//     data frame (normal mode does drive ACK bits and error frames).
//   - Port INV (inverter): normal mode ONLY with one-shot mode (OSM) set, so a
//     failed frame is never retransmitted by the controller; LOAD TX only into
//     a buffer from its start (0x40/0x42/0x44) with a complete standard frame
//     whose ID and DLC are on kInvTxAllow (no RTR, no extended ID); RTS only
//     for a single buffer loaded that way, at most once per ID per
//     kInvTxMinIntervalMs. A buffer must be reloaded before every RTS.
// Listen-only mode itself never drives the bus: no ACK, no error frames.
#include <stddef.h>
#include <stdint.h>

namespace mcp2515 {

// SPI instructions.
constexpr uint8_t INS_RESET = 0xC0;
constexpr uint8_t INS_READ = 0x03;
constexpr uint8_t INS_READ_RX0 = 0x90;  // RXB0SIDH.., clears RX0IF on CS high
constexpr uint8_t INS_READ_RX1 = 0x94;  // RXB1SIDH.., clears RX1IF
constexpr uint8_t INS_WRITE = 0x02;
constexpr uint8_t INS_BIT_MODIFY = 0x05;
constexpr uint8_t INS_READ_STATUS = 0xA0;
constexpr uint8_t INS_RX_STATUS = 0xB0;

// Registers.
constexpr uint8_t BFPCTRL = 0x0C;
constexpr uint8_t TXRTSCTRL = 0x0D;
constexpr uint8_t CANSTAT = 0x0E;
constexpr uint8_t CANCTRL = 0x0F;
constexpr uint8_t TEC = 0x1C;
constexpr uint8_t REC = 0x1D;
constexpr uint8_t CNF3 = 0x28;
constexpr uint8_t CNF2 = 0x29;
constexpr uint8_t CNF1 = 0x2A;
constexpr uint8_t CANINTE = 0x2B;
constexpr uint8_t CANINTF = 0x2C;
constexpr uint8_t EFLG = 0x2D;
constexpr uint8_t RXB0CTRL = 0x60;
constexpr uint8_t RXB1CTRL = 0x70;

// CANCTRL / CANSTAT operating modes (REQOP / OPMOD, bits 7..5).
constexpr uint8_t MODE_NORMAL = 0x00;      // ports B (ACK only) and INV (with OSM) only
constexpr uint8_t MODE_SLEEP = 0x20;
constexpr uint8_t MODE_LOOPBACK = 0x40;    // FORBIDDEN on every port
constexpr uint8_t MODE_LISTEN_ONLY = 0x60;
constexpr uint8_t MODE_CONFIG = 0x80;
constexpr uint8_t MODE_MASK = 0xE0;
constexpr uint8_t CANCTRL_ABAT = 0x10;     // abort all pending transmissions
constexpr uint8_t CANCTRL_OSM = 0x08;      // one-shot mode: no automatic retransmission

// TX buffers (port INV only).
constexpr uint8_t TXB0CTRL = 0x30;
constexpr uint8_t TXB_TXREQ = 0x08;
constexpr uint8_t TXB_ABTF = 0x40, TXB_MLOA = 0x20, TXB_TXERR = 0x10;
constexpr uint8_t INS_LOAD_TX0 = 0x40;     // LOAD TX BUFFER 0 from TXB0SIDH
constexpr uint8_t INS_RTS0 = 0x81;         // request to send, TX buffer 0 only

// CANINTF / CANINTE bits.
constexpr uint8_t INT_RX0 = 0x01;
constexpr uint8_t INT_RX1 = 0x02;
constexpr uint8_t INT_ERR = 0x20;
constexpr uint8_t INT_MERR = 0x80;

// EFLG bits.
constexpr uint8_t EFLG_RX0OVR = 0x40;
constexpr uint8_t EFLG_RX1OVR = 0x80;

struct BitTiming {
    uint32_t bitrate;
    uint8_t cnf1, cnf2, cnf3;
};

// Supported bitrates for an 8 MHz crystal, sample point 75%.
// 1 Mbit/s needs >= 5 time quanta of 125 ns: impossible at 8 MHz.
constexpr BitTiming kTimings8MHz[] = {
    // 125k: BRP=1 (TQ 500 ns), 16 TQ: SYNC 1 + PRSEG 4 + PS1 7 + PS2 4, SJW 2
    {125000, 0x41, 0xB3, 0x03},
    // 250k: BRP=0 (TQ 250 ns), 16 TQ: same segments
    {250000, 0x40, 0xB3, 0x03},
    // 500k: BRP=0 (TQ 250 ns), 8 TQ: SYNC 1 + PRSEG 2 + PS1 3 + PS2 2, SJW 1
    {500000, 0x00, 0x91, 0x01},
};

inline const BitTiming* timing_for(uint32_t bitrate) {
    for (const auto& t : kTimings8MHz)
        if (t.bitrate == bitrate) return &t;
    return nullptr;
}

// Decodes CNF1..3 (for tests and the log).
struct TimingFacts {
    uint32_t bitrate;
    unsigned tq_per_bit;
    unsigned sample_permille;
};
inline TimingFacts decode_timing(uint32_t fosc, uint8_t cnf1, uint8_t cnf2, uint8_t cnf3) {
    unsigned brp = cnf1 & 0x3F;
    unsigned prseg = (cnf2 & 0x07) + 1;
    unsigned ps1 = ((cnf2 >> 3) & 0x07) + 1;
    unsigned ps2 = (cnf3 & 0x07) + 1;
    unsigned n = 1 + prseg + ps1 + ps2;
    TimingFacts f;
    f.tq_per_bit = n;
    f.bitrate = fosc / (2 * (brp + 1) * n);
    f.sample_permille = 1000 * (1 + prseg + ps1) / n;
    return f;
}

inline bool tx_register(uint8_t addr) {
    return (addr >= 0x30 && addr <= 0x3D) || (addr >= 0x40 && addr <= 0x4D) ||
           (addr >= 0x50 && addr <= 0x5D);
}

// Per-port policy. Fixed by the port index (BMS_CAN_PORT_INV 0, A 1, B 2 in can/pins.h).
enum class Policy : uint8_t {
    ListenOnly,  // port A: no normal mode, no transmit
    AckOnly,     // port B: normal mode allowed (ACK), no LOAD TX / RTS
    InverterTx   // port INV: normal mode with OSM, allow-listed frames, rate-limited
};
constexpr Policy kPortPolicy[3] = {Policy::InverterTx, Policy::ListenOnly, Policy::AckOnly};
inline Policy policy_for_port(int port) {
    return (port >= 0 && port < 3) ? kPortPolicy[port] : Policy::ListenOnly;
}

// Port INV transmit allow-list: standard 11-bit IDs, data frames, fixed DLC.
struct TxAllow {
    uint16_t id;
    uint8_t dlc;
};
constexpr TxAllow kInvTxAllow[] = {
    {0x351, 8},  // CVL, CCL, DCL, DVL
    {0x355, 4},  // SOC, SOH
    {0x356, 6},  // voltage, current, temperature
    {0x359, 7},  // protection / alarm flags
    {0x35C, 2},  // charge / discharge request flags
    {0x35E, 8},  // manufacturer name
};
constexpr int kInvTxAllowCount = sizeof(kInvTxAllow) / sizeof(kInvTxAllow[0]);
constexpr uint32_t kInvTxMinIntervalMs = 1000;  // at most 1 Hz per ID
inline int tx_allow_index(uint16_t id, uint8_t dlc) {
    for (int i = 0; i < kInvTxAllowCount; i++)
        if (kInvTxAllow[i].id == id && kInvTxAllow[i].dlc == dlc) return i;
    return -1;
}

inline bool canctrl_value_allowed(Policy p, uint8_t v) {
    uint8_t mode = v & MODE_MASK;
    if (mode == MODE_CONFIG || mode == MODE_LISTEN_ONLY || mode == MODE_SLEEP) return true;
    if (mode == MODE_NORMAL) {
        if (p == Policy::AckOnly) return true;
        if (p == Policy::InverterTx) return (v & CANCTRL_OSM) != 0;  // never automatic retransmission
    }
    return false;  // loopback and 0xE0, and normal mode on port A
}
inline bool canctrl_value_allowed(uint8_t v) { return canctrl_value_allowed(Policy::ListenOnly, v); }

// The stateless part of the gate: every instruction except LOAD TX and RTS
// (which are refused here and only handled by Guard on port INV). `buf` is the
// full byte sequence clocked out (instruction, address, data...).
inline bool spi_command_allowed(Policy p, const uint8_t* buf, size_t len) {
    if (len == 0) return false;
    uint8_t ins = buf[0];
    switch (ins) {
        case INS_RESET:
        case INS_READ_STATUS:
        case INS_RX_STATUS:
            return true;
        case INS_READ:
            return len >= 2;
        case INS_READ_RX0:
        case INS_READ_RX1:
            return true;
        case INS_WRITE: {
            if (len < 3) return false;
            // Sequential write: the address auto-increments.
            for (size_t i = 2; i < len; i++) {
                uint8_t addr = (uint8_t)(buf[1] + (i - 2));
                uint8_t v = buf[i];
                if (tx_register(addr)) return false;
                if (addr == CANCTRL && !canctrl_value_allowed(p, v)) return false;
                if (addr == TXRTSCTRL && (v & 0x07) != 0) return false;  // TXnRTS pins may not trigger TX
            }
            return true;
        }
        case INS_BIT_MODIFY: {
            if (len != 4) return false;
            uint8_t addr = buf[1];
            // Mode changes go through WRITE only, so they can be checked fully.
            return !tx_register(addr) && addr != CANCTRL && addr != TXRTSCTRL;
        }
        default:
            return false;  // includes LOAD TX BUFFER (0x40-0x45) and RTS (0x80-0x87)
    }
}
// Port A's rules (the listen-only baseline).
inline bool spi_command_allowed(const uint8_t* buf, size_t len) {
    return spi_command_allowed(Policy::ListenOnly, buf, len);
}

// The single gate for every SPI transaction of one module. Stateful only for
// port INV: it remembers which allow-listed frame each TX buffer holds and when
// each ID was last requested, so the rate limit holds whatever the caller does.
class Guard {
public:
    explicit Guard(Policy p = Policy::ListenOnly) : policy_(p) { clear_buffers(); }
    Policy policy() const { return policy_; }
    // `now_ms` is a monotonic clock (only used on port INV).
    bool check(const uint8_t* buf, size_t len, uint64_t now_ms) {
        if (len == 0) return false;
        const uint8_t ins = buf[0];
        if (ins >= 0x40 && ins <= 0x45) return load_tx(buf, len);
        if (ins >= 0x80 && ins <= 0x87) return rts(ins, len, now_ms);
        if (!spi_command_allowed(policy_, buf, len)) return false;
        if (ins == INS_RESET) clear_buffers();  // reset clears the TX buffers
        return true;
    }

private:
    bool load_tx(const uint8_t* b, size_t len) {
        if (policy_ != Policy::InverterTx) return false;
        const uint8_t ins = b[0];
        if (ins & 0x01) return false;  // 0x41/0x43/0x45 start at the data bytes: refused
        const int n = (ins >> 1) & 0x03;  // 0x40 -> 0, 0x42 -> 1, 0x44 -> 2
        if (len < 6) return false;
        const uint8_t sidh = b[1], sidl = b[2], eid8 = b[3], eid0 = b[4], dlcr = b[5];
        if (sidl & 0x1F) return false;   // EXIDE (bit 3), EID17:16 and unused bits must be 0
        if (eid8 || eid0) return false;
        if (dlcr & 0xF0) return false;   // RTR (bit 6) and unused bits must be 0
        const uint16_t id = (uint16_t)(((uint16_t)sidh << 3) | (sidl >> 5));
        const uint8_t dlc = dlcr & 0x0F;
        const int idx = tx_allow_index(id, dlc);
        if (idx < 0) return false;
        if (len != (size_t)(6 + dlc)) return false;  // the complete frame, nothing more
        loaded_[n] = (int8_t)idx;
        return true;
    }
    bool rts(uint8_t ins, size_t len, uint64_t now_ms) {
        if (policy_ != Policy::InverterTx || len != 1) return false;
        int n;
        if (ins == 0x81) n = 0;
        else if (ins == 0x82) n = 1;
        else if (ins == 0x84) n = 2;
        else return false;  // 0x80 (none) and multi-buffer requests are refused
        const int idx = loaded_[n];
        if (idx < 0) return false;  // not loaded with an allow-listed frame (or already sent)
        if (sent_[idx] && now_ms - last_ms_[idx] < kInvTxMinIntervalMs) return false;
        sent_[idx] = true;
        last_ms_[idx] = now_ms;
        loaded_[n] = -1;  // one RTS per load
        return true;
    }
    void clear_buffers() {
        for (auto& l : loaded_) l = -1;
    }
    Policy policy_;
    int8_t loaded_[3];
    bool sent_[kInvTxAllowCount] = {};
    uint64_t last_ms_[kInvTxAllowCount] = {};
};

}  // namespace mcp2515
