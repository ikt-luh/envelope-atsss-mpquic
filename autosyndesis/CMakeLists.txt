cmake_minimum_required(VERSION 3.10)
project(autosyndesis)

set(CMAKE_C_STANDARD 11)

option(BUILD_EXAMPLE "Build example executable" ON)
option(ARM_ARCH "Build on AMR architecture" OFF)

option(ENABLE_DEBUG "Enable debug prints" OFF)
if(ENABLE_DEBUG)
    add_compile_definitions(ENABLE_DEBUG)
    MESSAGE("-- Debug printing set ON")
endif()

if (BUILD_EXAMPLE)
    IF (DEFINED ENV{FEMTO_QUIC_DIR})
        set(FEMTO_QUIC_DIR $ENV{FEMTO_QUIC_DIR})
    ELSE()
        MESSAGE(FATAL_ERROR "FEMTO_QUIC_DIR not defined")
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
endif()


add_library(autosyndesis
    autosyndesis_actv_stdby.c
    autosyndesis_smallest_delay.c
    autosyndesis_lb_rr.c
    autosyndesis.c)

if (BUILD_EXAMPLE)
    add_executable(autosyndesis_example
        example.c)
    target_link_libraries(autosyndesis_example autosyndesis)
endif()
