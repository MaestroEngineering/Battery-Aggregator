// Host test for components/ota/src/image_check.cpp (the OTA header checks).
//   g++ -std=c++17 -I components/ota/include tools/test_image_check.cpp
//     + components/ota/src/image_check.cpp -o /tmp/test_image_check
//   /tmp/test_image_check <expect: ok|reject> <file.bin>...
#include <cstdio>
#include <cstring>
#include <vector>

#include "ota/image_check.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s ok|reject file...\n", argv[0]);
        return 2;
    }
    bool expect_ok = strcmp(argv[1], "ok") == 0;
    int failures = 0;
    for (int i = 2; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        std::vector<uint8_t> buf(ota::kImageHeaderBytes);
        size_t n = fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        ota::ImageFacts facts;
        // Classic ESP32 (chip id 0), project bms-agg, chip revision v3.1.
        const char* why = ota::check_image_header(buf.data(), n, 0, "bms-agg", 301, &facts);
        bool ok = why == nullptr;
        printf("%-40s %s%s%s\n", argv[i], ok ? "ACCEPT" : "REJECT: ", why ? why : "",
               ok == expect_ok ? "" : "   <-- UNEXPECTED");
        if (ok) printf("%-40s   version=%s project=%s built=%s %s\n", "", facts.version,
                       facts.project_name, facts.date, facts.time);
        if (ok != expect_ok) failures++;
    }
    return failures ? 1 : 0;
}
