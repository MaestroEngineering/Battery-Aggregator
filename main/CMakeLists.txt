idf_component_register(SRCS "main.cpp" "selftest.cpp" "button.cpp" "serial_cmd.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES applog config net webserver ota can diag agg hamqtt heapmon
                                     esp_driver_gpio esp_driver_uart esp_timer app_update esp_app_format lwip)
