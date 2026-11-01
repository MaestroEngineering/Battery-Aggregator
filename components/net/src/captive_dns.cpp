#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace net {
namespace {

const char* TAG = "dns";
volatile bool s_run = false;
TaskHandle_t s_task = nullptr;
int s_sock = -1;

void dns_task(void*) {
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s_sock < 0 || bind(s_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed");
        if (s_sock >= 0) close(s_sock);
        s_sock = -1;
        s_task = nullptr;
        vTaskDelete(nullptr);
    }
    timeval tv = {1, 0};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "captive DNS running");
    uint8_t buf[512];
    while (s_run) {
        sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf) - 16, 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        // Only plain queries with exactly one question.
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        // Walk the QNAME.
        int p = 12;
        while (p < n && buf[p] != 0) p += buf[p] + 1;
        if (p + 5 > n) continue;
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int qend = p + 5;
        buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
        buf[3] = 0x00;                    // no error (RA=0)
        buf[6] = 0;
        buf[7] = (qtype == 1) ? 1 : 0;  // answer only A queries
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = qend;
        if (qtype == 1) {
            const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(s_sock, buf, len, 0, (sockaddr*)&from, fl);
    }
    close(s_sock);
    s_sock = -1;
    s_task = nullptr;
    ESP_LOGI(TAG, "captive DNS stopped");
    vTaskDelete(nullptr);
}

}  // namespace

void captive_dns_start() {
    if (s_task) return;
    s_run = true;
    xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 3, &s_task);
}

void captive_dns_stop() { s_run = false; }

}  // namespace net
