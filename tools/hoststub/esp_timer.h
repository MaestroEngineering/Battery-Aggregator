#pragma once
#include <stdint.h>
extern int64_t g_hoststub_now_us;  // the test drives the clock
static inline int64_t esp_timer_get_time() { return g_hoststub_now_us; }
