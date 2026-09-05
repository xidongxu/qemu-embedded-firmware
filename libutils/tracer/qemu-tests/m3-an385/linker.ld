/*
 * linker.ld - m3-an385 (QEMU mps2-an385, Cortex-M3)
 *
 * QEMU mps2 (non-TZ) memory: code/vectors @0x00000000 (SSRAM1 alias,
 * armv7m reset fetches from 0x0), RAM @0x20000000 (SSRAM2/3, 4MB).
 * The tracer needs the linker symbols _stext/_etext/_sstack/_estack.
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

    /* End of the flash-resident image; .data loads right after it. */
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

    /* Stack at the top of RAM; tracer reads _sstack/_estack. */
    . = ALIGN(8);
    _sstack = .;
    . = . + __STACK_SIZE;
    _estack = .;
    PROVIDE(__stack = _estack);
}
