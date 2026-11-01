#pragma once
// MCP2515 registers, bit timing for an 8 MHz crystal, and the NO-TRANSMIT
// guard. Pure C++ with no ESP-IDF dependencies so it is unit-tested on a PC
// (tools/test_mcp2515.cpp).
//
// NO-TRANSMIT GUARANTEE (Phase 2 tap; SPEC.md "Sniffer mode ... never
// transmits a frame of its own"). Every SPI transaction the driver sends is
// checked by spi_command_allowed() first. It rejects:
//   - the LOAD TX BUFFER (0x40-0x45) and RTS (0x80-0x87) instructions,
//   - any write or bit-modify of the TX buffer registers (0x30-0x3D,
//     0x40-0x4D, 0x50-0x5D, which include TXREQ),
//   - TXRTSCTRL values that would let the TXnRTS pins request a transmit,
//   - CANCTRL modes other than configuration, listen-only and sleep. Normal
//     mode would acknowledge frames (drive the ACK bit); loopback is refused too.
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
constexpr uint8_t MODE_NORMAL = 0x00;      // FORBIDDEN here
constexpr uint8_t MODE_SLEEP = 0x20;
constexpr uint8_t MODE_LOOPBACK = 0x40;    // FORBIDDEN here
constexpr uint8_t MODE_LISTEN_ONLY = 0x60;
constexpr uint8_t MODE_CONFIG = 0x80;
constexpr uint8_t MODE_MASK = 0xE0;

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

inline bool canctrl_value_allowed(uint8_t v) {
    uint8_t mode = v & MODE_MASK;
    return mode == MODE_CONFIG || mode == MODE_LISTEN_ONLY || mode == MODE_SLEEP;
}

// The single gate for every SPI transaction. `buf` is the full byte sequence
// clocked out (instruction, address, data...).
inline bool spi_command_allowed(const uint8_t* buf, size_t len) {
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
                if (addr == CANCTRL && !canctrl_value_allowed(v)) return false;
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

}  // namespace mcp2515
