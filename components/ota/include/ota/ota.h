#pragma once
// OTA: streamed upload to the inactive slot, validation, rollback.
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "esp_err.h"
#include "ota/image_check.h"

namespace ota {

// Only one update at a time. try_lock() fails while another upload runs.
bool try_lock();
void unlock();
bool busy();

class Upload {
public:
    Upload() = default;
    ~Upload();  // aborts an unfinished upload; the running image is never touched
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;

    // `total` is the full image size (HTTP Content-Length).
    esp_err_t begin(size_t total, std::string& err);
    esp_err_t write(const uint8_t* data, size_t len, std::string& err);
    // Verifies the full image (esp_ota_end checks every segment and the
    // appended SHA-256), runs the signature hook, then selects it for boot.
    esp_err_t finish(std::string& err);
    void abort();
    const ImageFacts& facts() const { return facts_; }

private:
    esp_err_t start_flash(std::string& err);
    const void* part_ = nullptr;
    uint32_t handle_ = 0;
    bool open_ = false;
    bool header_ok_ = false;
    size_t total_ = 0, received_ = 0;
    uint8_t header_[kImageHeaderBytes];
    size_t header_len_ = 0;
    ImageFacts facts_ = {};
};

struct SlotInfo {
    std::string label;
    uint32_t address = 0, size = 0;
    bool has_app = false;
    std::string version, project, date, time, state;
};

struct Info {
    SlotInfo running, other;
    std::string boot_label;
    bool rollback_possible = false;
    bool pending_verify = false;
};

Info info();
size_t max_image_size();

// Boot the other slot (keeps both images). Caller restarts the chip.
esp_err_t select_previous(std::string& err);

// Self-test support.
bool running_pending_verify();
esp_err_t mark_valid();
// Marks the running image invalid and reboots into the previous one. Returns
// only if that is impossible (no valid previous image).
esp_err_t rollback_and_reboot();

// Self-test state for display: "not-required" | "running" | "passed" | "failed".
void set_selftest_state(const char* state, const char* detail);
const char* selftest_state();
const char* selftest_detail();

}  // namespace ota
