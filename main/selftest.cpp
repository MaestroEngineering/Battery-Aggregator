#include "selftest.h"

#include <string.h>

#include "config/config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net/net.h"
#include "ota/ota.h"
#include "sdkconfig.h"
#include "webserver/webserver.h"

namespace selftest {
namespace {

const char* TAG = "selftest";

// Real request over loopback, so "HTTP server up" means it actually answers.
bool http_answers() {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        const char* rq = "GET /api/health HTTP/1.0\r\nHost: localhost\r\n\r\n";
        if (send(s, rq, strlen(rq), 0) == (int)strlen(rq)) {
            char buf[64] = {};
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            ok = n > 12 && strncmp(buf, "HTTP/1.", 7) == 0 && strncmp(buf + 9, "200", 3) == 0;
        }
    }
    close(s);
    return ok;
}

const char* check(bool& pass) {
    pass = false;
#ifdef BMS_FAIL_SELFTEST
    return "TEST BUILD: self-test forced to fail (BMS_FAIL_SELFTEST)";
#endif
    if (!cfg::loaded_ok()) return "config did not load";
    if (!webserver::running()) return "HTTP server not started";
    if (!(net::sta_connected() || net::ap_up())) return "neither WiFi nor AP is up";
    if (!http_answers()) return "HTTP server does not answer";
    pass = true;
    return "all checks passed";
}

void task(void*) {
    const bool enforce = ota::running_pending_verify();
    const int64_t deadline = (int64_t)CONFIG_BMS_SELFTEST_TIMEOUT_S * 1000000;
    if (enforce)
        ESP_LOGW(TAG, "new image is PENDING_VERIFY: must pass self-test within %d s or it rolls back",
                 CONFIG_BMS_SELFTEST_TIMEOUT_S);
    ota::set_selftest_state("running", "checking");
    const char* last = "";
    for (;;) {
        bool pass;
        const char* why = check(pass);
        if (why != last) {
            ESP_LOGI(TAG, "%s", why);
            last = why;
        }
        if (pass) {
            if (enforce) {
                esp_err_t e = ota::mark_valid();
                ESP_LOGI(TAG, "self-test passed: image marked valid (%s)", esp_err_to_name(e));
            } else {
                ESP_LOGI(TAG, "self-test passed (image already valid, nothing to confirm)");
            }
            ota::set_selftest_state("passed", why);
            break;
        }
        if (esp_timer_get_time() > deadline) {
            ota::set_selftest_state("failed", why);
            if (enforce) {
                ESP_LOGE(TAG, "self-test FAILED (%s): rolling back to the previous image", why);
                vTaskDelay(pdMS_TO_TICKS(200));  // let the log line out
                esp_err_t e = ota::rollback_and_reboot();
                // Only returns if there is no valid image to go back to.
                ESP_LOGE(TAG, "rollback impossible (%s); staying on this image", esp_err_to_name(e));
            } else {
                ESP_LOGE(TAG, "self-test failed (%s); image was not on probation, no rollback", why);
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(nullptr);
}

}  // namespace

void start() { xTaskCreate(task, "selftest", 4096, nullptr, 4, nullptr); }

}  // namespace selftest
