// BMS CAN aggregator gateway - Phase 1: platform bring-up
// (WiFi provisioning, web UI, OTA with rollback). No CAN code yet.
#include "applog/applog.h"
#include "button.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "net/net.h"
#include "selftest.h"
#include "webserver/webserver.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    applog::init();  // first, so the web log has the boot messages

    const esp_app_desc_t* d = esp_app_get_description();
    const esp_partition_t* run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "%s %s (git: unknown) built %s %s, IDF %s, running from %s", d->project_name,
             d->version, d->date, d->time, d->idf_ver, run ? run->label : "?");

    cfg::init();               // NVS + settings + LittleFS
    webserver::sync_assets();  // web UI -> LittleFS if it changed
    net::start();              // setup AP or station (+ fallback AP), mDNS, NTP
    webserver::start();
    selftest::start();         // confirms a new OTA image or rolls it back
    button::start();           // BOOT long press = factory reset
}
