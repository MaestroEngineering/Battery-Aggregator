// Web UI assets: the gzipped files are embedded in the firmware image and
// mirrored into LittleFS (/littlefs/www/<name>.gz) whenever the bundle id
// changes, i.e. after an OTA update that changed the UI. Requests are served
// from LittleFS; if the filesystem is unavailable they are served from the
// embedded copy, so the UI (and OTA) always remains reachable.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "config/config.h"
#include "esp_log.h"
#include "internal.h"
#include "webserver/webserver.h"
#include "www_assets.h"

namespace webserver {
namespace {

const char* TAG = "assets";
const std::string kDir = std::string(cfg::kFsMount) + "/www";
const std::string kStamp = kDir + "/.bundle";
bool s_fs_ok = false;

const www_asset_t* find(const char* path) {
    for (size_t i = 0; i < www_asset_count; i++)
        if (strcmp(www_assets[i].path, path) == 0) return &www_assets[i];
    return nullptr;
}

std::string fs_path(const char* path) { return kDir + path + ".gz"; }

bool write_file(const std::string& p, const uint8_t* d, size_t n) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

}  // namespace

void sync_assets() {
    if (!cfg::fs_mounted()) {
        ESP_LOGW(TAG, "LittleFS not mounted, serving the embedded web UI");
        return;
    }
    char stamp[32] = {};
    FILE* f = fopen(kStamp.c_str(), "r");
    if (f) {
        fgets(stamp, sizeof(stamp), f);
        fclose(f);
    }
    bool all_present = true;
    for (size_t i = 0; i < www_asset_count; i++) {
        struct stat st;
        if (stat(fs_path(www_assets[i].path).c_str(), &st) != 0 ||
            (size_t)st.st_size != www_assets[i].len)
            all_present = false;
    }
    if (strcmp(stamp, www_bundle_id) == 0 && all_present) {
        s_fs_ok = true;
        ESP_LOGI(TAG, "web UI in LittleFS is current (%s)", www_bundle_id);
        return;
    }
    ESP_LOGI(TAG, "installing web UI bundle %s into LittleFS (was '%s')", www_bundle_id, stamp);
    mkdir(kDir.c_str(), 0775);
    // Remove the old stamp first: an interrupted copy is redone on next boot.
    unlink(kStamp.c_str());
    DIR* d = opendir(kDir.c_str());
    if (d) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            unlink((kDir + "/" + e->d_name).c_str());
        }
        closedir(d);
    }
    bool ok = true;
    for (size_t i = 0; i < www_asset_count && ok; i++)
        ok = write_file(fs_path(www_assets[i].path), www_assets[i].data, www_assets[i].len);
    if (ok) ok = write_file(kStamp, (const uint8_t*)www_bundle_id, strlen(www_bundle_id));
    s_fs_ok = ok;
    if (ok) ESP_LOGI(TAG, "web UI installed (%u files)", (unsigned)www_asset_count);
    else ESP_LOGE(TAG, "writing web UI to LittleFS failed, serving embedded copy");
}

bool asset_exists(const char* path) { return find(path) != nullptr; }

esp_err_t serve_asset(httpd_req_t* req, const char* path) {
    const www_asset_t* a = find(path);
    if (!a) return httpd_resp_send_404(req);

    char inm[40] = {};
    std::string etag = std::string("\"") + www_bundle_id + "\"";
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        etag == inm) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_type(req, a->mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag.c_str());
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; img-src 'self' data:; connect-src 'self' ws: wss:; frame-ancestors 'none'");

    FILE* f = s_fs_ok ? fopen(fs_path(path).c_str(), "rb") : nullptr;
    if (!f) return httpd_resp_send(req, (const char*)a->data, a->len);
    char buf[1024];
    size_t n;
    esp_err_t e = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        e = httpd_resp_send_chunk(req, buf, n);
        if (e != ESP_OK) break;
    }
    fclose(f);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, nullptr, 0);
    return e;
}

}  // namespace webserver
