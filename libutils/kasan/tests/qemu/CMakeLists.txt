# QEMU firmware test for kasan (board: mps2-an505, Cortex-M33).
#
# Builds a bare-metal image whose C source is instrumented with
# -fsanitize=kernel-address, linked against the kasan library.  KHEAP_CASE
# selects the injected fault:
#   1 = heap overflow   2 = use-after-free   3 = double-free
#   4 = heap underflow
# Run under QEMU, then read kasan_* markers with gdb:
#   qemu-system-arm -machine mps2-an505 -cpu cortex-m33 -m 16M \
#       -kernel <elf> -display none -serial null -monitor none -no-reboot \
#       -gdb tcp::1237
#   arm-none-eabi-gdb <elf> -batch -ex 'target remote :1237' \
#       -ex 'p kasan_reports' -ex 'p kasan_report_type' -ex 'p kasan_report_addr'

set(KASAN_TEST_CASE 1 CACHE STRING
    "kasan QEMU test case (1=overflow 2=uaf 3=doublefree 4=underflow)")

add_executable(kasan_qemu_test
    kasan_test.c
    startup_asan.s
)

target_include_directories(kasan_qemu_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..      # kasan.h
)

# Instrument ONLY the C test code (asm must not see -fsanitize).
target_compile_options(kasan_qemu_test PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-fsanitize=kernel-address>")
target_compile_definitions(kasan_qemu_test PRIVATE KHEAP_CASE=${KASAN_TEST_CASE})

# Bare-metal: no CRT, custom vector table + linker script.
target_link_libraries(kasan_qemu_test PRIVATE kasan)
target_link_options(kasan_qemu_test PRIVATE
    -nostartfiles
    "-T;${CMAKE_CURRENT_SOURCE_DIR}/link_asan.ld")
