/*
 * startup_armv8m.s - minimal reset/vector entry for Cortex-M23/M33/M55/M85
 * (TrustZone-capable QEMU MPS2/MPS3 boards: an505/an521/an547/an555).
 *
 * No CMSIS.  Slot 7 is SecureFault_Handler (armv8-M).  Reset_Handler sets
 * VTOR (in case the code runs through an alias), copies .data, zeroes .bss
 * and jumps to main.  QEMU boots these machines in the Secure state by
 * default, so no SAU/NS setup is needed for the test firmware.
 */
    .syntax unified
    .thumb

    .section .isr_vector,"a",%progbits
    .align 2
    .globl _vectors
_vectors:
    .word _estack              /* 0  initial SP                  */
    .word Reset_Handler        /* 1  Reset                       */
    .word NMI_Handler          /* 2  NMI   (tracer strong)       */
    .word HardFault_Handler    /* 3  HardFault (tracer strong)   */
    .word MemManage_Handler    /* 4  MemManage (tracer strong)   */
    .word BusFault_Handler     /* 5  BusFault (tracer strong)    */
    .word UsageFault_Handler   /* 6  UsageFault (tracer strong)  */
    .word SecureFault_Handler  /* 7  SecureFault (tracer strong) */
    .word 0                    /* 8  reserved                    */
    .word 0                    /* 9  reserved                    */
    .word 0                    /* 10 reserved                    */
    .word Default_Handler      /* 11 SVCall                      */
    .word Default_Handler      /* 12 DebugMon                    */
    .word 0                    /* 13 reserved                    */
    .word Default_Handler      /* 14 PendSV                      */
    .word Default_Handler      /* 15 SysTick                     */
    .rept 480                  /* IRQ 0..479 -> Default_Handler  */
    .word Default_Handler
    .endr
    .equ _vectors_size, . - _vectors

    .section .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    ldr   r0, =_estack
    msr   msp, r0
    /* no PSP/limit setup needed for bare metal */

    /* VTOR = _vectors (covers alias-based boot) */
    ldr   r0, =_vectors
    ldr   r1, =0xE000ED08
    str   r0, [r1]
    dsb   sy
    isb

    /* copy .data from flash (_sidata) to RAM (_sdata.._edata)
     * (write-back addressing avoided so this assembles for armv8-m.BASE
     * (M23) as well as .MAIN (M33/M55/M85)) */
    ldr   r0, =_sidata
    ldr   r1, =_sdata
    ldr   r2, =_edata
    b     .L_copy_loop
.L_copy_word:
    ldr   r3, [r0]
    str   r3, [r1]
    adds  r0, r0, #4
    adds  r1, r1, #4
.L_copy_loop:
    cmp   r1, r2
    bcc   .L_copy_word

    /* zero .bss (_sbss.._ebss) */
    ldr   r0, =_sbss
    ldr   r1, =_ebss
    movs  r2, #0
    b     .L_zero_loop
.L_zero_word:
    str   r2, [r0]
    adds  r0, r0, #4
.L_zero_loop:
    cmp   r0, r1
    bcc   .L_zero_word

    bl    main
.L_dead:
    b     .L_dead

    .thumb_func
    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler
    .thumb_func
    .weak HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler
    .thumb_func
    .weak MemManage_Handler
    .thumb_set MemManage_Handler, Default_Handler
    .thumb_func
    .weak BusFault_Handler
    .thumb_set BusFault_Handler, Default_Handler
    .thumb_func
    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, Default_Handler
    .thumb_func
    .weak SecureFault_Handler
    .thumb_set SecureFault_Handler, Default_Handler

    .thumb_func
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b     Default_Handler

    .end
