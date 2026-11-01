#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* path;  // "/index.html"
    const char* mime;
    const uint8_t* data;  // gzip
    size_t len;
} www_asset_t;

extern const www_asset_t www_assets[];
extern const size_t www_asset_count;
extern const char www_bundle_id[];

#ifdef __cplusplus
}
#endif
