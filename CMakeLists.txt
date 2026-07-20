cmake_minimum_required(VERSION 3.20)

project(qemu_embedded_firmware VERSION 1.0 LANGUAGES C ASM)

set(BOARD mps2-an505 CACHE STRING "Target board")
set(PROJECT FreeRTOS CACHE STRING "Target project")
set_property(CACHE PROJECT PROPERTY STRINGS FreeRTOS BareMetal threadx)
set(TARGET_PROJECT "${PROJECT}")
set(FREERTOS_PORT GCC_ARM_CM33_NTZ_NONSECURE CACHE STRING "FreeRTOS port")
set(FREERTOS_HEAP 4 CACHE STRING "FreeRTOS heap implementation")

message(STATUS "Building board: ${BOARD}")
message(STATUS "Building project: ${PROJECT}")
message(STATUS "Using FreeRTOS port: ${FREERTOS_PORT}")

add_subdirectory(librtos)
add_subdirectory(libutils)
add_subdirectory(boards/${BOARD})
