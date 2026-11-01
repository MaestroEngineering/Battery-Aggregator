#include "button.h"

#include "config/config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/net.h"
#include "sdkconfig.h"

namespace button {
namespace {

const char* TAG = "button";

void task(void*) {
    const gpio_num_t pin = (gpio_num_t)CONFIG_BMS_BOOT_BUTTON_GPIO;
    const int hold_ms = CONFIG_BMS_FACTORY_RESET_HOLD_S * 1000;
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(pin) == 0) {
            held_ms += 100;
            if (held_ms == 1000)
                ESP_LOGW(TAG, "BOOT held: keep holding %d s for FACTORY RESET",
                         CONFIG_BMS_FACTORY_RESET_HOLD_S);
            else if (held_ms % 1000 == 0 && held_ms < hold_ms)
                ESP_LOGW(TAG, "factory reset in %d s", (hold_ms - held_ms) / 1000);
            if (held_ms >= hold_ms) {
                ESP_LOGW(TAG, "BOOT held %d s: FACTORY RESET", CONFIG_BMS_FACTORY_RESET_HOLD_S);
                cfg::factory_reset();
                net::stop();
                esp_restart();
            }
        } else {
            if (held_ms >= 1000) ESP_LOGI(TAG, "BOOT released, factory reset cancelled");
            held_ms = 0;
        }
    }
}

}  // namespace

void start() {
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << CONFIG_BMS_BOOT_BUTTON_GPIO;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&c);
    xTaskCreate(task, "button", 3072, nullptr, 3, nullptr);
}

}  // namespace button
