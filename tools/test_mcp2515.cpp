// Host test for components/can/include/can/mcp2515_regs.h:
// bit timing for the 8 MHz crystal and the per-port SPI guard (A listen-only,
// B ACK-only, INV allow-listed transmit). Exhaustive where the space is small.
//   g++ -std=c++17 -I components/can/include tools/test_mcp2515.cpp -o /tmp/test_mcp2515 && /tmp/test_mcp2515
#include <cstdio>
#include <cstring>
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
    CHECK(allowed({INS_WRITE, RXB0CTRL, 0x04}) && allowed({INS_WRITE, RXB1CTRL, 0x00}), "RXBnCTRL RXM=00 (0.1.8)");
    // 0.1.8 receive set-up: every filter and mask register, on every port's policy.
    for (Policy pol : {Policy::ListenOnly, Policy::AckOnly, Policy::InverterTx}) {
        bool ok = true;
        for (int a : {0x00, 0x04, 0x08, 0x10, 0x14, 0x18})
            for (int k = 0; k < 4; k++) {
                const uint8_t w[3] = {INS_WRITE, (uint8_t)(a + k), (uint8_t)(k == 1 ? 0x08 : 0x00)};
                ok &= spi_command_allowed(pol, w, 3);
            }
        for (int a = 0x20; a <= 0x27; a++) {
            const uint8_t w[3] = {INS_WRITE, (uint8_t)a, 0x00};
            ok &= spi_command_allowed(pol, w, 3);
        }
        CHECK(ok, "filter/mask writes allowed (policy %d)", (int)pol);
    }

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

    // ======================= Per-port guard (0.1.6 exceptions) =======================
    const Policy kAll[3] = {Policy::ListenOnly, Policy::AckOnly, Policy::InverterTx};
    const char* kName[3] = {"A", "B", "INV"};
    CHECK(policy_for_port(0) == Policy::InverterTx, "port 0 (INV) policy");
    CHECK(policy_for_port(1) == Policy::ListenOnly, "port 1 (A) policy");
    CHECK(policy_for_port(2) == Policy::AckOnly, "port 2 (B) policy");
    CHECK(policy_for_port(3) == Policy::ListenOnly && policy_for_port(-1) == Policy::ListenOnly, "unknown port -> listen-only");
    CHECK(Guard().policy() == Policy::ListenOnly, "default-constructed guard is listen-only");
    auto g_ok = [](Guard& g, std::initializer_list<int> bytes, uint64_t now = 0) {
        std::vector<uint8_t> v;
        for (int b : bytes) v.push_back((uint8_t)b);
        return g.check(v.data(), v.size(), now);
    };

    // 1. Every CANCTRL value through WRITE, per port. Exactly the expected modes pass.
    for (int pi = 0; pi < 3; pi++) {
        int pass_normal = 0, pass_loop = 0, pass_e0 = 0, pass_normal_no_osm = 0;
        for (int v = 0; v < 256; v++) {
            Guard g(kAll[pi]);
            bool ok = g_ok(g, {INS_WRITE, CANCTRL, v});
            uint8_t mode = v & MODE_MASK;
            bool expect = mode == MODE_CONFIG || mode == MODE_LISTEN_ONLY || mode == MODE_SLEEP ||
                          (mode == MODE_NORMAL && kAll[pi] == Policy::AckOnly) ||
                          (mode == MODE_NORMAL && kAll[pi] == Policy::InverterTx && (v & CANCTRL_OSM));
            CHECK(ok == expect, "port %s CANCTRL=0x%02X: got %d expected %d", kName[pi], v, ok, expect);
            if (mode == MODE_NORMAL && ok) pass_normal++;
            if (mode == MODE_NORMAL && ok && !(v & CANCTRL_OSM)) pass_normal_no_osm++;
            if (mode == MODE_LOOPBACK && ok) pass_loop++;
            if (mode == 0xE0 && ok) pass_e0++;
            // Bit-modify of CANCTRL is refused on every port, whatever the mask/value.
            Guard g2(kAll[pi]);
            CHECK(!g_ok(g2, {INS_BIT_MODIFY, CANCTRL, 0xFF, v}), "port %s bit-modify CANCTRL 0x%02X", kName[pi], v);
            // Sequential write reaching CANCTRL from CANSTAT.
            Guard g3(kAll[pi]);
            CHECK(g_ok(g3, {INS_WRITE, CANSTAT, 0, v}) == expect, "port %s sequential CANCTRL 0x%02X", kName[pi], v);
        }
        CHECK(pass_loop == 0 && pass_e0 == 0, "port %s: loopback / 0xE0 never allowed", kName[pi]);
        if (kAll[pi] == Policy::ListenOnly) CHECK(pass_normal == 0, "port A: no normal mode at all");
        if (kAll[pi] == Policy::InverterTx) CHECK(pass_normal_no_osm == 0, "port INV: normal mode only with OSM");
        if (kAll[pi] == Policy::AckOnly) CHECK(pass_normal == 32, "port B: normal mode allowed (%d)", pass_normal);
    }

    // 2. TX registers, TXRTSCTRL: refused for WRITE and BIT_MODIFY on every port, every value.
    for (int pi = 0; pi < 3; pi++) {
        for (int addr = 0; addr < 256; addr++)
            for (int v : {0x00, 0x08, 0x0B, 0xFF}) {
                Guard g(kAll[pi]);
                bool w = g_ok(g, {INS_WRITE, addr, v});
                bool bm = g_ok(g, {INS_BIT_MODIFY, addr, 0xFF, v});
                if (tx_register((uint8_t)addr)) {
                    CHECK(!w && !bm, "port %s write/bit-modify TX register 0x%02X", kName[pi], addr);
                } else if (addr == TXRTSCTRL) {
                    CHECK(w == ((v & 0x07) == 0) && !bm, "port %s TXRTSCTRL 0x%02X", kName[pi], v);
                } else if (addr == CANCTRL) {
                    // covered above
                } else {
                    CHECK(w && bm, "port %s ordinary register 0x%02X must stay writable", kName[pi], addr);
                }
            }
        // A sequential write may not run into any TX buffer.
        Guard g(kAll[pi]);
        CHECK(!g_ok(g, {INS_WRITE, 0x2E, 0, 0, 0x08}), "port %s sequential write into TXB0CTRL", kName[pi]);
    }

    // 3. LOAD TX and RTS on A and B: every instruction byte, every argument shape, refused.
    for (int pi = 0; pi < 2; pi++) {
        Guard g(kAll[pi]);
        uint8_t frame[14] = {0, 0x6A, 0x20, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8};  // 0x351, DLC 8
        for (int ins = 0x40; ins <= 0x45; ins++) {
            frame[0] = (uint8_t)ins;
            for (size_t len = 1; len <= 14; len++)
                CHECK(!g.check(frame, len, 0), "port %s LOAD TX 0x%02X len %zu", kName[pi], ins, len);
        }
        for (int ins = 0x80; ins <= 0x87; ins++)
            for (uint64_t t : {0ULL, 5000ULL, 1000000ULL}) {
                uint8_t b = (uint8_t)ins;
                CHECK(!g.check(&b, 1, t), "port %s RTS 0x%02X", kName[pi], ins);
            }
        // Exhaustive instruction bytes: the same 8 as the stateless baseline.
        int passed2 = 0;
        for (int ins = 0; ins < 256; ins++) {
            Guard gg(kAll[pi]);
            uint8_t b[4] = {(uint8_t)ins, CNF1, 0, 0};
            if (gg.check(b, 4, 0)) passed2++;
        }
        CHECK(passed2 == 8, "port %s: %d instruction bytes allowed, expected 8", kName[pi], passed2);
    }
    // Port A's guard is identical to the original no-transmit guard on a large sample.
    {
        unsigned seed = 12345;
        int diff = 0;
        for (int k = 0; k < 200000; k++) {
            uint8_t b[16];
            seed = seed * 1103515245u + 12345u;
            size_t len = 1 + (seed >> 16) % 16;
            for (size_t i = 0; i < len; i++) {
                seed = seed * 1103515245u + 12345u;
                b[i] = (uint8_t)(seed >> 16);
            }
            Guard g(Policy::ListenOnly);
            if (g.check(b, len, k) != spi_command_allowed(b, len)) diff++;
        }
        CHECK(diff == 0, "port A guard differs from the baseline in %d cases", diff);
    }

    // 4. Port INV: LOAD TX allow-list. Every standard ID x every DLC: only the 6 pairs pass.
    {
        int passed_pairs = 0;
        for (int id = 0; id < 0x800; id++)
            for (int dlc = 0; dlc <= 15; dlc++) {
                Guard g(Policy::InverterTx);
                uint8_t b[22] = {INS_LOAD_TX0, (uint8_t)(id >> 3), (uint8_t)((id & 7) << 5), 0, 0, (uint8_t)dlc};
                size_t len = 6 + (dlc > 8 ? 8 : dlc);
                bool ok = g.check(b, len, 0);
                bool expect = tx_allow_index((uint16_t)id, (uint8_t)dlc) >= 0;
                if (ok) passed_pairs++;
                if (ok != expect) CHECK(false, "INV LOAD TX id 0x%03X dlc %d: got %d", id, dlc, ok);
            }
        CHECK(passed_pairs == 6, "INV allow-list: %d (id, dlc) pairs pass, expected 6", passed_pairs);
        const int ids[6] = {0x351, 0x355, 0x356, 0x359, 0x35C, 0x35E}, dlcs[6] = {8, 4, 6, 7, 2, 8};
        for (int k = 0; k < 6; k++) CHECK(kInvTxAllow[k].id == ids[k] && kInvTxAllow[k].dlc == dlcs[k], "allow-list entry %d", k);
        CHECK(kInvTxMinIntervalMs == 1000, "1 Hz");
        // Variants of an allowed frame that must be refused.
        auto load = [&](Guard& g, int ins, int sidh, int sidl, int e8, int e0, int dlcr, size_t len) {
            uint8_t b[16] = {(uint8_t)ins, (uint8_t)sidh, (uint8_t)sidl, (uint8_t)e8, (uint8_t)e0, (uint8_t)dlcr};
            return g.check(b, len, 0);
        };
        Guard g(Policy::InverterTx);
        CHECK(load(g, 0x40, 0x6A, 0x20, 0, 0, 8, 14), "0x351 DLC 8 into TXB0 allowed");
        CHECK(load(g, 0x42, 0x6A, 0x20, 0, 0, 8, 14), "TXB1 allowed");
        CHECK(load(g, 0x44, 0x6A, 0x20, 0, 0, 8, 14), "TXB2 allowed");
        for (int ins : {0x41, 0x43, 0x45}) CHECK(!load(g, ins, 0x6A, 0x20, 0, 0, 8, 14), "LOAD TX 0x%02X (data only) refused", ins);
        CHECK(!load(g, 0x40, 0x6A, 0x28, 0, 0, 8, 14), "extended (EXIDE) refused");
        CHECK(!load(g, 0x40, 0x6A, 0x21, 0, 0, 8, 14), "EID17:16 set refused");
        CHECK(!load(g, 0x40, 0x6A, 0x30, 0, 0, 8, 14), "SRR/unused bit 4 refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 1, 0, 8, 14), "EID8 non-zero refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 1, 8, 14), "EID0 non-zero refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 0, 0x48, 14), "RTR refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 0, 0x88, 14), "unused DLC bits refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 0, 8, 13), "short load refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 0, 8, 15), "long load refused");
        CHECK(!load(g, 0x40, 0x6A, 0x20, 0, 0, 7, 13), "0x351 with DLC 7 refused");
        CHECK(!load(g, 0x40, 0x60, 0xA0, 0, 0, 8, 14), "0x305 refused");
    }

    // 5. Port INV: RTS rules and the 1 Hz limit.
    {
        auto load351 = [&](Guard& g, int ins) {
            uint8_t b[14] = {(uint8_t)ins, 0x6A, 0x20, 0, 0, 8};
            return g.check(b, 14, 0);
        };
        auto load355 = [&](Guard& g, int ins) {
            uint8_t b[10] = {(uint8_t)ins, 0x6A, 0xA0, 0, 0, 4};
            return g.check(b, 10, 0);
        };
        auto rts = [&](Guard& g, int ins, uint64_t t) { uint8_t b = (uint8_t)ins; return g.check(&b, 1, t); };
        Guard g(Policy::InverterTx);
        for (int ins = 0x80; ins <= 0x87; ins++) CHECK(!rts(g, ins, 10000), "RTS 0x%02X with nothing loaded refused", ins);
        CHECK(load351(g, 0x40), "load");
        for (int ins : {0x80, 0x83, 0x85, 0x86, 0x87}) CHECK(!rts(g, ins, 10000), "RTS 0x%02X (none / several buffers) refused", ins);
        CHECK(!rts(g, 0x82, 10000) && !rts(g, 0x84, 10000), "RTS for an unloaded buffer refused");
        { uint8_t b[2] = {0x81, 0}; CHECK(!g.check(b, 2, 10000), "RTS with extra bytes refused"); }
        CHECK(rts(g, 0x81, 10000), "first RTS of 0x351 allowed");
        CHECK(!rts(g, 0x81, 12000), "second RTS without reload refused");
        CHECK(load351(g, 0x40), "reload");
        CHECK(!rts(g, 0x81, 10999), "0x351 again after 999 ms refused");
        CHECK(rts(g, 0x81, 11000), "0x351 again after 1000 ms allowed");
        // Other buffer, same ID: the limit is per ID, not per buffer.
        CHECK(load351(g, 0x42), "load 0x351 into TXB1");
        CHECK(!rts(g, 0x82, 11500), "same ID via another buffer within 1 s refused");
        // Different ID is independent.
        CHECK(load355(g, 0x44), "load 0x355 into TXB2");
        CHECK(rts(g, 0x84, 11500), "0x355 allowed independently");
        // RESET clears what the guard believes is loaded (the chip clears the buffers too).
        CHECK(load351(g, 0x40), "load");
        CHECK(g_ok(g, {INS_RESET}), "reset");
        CHECK(!rts(g, 0x81, 20000), "RTS after reset (empty buffer) refused");
        // The rate limit survives a reset.
        CHECK(load351(g, 0x40), "load");
        CHECK(rts(g, 0x81, 20000), "RTS");
        CHECK(g_ok(g, {INS_RESET}), "reset");
        CHECK(load351(g, 0x40), "load");
        CHECK(!rts(g, 0x81, 20500), "rate limit not reset by RESET");
        // Sustained run: 1 Hz exactly passes, at most one per ID per second at 10 ms attempts.
        Guard h(Policy::InverterTx);
        int sent = 0;
        for (uint64_t t = 100000; t < 160000; t += 10) {
            load351(h, 0x40);
            if (rts(h, 0x81, t)) sent++;
        }
        CHECK(sent == 60, "60 s of attempts every 10 ms: %d sent, expected 60", sent);
        // Baseline rules unchanged on INV for everything else.
        Guard k(Policy::InverterTx);
        CHECK(g_ok(k, {INS_READ, TXB0CTRL, 0}), "INV may read TXB0CTRL");
        CHECK(!g_ok(k, {INS_WRITE, TXB0CTRL, TXB_TXREQ}), "INV may not set TXREQ by WRITE");
        CHECK(!g_ok(k, {INS_BIT_MODIFY, TXB0CTRL, TXB_TXREQ, TXB_TXREQ}), "INV may not set TXREQ by bit-modify");
        CHECK(!g_ok(k, {INS_WRITE, TXRTSCTRL, 0x07}), "INV TXnRTS pins refused");
        CHECK(!g_ok(k, {INS_WRITE, CANCTRL, MODE_LOOPBACK | CANCTRL_OSM}), "INV loopback refused");
        CHECK(g_ok(k, {INS_WRITE, CANCTRL, MODE_NORMAL | CANCTRL_OSM | CANCTRL_ABAT}), "INV abort (ABAT) with OSM allowed");
        CHECK(!g_ok(k, {INS_WRITE, CANCTRL, MODE_NORMAL | CANCTRL_ABAT}), "INV normal without OSM refused");
    }

    printf(failures ? "\n%d FAILURE(S)\n" : "\nall MCP2515 checks passed\n", failures);
    return failures ? 1 : 0;
}
