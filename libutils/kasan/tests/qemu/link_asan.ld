/* Linker script for the mps2-an505 kasan QEMU test image.
 *   FLASH @0x10000000 (4MB) -- code/vectors/rodata
 *   RAM   @0x80000000 (256KB) -- .data/.bss/stack, INSTRUMENTED by kasan
 *        (its shadow lives in the separate 32KB real RAM at 0x38000000;
 *         see kasan.h defaults -- keep KASAN_REGION_SIZE <= 256KB here.)
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 0x00400000
  RAM   (rwx) : ORIGIN = 0x80000000, LENGTH = 0x00040000
}

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text :
  {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(.init_array*))
    __init_array_end = .;
    . = ALIGN(4);
    __fini_array_start = .;
    KEEP(*(.fini_array*))
    __fini_array_end = .;
  } > FLASH

  /* .ARM.exidx unwind tables: repo builds with -funwind-tables globally,
   * so libgcc's _Unwind references __exidx_start/__exidx_end. */
  . = ALIGN(4);
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  . = ALIGN(4);
  _sidata = .;

  .data : AT(_sidata)
  {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > RAM

  .bss (NOLOAD) :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > RAM

  . = ALIGN(8);
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
