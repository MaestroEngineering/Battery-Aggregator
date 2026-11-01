#include "applog/applog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace applog {
namespace {

constexpr size_t kRingBytes = 16 * 1024;  // ~200 lines of ~80 chars
constexpr size_t kLineMax = 160;          // longer lines are truncated

char s_ring[kRingBytes];
size_t s_start = 0;  // index of the oldest byte
size_t s_used = 0;   // bytes in use
size_t s_lines = 0;  // complete lines stored

// Partial line being assembled (ESP_LOG normally emits whole lines).
char s_pending[kLineMax + 2];
size_t s_pending_len = 0;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_uart_vprintf = nullptr;

// Drop the oldest line. Caller holds s_mux.
void drop_oldest() {
    while (s_used > 0) {
        char c = s_ring[s_start];
        s_start = (s_start + 1) % kRingBytes;
        s_used--;
        if (c == '\n') break;
    }
    if (s_lines > 0) s_lines--;
}

// Append one complete line (must end in '\n'). Caller holds s_mux.
void push_line(const char* p, size_t n) {
    while (s_used + n > kRingBytes || s_lines >= kMaxLines) drop_oldest();
    for (size_t i = 0; i < n; i++) s_ring[(s_start + s_used + i) % kRingBytes] = p[i];
    s_used += n;
    s_lines++;
}

int hook(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    char buf[kLineMax + 1];
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n < kLineMax ? (size_t)n : kLineMax;
        portENTER_CRITICAL(&s_mux);
        for (size_t i = 0; i < len; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (s_pending_len < kLineMax) s_pending[s_pending_len++] = c;
            if (c == '\n') {
                push_line(s_pending, s_pending_len);
                s_pending_len = 0;
            }
        }
        // Truncated long line: terminate it so lines never merge.
        if ((size_t)n >= kLineMax && s_pending_len > 0) {
            s_pending[s_pending_len++] = '\n';
            push_line(s_pending, s_pending_len);
            s_pending_len = 0;
        }
        portEXIT_CRITICAL(&s_mux);
    }
    return s_uart_vprintf ? s_uart_vprintf(fmt, args) : vprintf(fmt, args);
}

}  // namespace

void init() {
    if (s_uart_vprintf) return;
    s_uart_vprintf = esp_log_set_vprintf(hook);
}

std::string dump() {
    std::string out;
    out.reserve(s_used + 1);
    // Copy in chunks so the critical section stays short.
    static char chunk[512];
    size_t pos = 0;
    for (;;) {
        size_t n = 0;
        portENTER_CRITICAL(&s_mux);
        size_t used = s_used;
        size_t start = s_start;
        while (pos + n < used && n < sizeof(chunk)) {
            chunk[n] = s_ring[(start + pos + n) % kRingBytes];
            n++;
        }
        portEXIT_CRITICAL(&s_mux);
        if (n == 0) break;
        out.append(chunk, n);
        pos += n;
    }
    return out;
}

}  // namespace applog
