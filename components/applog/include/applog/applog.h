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

}  // namespace applog
