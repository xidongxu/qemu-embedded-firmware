/*
 * tracer_gnugcc.s -- Cortex-M fault vector entry points (M3..M85), GCC/Clang.
 * (IAR uses tracer_iccarm.s, MDK/armasm uses tracer_armcc.s.)
 *
 * Each entry:
 *   1. saves r4..r11 (values at handler entry, close to the fault site) on
 *      the current (handler) stack,
 *   2. locates the hardware exception frame (r0,r1,r2,r3,r12,lr,pc,xpsr) --
 *      it lives on the PSP if the fault happened in Thread mode, else on the
 *      MSP (below the pushed r4..r11),
 *   3. calls tracer_fault_handler(exc_frame, EXC_RETURN, core_regs).
 *
 * The handler never returns (traps), so `b .` is only a safety net.
 * These symbols are STRONG, overriding the WEAK defaults in the board
 * startup file -- no startup edit needed.  On M0/M0+ (Thumb-1 only) the
 * `push {r4-r11}` must be split into two pushes.
 */
    .syntax unified
    .thumb
    .text

    .macro TRACER_FAULT_HANDLER name
    .thumb_func
    .type \name, %function
    .globl \name
\name:
    push {r4-r11}                       /* save core regs on handler stack    */
    mov  r2, sp                         /* r2 = &core_regs (r4..r11)          */
    movs r0, #4                         /* EXC_RETURN bit2: 0=MSP 1=PSP       */
    mov  r1, lr
    tst  r0, r1
    beq  .Lmsp\@
    mrs  r0, psp                        /* fault in Thread mode -> frame on PSP */
    b    .Lgo\@
.Lmsp\@:
    mrs  r0, msp                        /* fault in Handler mode -> frame on MSP */
    add  r0, r0, #32                    /* skip the pushed r4..r11             */
.Lgo\@:
    mov  r1, lr                         /* EXC_RETURN                          */
    ldr  r3, =tracer_fault_handler
    blx  r3                             /* never returns                       */
    b    .                              /* safety                              */
    .size \name, . - \name
    .endm

    TRACER_FAULT_HANDLER NMI_Handler
    TRACER_FAULT_HANDLER HardFault_Handler
    TRACER_FAULT_HANDLER MemManage_Handler
    TRACER_FAULT_HANDLER BusFault_Handler
    TRACER_FAULT_HANDLER UsageFault_Handler
    TRACER_FAULT_HANDLER SecureFault_Handler

    .end
