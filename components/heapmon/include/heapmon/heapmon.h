#pragma once
// Heap watchdog: samples free heap and the largest free block once a second, keeps the lows,
// records named checkpoints (boot, WiFi up, MQTT discovery ...) and warns in the log when the
// largest block gets small. It never reboots the device (a reboot silences the inverter
// output); instead optional work backs off while low() is true (MQTT discovery, see hamqtt).
#include <stddef.h>
#include <stdint.h>

namespace heapmon {

constexpr uint32_t kLowLargest = 12 * 1024;      // below this: low(), optional work waits
constexpr uint32_t kCriticalLargest = 6 * 1024;  // below this: critical() (logged as an error)
constexpr int kMaxMarks = 10;

struct Mark {
    const char* what;  // string literal
    uint32_t uptime_s, free, largest;
};

struct Stats {
    uint32_t free, largest;          // now
    uint32_t min_free;               // lowest ever (ESP-IDF counter, catches short peaks)
    uint32_t min_largest;            // lowest sampled largest block (1 s sampling)
    uint32_t low_episodes;           // times the largest block went below kLowLargest
    int64_t last_low_age_s;          // -1 = never
    int nmarks;
    Mark marks[kMaxMarks];
};

void start();                    // starts the 1 s sampler (call early in app_main)
void mark(const char* what);     // checkpoint: logged and kept (first kMaxMarks only)
bool low();                      // largest free block below kLowLargest (last sample or now)
bool critical();
Stats stats();

}  // namespace heapmon
