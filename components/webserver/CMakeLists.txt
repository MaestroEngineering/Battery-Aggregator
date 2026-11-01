# Web UI sources live in <project>/web. tools/pack_www.py gzips them into
# generated/www_assets.c (PlatformIO runs it before every build via
# tools/pio_prebuild.py; it is also run here at configure time as a fallback).
idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(python PYTHON)
set(WWW_GEN "${CMAKE_CURRENT_LIST_DIR}/generated/www_assets.c")
if(NOT EXISTS "${WWW_GEN}")
    execute_process(COMMAND ${python} "${project_dir}/tools/pack_www.py" "${project_dir}/web" "${WWW_GEN}"
                    RESULT_VARIABLE _pack_rc)
    if(NOT _pack_rc EQUAL 0)
        message(FATAL_ERROR "pack_www.py failed")
    endif()
endif()

idf_component_register(SRCS "src/server.cpp" "src/api.cpp" "src/assets.cpp" "src/can_api.cpp" "${WWW_GEN}"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES config net ota applog can diag espressif__cjson app_update esp_app_format
                                     esp_timer esp_system esp_hw_support lwip esp_wifi esp_partition)
