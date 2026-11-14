idf_component_register(SRCS "src/heapmon.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES heap
                       PRIV_REQUIRES esp_timer log)
