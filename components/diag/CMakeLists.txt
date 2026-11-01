# Verbose Diagnostics (SPEC.md): batched upload of CAN traffic to a server you run.
idf_component_register(SRCS "src/diag.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES can
                       PRIV_REQUIRES config net esp_http_client esp_timer esp_hw_support esp_app_format
                                     mbedtls freertos)
