// Serial commands on the USB console (UART0), e.g. from `pio device monitor`.
// Type a command and press Enter:
//   intmap  - BENCH ONLY: map the INT lines (tools/bench_can_check.py --bench --tests 4
//             must be sending; the result table is written to the log)
//   probe   - hardware check of all three MCP2515 modules
//   help    - this list
// Nothing here can transmit on CAN: the commands only start the same checks as the web UI.
#include "serial_cmd.h"

#include <ctype.h>
#include <string.h>

#include "can/can.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace serial_cmd {
namespace {

const char* TAG = "serial";

void run(const char* line) {
    if (!strcmp(line, "intmap")) {
        esp_err_t e = can::start_intmap();
        if (e == ESP_OK)
            ESP_LOGW(TAG, "INT mapping started (BENCH ONLY). The bench script must be sending test frames; "
                          "results follow in the log");
        else
            ESP_LOGE(TAG, "intmap: a check, detection or INT mapping is already running");
    } else if (!strcmp(line, "probe")) {
        if (can::start_probe() == ESP_OK) ESP_LOGI(TAG, "hardware check started");
        else ESP_LOGE(TAG, "probe: a check, detection or INT mapping is already running");
    } else if (!strcmp(line, "help") || !strcmp(line, "?")) {
        ESP_LOGI(TAG, "commands: intmap (BENCH ONLY: map INT lines), probe (check modules), help");
    } else if (line[0]) {
        ESP_LOGW(TAG, "unknown command '%s' (try: help)", line);
    }
}

void task(void*) {
    char line[32];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (!overflow) run(line);
            n = 0;
            overflow = false;
        } else if (n + 1 < sizeof(line)) {
            line[n++] = (char)tolower(c);
        } else {
            overflow = true;
        }
    }
}

}  // namespace

void start() {
    // RX only through the driver; log output keeps using the console as before.
    if (uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 driver install failed: no serial commands");
        return;
    }
    xTaskCreate(task, "serial_cmd", 3072, nullptr, 2, nullptr);
}

}  // namespace serial_cmd
