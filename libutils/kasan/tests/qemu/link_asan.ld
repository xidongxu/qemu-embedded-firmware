/* Linker script for the mps2-an505 kasan QEMU test image.
 *   FLASH @0x10000000 (4MB) -- code/vectors/rodata
 *   RAM   @0x80000000 (224KB usable) -- .data/.bss/stack, INSTRUMENTED.
 * The shadow map is carved from the TAIL of the 256KB region
 * (0x80038000..0x80040000, 32KB); it stays out of this link so .data/.bss/
 * stack never land on it (kasan.h defaults: KASAN_REGION_SIZE=0x40000).
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x10000000, LENGTH = 0x00400000
  RAM   (rwx) : ORIGIN = 0x80000000, LENGTH = 0x00038000
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
