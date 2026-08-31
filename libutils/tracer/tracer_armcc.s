;******************************************************************************
; tracer_armcc.s -- MDK-ARM (armasm, ARMCC5) entry points (Cortex-M3..M85)
;
; Equivalent of tracer_gnugcc.s (GCC) for Keil's armasm.  Each handler:
;   1. saves r4..r11 (values at handler entry) on the handler stack,
;   2. locates the hardware exception frame: on the PSP for a Thread-mode
;      fault, else on the MSP below the pushed r4..r11,
;   3. calls tracer_fault_handler(exc_frame, EXC_RETURN, &core_regs).
;
; The handler never returns (traps); `B` to self is the safety net.
; Add this file (NOT tracer_gnugcc.s) to an MDK project.
; NOTE: with ARMCC6 (armclang) use the GNU-style tracer_gnugcc.s instead.
;******************************************************************************
    AREA    TRACER_CODE, CODE, READONLY
    THUMB

    EXPORT  NMI_Handler
    EXPORT  HardFault_Handler
    EXPORT  MemManage_Handler
    EXPORT  BusFault_Handler
    EXPORT  UsageFault_Handler
    EXPORT  SecureFault_Handler

    IMPORT  tracer_fault_handler

    MACRO
    TRACER_FAULT_HANDLER $name
$name
    PUSH    {R4-R11}                    ; save core regs on handler stack
    MOV     R2, SP                      ; R2 = &core_regs (r4..r11)
    MOVS    R0, #4                      ; EXC_RETURN bit2: 0=MSP 1=PSP
    MOV     R1, LR
    TST     R0, R1
    BEQ     msp_$name
    MRS     R0, PSP                     ; Thread-mode fault -> frame on PSP
    B       go_$name
msp_$name
    MRS     R0, MSP                     ; Handler-mode fault -> frame on MSP
    ADD     R0, R0, #32                 ; skip the pushed r4..r11
go_$name
    MOV     R1, LR                      ; EXC_RETURN
    LDR     R3, =tracer_fault_handler
    BLX     R3                          ; never returns
    B       go_$name                    ; safety
    MEND

    TRACER_FAULT_HANDLER NMI_Handler
    TRACER_FAULT_HANDLER HardFault_Handler
    TRACER_FAULT_HANDLER MemManage_Handler
    TRACER_FAULT_HANDLER BusFault_Handler
    TRACER_FAULT_HANDLER UsageFault_Handler
    TRACER_FAULT_HANDLER SecureFault_Handler

    END
