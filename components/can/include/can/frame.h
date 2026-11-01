#pragma once
#include <stdint.h>

namespace can {

constexpr uint32_t kIdExtended = 0x80000000u;
constexpr uint32_t kIdRtr = 0x40000000u;
constexpr uint32_t kIdMask = 0x1FFFFFFFu;

constexpr uint8_t kFlagSnapshot = 0x01;  // re-sent unchanged frame (change-only snapshot)

// One received frame. 24 bytes; the same layout is used in the web capture
// stream and in Verbose Diagnostics batches (little-endian on the wire).
struct Record {
    uint64_t ts_us;   // esp_timer time (since boot) when read from the controller
    uint32_t id;      // 11/29-bit ID | kIdExtended | kIdRtr
    uint8_t dlc;      // 0..8
    uint8_t port;     // BMS_CAN_PORT_*
    uint8_t flags;    // kFlag*
    uint8_t rsvd;
    uint8_t data[8];
};
static_assert(sizeof(Record) == 24, "Record must be 24 bytes");

}  // namespace can
