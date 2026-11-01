#include "ota/image_check.h"

#include <stdio.h>
#include <string.h>

namespace ota {
namespace {

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void copy_field(char* dst, size_t dst_size, const uint8_t* src, size_t src_len) {
    size_t n = 0;
    while (n < src_len && n + 1 < dst_size && src[n] != 0) {
        char c = (char)src[n];
        dst[n] = (c >= 0x20 && c < 0x7f) ? c : '?';
        n++;
    }
    dst[n] = 0;
}

constexpr uint8_t kImageMagic = 0xE9;           // ESP_IMAGE_HEADER_MAGIC
constexpr uint32_t kAppDescMagic = 0xABCD5432;  // ESP_APP_DESC_MAGIC_WORD
constexpr uint8_t kMaxSegments = 16;            // ESP_IMAGE_MAX_SEGMENTS

// Offsets inside the image.
constexpr size_t kOffSegCount = 1;
constexpr size_t kOffChipId = 12;
constexpr size_t kOffMinRevFull = 15;
constexpr size_t kOffMaxRevFull = 17;
constexpr size_t kOffDesc = 24 + 8;
constexpr size_t kOffDescVersion = kOffDesc + 16;
constexpr size_t kOffDescProject = kOffDesc + 48;
constexpr size_t kOffDescTime = kOffDesc + 80;
constexpr size_t kOffDescDate = kOffDesc + 96;

const char* chip_name(uint16_t id) {
    switch (id) {
        case 0x0000: return "ESP32";
        case 0x0002: return "ESP32-S2";
        case 0x0005: return "ESP32-C3";
        case 0x0009: return "ESP32-S3";
        case 0x000C: return "ESP32-C2";
        case 0x000D: return "ESP32-C6";
        case 0x0010: return "ESP32-H2";
        case 0x0012: return "ESP32-P4";
        default: return "another chip";
    }
}

}  // namespace

const char* check_image_header(const uint8_t* buf, size_t len, uint16_t expected_chip,
                               const char* project, unsigned chip_rev, ImageFacts* f) {
    static char msg[160];
    if (len < kImageHeaderBytes) return "file too small to be a firmware image";
    if (buf[0] != kImageMagic) return "not an ESP firmware image (bad magic byte)";
    if (buf[kOffSegCount] == 0 || buf[kOffSegCount] > kMaxSegments)
        return "corrupt image header (segment count)";

    ImageFacts local;
    if (!f) f = &local;
    memset(f, 0, sizeof(*f));
    f->chip_id = le16(buf + kOffChipId);
    f->min_chip_rev_full = le16(buf + kOffMinRevFull);
    f->max_chip_rev_full = le16(buf + kOffMaxRevFull);
    copy_field(f->version, sizeof(f->version), buf + kOffDescVersion, 32);
    copy_field(f->project_name, sizeof(f->project_name), buf + kOffDescProject, 32);
    copy_field(f->time, sizeof(f->time), buf + kOffDescTime, 16);
    copy_field(f->date, sizeof(f->date), buf + kOffDescDate, 16);

    if (f->chip_id != expected_chip) {
        snprintf(msg, sizeof(msg), "wrong chip: image is for %s (id %u), this device is %s (id %u)",
                 chip_name(f->chip_id), f->chip_id, chip_name(expected_chip), expected_chip);
        return msg;
    }
    if (chip_rev < f->min_chip_rev_full) {
        snprintf(msg, sizeof(msg), "image needs chip revision >= v%u.%u, this chip is v%u.%u",
                 f->min_chip_rev_full / 100, f->min_chip_rev_full % 100, chip_rev / 100,
                 chip_rev % 100);
        return msg;
    }
    // 0xFFFF / 0 mean "no upper limit".
    if (f->max_chip_rev_full != 0 && f->max_chip_rev_full != 0xFFFF &&
        chip_rev > f->max_chip_rev_full) {
        snprintf(msg, sizeof(msg), "image supports chip revision <= v%u.%u only",
                 f->max_chip_rev_full / 100, f->max_chip_rev_full % 100);
        return msg;
    }
    if (le32(buf + kOffDesc) != kAppDescMagic) return "no app descriptor (not an application image)";
    if (strncmp(f->project_name, project, 32) != 0) {
        snprintf(msg, sizeof(msg), "wrong project: image is '%.32s', expected '%.32s'",
                 f->project_name, project);
        return msg;
    }
    if (f->version[0] == 0) return "image has no version";
    return nullptr;
}

}  // namespace ota
