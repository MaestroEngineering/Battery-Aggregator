#include "mcp2515.h"

#include <string.h>

#include "can/mcp2515_regs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace can {
namespace {
const char* TAG = "mcp2515";
using namespace mcp2515;
}  // namespace

esp_err_t Mcp2515::attach(spi_host_device_t host, int cs_gpio, int hz, int port) {
    guard_ = Guard(policy_for_port(port));
    spi_device_interface_config_t dc = {};
    dc.mode = 0;
    dc.clock_speed_hz = hz;
    dc.spics_io_num = cs_gpio;
    dc.queue_size = 1;
    return spi_bus_add_device(host, &dc, &dev_);
}

esp_err_t Mcp2515::xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    // SPI GATE: every byte sequence sent to the chip is checked by this port's guard.
    if (!guard_.check(tx, len, (uint64_t)(esp_timer_get_time() / 1000))) {
        blocked_++;
        ESP_LOGE(TAG, "BLOCKED SPI command 0x%02x (not allowed on this port)", tx[0]);
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

esp_err_t Mcp2515::reset() {
    uint8_t rst = INS_RESET, dummy;
    esp_err_t e = xfer(&rst, &dummy, 1);
    vTaskDelay(pdMS_TO_TICKS(5));  // oscillator start-up after reset
    return e;
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
    return start(bitrate, MODE_LISTEN_ONLY, why);
}

esp_err_t Mcp2515::send(uint16_t id, uint8_t dlc, const uint8_t* data) {
    if (dlc > 8) return ESP_ERR_INVALID_ARG;
    uint8_t ctrl = read_reg(TXB0CTRL);
    if (ctrl == 0xFF || (ctrl & TXB_TXREQ)) return ESP_ERR_INVALID_STATE;
    uint8_t tx[14] = {INS_LOAD_TX0, (uint8_t)(id >> 3), (uint8_t)((id & 0x07) << 5), 0, 0, dlc}, rx[14];
    memcpy(tx + 6, data, dlc);
    esp_err_t e = xfer(tx, rx, 6 + dlc);
    if (e != ESP_OK) return e;
    uint8_t rts = INS_RTS0, d;
    return xfer(&rts, &d, 1);
}

esp_err_t Mcp2515::start(uint32_t bitrate, uint8_t mode, const char** why) {
    const BitTiming* t = timing_for(bitrate);
    if (!t) {
        *why = "unsupported bitrate for an 8 MHz crystal";
        return ESP_ERR_INVALID_ARG;
    }
    reset();
    // After reset the chip is in configuration mode (it cannot transmit).
    if (opmode() != MODE_CONFIG) {
        *why = "MCP2515 not responding after reset (check wiring, power, CS)";
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
    // Receive every VALID frame, standard and extended. Not RXM=11: in that (debug) mode the
    // MCP2515 also loads a message that had an error before EOF, as far as it was assembled
    // (datasheet 4.2): that produced the phantom 0x71F/zero frames on port INV at the first
    // transmit after the start-up hold, and corrupt frames during bus errors. Bus errors are
    // still counted (MERR/ERR interrupts). Masks 0 = all bits "don't care"; the filters' EXIDE
    // bit still selects standard/extended and is undefined after reset, so all six are written:
    // RXF0/2/4 standard, RXF1/3/5 extended.
    static const uint8_t kFilt[6] = {0x00, 0x04, 0x08, 0x10, 0x14, 0x18};  // RXFnSIDH addresses
    for (int f = 0; f < 6; f++) {
        write_reg(kFilt[f], 0x00);
        write_reg((uint8_t)(kFilt[f] + 1), (f & 1) ? 0x08 : 0x00);  // SIDL: EXIDE
        write_reg((uint8_t)(kFilt[f] + 2), 0x00);
        write_reg((uint8_t)(kFilt[f] + 3), 0x00);
    }
    for (uint8_t a = 0x20; a <= 0x27; a++) write_reg(a, 0x00);  // RXM0, RXM1: accept all
    write_reg(RXB0CTRL, 0x04);   // RXM=00 (valid frames through the filters), BUKT=1 (roll over to RXB1)
    write_reg(RXB1CTRL, 0x00);   // RXM=00
    if (read_reg(RXB0CTRL) & 0x60 || read_reg(0x05) != 0x08 || read_reg(0x21) != 0x00) {
        *why = "MCP2515 receive filter set-up did not read back";
        return ESP_ERR_INVALID_RESPONSE;
    }
    write_reg(CANINTF, 0x00);
    write_reg(CANINTE, INT_RX0 | INT_RX1 | INT_ERR | INT_MERR);
    if (write_reg(CANCTRL, mode) != ESP_OK) {
        enter_config();
        *why = "operating mode not allowed on this port (SPI guard)";
        return ESP_ERR_NOT_ALLOWED;
    }
    for (int i = 0; i < 20; i++) {
        if (opmode() == (mode & MODE_MASK)) {
            auto f = decode_timing(8000000, t->cnf1, t->cnf2, t->cnf3);
            ESP_LOGI(TAG, "%s at %lu bit/s (%u TQ, sample point %u.%u%%)",
                     (mode & MODE_MASK) == MODE_LISTEN_ONLY ? "listen-only"
                     : (mode & CANCTRL_OSM)                 ? "normal, one-shot"
                                                            : "normal (ACK only)",
                     (unsigned long)f.bitrate, f.tq_per_bit, f.sample_permille / 10, f.sample_permille % 10);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    enter_config();
    *why = "MCP2515 did not confirm the operating mode";
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
