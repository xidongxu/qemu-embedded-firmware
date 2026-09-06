/*
 * linker.ld - m85-an555 (QEMU mps3-an555, Cortex-M85, SSE-310)
 *
 * NOTE: mps3-an555 (Cortex-M85) appeared in newer QEMU releases than the
 * 9.0.0 / 11.0.93 builds tested here, so this board currently gets
 * COMPILE-LEVEL coverage only (test_all skips the run when the machine is
 * missing).  Layout below mirrors the m55-an547 assumption (boot code at
 * 0x0, SSRAM at 0x21000000) and is marked PENDING real-QEMU verification.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x00080000
    RAM   (rw) : ORIGIN = 0x21000000, LENGTH = 0x00200000
}

__STACK_SIZE = 0x4000;

SECTIONS
{
    .isr_vector :
    {
        KEEP(*(.isr_vector))
    } > FLASH

    .text :
    {
        _stext = .;
        *(.text*)
        _etext = .;
    } > FLASH

    .rodata :
    {
        *(.rodata*)
        *(.got*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    /* GCC -fprofile-info-section: one gcov_info* per instrumented TU. */
    . = ALIGN(4);
    .gcov_info :
    {
        __gcov_info_start = .;
        KEEP(*(.gcov_info*))
        __gcov_info_end = .;
    } > FLASH

    . = ALIGN(4);
    __etext = .;
    _sidata = __etext;
    .data : AT(__etext)
    {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM

    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM

    . = ALIGN(8);
    _sstack = .;
    . = . + __STACK_SIZE;
    _estack = .;
    PROVIDE(__stack = _estack);
}
