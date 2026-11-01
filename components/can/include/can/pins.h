#pragma once
// Pin map for the three MCP2515 CAN modules (SPEC.md "Hardware platform").
//
// Phase 1: NOTHING IS ASSIGNED YET. Every pin is -1 and nothing uses this file.
//
// Wiring rules from the spec:
//  - The three modules share one SPI bus: SCK, MOSI, MISO.
//  - Each module has its own chip select (CS).
//  - Each module has its own INT line on its own GPIO (INT is push-pull: never
//    wire-OR them), through a divider when the module runs at 5 V.
//  - SCK, MOSI, MISO and the three CS lines go through a level shifter on 5 V
//    (TJA1050) boards. Start SPI at 1-2 MHz.
//  - Avoid strapping pins (classic ESP32: GPIO0, 2, 5, 12, 15) and
//    flash/PSRAM pins (GPIO6-11; on S3 also 26-32, and 33-37 with octal PSRAM).
//    GPIO0 is the BOOT button (10 s press = factory reset).

// TODO(pins): shared SPI bus
#define BMS_CAN_SPI_SCK   (-1)  // TODO
#define BMS_CAN_SPI_MOSI  (-1)  // TODO
#define BMS_CAN_SPI_MISO  (-1)  // TODO
#define BMS_CAN_SPI_HZ    (1000000)  // 1 MHz to start

// TODO(pins): port INV -> inverter (MCP2515 #1)
#define BMS_CAN_INV_CS    (-1)  // TODO
#define BMS_CAN_INV_INT   (-1)  // TODO

// TODO(pins): port A -> battery A (MCP2515 #2)
#define BMS_CAN_A_CS      (-1)  // TODO
#define BMS_CAN_A_INT     (-1)  // TODO

// TODO(pins): port B -> battery B (MCP2515 #3)
#define BMS_CAN_B_CS      (-1)  // TODO
#define BMS_CAN_B_INT     (-1)  // TODO

// TODO(crystal): MCP2515 module crystal (commonly 8 MHz, sometimes 16 MHz).
#define BMS_CAN_MCP2515_XTAL_HZ (0)  // TODO
