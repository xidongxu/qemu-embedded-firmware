/* Minimal Cortex-M33 startup for the mps2-an505 kasan QEMU test.
 * Vectors at 0x10000000 (QEMU an505 init_svtor).  Runs .data copy / .bss
 * zeroing and traverses .init_array (empty here; kept so the file is a
 * generic drop-in), then jumps to main. */
    .syntax unified
    .cpu    cortex-m33
    .thumb

    .section .isr_vector, "a", %progbits
    .align  2
    .globl  __Vectors
__Vectors:
    .word   _estack
    .word   Reset_Handler
    .word   Default_Handler          /* NMI       */
    .word   Default_Handler          /* HardFault */
    .word   Default_Handler          /* MemManage */
    .word   Default_Handler          /* BusFault  */
    .word   Default_Handler          /* UsageFault*/
    .word   0
    .word   0
    .word   0
    .word   0
    .word   Default_Handler          /* SVCall    */
    .word   Default_Handler          /* DebugMon  */
    .word   0
    .word   Default_Handler          /* PendSV    */
    .word   Default_Handler          /* SysTick   */

    .section .text
    .thumb_func
    .globl  Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:
    /* Enable FPU (the repo toolchain builds hard-float: -mfpu=fpv5-sp-d16
     * -mfloat-abi=hard, and -O2 may use FP registers for block fills).
     * CPACR @0xE000ED88: full access to CP10/CP11. */
    ldr     r0, =0xE000ED88
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb
    ldr     r0, =_sdata
    ldr     r1, =_edata
    ldr     r2, =_sidata
copy_loop:
    cmp     r0, r1
    bge     copy_done
    ldr     r3, [r2]
    str     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, #4
    b       copy_loop
copy_done:
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
zero_loop:
    cmp     r0, r1
    bge     zero_done
    str     r2, [r0]
    adds    r0, r0, #4
    b       zero_loop
zero_done:
    /* init_array walk uses r4/r5 (callee-saved): the constructors called via
     * blx may clobber r0-r3 (AAPCS caller-saved), which would corrupt the
     * table pointer if it were kept in r0/r1. */
    ldr     r4, =__init_array_start
    ldr     r5, =__init_array_end
init_loop:
    cmp     r4, r5
    bge     init_done
    ldr     r2, [r4]
    adds    r4, r4, #4
    blx     r2
    b       init_loop
init_done:
    bl      main
dead:
    b       dead
    .size   Reset_Handler, . - Reset_Handler

    .thumb_func
    .globl  Default_Handler
    .type   Default_Handler, %function
Default_Handler:
    b       .
    .size   Default_Handler, . - Default_Handler
