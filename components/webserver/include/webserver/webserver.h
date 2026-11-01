#pragma once
#include "esp_err.h"

namespace webserver {

// Copies the embedded (gzipped) web UI into LittleFS when it changed.
void sync_assets();

esp_err_t start();
bool running();

// Restart the chip after `delay_ms` (lets the HTTP response go out first).
void schedule_restart(int delay_ms, bool factory_reset);

}  // namespace webserver
