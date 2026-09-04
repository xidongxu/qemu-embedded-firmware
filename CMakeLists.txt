cmake_minimum_required(VERSION 3.20)

project(qemu_embedded_firmware VERSION 1.0 LANGUAGES C CXX ASM)

set(BOARD mps2-an505 CACHE STRING "Target board")
set(PROJECT FreeRTOS CACHE STRING "Target project")
set_property(CACHE PROJECT PROPERTY STRINGS FreeRTOS BareMetal threadx)
set(TARGET_PROJECT "${PROJECT}")
message(STATUS "Building board: ${BOARD}")
message(STATUS "Building project: ${PROJECT}")

add_compile_options(-O2)
# Debug aid: build EVERY target with .ARM.exidx unwind tables so tracer's
# exact backtrace (tracer_dump_callstack / tracer_get_callstack with
# TRACER_USE_EXIDX) can walk through ANY library (app, lvgl, pjproject, ...).
# An exidx unwind needs unwind tables in every frame of the chain, so this is
# global rather than per-library.  Costs a little flash for the extra
# .ARM.exidx tables.  (tracer and the app CMake still list it explicitly --
# harmless redundancy that keeps the requirement obvious per target.)
add_compile_options(-funwind-tables)

if(TARGET_PROJECT STREQUAL "FreeRTOS")
    set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
    # Unified TLSF allocator (libmem/tlsf): C malloc/new + FreeRTOS kernel
    # heap share one TLSF pool.  Enabled by default on mps2-an505.
    if(BOARD STREQUAL "mps2-an505")
        set(TLSF_MALLOC ON CACHE BOOL "Use unified TLSF allocator (malloc/new + FreeRTOS heap)")
    else()
        set(TLSF_MALLOC OFF CACHE BOOL "Use unified TLSF allocator (malloc/new + FreeRTOS heap)")
    endif()
    if(TLSF_MALLOC)
        # Route the FreeRTOS kernel heap (pvPortMalloc) through TLSF too.
        set(FREERTOS_HEAP ${CMAKE_SOURCE_DIR}/libmem/tlsf/ports/freertos/tlsf_freertos.c CACHE STRING "FreeRTOS heap implementation" FORCE)
    else()
        set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")
    endif()
    message(STATUS "Using FreeRTOS port: ${FREERTOS_PORT}")
    message(STATUS "TLSF_MALLOC: ${TLSF_MALLOC}")
    message(STATUS "FreeRTOS heap: ${FREERTOS_HEAP}")
endif()
if(TARGET_PROJECT STREQUAL "threadx")
    add_compile_definitions(
        TX_INCLUDE_USER_DEFINE_FILE
        TX_ENABLE_STACK_CHECKING
        TX_ENABLE_FPU_SUPPORT
        TX_SINGLE_MODE_SECURE
    )
endif()

add_subdirectory(librtos)
add_subdirectory(libmem)
add_subdirectory(libutils)
add_subdirectory(boards/${BOARD})
