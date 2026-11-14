#pragma once
// Single-threaded host test: a mutex that only checks it is not taken twice.
#include <stdlib.h>
struct HostMutex { int held = 0; };
typedef HostMutex* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new HostMutex; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, uint32_t) { if (m->held++) abort(); return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { m->held--; return pdTRUE; }
