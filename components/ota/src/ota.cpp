#include "ota/ota.h"

#include <string.h>

#include <atomic>

#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "sdkconfig.h"

// FAIL-SAFE RULE 7 HOOK (SPEC.md): the CAN output must be silent during a
// firmware update. Phase 1 has no CAN code; the CAN component will provide a
// strong definition of this function. Never make that behaviour optional.
extern "C" __attribute__((weak)) void bms_ota_activity(bool active) { (void)active; }

namespace ota {
namespace {

const char* TAG = "ota";
std::atomic<bool> s_busy{false};

const esp_partition_t* P(const void* p) { return static_cast<const esp_partition_t*>(p); }

// ---------------------------------------------------------------------------
// SIGNATURE VERIFICATION HOOK (not part of Phase 1).
//
// Called after the whole image has been written to the inactive slot and its
// SHA-256 verified, before it is selected for boot. Implement real signature
// verification here (e.g. an ECDSA signature block appended to the image and
// checked against a public key compiled into the firmware).
//
// Setting `require_signed_images` (System page / config export) controls it.
// Until verification exists, enabling the setting rejects every image: the
// check fails closed, never open.
// ---------------------------------------------------------------------------
esp_err_t verify_signature(const esp_partition_t* part, std::string& err) {
    (void)part;
    if (cfg::get().require_signed_images) {
        err = "signed images are required, but this firmware cannot verify signatures yet "
              "(Phase 1); turn off 'Require signed images' to accept unsigned images";
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;  // unsigned images accepted (developer mode)
}

const char* state_name(const esp_partition_t* p) {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) return "flashed (no OTA record)";
    switch (st) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted (failed self-test)";
        default: return "undefined";
    }
}

SlotInfo slot_info(const esp_partition_t* p) {
    SlotInfo s;
    if (!p) return s;
    s.label = p->label;
    s.address = p->address;
    s.size = p->size;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        s.has_app = true;
        s.version = d.version;
        s.project = d.project_name;
        s.date = d.date;
        s.time = d.time;
    }
    s.state = s.has_app ? state_name(p) : "empty";
    return s;
}

bool usable_previous(const esp_partition_t* p) {
    if (!p) return false;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(p, &d) != ESP_OK) return false;
    if (strncmp(d.project_name, esp_app_get_description()->project_name, 32) != 0) return false;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED))
        return false;
    return true;
}

}  // namespace

bool try_lock() {
    bool expected = false;
    return s_busy.compare_exchange_strong(expected, true);
}
void unlock() { s_busy = false; }
bool busy() { return s_busy; }

size_t max_image_size() {
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->size : 0;
}

Upload::~Upload() { abort(); }

void Upload::abort() {
    if (open_) {
        esp_ota_abort(handle_);
        open_ = false;
        ESP_LOGW(TAG, "upload aborted after %u bytes; running image untouched", (unsigned)received_);
        bms_ota_activity(false);
    }
}

esp_err_t Upload::begin(size_t total, std::string& err) {
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        err = "no OTA slot available";
        return ESP_ERR_NOT_FOUND;
    }
    if (part == esp_ota_get_running_partition()) {
        err = "internal error: target slot is the running slot";
        return ESP_FAIL;
    }
    if (total < kImageHeaderBytes) {
        err = "file too small to be a firmware image";
        return ESP_ERR_INVALID_SIZE;
    }
    if (total > part->size) {
        err = "image (" + std::to_string(total) + " bytes) is larger than the OTA slot (" +
              std::to_string(part->size) + " bytes)";
        return ESP_ERR_INVALID_SIZE;
    }
    if (running_pending_verify()) {
        err = "the running firmware is still in its self-test; try again in a minute";
        return ESP_ERR_INVALID_STATE;
    }
    // Fail early (before receiving 1.5 MB) if signatures are required.
    if (cfg::get().require_signed_images) {
        verify_signature(part, err);
        return ESP_ERR_NOT_SUPPORTED;
    }
    part_ = part;
    total_ = total;
    received_ = 0;
    header_len_ = 0;
    header_ok_ = false;
    ESP_LOGI(TAG, "upload of %u bytes to %s @0x%lx", (unsigned)total, part->label,
             (unsigned long)part->address);
    return ESP_OK;
}

