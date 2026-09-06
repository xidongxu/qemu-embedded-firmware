/*
 * linker.ld - m55-an547 (QEMU mps3-an547, Cortex-M55, SSE-300)
 *
 * QEMU mps3-an547 boots secure from boot SRAM @0x0 (init_svtor=0,
 * 512 KiB); use SSE-300 internal SSRAM @0x21000000 (no MPC) as RAM.
 * UART0 (CMSDK APB UART, NS view) @0x49303000.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x00080000   /* 512K boot SRAM */
    RAM   (rw) : ORIGIN = 0x21000000, LENGTH = 0x00200000   /* 2M SSRAM */
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
