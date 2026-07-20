set(TARGET an505-qemu)

add_executable(${TARGET})
set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "${TARGET}"
    SUFFIX ".elf"
)

file(GLOB APP_SOURCES
    "application/*.c"
    "../Core/Src/*.c"
    "../../../librtos/threadx/common/src/*.c"
    "../../../librtos/threadx/ports/cortex_m33/gnu/src/*.c"
    "../../../librtos/threadx/ports/cortex_m33/gnu/src/*.S"
)

file(GLOB STARTUP_SOURCES
    "startup/gcc/startup_ARMCM33.s"
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
    ../../../librtos/threadx/common/inc
    ../../../librtos/threadx/ports/cortex_m33/gnu/inc
)

target_compile_definitions(${TARGET} PRIVATE
    ARMCM33_DSP_FP
    TX_INCLUDE_USER_DEFINE_FILE
    TX_ENABLE_STACK_CHECKING
    TX_ENABLE_FPU_SUPPORT
    TX_SINGLE_MODE_SECURE
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

target_link_libraries(${TARGET} PRIVATE utils)

target_link_options(${TARGET} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/startup/gcc/gcc_arm.ld
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.map,--cref
    -Wl,--gc-sections
    -specs=nano.specs
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
