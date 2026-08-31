;******************************************************************************
; tracer_iccarm.s -- IAR EWARM entry points (Cortex-M3..M85)
;
; Equivalent of tracer_gnugcc.s (GCC) for IAR's iasmarm.  Each handler:
;   1. saves r4..r11 (values at handler entry) on the handler stack,
;   2. locates the hardware exception frame: on the PSP for a Thread-mode
;      fault, else on the MSP below the pushed r4..r11,
;   3. calls tracer_fault_handler(exc_frame, EXC_RETURN, &core_regs).
;
; The handler never returns (traps); `B` to self is the safety net.
; Add this file (NOT tracer_gnugcc.s) to an IAR project.
; NOTE: handlers are expanded (no macro) because IAR macro parameters cannot
; be used as labels.
;******************************************************************************
    MODULE  tracer_vectors

    SECTION .text:CODE:REORDER:NOROOT(2)
    THUMB

    PUBLIC  NMI_Handler
    PUBLIC  HardFault_Handler
    PUBLIC  MemManage_Handler
    PUBLIC  BusFault_Handler
    PUBLIC  UsageFault_Handler
    PUBLIC  SecureFault_Handler

    EXTERN  tracer_fault_handler

NMI_Handler
    PUSH    {R4-R11}                    ; save core regs on handler stack
    MOV     R2, SP                      ; R2 = &core_regs (r4..r11)
    MOVS    R0, #4                      ; EXC_RETURN bit2: 0=MSP 1=PSP
    MOV     R1, LR
    TST     R0, R1
    BEQ     nmi_msp
    MRS     R0, PSP                     ; Thread-mode fault -> frame on PSP
    B       nmi_go
nmi_msp
    MRS     R0, MSP                     ; Handler-mode fault -> frame on MSP
    ADD     R0, R0, #32                 ; skip the pushed r4..r11
nmi_go
    MOV     R1, LR                      ; EXC_RETURN
    LDR     R3, =tracer_fault_handler
    BLX     R3                          ; never returns
    B       nmi_go                      ; safety

HardFault_Handler
    PUSH    {R4-R11}
    MOV     R2, SP
    MOVS    R0, #4
    MOV     R1, LR
    TST     R0, R1
    BEQ     hard_msp
    MRS     R0, PSP
    B       hard_go
hard_msp
    MRS     R0, MSP
    ADD     R0, R0, #32
hard_go
    MOV     R1, LR
    LDR     R3, =tracer_fault_handler
    BLX     R3
    B       hard_go

MemManage_Handler
    PUSH    {R4-R11}
    MOV     R2, SP
    MOVS    R0, #4
    MOV     R1, LR
    TST     R0, R1
    BEQ     mem_msp
    MRS     R0, PSP
    B       mem_go
mem_msp
    MRS     R0, MSP
    ADD     R0, R0, #32
mem_go
    MOV     R1, LR
    LDR     R3, =tracer_fault_handler
    BLX     R3
    B       mem_go

BusFault_Handler
    PUSH    {R4-R11}
    MOV     R2, SP
    MOVS    R0, #4
    MOV     R1, LR
    TST     R0, R1
    BEQ     bus_msp
    MRS     R0, PSP
    B       bus_go
bus_msp
    MRS     R0, MSP
    ADD     R0, R0, #32
bus_go
    MOV     R1, LR
    LDR     R3, =tracer_fault_handler
    BLX     R3
    B       bus_go

UsageFault_Handler
    PUSH    {R4-R11}
    MOV     R2, SP
    MOVS    R0, #4
    MOV     R1, LR
    TST     R0, R1
    BEQ     usage_msp
    MRS     R0, PSP
    B       usage_go
usage_msp
    MRS     R0, MSP
    ADD     R0, R0, #32
usage_go
    MOV     R1, LR
    LDR     R3, =tracer_fault_handler
    BLX     R3
    B       usage_go

SecureFault_Handler
    PUSH    {R4-R11}
    MOV     R2, SP
    MOVS    R0, #4
    MOV     R1, LR
    TST     R0, R1
    BEQ     sec_msp
    MRS     R0, PSP
    B       sec_go
sec_msp
    MRS     R0, MSP
    ADD     R0, R0, #32
sec_go
    MOV     R1, LR
    LDR     R3, =tracer_fault_handler
    BLX     R3
    B       sec_go

    END
