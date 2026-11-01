idf_component_register(SRCS "src/net.cpp" "src/captive_dns.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event
                       PRIV_REQUIRES config lwip esp_timer espressif__mdns)
