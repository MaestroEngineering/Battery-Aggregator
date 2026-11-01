#include "mcp2515.h"

#include <string.h>

#include "can/mcp2515_regs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace can {
namespace {
const char* TAG = "mcp2515";
using namespace mcp2515;
}  // namespace

esp_err_t Mcp2515::attach(spi_host_device_t host, int cs_gpio, int hz) {
    spi_device_interface_config_t dc = {};
    dc.mode = 0;
    dc.clock_speed_hz = hz;
    dc.spics_io_num = cs_gpio;
    dc.queue_size = 1;
    return spi_bus_add_device(host, &dc, &dev_);
}

esp_err_t Mcp2515::xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    // NO-TRANSMIT GATE: every byte sequence sent to the chip is checked.
    if (!spi_command_allowed(tx, len)) {
        blocked_++;
        ESP_LOGE(TAG, "BLOCKED SPI command 0x%02x (would allow transmit)", tx[0]);
        return ESP_ERR_NOT_ALLOWED;
    }
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(dev_, &t);
}

uint8_t Mcp2515::read_reg(uint8_t addr) {
    uint8_t tx[3] = {INS_READ, addr, 0}, rx[3] = {};
    if (xfer(tx, rx, 3) != ESP_OK) return 0xFF;
    return rx[2];
}

esp_err_t Mcp2515::write_reg(uint8_t addr, uint8_t v) {
    uint8_t tx[3] = {INS_WRITE, addr, v}, rx[3];
    return xfer(tx, rx, 3);
}

esp_err_t Mcp2515::bit_modify(uint8_t addr, uint8_t mask, uint8_t v) {
    uint8_t tx[4] = {INS_BIT_MODIFY, addr, mask, v}, rx[4];
    return xfer(tx, rx, 4);
}

uint8_t Mcp2515::opmode() { return read_reg(CANSTAT) & MODE_MASK; }

esp_err_t Mcp2515::enter_config() {
    esp_err_t e = write_reg(CANCTRL, MODE_CONFIG);
    for (int i = 0; i < 20 && e == ESP_OK; i++) {
        if (opmode() == MODE_CONFIG) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t Mcp2515::start_listen_only(uint32_t bitrate, const char** why) {
    const BitTiming* t = timing_for(bitrate);
    if (!t) {
        *why = "unsupported bitrate for an 8 MHz crystal";
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t rst = INS_RESET, dummy;
    xfer(&rst, &dummy, 1);
    vTaskDelay(pdMS_TO_TICKS(5));  // oscillator start-up after reset
    // After reset the chip is in configuration mode (it cannot transmit).
    if (opmode() != MODE_CONFIG) {
        *why = "MCP2515 not responding after reset (check wiring, power, CS 26)";
        return ESP_ERR_NOT_FOUND;
    }
    write_reg(CNF1, t->cnf1);
    write_reg(CNF2, t->cnf2);
    write_reg(CNF3, t->cnf3);
    if (read_reg(CNF1) != t->cnf1 || read_reg(CNF2) != t->cnf2 || read_reg(CNF3) != t->cnf3) {
        *why = "MCP2515 register read-back failed (check MISO/level shifter)";
        return ESP_ERR_INVALID_RESPONSE;
    }
    write_reg(TXRTSCTRL, 0x00);  // TXnRTS pins are plain inputs, cannot request TX
    write_reg(BFPCTRL, 0x00);    // RXnBF pins off
    write_reg(RXB0CTRL, 0x64);   // RXM=11 (accept everything), BUKT=1 (roll over to RXB1)
    write_reg(RXB1CTRL, 0x60);   // RXM=11
    write_reg(CANINTF, 0x00);
    write_reg(CANINTE, INT_RX0 | INT_RX1 | INT_ERR | INT_MERR);
    write_reg(CANCTRL, MODE_LISTEN_ONLY);
    for (int i = 0; i < 20; i++) {
        if (opmode() == MODE_LISTEN_ONLY) {
            auto f = decode_timing(8000000, t->cnf1, t->cnf2, t->cnf3);
            ESP_LOGI(TAG, "listen-only at %lu bit/s (%u TQ, sample point %u.%u%%)",
                     (unsigned long)f.bitrate, f.tq_per_bit, f.sample_permille / 10,
                     f.sample_permille % 10);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    enter_config();
    *why = "MCP2515 did not confirm listen-only mode";
    return ESP_ERR_TIMEOUT;
}

esp_err_t Mcp2515::read_rx(int buf, Record& r) {
    uint8_t tx[14] = {(uint8_t)(buf == 0 ? INS_READ_RX0 : INS_READ_RX1)}, rx[14] = {};
    esp_err_t e = xfer(tx, rx, 14);
    if (e != ESP_OK) return e;
    const uint8_t sidh = rx[1], sidl = rx[2], eid8 = rx[3], eid0 = rx[4], dlc = rx[5];
    uint32_t sid = ((uint32_t)sidh << 3) | (sidl >> 5);
    memset(&r, 0, sizeof(r));
    if (sidl & 0x08) {  // IDE: extended frame
        r.id = (sid << 18) | ((uint32_t)(sidl & 0x03) << 16) | ((uint32_t)eid8 << 8) | eid0;
        r.id |= kIdExtended;
        if (dlc & 0x40) r.id |= kIdRtr;
    } else {
        r.id = sid;
        if (sidl & 0x10) r.id |= kIdRtr;  // SRR = remote request for standard frames
    }
    r.dlc = dlc & 0x0F;
    if (r.dlc > 8) r.dlc = 8;
    if (!(r.id & kIdRtr)) memcpy(r.data, rx + 6, r.dlc);
    return ESP_OK;
}

}  // namespace can
