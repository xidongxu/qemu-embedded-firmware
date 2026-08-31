set(TARGET an505-qemu)

# Dual-QEMU inter-instance call role (stage 11): "none" runs the normal
# single-instance loopback suite; "caller" builds the UAC that dials out;
# "callee" builds the UAS that answers.  Caller/callee elf files are built
# into separate build trees (e.g. build-caller / build-callee).
set(PJ_DUAL_ROLE "none" CACHE STRING "Dual-QEMU role: none|caller|callee")
set(PJ_HOST_CALL OFF CACHE BOOL "Caller dials a HOST SIP UA (pjsua) instead of a peer QEMU")
set(PJ_PHONE OFF CACHE BOOL "Build the PJSUA high-level phone app (pj_phone.c)")

add_executable(${TARGET})
set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "${TARGET}"
    SUFFIX ".elf"
)

file(GLOB APP_SOURCES
    "application/*.c"
)

# Unified TLSF allocator: replace the stock CubeMX sysmem.c (_sbrk over the
# whole main heap) with sysmem_tlsf.c (separate small backing pool for libc
# internals) so the TLSF pool never overlaps newlib's _sbrk region.
if(TLSF_MALLOC)
    list(REMOVE_ITEM APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/application/sysmem.c")
else()
    list(REMOVE_ITEM APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/application/sysmem_tlsf.c")
endif()

# Dual-role-only tests (#error without PJ_DUAL_ROLE_*): exclude them in the
# normal (non-dual) single-instance build, which runs the pjlab experiments
# (pj_test..pj_call).  pj_sip_dual_test.c already lives in pjlab/ and is
# excluded by pjlab/CMakeLists.txt; net_burst_test.c stays here in
# application/ (bare-network burst test, no pjproject).
if(PJ_DUAL_ROLE STREQUAL "none")
    list(REMOVE_ITEM APP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/application/net_burst_test.c")
endif()

# Only one lwIP demo app is built at a time: NO_SYS (raw API) vs OS mode.
if(LWIP_OS STREQUAL "FreeRTOS")
    list(REMOVE_ITEM APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/application/lwip_test.c")
else()
    list(REMOVE_ITEM APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/application/lwip_os_test.c")
endif()

file(GLOB CORE_SOURCES
    "../Core/Src/*.c"
)

file(GLOB STARTUP_SOURCES
    "startup/gcc/*.s"
    "startup/gcc/*.S"
)

target_sources(${TARGET} PRIVATE
    ${APP_SOURCES}
    ${CORE_SOURCES}
    ${STARTUP_SOURCES}
)

# pjlab: pjproject learning experiments (real sources moved out of
# application/; application/ is reserved for application development).
add_subdirectory(pjlab)

target_include_directories(${TARGET} PRIVATE
    application
    ../Core/Inc
    ../Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ../Drivers/CMSIS_5/CMSIS/Core/Include
    ../Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ../Drivers/CMSIS_5/CMSIS/Core/Include
    ${CMAKE_SOURCE_DIR}/libutils/lwip/src/include
    ${CMAKE_SOURCE_DIR}/libutils/lwip/ports
    ${CMAKE_SOURCE_DIR}/libutils/pjprojec/pjnath/include
)
if(LWIP_OS STREQUAL "FreeRTOS")
    # OS mode pulls in arch/sys_arch.h via lwip/sys.h (local port copy).
    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/libutils/lwip/ports/freertos/include
    )
endif()

target_compile_definitions(${TARGET} PRIVATE
    ARMCM33_DSP_FP
    LAN9118_USE_FREERTOS=1
    LAN9118_HAVE_LWIP=1
)
if(PJ_DUAL_ROLE STREQUAL "caller")
    target_compile_definitions(${TARGET} PRIVATE PJ_DUAL_ROLE_CALLER=1)
    if(PJ_HOST_CALL)
        # PJ_HOST_CALL: dial a SIP UA on the HOST (e.g. pjsua) instead of a
        # peer QEMU.  Requires the caller role; see pj_sip_dual_test.c for
        # the slirp address-rewriting this implies.
        target_compile_definitions(${TARGET} PRIVATE PJ_HOST_CALL=1)
    endif()
elseif(PJ_DUAL_ROLE STREQUAL "callee")
    target_compile_definitions(${TARGET} PRIVATE PJ_DUAL_ROLE_CALLEE=1)
elseif(NOT PJ_DUAL_ROLE STREQUAL "none")
    message(FATAL_ERROR "PJ_DUAL_ROLE must be one of none|caller|callee")
endif()
if(PJ_PHONE)
    # PJ_PHONE: the mpsx audio/mic audiodev backend (application/mpsx_dev.c)
    # is registered at runtime from pj_phone.c via pjmedia_aud_register_factory().
    # The macro is board-scoped so other boards sharing ports/freertos are
    # unaffected (their pj_phone.c falls back to the null sound device).
    target_compile_definitions(${TARGET} PRIVATE PJ_PHONE=1 PJMEDIA_AUDIO_DEV_HAS_MPSX=1)
endif()
option(PJ_PHONE_OPUS_BENCH "Run the standalone Opus encode/decode benchmark only (no pjsua)" OFF)
if(PJ_PHONE_OPUS_BENCH)
    target_compile_definitions(${TARGET} PRIVATE PJ_PHONE_OPUS_BENCH=1)
endif()
if(LWIP_OS STREQUAL "FreeRTOS")
    # Flip NO_SYS=0 for this target too so lan9118_netif.c (Core/Src) and
    # lwip_os_test.c build in OS mode (input = tcpip_input, etc.).
    target_compile_definitions(${TARGET} PRIVATE LWIP_USE_FREERTOS=1)
endif()

target_compile_options(${TARGET} PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfpu=fpv5-sp-d16
    -mfloat-abi=hard
    -Wall
    -fdata-sections
    -ffunction-sections
    -specs=nano.specs
)

target_link_options(${TARGET} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/startup/gcc/gcc_arm.ld
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.map,--cref
    -Wl,--gc-sections
    -specs=nano.specs
    -lc
    -lm
)

if(TLSF_MALLOC)
    # Unified memory: route C malloc/free/calloc/realloc/memalign through TLSF
    target_link_options(${TARGET} PRIVATE
        -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign
    )
endif()

target_link_libraries(${TARGET} PRIVATE
    rtos
    utils
    lvgl
    lwipcore
    littlefs
    fatfs
    pjlib
    pjlib-util
    pjnath
    pjmedia
    pjmedia-audiodev
    pjsip
    pjsip-simple
    pjsip-ua
    pjsua-lib
)

if(TLSF_MALLOC)
    target_link_libraries(${TARGET} PRIVATE tlsf)
endif()

set(HEX_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.hex")
set(BIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.bin")

add_custom_command(
    OUTPUT ${HEX_FILE}
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET}> ${HEX_FILE}
    DEPENDS ${TARGET}
    COMMENT "Generating ${TARGET}.hex"
    VERBATIM
)

add_custom_command(
    OUTPUT ${BIN_FILE}
    COMMAND ${CMAKE_OBJCOPY} -O binary -S $<TARGET_FILE:${TARGET}> ${BIN_FILE}
    DEPENDS ${TARGET}
    COMMENT "Generating ${TARGET}.bin"
    VERBATIM
)

add_custom_target(${TARGET}_images ALL
    DEPENDS ${HEX_FILE} ${BIN_FILE}
)
