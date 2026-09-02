# Host unit tests for libutils/tracer.  Not part of any firmware build.
# They compile tracer.c on the HOST (gcc/clang, no ARM toolchain) and run the
# offline parser against synthetic dumps.  Enable from
# libutils/tracer/CMakeLists.txt via -DTRACER_BUILD_TESTS=ON.

cmake_minimum_required(VERSION 3.16)

add_executable(test_tracer_host test_tracer.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_host PRIVATE -Wall -Wextra)
endif()
target_include_directories(test_tracer_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME tracer_host COMMAND test_tracer_host)

# Lock-free mini-printf (TRACER_PUTCHAR mode) format regression test.
add_executable(test_tracer_miniprint test_miniprint.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_miniprint PRIVATE -Wall -Wextra)
endif()
target_include_directories(test_tracer_miniprint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME tracer_miniprint COMMAND test_tracer_miniprint)

# Crash-log "black box" (TRACER_USE_CRASHLOG): pre-crash ring log, dump
# capture mirror + CRC footer assembly.
add_executable(test_tracer_crashlog test_crashlog.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_crashlog PRIVATE -Wall -Wextra)
endif()
target_include_directories(test_tracer_crashlog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME tracer_crashlog COMMAND test_tracer_crashlog)

# Media-agnostic crash-record store (tracer_crash_store.c): double-slot
# strategy tested against a RAM "flash" (link crash_store with the test).
add_executable(test_tracer_crash_store
    test_crash_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../tracer_crash_store.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_crash_store PRIVATE -Wall -Wextra)
endif()
target_compile_definitions(test_tracer_crash_store PRIVATE TRACER_USE_CRASHLOG=1)
target_include_directories(test_tracer_crash_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_test(NAME tracer_crash_store COMMAND test_tracer_crash_store)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tracer_parser
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_parser.py)
endif()