esp_err_t Upload::start_flash(std::string& err) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char* why = check_image_header(header_, header_len_, CONFIG_IDF_FIRMWARE_CHIP_ID,
                                         esp_app_get_description()->project_name, chip.revision,
                                         &facts_);
    if (why) {
        err = why;
        ESP_LOGE(TAG, "image rejected: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "image header ok: %s %s (%s %s)", facts_.project_name, facts_.version,
             facts_.date, facts_.time);
    bms_ota_activity(true);
    esp_ota_handle_t h;
    // Sequential writes erase sector by sector, so no long blocking erase.
    esp_err_t e = esp_ota_begin(P(part_), OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        err = std::string("esp_ota_begin failed: ") + esp_err_to_name(e);
        bms_ota_activity(false);
        return e;
    }
    handle_ = h;
    open_ = true;
    header_ok_ = true;
    e = esp_ota_write(handle_, header_, header_len_);
    if (e != ESP_OK) err = std::string("flash write failed: ") + esp_err_to_name(e);
    return e;
}

esp_err_t Upload::write(const uint8_t* data, size_t len, std::string& err) {
    if (!part_) {
        err = "upload not started";
        return ESP_ERR_INVALID_STATE;
    }
    if (received_ + len > total_) {
        err = "more data than announced";
        return ESP_ERR_INVALID_SIZE;
    }
    received_ += len;
    if (!header_ok_) {
        size_t take = std::min(len, kImageHeaderBytes - header_len_);
        memcpy(header_ + header_len_, data, take);
        header_len_ += take;
        data += take;
        len -= take;
        if (header_len_ < kImageHeaderBytes) return ESP_OK;
        esp_err_t e = start_flash(err);
        if (e != ESP_OK) return e;
    }
    if (len == 0) return ESP_OK;
    esp_err_t e = esp_ota_write(handle_, data, len);
    if (e != ESP_OK) err = std::string("flash write failed: ") + esp_err_to_name(e);
    return e;
}

esp_err_t Upload::finish(std::string& err) {
    if (!open_ || received_ != total_) {
        err = "upload incomplete";
        return ESP_ERR_INVALID_STATE;
    }
    open_ = false;
    esp_err_t e = esp_ota_end(handle_);  // full image + SHA-256 verification
    if (e != ESP_OK) {
        err = e == ESP_ERR_OTA_VALIDATE_FAILED
                  ? "image verification failed (corrupt or truncated file)"
                  : std::string("esp_ota_end failed: ") + esp_err_to_name(e);
        ESP_LOGE(TAG, "%s", err.c_str());
        bms_ota_activity(false);
        return e;
    }
    e = verify_signature(P(part_), err);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s", err.c_str());
        bms_ota_activity(false);
        return e;
    }
    e = esp_ota_set_boot_partition(P(part_));
    if (e != ESP_OK) {
        err = std::string("could not select new image: ") + esp_err_to_name(e);
        bms_ota_activity(false);
        return e;
    }
    ESP_LOGI(TAG, "new image %s in %s selected for next boot (must pass self-test)",
             facts_.version, P(part_)->label);
    return ESP_OK;
}

Info info() {
    Info i;
    const esp_partition_t* run = esp_ota_get_running_partition();
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    i.running = slot_info(run);
    i.other = slot_info(other);
    i.boot_label = boot ? boot->label : "";
    i.rollback_possible = usable_previous(other);
    i.pending_verify = running_pending_verify();
    return i;
}

esp_err_t select_previous(std::string& err) {
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    if (!usable_previous(other)) {
        err = "no valid previous firmware in the other slot";
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t e = esp_ota_set_boot_partition(other);  // re-verifies the image
    if (e != ESP_OK) {
        err = std::string("cannot boot previous image: ") + esp_err_to_name(e);
        return e;
    }
    ESP_LOGW(TAG, "rollback requested: next boot from %s", other->label);
    return ESP_OK;
}

bool running_pending_verify() {
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t mark_valid() { return esp_ota_mark_app_valid_cancel_rollback(); }

esp_err_t rollback_and_reboot() { return esp_ota_mark_app_invalid_rollback_and_reboot(); }

namespace {
const char* volatile s_st_state = "running";
const char* volatile s_st_detail = "";
}  // namespace
void set_selftest_state(const char* state, const char* detail) {
    s_st_detail = detail;  // static strings only
    s_st_state = state;
}
const char* selftest_state() { return s_st_state; }
const char* selftest_detail() { return s_st_detail; }

}  // namespace ota
