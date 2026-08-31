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
    ${TRACER_ASM}
)

target_include_directories(tracer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# tracer must itself be built with -funwind-tables so it emits .ARM.exidx
# unwind tables: _Unwind_Backtrace needs an entry for the current (library
# internal) frame, otherwise a backtrace started from tracer_get_callstack /
# tracer_dump_callstack stops at 0 frames.  This is a library-inherent
# property, so it lives here rather than in each consumer's script.
#
# Whether the exact .ARM.exidx path is actually used is decided by
# TRACER_USE_EXIDX (tracer.h, default 0; GCC/armclang only, others fall back
# to the BL/BLX scan).  Opt in from the consuming project with:
#   target_compile_definitions(tracer PRIVATE TRACER_USE_EXIDX=1)
# The consumer should also add -funwind-tables to its OWN sources so the
# unwind chain reaches frames beyond the first app frame.
if(TRACER_EXIDX_FLAG)
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
