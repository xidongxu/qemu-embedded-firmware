# kasan -- minimal KASan (memory-safety checker) library for Cortex-M
#
# Self-contained, single-directory, no CMSIS / RTOS / printf dependency.
# Provides the shadow map + heap allocator + the __asan_{load,store}N_noabort
# hooks that `-fsanitize=kernel-address`-instrumented code calls on every
# memory access.
#
# IMPORTANT: this library must be compiled WITHOUT -fsanitize (it touches
# shadow RAM and the poisoned arena directly).  Only the CODE UNDER TEST is
# instrumented -- add -fsanitize=kernel-address to those sources, e.g.:
#
#   target_compile_options(<app> PRIVATE -fsanitize=kernel-address)
#   target_link_libraries(<app> PRIVATE kasan)
#
# Region / shadow / heap layout is configurable via -D on this target, e.g.
# for a board whose instrumented RAM and shadow RAM differ from the defaults:
#
#   target_compile_definitions(kasan PRIVATE
#       KASAN_REGION_BASE=0x20000000 KASAN_REGION_SIZE=0x1000000
#       KASAN_SHADOW_BASE=0x40000000)
#
cmake_minimum_required(VERSION 3.16)
project(kasan C)

include(GNUInstallDirs)

add_library(kasan STATIC kasan.c)

target_include_directories(kasan PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/kasan>"
)

# kasan.c uses C99 constructs; enforce it (only for the C TU).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|ARMClang")
    target_compile_options(kasan PRIVATE "$<$<COMPILE_LANGUAGE:C>:-std=c99>")
elseif(CMAKE_C_COMPILER_ID STREQUAL "ARMCC")
    target_compile_options(kasan PRIVATE "$<$<COMPILE_LANGUAGE:C>:--c99>")
endif()

# kasan.c is deliberately compiled WITHOUT -fsanitize=kernel-address -- see
# the header comment.  The hooks must be able to read shadow and walk the
# poisoned arena without re-entering a check on their own accesses.
target_compile_options(kasan PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-fno-sanitize=all>")

# Export a modern alias target: consumers can link kasan::kasan.
add_library(kasan::kasan ALIAS kasan)

# Optional tests: KASAN_BUILD_TESTS=ON builds tests/ -- host unit tests on
# a host-only configure, plus the QEMU firmware test on mps2-an505.
option(KASAN_BUILD_TESTS "Build kasan tests" OFF)
if(KASAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install / export (find_package(kasan) consumers).
include(CMakePackageConfigHelpers)

install(TARGETS kasan EXPORT kasanTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES kasan.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kasan)

install(EXPORT kasanTargets
        FILE kasanTargets.cmake
        NAMESPACE kasan::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kasan)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/kasanConfigVersion.cmake"
        VERSION 0.1.0
        COMPATIBILITY SameMajorVersion)
configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/kasanConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/kasanConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kasan)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/kasanConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/kasanConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kasan)
