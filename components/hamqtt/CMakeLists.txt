# MQTT and Home Assistant discovery (SPEC.md "MQTT and Home Assistant"). Read-only:
# it publishes the aggregator state; nothing can be configured over MQTT.
idf_component_register(SRCS "src/hamqtt.cpp" "src/discovery_json.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES heapmon espressif__mqtt config agg net esp_timer esp_app_format esp_hw_support freertos log
                                     espressif__cjson)
