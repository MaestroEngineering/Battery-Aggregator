#pragma once
// Wire format of a Verbose Diagnostics batch (version 1). Pure C++ so the
// encoder is compiled into the host test that feeds the reference server.
//
// POST <diag_url>
//   Authorization: Bearer <token>
//   Content-Type: application/vnd.bms-agg.batch
//   body: 64-byte header + n_frames * 24-byte records, all little-endian.
//
// Header:
//   0  char[4]  magic "BMSB"
//   4  u16      version (1)
//   6  u16      header length (64)
//   8  u8[6]    device MAC (WiFi STA)
//  14  u16      flags (kFlag*)
//  16  u32      boot_id   (random per boot)
//  20  u32      seq       (per boot; (mac, boot_id, seq) is unique -> dedup)
//  24  u32      n_frames
//  28  u32      dropped   (frames lost since the previous batch: buffers full)
//  32  i64      epoch_offset_us: unix_us = record.ts_us + offset (0 = clock not synced)
//  40  u32      CAN bitrate
//  44  u32      event CAN id (event batches), else 0
//  48  u8[8]    event frame data
//  56  char[8]  firmware version (NUL-padded)
// Record (can::Record):
//   0 u64 ts_us (since boot), 8 u32 id (bit31 extended, bit30 RTR),
//  12 u8 dlc, 13 u8 port (0 INV, 1 A, 2 B), 14 u8 flags (bit0 snapshot), 15 u8 0,
//  16 u8[8] data
#include <stdint.h>
#include <string.h>

namespace diag {

constexpr uint16_t kVersion = 1;
constexpr uint16_t kHeaderLen = 64;
constexpr uint16_t kRecordLen = 24;

constexpr uint16_t kFlagTimeSynced = 0x01;
constexpr uint16_t kFlagChangeOnly = 0x02;
constexpr uint16_t kFlagEvent = 0x04;      // pre-trigger capture for an event
constexpr uint16_t kFlagTruncated = 0x08;  // event window shorter than requested

struct BatchHeader {
    uint8_t mac[6];
    uint16_t flags;
    uint32_t boot_id, seq, n_frames, dropped;
    int64_t epoch_offset_us;
    uint32_t bitrate;
    uint32_t event_id;
    uint8_t event_data[8];
    char fw[8];
};

inline void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
inline void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }

inline void encode_header(const BatchHeader& h, uint8_t out[kHeaderLen]) {
    memset(out, 0, kHeaderLen);
    memcpy(out, "BMSB", 4);
    put16(out + 4, kVersion);
    put16(out + 6, kHeaderLen);
    memcpy(out + 8, h.mac, 6);
    put16(out + 14, h.flags);
    put32(out + 16, h.boot_id);
    put32(out + 20, h.seq);
    put32(out + 24, h.n_frames);
    put32(out + 28, h.dropped);
    put64(out + 32, (uint64_t)h.epoch_offset_us);
    put32(out + 40, h.bitrate);
    put32(out + 44, h.event_id);
    memcpy(out + 48, h.event_data, 8);
    memcpy(out + 56, h.fw, 8);
}

}  // namespace diag
