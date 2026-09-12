# QEMU firmware test for kasan (board: mps2-an505, Cortex-M33).
#
# Builds a bare-metal image whose C source is instrumented with
# -fsanitize=kernel-address, linked against the kasan library.  KHEAP_CASE
# selects the injected fault:
#   1 = heap overflow   2 = use-after-free   3 = double-free
#   4 = heap underflow  5 = realloc-move-uaf 6 = realloc-shrink
#   7 = partial-granule-tail  8 = memcpy-read-overflow  9 = memset-write-overflow
#  10 = quarantine-uaf  11 = global-overflow  12 = stack-overflow
#  13 = multi-region (a second instrumented segment)
#  14 = multi-heap (a runtime-registered second heap)
# Run under QEMU, then read kasan_* markers with gdb:
#   qemu-system-arm -machine mps2-an505 -cpu cortex-m33 -m 16M \
#       -kernel <elf> -display none -serial null -monitor none -no-reboot \
#       -gdb tcp::1237
#   arm-none-eabi-gdb <elf> -batch -ex 'target remote :1237' \
#       -ex 'p kasan_reports' -ex 'p kasan_report_type' -ex 'p kasan_report_addr'

set(KASAN_TEST_CASE 1 CACHE STRING
    "kasan QEMU test case (1=overflow 2=uaf 3=doublefree 4=underflow 5=realloc-move 6=realloc-shrink 7=partial-granule 8=memcpy-read 9=memset-write 10=quarantine-uaf 11=global-overflow 12=stack-overflow 13=multi-region 14=multi-heap)")

add_executable(kasan_qemu_test
    kasan_test.c
    startup_asan.s
)

target_include_directories(kasan_qemu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..      # kasan.h
)

# Instrument ONLY the C test code (asm must not see -fsanitize).
# --param asan-globals=1 makes the compiler emit a per-global redzone plus a
# .init_array call to __asan_register_globals (global redzone detection).
# --param asan-stack=1 + -fasan-shadow-offset make the compiler inline-poison
# each function's stack redzone.  The offset MUST equal
# KASAN_SHADOW_BASE - KASAN_REGION_BASE/8 (0x80038000 - 0x80000000/8); keep
# it in sync with kasan.h if the region macros change.
target_compile_options(kasan_qemu_test PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>"
    "$<$<COMPILE_LANGUAGE:C>:--param=asan-globals=1>"
    "$<$<COMPILE_LANGUAGE:C>:--param=asan-stack=1>"
    "$<$<COMPILE_LANGUAGE:C>:-fasan-shadow-offset=0x70038000>")
target_compile_definitions(kasan_qemu_test PRIVATE KHEAP_CASE=${KASAN_TEST_CASE})

# Case 13 exercises multi-region coverage: enable a second instrumented
# segment (0x80100000, 128 KB -- inside mps2-an505's 16 MB system RAM) on
# the kasan library itself.  The app only uses literal addresses; the shadow
# mapping for the extra segment lives in kasan.c.
if(KASAN_TEST_CASE STREQUAL "13")
    target_compile_definitions(kasan PRIVATE
        KASAN_REGION1_BASE=0x80100000u KASAN_REGION1_SIZE=0x20000u)
endif()

# Bare-metal: no CRT, custom vector table + linker script.  Wrap
# memcpy/memset/memmove so instrumented app code's bulk copies are checked.
target_link_libraries(kasan_qemu_test PRIVATE kasan)
target_link_options(kasan_qemu_test PRIVATE
    -nostartfiles
    "-T;${CMAKE_CURRENT_SOURCE_DIR}/link_asan.ld"
    -Wl,--wrap=memcpy
    -Wl,--wrap=memset
    -Wl,--wrap=memmove)
