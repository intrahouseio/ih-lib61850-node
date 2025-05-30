cmake_minimum_required(VERSION 3.10)
project(ih-libiec61850-node)

# Build libiec61850
add_subdirectory(third_party/libiec61850)

# Build mbedtls
add_subdirectory(third_party/mbedtls)

# Ensure libiec61850 is built with TLS support
set(CONFIG_MMS_TLS_SUPPORT ON CACHE BOOL "Enable TLS support")
set(MBEDTLS_DIR ${CMAKE_SOURCE_DIR}/third_party/mbedtls)

# Install libiec61850 headers and static library
install(TARGETS iec61850 ARCHIVE DESTINATION lib)
install(DIRECTORY third_party/libiec61850/src/ DESTINATION include
        FILES_MATCHING PATTERN "*.h")