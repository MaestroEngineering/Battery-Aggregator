idf_component_register(SRCS "src/ota.cpp" "src/image_check.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES app_update esp_app_format bootloader_support spi_flash
                                     esp_partition esp_hw_support config)
