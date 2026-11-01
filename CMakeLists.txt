cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Version: read from the VERSION file (single source of truth). It ends up in
# the app descriptor (esp_app_desc_t.version) and in the release .bin name.
# Setting PROJECT_VER here also stops ESP-IDF from calling `git describe`.
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" BMS_VERSION LIMIT_COUNT 1)
string(STRIP "${BMS_VERSION}" BMS_VERSION)
if(NOT BMS_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([-+][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "VERSION file must hold a version like 0.1.0, got '${BMS_VERSION}'")
endif()
set(PROJECT_VER "${BMS_VERSION}")

# ---------------------------------------------------------------------------
# Flash size comes from platformio.ini [target] flash_size via
# -DBMS_FLASH_SIZE=... ; it selects config/sdkconfig.flash_<size>, which sets
# the flash size and the matching partitions/<size>.csv.
# ---------------------------------------------------------------------------
if(NOT DEFINED BMS_FLASH_SIZE)
    message(FATAL_ERROR "BMS_FLASH_SIZE not set. Build with PlatformIO (it is set from "
                        "platformio.ini [target]) or pass -DBMS_FLASH_SIZE=4MB.")
endif()
set(_flash_defaults "${CMAKE_CURRENT_LIST_DIR}/config/sdkconfig.flash_${BMS_FLASH_SIZE}")
if(NOT EXISTS "${_flash_defaults}")
    message(FATAL_ERROR "No ${_flash_defaults} for flash size ${BMS_FLASH_SIZE}")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${_flash_defaults}")

# Test-only build that deliberately fails the post-OTA self-test.
if(BMS_FAIL_SELFTEST)
    add_compile_definitions(BMS_FAIL_SELFTEST=1)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The project name is written into the app descriptor and is checked on every
# OTA upload; images with another project name are rejected.
project(bms-agg)
