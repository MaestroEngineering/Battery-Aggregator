idf_component_register(SRCS "main.cpp" "selftest.cpp" "button.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES applog config net webserver ota can
                                     esp_driver_gpio esp_timer app_update esp_app_format lwip)
