# Host unit tests for tracer (tests/host).  Not part of any firmware build.
# They compile tracer.c on the HOST (gcc/clang, no ARM toolchain) and run the
# offline parser against synthetic dumps.  Enable from the repo root
# CMakeLists.txt via -DTRACER_BUILD_TESTS=ON.

cmake_minimum_required(VERSION 3.16)

add_executable(test_tracer_host test_tracer.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_host PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_include_directories(test_tracer_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_host COMMAND test_tracer_host)

# Fault-handler host test: drives tracer_fault_handler() directly with a RAM
# MMIO backend (registers + fake exception frames), asserting the emitted
# dump text.
add_executable(test_tracer_fault test_fault_handler.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_fault PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_include_directories(test_tracer_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_fault COMMAND test_tracer_fault)

# Lock-free mini-printf (TRACER_PUTCHAR mode) format regression test.
add_executable(test_tracer_miniprint test_miniprint.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_miniprint PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_include_directories(test_tracer_miniprint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_miniprint COMMAND test_tracer_miniprint)

# Crash "black box" (TRACER_USE_CRASH): pre-crash ring log, dump
# capture mirror + CRC footer assembly.
add_executable(test_tracer_crashlog test_crashlog.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_crashlog PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_include_directories(test_tracer_crashlog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_crashlog COMMAND test_tracer_crashlog)

# Leveled runtime log (TRACER_USE_LOG + TRACER_USE_CRASH, tracer.c
# #included): runtime level filter, unified serial+ring output, truncation,
# incremental drain, and the crash-record replay including recent logs.
add_executable(test_tracer_log test_tracer_log.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_log PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_include_directories(test_tracer_log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_log COMMAND test_tracer_log)

# Leveled runtime log: weak per-line sink override (tracer.c is linked as a
# separate object so the test's strong tracer_log_sink replaces the weak
# default, as a real app would for file/flash logging).
add_executable(test_tracer_log_sink
    test_tracer_log_sink.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tracer.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_log_sink PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_compile_definitions(test_tracer_log_sink PRIVATE
    TRACER_USE_LOG=1
    TRACER_STACK_DUMP_BYTES=0
    TRACER_STACK_BASE=0x800
    TRACER_STACK_TOP=0x1000
    TRACER_TEXT_START=0x08000000
    TRACER_TEXT_END=0x08020000)
target_include_directories(test_tracer_log_sink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_log_sink COMMAND test_tracer_log_sink)

# Media-agnostic crash-record store (tracer_crash_store.c): double-slot
# strategy tested against a RAM "flash" (link crash_store with the test).
add_executable(test_tracer_crash_store
    test_crash_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tracer_crash_store.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_tracer_crash_store PRIVATE
        -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
endif()
target_compile_definitions(test_tracer_crash_store PRIVATE TRACER_USE_CRASH=1)
target_include_directories(test_tracer_crash_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME tracer_crash_store COMMAND test_tracer_crash_store)

# Weak-media default stubs: link only part of the media layer (see the test
# header) so crash_store's weak get_media/erase/write/read defaults execute.
foreach(mode 0 1 2)
    add_executable(test_crash_store_stubs${mode} test_crash_store_stubs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tracer_crash_store.c)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_crash_store_stubs${mode} PRIVATE
            -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast)
    endif()
    target_compile_definitions(test_crash_store_stubs${mode} PRIVATE
        TRACER_USE_CRASH=1 TCS_STUB_MODE=${mode})
    target_include_directories(test_crash_store_stubs${mode} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    add_test(NAME crash_store_stubs${mode} COMMAND test_crash_store_stubs${mode})
endforeach()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tracer_parser
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_parser.py)
endif()
