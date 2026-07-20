file(GLOB UTILS_SRC
    "src/*.c"
    "cpu/cortex-m33/*.c"
    "cpu/cortex-m33/fault-gcc.s"
)

if(TARGET_PROJECT STREQUAL "FreeRTOS")
    file(GLOB FREERTOS_SYS_SRC "sys/FreeRTOS/*.c")
    list(APPEND UTILS_SRC ${FREERTOS_SYS_SRC})
elseif(TARGET_PROJECT STREQUAL "BareMetal" OR TARGET_PROJECT STREQUAL "threadx")
    file(GLOB BAREMETAL_SYS_SRC "sys/BareMetal/*.c")
    list(APPEND UTILS_SRC ${BAREMETAL_SYS_SRC})
else()
    message(FATAL_ERROR "Unsupported PROJECT=\"${TARGET_PROJECT}\" for utils fault-dump. Use FreeRTOS, BareMetal or threadx.")
endif()

add_library(utils STATIC
    ${UTILS_SRC}
)

target_include_directories(utils PUBLIC
    inc
    cpu/cortex-m33
    ${CMAKE_SOURCE_DIR}/boards/${BOARD}/Drivers/CMSIS_5/Device/ARM/ARMCM33/Include
    ${CMAKE_SOURCE_DIR}/boards/${BOARD}/Drivers/CMSIS_5/CMSIS/Core/Include
    ${CMAKE_SOURCE_DIR}/librtos/FreeRTOS/include
)

if(TARGET_PROJECT STREQUAL "FreeRTOS")
    target_include_directories(utils PUBLIC
        ${CMAKE_SOURCE_DIR}/boards/${BOARD}/FreeRTOS/application
    )
    target_link_libraries(utils PUBLIC
        rtos
        freertos_kernel_port_headers
    )
endif()

target_compile_definitions(utils PUBLIC ARMCM33_DSP_FP)
