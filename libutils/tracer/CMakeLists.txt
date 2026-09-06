# tracer -- minimal Cortex-M fault dump library (M3..M85)
#
# Single-directory, self-contained (no CMSIS / RTOS / printf dependency).
# CPU flags (-mcpu/-mfloat-abi) come from the global toolchain, so this
# library builds unchanged for any Cortex-M (M0/M0+/M3/M4/M7/M23/M33/M55/M85).
#
# Usage from an app CMakeLists:
#   add_subdirectory(<path>/tracer)
#   target_link_libraries(<app> PRIVATE tracer)
#
# The library exports strong symbols for the standard CMSIS fault vector
# names (HardFault_Handler, MemManage_Handler, ...) which override the weak
# defaults in the board startup file -- no startup edit needed.
#
# The vector-entry assembly file is selected automatically from the active
# toolchain (see the guard below), so this CMakeLists works unchanged for
# GCC, armclang (AC6), IAR and ARMCC.

cmake_minimum_required(VERSION 3.16)
project(tracer C ASM)

# Standard install dirs (used by the BUILD/INSTALL_INTERFACE include paths and
# the install() rules at the bottom).
include(GNUInstallDirs)

# --- Toolchain guard: pick the matching vector-entry asm and per-toolchain
# options.  GCC and armclang (AC6) both accept GNU asm syntax and share
# tracer_gnugcc.s; IAR and ARMCC need their own files (they cannot parse
# GNU asm).  Any other compiler is rejected with a clear message. ---
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|ARMClang")
    set(TRACER_ASM tracer_gnugcc.s)
    # -funwind-tables emits .ARM.exidx (see note below); GCC/armclang only.
    set(TRACER_EXIDX_FLAG -funwind-tables)
elseif(CMAKE_C_COMPILER_ID STREQUAL "IAR")
    set(TRACER_ASM tracer_iccarm.s)
    set(TRACER_EXIDX_FLAG "")
elseif(CMAKE_C_COMPILER_ID STREQUAL "ARMCC")
    set(TRACER_ASM tracer_armcc.s)
    set(TRACER_EXIDX_FLAG "")
else()
    message(FATAL_ERROR
        "tracer: unsupported C compiler '${CMAKE_C_COMPILER_ID}'.\n"
        "Supported toolchains and their vector-entry asm:\n"
        "  GNU/armclang (AC6) -> tracer_gnugcc.s\n"
        "  IAR                -> tracer_iccarm.s\n"
        "  ARMCC (MDK)        -> tracer_armcc.s")
endif()

add_library(tracer STATIC
    tracer.c
    tracer_crash_store.c
)
# The fault-vector entry assembly is ARM-only.  Host unit-test builds
# (TRACER_BUILD_TESTS=ON: gcc/clang on x86, no ARM assembler) compile the
# library purely for its C logic and never need the .s -- the tests
# #include tracer.c directly.  So only add the vector entry when building
# for a target (the normal embedded add_subdirectory / find_package path).
if(NOT TRACER_BUILD_TESTS)
    target_sources(tracer PRIVATE ${TRACER_ASM})
endif()

# Build-tree consumers see the source dir; the installed (find_package) target
# sees the headers relative to its install prefix.  (Plain source-dir paths are
# rejected by install(EXPORT), hence the BUILD_INTERFACE/INSTALL_INTERFACE.)
target_include_directories(tracer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/tracer>"
)

# tracer.c requires C99 (for-loop declarations, stdint use).  Enforce it for
# the C translation unit only (the vector-entry asm is unaffected).  ARMCC5
# needs --c99 (its CMake backend has no c_std_99 feature, so pass the flag).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|ARMClang")
    target_compile_options(tracer PRIVATE "$<$<COMPILE_LANGUAGE:C>:-std=c99>")
elseif(CMAKE_C_COMPILER_ID STREQUAL "ARMCC")
    target_compile_options(tracer PRIVATE "$<$<COMPILE_LANGUAGE:C>:--c99>")
endif()

# Whether tracer.c itself is built with -funwind-tables (.ARM.exidx).  Only
# needed for the exact TRACER_USE_EXIDX backtrace: _Unwind_Backtrace must find
# an unwind entry for the current (library-internal) frame, otherwise it stops
# at 0 frames.  The BL/BLX scan and the offline parser do NOT need it, so it
# is OFF by default to avoid the .ARM.exidx flash/size overhead.
#
# The exact .ARM.exidx path is decided by TRACER_USE_EXIDX (tracer.h, default
# 0; GCC/armclang only, others fall back to the BL/BLX scan).  To enable it
# from a consuming project, do BOTH:
#   -DTRACER_EXIDX_TABLES=ON          (this option; emits .ARM.exidx for tracer)
#   target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)
# The consumer should also add -funwind-tables to its OWN sources so the
# unwind chain reaches frames beyond the first app frame.
option(TRACER_EXIDX_TABLES "Build tracer.c with -funwind-tables (needed for TRACER_USE_EXIDX)" OFF)
if(TRACER_EXIDX_TABLES AND TRACER_EXIDX_FLAG)
    target_compile_options(tracer PRIVATE ${TRACER_EXIDX_FLAG})
endif()

# Alternative backtrace: frame-pointer chain (TRACER_USE_FP, tracer.h;
# GCC/armclang only, implemented with __builtin_return_address).  It needs
# -fno-omit-frame-pointer on the WHOLE project (this library AND every
# consumer source), so unlike -funwind-tables it is NOT added here
# unconditionally -- it has a real code-size/perf cost.  NOTE: on Cortex-M
# (Thumb) this yields only the innermost frame(s) (GCC's r7 layout + ARM
# __builtin_return_address depth limit); TRACER_USE_EXIDX is the complete
# exact backtrace.  Enable from the consumer, e.g.:
#   target_compile_definitions(tracer PRIVATE TRACER_USE_FP=1)
#   target_compile_options(tracer PRIVATE -fno-omit-frame-pointer)
#   target_compile_options(<app> PRIVATE -fno-omit-frame-pointer)
# Precedence: TRACER_USE_EXIDX > TRACER_USE_FP > BL/BLX scan.

# --- Export a modern alias target: consumers can link tracer::tracer. ---
add_library(tracer::tracer ALIAS tracer)

# --- Optional host unit tests (tracer.c pure logic + offline parser). -------
# Enable with -DTRACER_BUILD_TESTS=ON.  Tests build and run on the HOST
# (gcc/clang + python3), no ARM toolchain / firmware needed.
option(TRACER_BUILD_TESTS "Build tracer host unit tests" OFF)
if(TRACER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/host)
endif()

# --- Install / export: lets an external project use find_package(tracer) ---
# (the in-tree usage above -- add_subdirectory + target_link_libraries -- is
# unchanged and remains the primary integration path).
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS tracer EXPORT tracerTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES tracer.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tracer)

install(EXPORT tracerTargets
        FILE tracerTargets.cmake
        NAMESPACE tracer::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tracer)

write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/tracerConfigVersion.cmake"
        VERSION 1.0.0
        COMPATIBILITY SameMajorVersion)
configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tracerConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/tracerConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tracer)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/tracerConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/tracerConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tracer)
