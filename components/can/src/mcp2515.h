#pragma once
// Minimal MCP2515 driver. Every SPI transaction passes the module's
// mcp2515::Guard (see can/mcp2515_regs.h), whose policy is fixed by the port at
// attach(): port A listen-only, port B ACK-only, port INV allow-listed transmit.
// send() exists for port INV only; on every other port the guard refuses it.
#include <stdint.h>

#include "can/frame.h"
#include "can/mcp2515_regs.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace can {

class Mcp2515 {
public:
    // `port` (BMS_CAN_PORT_*) fixes the guard policy for the life of the object.
    esp_err_t attach(spi_host_device_t host, int cs_gpio, int hz, int port);
    // Reset, verify the chip answers, set bit timing and receive config, and
    // enter `mode` (CANCTRL value; verified by reading CANSTAT back). The guard
    // decides which modes this port may use.
    esp_err_t start(uint32_t bitrate, uint8_t mode, const char** why);
    esp_err_t start_listen_only(uint32_t bitrate, const char** why);
    // Port INV only: loads TX buffer 0 with a standard data frame and requests it.
    // ESP_ERR_INVALID_STATE if buffer 0 is still busy, ESP_ERR_NOT_ALLOWED if the
    // guard refuses (ID/DLC not allow-listed, faster than 1 Hz, wrong port).
    esp_err_t send(uint16_t id, uint8_t dlc, const uint8_t* data);
    uint8_t txb0ctrl() { return read_reg(0x30); }
    esp_err_t reset();  // RESET instruction + oscillator start-up wait (ends in config mode)
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
    mcp2515::Guard guard_;
};

}  // namespace can
