#pragma once
// Pin map for the three MCP2515 CAN modules on a classic ESP32 (esp32dev).
// Approved 2026-11-01. See SPEC.md "Hardware platform".
//
//  - One shared SPI bus (VSPI on its native IO_MUX pins), one CS per module.
//  - Each module has its own INT line on an input-only GPIO (34/35/36), via a
//    divider from the 5 V module. INT is push-pull: never wire-OR them.
//  - SCK, MOSI, MISO and the three CS lines go through the level shifter.
//    10k pull-ups to 3.3 V on each CS keep the modules deselected during boot.
//  - Avoided: strapping pins 0, 2, 5, 12, 15; flash pins 6-11; UART0 1/3.
//  - GPIO36/39 erratum: ~80 ns low glitches while the ADC is powered. INT is
//    handled by reading CANINTF first, so a glitch only costs one SPI read.
//  - Free for later: 4, 13, 14, 16, 17, 21, 22, 32, 33. GPIO0 = BOOT button.

#define BMS_CAN_SPI_SCK   18
#define BMS_CAN_SPI_MOSI  23
#define BMS_CAN_SPI_MISO  19
#define BMS_CAN_SPI_HZ    1000000  // SPEC.md: start SPI at 1-2 MHz

// Port INV -> inverter (MCP2515 #1). Not used yet (Phase 3).
#define BMS_CAN_INV_CS    25
#define BMS_CAN_INV_INT   34

// Port A -> battery A (MCP2515 #2). Phase 2: the listen-only tap.
#define BMS_CAN_A_CS      26
#define BMS_CAN_A_INT     35

// Port B -> battery B (MCP2515 #3). Not used yet (Phase 3).
#define BMS_CAN_B_CS      27
#define BMS_CAN_B_INT     36

// MCP2515 module crystal. With 8 MHz the fastest possible bitrate is 500 kbit/s.
#define BMS_CAN_MCP2515_XTAL_HZ 8000000

// Port numbers used in frame records.
#define BMS_CAN_PORT_INV 0
#define BMS_CAN_PORT_A   1
#define BMS_CAN_PORT_B   2
