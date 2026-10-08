cmake_minimum_required(VERSION 3.10)
project(atsss_project)

set(CMAKE_C_STANDARD 11)

IF (DEFINED ENV{FEMTO_QUIC_DIR})
    set(FEMTO_QUIC_DIR $ENV{FEMTO_QUIC_DIR})
ELSE()
    MESSAGE(FATAL_ERROR "FEMTO_QUIC_DIR not defined")
ENDIF()

IF (DEFINED ENV{AUTOSYNDESIS_DIR})
    set(AUTOSYNDESIS_DIR $ENV{AUTOSYNDESIS_DIR})
ELSE()
    MESSAGE(FATAL_ERROR "AUTOSYNDESIS_DIR not defined")
ENDIF()

IF (DEFINED ENV{PICOQUIC_DIR})
    set(PICOQUIC_DIR $ENV{PICOQUIC_DIR})
ELSE()
    MESSAGE(FATAL_ERROR "PICOQUIC_DIR not defined")
ENDIF()

IF (DEFINED ENV{PICOTLS_DIR})
    set(PICOTLS_DIR $ENV{PICOTLS_DIR})
ELSE()
    MESSAGE(FATAL_ERROR "PICOTLS_DIR not defined")
ENDIF()

option (ARM_ARCH "Build on ARM Architecture" OFF)

# turn on debug prints
option(ENABLE_DEBUG "Enable debug prints" OFF)
if(ENABLE_DEBUG)
    add_compile_definitions(ENABLE_DEBUG)
    MESSAGE("-- Debug printing set ON")
endif()

# AuToSyndeSiS library
include_directories(${AUTOSYNDESIS_DIR})
link_libraries(${AUTOSYNDESIS_DIR}/build/libautosyndesis.a)

# Femto-QUIC library
include_directories(${FEMTO_QUIC_DIR})
link_libraries(${FEMTO_QUIC_DIR}/build/libfemto.a)

# PicoQUIC loglib library
include_directories(${PICOQUIC_DIR}/loglib)
link_libraries(${PICOQUIC_DIR}/libpicoquic-log.a)

# PicoQUIC library
include_directories(${PICOQUIC_DIR}/picoquic)
link_libraries(${PICOQUIC_DIR}/libpicoquic-core.a)

# PicoTLS library
include_directories(${PICOTLS_DIR}/include)
link_libraries(${PICOTLS_DIR}/libpicotls-core.a)
if(NOT ARM_ARCH AND EXISTS "${PICOTLS_DIR}/libpicotls-fusion.a")
    link_libraries(${PICOTLS_DIR}/libpicotls-fusion.a)
endif()
link_libraries(${PICOTLS_DIR}/libpicotls-openssl.a)
link_libraries(${PICOTLS_DIR}/libpicotls-minicrypto.a)

find_package(OpenSSL REQUIRED)
link_libraries(OpenSSL::SSL OpenSSL::Crypto)
link_libraries(pthread)
link_libraries(m) # For math library if needed

#curl
find_package(CURL REQUIRED)

add_executable(atsss_ue
    ue_main.c
    curl_functions.c
    cJSON.c)
target_link_libraries(atsss_ue
    PRIVATE CURL::libcurl)

add_executable(atsss_ane
    ane_main.c
    deduplication.c
    curl_functions.c
    cJSON.c)
target_link_libraries(atsss_ane
    PRIVATE CURL::libcurl)

add_executable(atsss_server
    server_main.c)
