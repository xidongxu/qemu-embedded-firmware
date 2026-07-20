set(TARGET Core)

add_executable(
    ${TARGET}
)

file(GLOB CORE_SOURCES
    "Src/*.c"
)

target_sources(${TARGET} PRIVATE
    ${CORE_SOURCES}
)

target_include_directories(${TARGET} PRIVATE
    Inc
)

target_compile_options(${TARGET} PRIVATE
    -mcpu=cortex-m33
    -mthumb
)
