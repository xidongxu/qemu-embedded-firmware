cmake_minimum_required(VERSION 3.20)

project(qemu_embedded_firmware VERSION 1.0 LANGUAGES C ASM)

set(BOARD mps2-an505 CACHE STRING "Target board")
set(PROJECT FreeRTOS CACHE STRING "Target project")
set_property(CACHE PROJECT PROPERTY STRINGS FreeRTOS BareMetal threadx)
set(TARGET_PROJECT "${PROJECT}")
message(STATUS "Building board: ${BOARD}")
message(STATUS "Building project: ${PROJECT}")

add_compile_options(-O2)

if(TARGET_PROJECT STREQUAL "FreeRTOS")
    set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
    set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")
    message(STATUS "Using FreeRTOS port: ${FREERTOS_PORT}")
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
add_subdirectory(libutils)
add_subdirectory(boards/${BOARD})
