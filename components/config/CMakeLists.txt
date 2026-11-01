idf_component_register(SRCS "src/config.cpp" "src/password.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES nvs_flash mbedtls esp_partition joltwallet__littlefs)
