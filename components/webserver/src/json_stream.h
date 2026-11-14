#pragma once
// Streaming JSON writer: no tree, no heap. Output goes through a small fixed buffer to a
// sink (httpd_resp_send_chunk on the device, a string in host tests). Used for large replies
// (GET /api/agg/status) so a request never needs a large contiguous heap block.
// Pure C++ (host-tested by tools/test_status_json.cpp).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

class JsonOut {
public:
    // Returns false when the client is gone; later writes are then dropped.
    typedef bool (*Sink)(void* ctx, const char* data, size_t len);
    JsonOut(Sink sink, void* ctx) : sink_(sink), ctx_(ctx) {}

    void begin() { open('{'); }
    bool end() {  // closes the root object and flushes; false = a write failed
        close('}');
        flush();
        return ok_;
    }
    void obj(const char* key) { this->key(key), open('{'); }
    void arr(const char* key) { this->key(key), open('['); }
    void obj() { sep(), open('{'); }  // array element
    void end_obj() { close('}'); }
    void end_arr() { close(']'); }

    void str(const char* k, const char* v) { key(k), qstr(v ? v : ""); }
    void boolean(const char* k, bool v) { key(k), raw(v ? "true" : "false"); }
    void num(const char* k, double v) { key(k), number(v); }
    void num(double v) { sep(), number(v); }  // array element

    bool ok() const { return ok_; }
    size_t bytes() const { return total_; }

private:
    void open(char c) {
        if (depth_ < kDepth) first_[depth_] = true;
        depth_++;
        put(c);
    }
    void close(char c) {
        if (depth_ > 0) depth_--;
        put(c);
    }
    void sep() {
        if (depth_ > 0 && depth_ <= kDepth) {
            if (!first_[depth_ - 1]) put(',');
            first_[depth_ - 1] = false;
        }
    }
    void key(const char* k) {
        sep();
        qstr(k);
        put(':');
    }
    void number(double v) {
        if (!isfinite(v)) return raw("null");  // same as cJSON
        char b[32];
        if (v == (double)(int64_t)v && fabs(v) < 1e15)
            snprintf(b, sizeof(b), "%lld", (long long)v);
        else
            snprintf(b, sizeof(b), "%.7g", v);  // floats from the engine: 7 significant digits
        raw(b);
    }
    // Strings come from BMS frames too (names): control characters and bytes >= 0x80 are
    // escaped, so the reply is always valid JSON (and valid UTF-8).
    void qstr(const char* s) {
        put('"');
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') {
                put('\\'), put((char)c);
            } else if (c < 0x20 || c >= 0x7F) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                raw(b);
            } else {
                put((char)c);
            }
        }
        put('"');
    }
    void raw(const char* s) {
        while (*s) put(*s++);
    }
    void put(char c) {
        if (n_ == sizeof(buf_)) flush();
        buf_[n_++] = c;
        total_++;
    }
    void flush() {
        if (n_ && ok_) ok_ = sink_(ctx_, buf_, n_);
        n_ = 0;
    }

    static constexpr int kDepth = 8;
    Sink sink_;
    void* ctx_;
    char buf_[256];
    size_t n_ = 0, total_ = 0;
    int depth_ = 0;
    bool first_[kDepth] = {};
    bool ok_ = true;
};
