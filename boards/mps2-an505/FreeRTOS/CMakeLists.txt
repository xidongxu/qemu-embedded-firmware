set(TARGET an505-qemu)

add_executable(${TARGET})
set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "${TARGET}"
    SUFFIX ".elf"
)

file(GLOB APP_SOURCES
    "application/*.c"
)

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

target_include_directories(${TARGET} PRIVATE
    application
    ../Core/Inc
    ../Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ../Drivers/CMSIS_5/CMSIS/Core/Include
    ../Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ../Drivers/CMSIS_5/CMSIS/Core/Include
    ${CMAKE_SOURCE_DIR}/libutils/lwip/src/include
    ${CMAKE_SOURCE_DIR}/libutils/lwip/ports
)

target_compile_definitions(${TARGET} PRIVATE
    ARMCM33_DSP_FP
    LAN9118_USE_FREERTOS=1
    LAN9118_HAVE_LWIP=1
)

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

target_link_libraries(${TARGET} PRIVATE
    rtos
    utils
    lvgl
    lwipcore
)

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
