set(TARGET an505-qemu)

add_executable(${TARGET})
set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "${TARGET}"
    SUFFIX ".elf"
)

file(GLOB APP_SOURCES
    "application/main.c"
    "application/system_ARMCM33.c"
    "../Core/Src/*.c"
    "../../../libutils/fault-dump/src/*.c"
    "../../../libutils/fault-dump/cpu/cortex-m33/fault-hal.c"
)

file(GLOB STARTUP_SOURCES
    "startup/gcc/startup_ARMCM33.s"
    "../../../libutils/fault-dump/cpu/cortex-m33/fault-gcc.s"
)

target_sources(${TARGET} PRIVATE
    ${APP_SOURCES}
    ${STARTUP_SOURCES}
)

target_include_directories(${TARGET} PRIVATE
    application
    ../Core/Inc
    ../Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ../Drivers/CMSIS_5/CMSIS/Core/Include
    ${CMAKE_SOURCE_DIR}/libutils/fault-dump/inc
    ${CMAKE_SOURCE_DIR}/libutils/fault-dump/cpu/cortex-m33
)

target_compile_definitions(${TARGET} PRIVATE
    ARMCM33_DSP_FP
)

target_compile_options(${TARGET} PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfpu=fpv5-sp-d16
    -mfloat-abi=hard
    -Wall
    -specs=nosys.specs
    -fdata-sections
    -ffunction-sections
    -fno-exceptions
)

target_link_libraries(${TARGET} PRIVATE utils)

target_link_options(${TARGET} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/startup/gcc/gcc_arm.ld
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.map,--cref
    -Wl,--gc-sections
    -lc
    -lm
    -lnosys
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
