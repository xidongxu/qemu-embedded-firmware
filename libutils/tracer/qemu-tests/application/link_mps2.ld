/*
 * link_mps2.ld - shared layout for QEMU MPS2 non-TrustZone boards
 * (mps2-an385 M3 / an386 M4 / an500 M7).
 *
 * QEMU mps2 (non-TZ): code/vectors @0x00000000 (reset fetches from 0x0),
 * RAM @0x20000000 (SSRAM2/3, 4MB).  Tracer needs _stext/_etext/_sstack/_estack.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x00100000   /* 1M (out of 4M SSRAM1) */
    RAM   (rw) : ORIGIN = 0x20000000, LENGTH = 0x00200000   /* 2M (out of 4M SSRAM23) */
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
