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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME tracer_parser
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_parser.py)
endif()
