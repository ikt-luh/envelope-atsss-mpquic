cmake_minimum_required(VERSION 3.10)
project(femtoquic)

set(CMAKE_C_STANDARD 11)

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

option(ARM_ARCH "Build on ARM architecture" OFF)

#turn on printf debug
option(ENABLE_DEBUG "Enable debug prints" OFF)
if(ENABLE_DEBUG)
    add_compile_definitions(ENABLE_DEBUG)
    MESSAGE("-- Debug printing set ON")
endif()

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

option(BUILD_EXAMPLE "Build example executable" ON)

add_library(femto
    femto_client.c
    femto_server.c
    femto_utils.c
    femto_mp_client.c
    femto_mp_server.c
    femto_interface_monitor.c
    queue.c)

if (BUILD_EXAMPLE)
    add_executable(femtoquic_example
        example.c)
    target_link_libraries(femtoquic_example femto)
endif()
