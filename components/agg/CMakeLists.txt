# Aggregator: decode the battery sources, combine, fail-safe (SPEC.md "Combine engine",
# "Fail-safe behaviour"). engine.cpp is pure C++ (host-tested by tools/test_agg.cpp and
# tools/sim_agg.cpp); glue.cpp ties it to the CAN task, the settings and the OTA hook.
idf_component_register(SRCS "src/engine.cpp" "src/glue.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES can
                       PRIV_REQUIRES config esp_timer freertos log nvs_flash)
