#pragma once
// Pure header checks for an uploaded app image. No ESP-IDF dependencies so it
// can also be compiled and tested on a PC (tools/test_image_check.cpp).
//
// Image layout: esp_image_header_t (24 bytes), first esp_image_segment_header_t
// (8 bytes), then esp_app_desc_t (256 bytes) at the start of the first segment.
#include <stddef.h>
#include <stdint.h>

namespace ota {

constexpr size_t kImageHeaderBytes = 24 + 8 + 256;

struct ImageFacts {
    uint16_t chip_id;
    uint16_t min_chip_rev_full;
    uint16_t max_chip_rev_full;
    char version[33];
    char project_name[33];
    char date[17];
    char time[17];
};

// Returns nullptr when the header is acceptable, otherwise a human-readable
// reason. `expected_chip` is CONFIG_IDF_FIRMWARE_CHIP_ID, `project` the running
// app's project name, `chip_rev` the running chip's revision (major*100+minor).
const char* check_image_header(const uint8_t* buf, size_t len, uint16_t expected_chip,
                               const char* project, unsigned chip_rev, ImageFacts* facts);

}  // namespace ota
