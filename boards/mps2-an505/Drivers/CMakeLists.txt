set(TARGET firmware)

add_executable(${TARGET})

target_sources(${TARGET} PRIVATE
    GCC-Secure/main.c
    GCC-Secure/startup.s
)

target_include_directories(${TARGET} PRIVATE
    GCC-Secure
)

target_compile_options(${TARGET} PRIVATE
    -mcpu=cortex-m33
    -mthumb
)

target_link_options(${TARGET} PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/GCC-Secure/linker.ld
)

target_link_libraries(${TARGET} PRIVATE
    rtos
    utils
)
