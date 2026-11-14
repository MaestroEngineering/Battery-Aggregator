#pragma once
// In-RAM ring buffer of the most recent log lines (about 200), fed by the
// ESP-IDF log output. Everything still goes to the UART as well.
#include <stddef.h>
#include <string>

namespace applog {

constexpr size_t kMaxLines = 200;

// Install the log hook. Call first thing in app_main.
void init();

// Copy the buffered lines, oldest first, one per '\n'.
std::string dump();

// Same, streamed in 256-byte pieces to `sink` (no heap). False if the sink failed.
bool dump_to(bool (*sink)(void* ctx, const char* data, size_t len), void* ctx);

}  // namespace applog
