// BMS CAN aggregator gateway - Phase 1: platform bring-up
// (WiFi provisioning, web UI, OTA with rollback). No CAN code yet.
#include "agg/agg.h"
#include "hamqtt/hamqtt.h"
#include "heapmon/heapmon.h"
#include "applog/applog.h"
#include "button.h"
#include "can/can.h"
#include "diag/diag.h"
#include "esp_system.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "net/net.h"
#include "selftest.h"
#include "serial_cmd.h"
#include "webserver/webserver.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    applog::init();  // first, so the web log has the boot messages
    heapmon::start();  // heap watchdog: 1 s samples, checkpoints, low-heap warnings

    cfg::init();               // NVS + settings + LittleFS; applies the timezone

    const esp_app_desc_t* d = esp_app_get_description();
    const esp_partition_t* run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "%s %s (git: unknown) built %s %s, IDF %s, running from %s", d->project_name,
             d->version, d->date, d->time, d->idf_ver, run ? run->label : "?");

    webserver::sync_assets();  // web UI -> LittleFS if it changed
    net::start();              // setup AP or station (+ fallback AP), mDNS, NTP
    webserver::start();
    can::set_sink(diag::on_frame);
    agg::start();              // aggregator (inverter output; sent on INV only when agg_tx_enabled)
    can::set_agg_hooks(agg::on_can_frame, agg::tick, agg::set_ota, agg::tx_frames);
    diag::start();             // Verbose Diagnostics uploader (low priority)
    can::start();              // listen-only tap on port A (if enabled)
    hamqtt::start();           // MQTT / Home Assistant (off until configured)
    selftest::start();         // confirms a new OTA image or rolls it back
    button::start();           // BOOT long press = factory reset
    serial_cmd::start();       // console commands (intmap, probe, help)
    heapmon::mark("boot done");
}
