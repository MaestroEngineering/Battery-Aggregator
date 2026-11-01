// Host test for components/can/include/can/mcp2515_regs.h:
// bit timing for the 8 MHz crystal and the no-transmit SPI guard.
//   g++ -std=c++17 -I components/can/include tools/test_mcp2515.cpp -o /tmp/test_mcp2515 && /tmp/test_mcp2515
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "can/mcp2515_regs.h"

using namespace mcp2515;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool allowed(std::initializer_list<int> bytes) {
    std::vector<uint8_t> v;
    for (int b : bytes) v.push_back((uint8_t)b);
    return spi_command_allowed(v.data(), v.size());
}

int main() {
    // --- Bit timing ---
    for (const auto& t : kTimings8MHz) {
        auto f = decode_timing(8000000, t.cnf1, t.cnf2, t.cnf3);
        unsigned prseg = (t.cnf2 & 7) + 1, ps1 = ((t.cnf2 >> 3) & 7) + 1, ps2 = (t.cnf3 & 7) + 1;
        unsigned sjw = (t.cnf1 >> 6) + 1;
        printf("%7u bit/s: CNF %02X %02X %02X -> %u bit/s, %u TQ, sample %.1f%%, SJW %u\n",
               (unsigned)t.bitrate, t.cnf1, t.cnf2, t.cnf3, (unsigned)f.bitrate, f.tq_per_bit,
               f.sample_permille / 10.0, sjw);
        CHECK(f.bitrate == t.bitrate, "bitrate %u", (unsigned)t.bitrate);
        CHECK(f.tq_per_bit >= 5 && f.tq_per_bit <= 25, "TQ count");
        CHECK(t.cnf2 & 0x80, "BTLMODE must be set (PS2 from CNF3)");
        CHECK(ps2 >= 2 && ps2 >= sjw && prseg + ps1 >= ps2, "segment rules");
        CHECK(f.sample_permille >= 700 && f.sample_permille <= 875, "sample point");
    }
    CHECK(timing_for(1000000) == nullptr, "1 Mbit/s must be rejected for 8 MHz");

    // --- Guard: allowed ---
    CHECK(allowed({INS_RESET}), "reset");
    CHECK(allowed({INS_READ, CANSTAT, 0}), "read");
    CHECK(allowed({INS_READ_RX0, 0, 0, 0}), "read rx0");
    CHECK(allowed({INS_READ_STATUS, 0}), "read status");
    CHECK(allowed({INS_WRITE, CANCTRL, MODE_LISTEN_ONLY}), "listen-only");
    CHECK(allowed({INS_WRITE, CANCTRL, MODE_CONFIG}), "config");
    CHECK(allowed({INS_WRITE, CNF3, 0x03, 0xB3, 0x40}), "sequential CNF write");
    CHECK(allowed({INS_WRITE, TXRTSCTRL, 0x00}), "TXRTSCTRL = 0");
    CHECK(allowed({INS_BIT_MODIFY, CANINTF, INT_MERR, 0}), "clear MERRF");
    CHECK(allowed({INS_WRITE, RXB0CTRL, 0x64}), "RXB0CTRL");

    // --- Guard: everything that could transmit or ACK is refused ---
    CHECK(!allowed({INS_WRITE, CANCTRL, MODE_NORMAL}), "normal mode must be refused");
    CHECK(!allowed({INS_WRITE, CANCTRL, MODE_LOOPBACK}), "loopback must be refused");
    CHECK(!allowed({INS_WRITE, CANCTRL, 0xE0}), "power-up mode value refused");
    CHECK(!allowed({INS_WRITE, 0x0E, 0x00, 0x00}), "sequential write reaching CANCTRL=normal refused");
    CHECK(!allowed({INS_BIT_MODIFY, CANCTRL, 0xE0, 0x00}), "bit-modify CANCTRL refused");
    CHECK(!allowed({INS_WRITE, TXRTSCTRL, 0x01}), "TXnRTS pin mode refused");
    for (int base : {0x30, 0x40, 0x50}) {
        CHECK(!allowed({INS_WRITE, base, 0x08}), "TXREQ in TXB%dCTRL refused", (base - 0x30) / 16);
        CHECK(!allowed({INS_BIT_MODIFY, base, 0x08, 0x08}), "bit-modify TXB%dCTRL refused", (base - 0x30) / 16);
        CHECK(!allowed({INS_WRITE, base + 6, 0x11}), "TX data write refused");
    }
    CHECK(!allowed({INS_WRITE, 0x2F, 0, 0x00}), "sequential write into TXB0CTRL refused");
    for (int ins = 0x40; ins <= 0x45; ins++) CHECK(!allowed({ins, 0}), "LOAD TX BUFFER 0x%02X refused", ins);
    for (int ins = 0x80; ins <= 0x87; ins++) CHECK(!allowed({ins}), "RTS 0x%02X refused", ins);
    // Exhaustive: no unknown instruction byte passes.
    int passed = 0;
    for (int ins = 0; ins < 256; ins++) {
        uint8_t b[4] = {(uint8_t)ins, CNF1, 0, 0};  // harmless register; WRITE here reaches CANINTE, not CANCTRL
        if (spi_command_allowed(b, 4)) passed++;
    }
    printf("instruction bytes allowed (any args): %d of 256\n", passed);
    CHECK(passed == 8, "expected exactly 8 allowed instruction bytes, got %d", passed);

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall MCP2515 checks passed\n", failures);
    return failures ? 1 : 0;
}
