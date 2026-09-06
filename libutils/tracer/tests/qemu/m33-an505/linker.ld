/*
 * linker.ld - m33-an505 (QEMU mps2-an505, Cortex-M33, SSE-200/IoTKit TZ)
 *
 * QEMU mps2-tz: code/vectors @0x10000000 (SSRAM alias; init_svtor, secure
 * boot), RAM @0x38000000 (SSRAM alias).  Tracer needs the linker symbols.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x10000000, LENGTH = 0x00100000
    RAM   (rw) : ORIGIN = 0x38000000, LENGTH = 0x00100000
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
