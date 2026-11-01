# Phase 2: one MCP2515 in listen-only mode (the tap on port A).
# Nothing in this component can transmit on the CAN bus: see mcp2515_regs.h.
idf_component_register(SRCS "src/mcp2515.cpp" "src/tap.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_spi esp_driver_gpio esp_timer esp_system freertos config)
