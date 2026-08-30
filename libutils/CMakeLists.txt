add_subdirectory(fault-dump)
if(TARGET_PROJECT STREQUAL "FreeRTOS")
    set(LV_BUILD_CONF_PATH ${CMAKE_SOURCE_DIR}/boards/${BOARD}/${PROJECT}/application/lv_conf.h CACHE FILEPATH "" FORCE)
    set(LVGL_ROOT_DIR ${CMAKE_SOURCE_DIR}/libutils/lvgl/ CACHE FILEPATH "" FORCE)
    add_subdirectory(lvgl)
endif()
if(TARGET_PROJECT STREQUAL "FreeRTOS")
    # lwIP OS backend:
    #   FreeRTOS -> OS mode (NO_SYS=0) with a dedicated tcpip_thread, backed by
    #               the local FreeRTOS sys_arch port under lwip/ports/freertos;
    #   none      -> classic NO_SYS (raw API only) build.
    # (CACHE set with a default only writes if the option is not already set,
    #  so a user-provided -DLWIP_OS=... is always preserved.)
    set(LWIP_OS FreeRTOS CACHE STRING "lwIP OS backend (none|FreeRTOS)")
    set_property(CACHE LWIP_OS PROPERTY STRINGS none FreeRTOS)
    if(NOT (LWIP_OS STREQUAL "FreeRTOS" OR LWIP_OS STREQUAL "none"))
        message(WARNING "Unsupported LWIP_OS=\"${LWIP_OS}\"; falling back to none (NO_SYS)")
        set(LWIP_OS none CACHE STRING "lwIP OS backend (none|FreeRTOS)" FORCE)
    endif()
    message(STATUS "lwIP OS backend: ${LWIP_OS}")

    # Evaluate the OS backend once instead of repeating the string compare.
    set(LWIP_OS_ENABLED 0)
    if(LWIP_OS STREQUAL "FreeRTOS")
        set(LWIP_OS_ENABLED 1)
    endif()

    set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lwip)
    set(LWIP_INCLUDE_DIRS
        ${LWIP_DIR}/src/include
        ${LWIP_DIR}/ports
    )
    # Variables consumed by lwIP's Filelists.cmake when it creates lwipcore.
    set(LWIP_COMPILER_FLAGS "")
    set(LWIP_DEFINITIONS "")
    set(LWIP_MBEDTLS_DEFINITIONS "")
    set(LWIP_MBEDTLS_INCLUDE_DIRS "")

    if(LWIP_OS_ENABLED)
        # FreeRTOS sys_arch port: LOCAL copy under ports/freertos.  lwIP
        # upstream contrib/ports/freertos stays pristine for future upgrades.
        list(APPEND LWIP_INCLUDE_DIRS ${LWIP_DIR}/ports/freertos/include)
        list(APPEND LWIP_DEFINITIONS LWIP_USE_FREERTOS=1)
    endif()

    include(${LWIP_DIR}/src/Filelists.cmake)

    # SNTP client (boot time sync for mbedtls X.509 validity checks).  The
    # apps live in separate *_SRCS vars; fold the SNTP one into lwipcore.
    target_sources(lwipcore PRIVATE ${lwipsntp_SRCS})

    if(LWIP_OS_ENABLED)
        # FreeRTOS sys_arch backend (semaphores/mailboxes/threads/mutexes).
        target_sources(lwipcore PRIVATE ${LWIP_DIR}/ports/freertos/sys_arch.c)
        # lwIP sees the FreeRTOS kernel headers + FreeRTOSConfig.h via freertos_kernel.
        target_link_libraries(lwipcore PRIVATE freertos_kernel)
        message(STATUS "lwIP built with FreeRTOS OS support (NO_SYS=0, tcpip thread)")
    else()
        message(STATUS "lwIP built without OS support (NO_SYS=1, raw API)")
    endif()
endif()

# Embedded filesystems on the SPI NOR flash (ports/spi_flash under each lib).
# Both are generic and RTOS-independent; they are built for the main FreeRTOS
# project where the SPI flash demo lives.
if(TARGET_PROJECT STREQUAL "FreeRTOS")
    add_subdirectory(littlefs)
    add_subdirectory(fatfs)
    # mbedtls 4.2 (TLS/X509/crypto, 含 TF-PSA-Crypto) FreeRTOS port.
    # 配置集中在 libutils/mbedtls/ports/（含裸机时间/熵对接）。
    add_subdirectory(mbedtls/ports)
    # PJLIB (SIP/RTP stack) FreeRTOS port - stage 1: pjlib core only.
    add_subdirectory(pjprojec/ports/freertos)
endif()
