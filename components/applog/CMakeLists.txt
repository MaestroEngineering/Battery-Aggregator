# "applog" rather than "log": a project component named "log" would replace
# ESP-IDF's own log component.
idf_component_register(SRCS "src/applog.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES log freertos)
