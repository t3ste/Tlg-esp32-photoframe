cmake_minimum_required(VERSION 3.16)

# idf.py always passes its own running interpreter as -DPYTHON=<path> on the
# cmake command line (tools/idf_py_actions/tools.py) - use that for the two
# helper scripts below instead of a bare "python3". On Windows, "python3"
# often resolves to the Microsoft Store stub (prints "Python was not found"
# and exits) rather than a real interpreter, which made both scripts fail
# silently: the version fell back to "dev-unknown" and partitions.csv was
# never (re)generated. Falls back to "python3" only for a direct `cmake`
# invocation that bypasses idf.py entirely.
if(NOT PYTHON)
    set(PYTHON python3)
endif()

# Get version from centralized script
execute_process(
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/scripts/get_version.py --type auto
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Fallback if script fails
if(NOT PROJECT_VER)
    set(PROJECT_VER "dev-unknown")
endif()

message(STATUS "Project version: ${PROJECT_VER}")

# Allow FIRMWARE_VERSION to be overridden from command line, otherwise use PROJECT_VER
if(NOT DEFINED FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "${PROJECT_VER}")
endif()

message(STATUS "Firmware version: ${FIRMWARE_VERSION}")

add_compile_options(-Wno-missing-field-initializers)
add_compile_definitions(FIRMWARE_VERSION="${FIRMWARE_VERSION}")

# Generate partitions.csv by reading config from sdkconfig / defaults files.
set(_config_files "")
if(DEFINED SDKCONFIG_DEFAULTS)
    foreach(_f ${SDKCONFIG_DEFAULTS})
        list(APPEND _config_files ${CMAKE_SOURCE_DIR}/${_f})
    endforeach()
else()
    list(APPEND _config_files ${CMAKE_SOURCE_DIR}/sdkconfig.defaults)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/sdkconfig)
    list(APPEND _config_files ${CMAKE_SOURCE_DIR}/sdkconfig)
endif()

execute_process(
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/scripts/generate_partitions.py
        --sdkconfig ${_config_files}
        --output ${CMAKE_SOURCE_DIR}/partitions.csv
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-photoframe)
