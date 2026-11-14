#pragma once
// No flash on the host: every open fails, so the energy counters start at 0 and are not saved.
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
static inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return ESP_FAIL; }
static inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_FAIL; }
static inline void nvs_close(nvs_handle_t) {}
