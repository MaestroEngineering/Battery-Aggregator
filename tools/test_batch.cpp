// Host test: writes Verbose Diagnostics batches with the firmware's own encoder
// (components/diag/include/diag/batch_format.h + can::Record) for the
// reference server test.
//   g++ -std=c++17 -I components/diag/include -I components/can/include
//     + tools/test_batch.cpp -o /tmp/test_batch && /tmp/test_batch <out_dir>
#include <cstdio>
#include <cstring>
#include <string>

#include "can/frame.h"
#include "diag/batch_format.h"

static void write_batch(const std::string& path, uint32_t seq, uint16_t flags, uint32_t dropped,
                        int n, bool synced) {
    diag::BatchHeader h = {};
    const uint8_t mac[6] = {0x2c, 0xbc, 0xbb, 0x06, 0x73, 0x24};
    memcpy(h.mac, mac, 6);
    h.flags = flags | (synced ? diag::kFlagTimeSynced : 0);
    h.boot_id = 0xA1B2C3D4;
    h.seq = seq;
    h.n_frames = n;
    h.dropped = dropped;
    h.epoch_offset_us = synced ? 1793520000000000LL : 0;  // 2026-11-01 05:20 UTC
    h.bitrate = 250000;
    strncpy(h.fw, "0.1.2", 8);
    if (flags & diag::kFlagEvent) {
        h.event_id = 0x35A;
        const uint8_t d[8] = {0xA8, 0xAA, 0x82, 0x03, 0xA8, 0xAA, 0x02, 0x02};
        memcpy(h.event_data, d, 8);
    }
    uint8_t hdr[diag::kHeaderLen];
    diag::encode_header(h, hdr);
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(hdr, 1, sizeof(hdr), f);
    for (int i = 0; i < n; i++) {
        can::Record r = {};
        r.ts_us = 10'000'000ULL + i * 45'000ULL;
        r.port = 1;
        if (i % 3 == 0) {  // 0x351 CVL 55.2 V, CCL 100 A, DCL 200 A, DVL 48 V
            r.id = 0x351; r.dlc = 8;
            const uint8_t d[8] = {0x28, 0x02, 0xE8, 0x03, 0xD0, 0x07, 0xE0, 0x01};
            memcpy(r.data, d, 8);
        } else if (i % 3 == 1) {
            r.id = 0x18FF50E5 | can::kIdExtended; r.dlc = 4;
            r.data[0] = i & 0xFF;
        } else {
            r.id = 0x305 | can::kIdRtr; r.dlc = 0;
        }
        fwrite(&r, 1, sizeof(r), f);
    }
    fclose(f);
    printf("%s: seq %u, %d frames\n", path.c_str(), seq, n);
}

int main(int argc, char** argv) {
    std::string d = argc > 1 ? argv[1] : ".";
    write_batch(d + "/b1.bin", 1, 0, 0, 500, true);
    write_batch(d + "/b2.bin", 2, diag::kFlagChangeOnly, 37, 9, true);
    write_batch(d + "/ev.bin", 3, diag::kFlagEvent, 0, 6, true);
    write_batch(d + "/unsynced.bin", 4, 0, 0, 3, false);
    return 0;
}
