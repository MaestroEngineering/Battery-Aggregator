#pragma once
// Minimal MCP2515 driver: receive only. There is deliberately no function that
// loads or requests a transmit buffer, and every SPI transaction passes
// mcp2515::spi_command_allowed() (see can/mcp2515_regs.h).
#include <stdint.h>

#include "can/frame.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace can {

class Mcp2515 {
public:
    esp_err_t attach(spi_host_device_t host, int cs_gpio, int hz);
    // Reset, verify the chip answers, set bit timing and receive config, and
    // enter listen-only mode (verified by reading CANSTAT back).
    esp_err_t start_listen_only(uint32_t bitrate, const char** why);
    esp_err_t enter_config();
    uint8_t opmode();  // CANSTAT & MODE_MASK
    uint8_t read_reg(uint8_t addr);
    esp_err_t write_reg(uint8_t addr, uint8_t v);
    esp_err_t bit_modify(uint8_t addr, uint8_t mask, uint8_t v);
    // Reads RX buffer 0 or 1 into `r` (also clears its RXnIF).
    esp_err_t read_rx(int buf, Record& r);
    uint32_t blocked_commands() const { return blocked_; }

private:
    esp_err_t xfer(const uint8_t* tx, uint8_t* rx, size_t len);
    spi_device_handle_t dev_ = nullptr;
    uint32_t blocked_ = 0;
};

}  // namespace can
