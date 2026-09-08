# kasan tests
#
# host unit tests: build and run on a HOST-only configure (no ARM cross
# toolchain), exercising kasan.c pure logic with a RAM-backed fake region:
#   cmake -B build-host -S libutils/kasan -DKASAN_BUILD_TESTS=ON
#   cmake --build build-host && ctest --test-dir build-host
# (on a gcc-less Windows box add -DCMAKE_C_COMPILER=<zig cc>).
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(host)
endif()

# QEMU firmware test: an instrumented (-fsanitize=kernel-address) bare-metal
# image for the mps2-an505 machine.  Built in the normal ARM cross configure
# with -DKASAN_BUILD_TESTS=ON; injects a fault (KASAN_TEST_CASE) and parks
# the report in kasan_* markers for gdb to read.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|ARMClang" AND BOARD STREQUAL "mps2-an505")
    add_subdirectory(qemu)
endif()

