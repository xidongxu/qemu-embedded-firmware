/*
 * tracer.h -- minimal Cortex-M fault dump library (M3..M85)
 *
 * Self-contained (no CMSIS / RTOS / printf dependency).  To build for a
 * specific core just compile with that core's -mcpu (e.g. -mcpu=cortex-m85
 * -mthumb).  TrustZone/FPU/MVE cores are detected at compile time and their
 * extra fields are only decoded where available.
 *
 * Wiring:
 *  1. Add the two sources to your build and link this library.
 *     Toolchain-specific vector entry file (pick ONE, do not mix):
 *       GCC/Clang    tracer_gnugcc.s   (also armclang / ARMCC6)
 *       IAR EWARM    tracer_iccarm.s
 *       MDK (armasm) tracer_armcc.s
 *     The entry exports STRONG symbols for the standard CMSIS fault vectors
 *     (HardFault_Handler, MemManage_Handler, BusFault_Handler,
 *     UsageFault_Handler, NMI_Handler, SecureFault_Handler) that override
 *     the weak defaults in the board startup file.
 *  2. (optional) Override the weak hooks:
 *       tracer_on_fault()   - e.g. print the current FreeRTOS task name
 *       tracer_stack_limit()- e.g. return the current task's stack top
 *  3. (optional) Redefine TRACER_PRINTF before including this header if the
 *     project does not provide a standard printf().
 *
 * Output example (serial):
 *   ===== Tracer: Cortex-M33 Fault Dump =====
 *   Exception : HardFault (IPSR=3)
 *   EXC_RETURN: 0xFFFFFFFD  [Thread mode, PSP, NonSecure]
 *   R0..R11 / R12 SP LR PC / xPSR
 *   CFSR/HFSR/DFSR/MMFAR/BFAR (+UFSR, valid flags)
 *   Call stack (BL/BLX scan): addr addr ...
 */
#ifndef TRACER_H
#define TRACER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output function.  Defaults to the standard printf(); redefine before
 * including this header to route output elsewhere (must support printf
 * format strings). */
#ifndef TRACER_PRINTF
#include <stdio.h>
#define TRACER_PRINTF printf
#endif

/* Maximum number of call-stack entries dumped. */
#ifndef TRACER_STACK_DEPTH
#define TRACER_STACK_DEPTH 32u
#endif

/* Use .ARM.exidx + _Unwind_Backtrace for exact on-demand backtraces
 * (tracer_get_callstack/tracer_dump_callstack) instead of the heuristic
 * BL/BLX stack scan.  Available on GCC and armclang (ARM Compiler 6) only:
 * IAR and ARMCC5 do not provide <unwind.h>/_Unwind_Backtrace, so on those
 * toolchains TRACER_USE_EXIDX is ignored and the BL/BLX scan is always used.
 * Requires the project to be compiled with -funwind-tables.  The fault-path
 * dump still uses the stack scan, because the handler's own stack differs
 * from the faulting context.  Define to 1 to enable (default 0). */
#ifndef TRACER_USE_EXIDX
#define TRACER_USE_EXIDX 0
#endif

/* Use a frame-pointer-chain backtrace (via __builtin_return_address) instead
 * of the BL/BLX scan.  Requires the WHOLE project to be compiled with
 * -fno-omit-frame-pointer.  GCC/armclang only.
 *
 * LIMITATION on Cortex-M (Thumb): GCC/armclang keep r7 (not AAPCS r11) as
 * the frame pointer with a non-standard per-frame layout, and on ARM
 * __builtin_return_address is only fully reliable at level 0 -- so on
 * Cortex-M this yields only the innermost frame(s).  Use TRACER_USE_EXIDX
 * for a complete exact backtrace; this FP option is a cheap upgrade over the
 * scan when -funwind-tables is unavailable.  Precedence when several are
 * enabled: TRACER_USE_EXIDX > TRACER_USE_FP > scan.  Define to 1 to enable
 * (default 0). */
#ifndef TRACER_USE_FP
#define TRACER_USE_FP 0
#endif

/* Memory-map bounds constraining the text/stack scan.  Defaults come from
 * linker-script symbols in tracer.c (GNU: _stext/_etext/_sstack/_estack;
 * ARMCC: Image$$ER_IROM1$$/STACK$$; IAR: __section_begin/__section_end).
 * If your linker script does not export them, define on the compiler
 * command line, e.g.
 *   -DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08100000
 *   -DTRACER_STACK_BASE=0x20000000 -DTRACER_STACK_TOP=0x20020000 */

/* Registers saved by the hardware on exception entry (pushed first). */
typedef struct {
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;
} tracer_exc_frame_t;

/* Core registers r4..r11 captured by the assembly entry (pushed before the
 * exception frame so they are restored untouched). */
typedef struct {
    uint32_t r4, r5, r6, r7, r8, r9, r10, r11;
} tracer_core_frame_t;

/* Full decoded fault context passed to tracer_on_fault(). */
typedef struct {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
    uint32_t r8, r9, r10, r11, r12;
    /* Active stack pointer (MSP or PSP). */
    uint32_t sp;
    uint32_t lr, pc, xpsr;
    /* EXC_RETURN value (MSP/PSP, mode, security). */
    uint32_t exc_return;
    /* Combined CFSR (MMFSR[7:0] | BFSR[15:8] | UFSR[31:16]). */
    uint32_t cfsr;
    /* MemManage fault status (CFSR[7:0]). */
    uint32_t mmfsr;
    /* BusFault status (CFSR[15:8]). */
    uint32_t bfsr;
    /* UsageFault status (16-bit). */
    uint32_t ufsr;
    /* HardFault status. */
    uint32_t hfsr;
    /* DebugFault status. */
    uint32_t dfsr;
    /* MemManage fault address (valid if MMFSR.MMARVALID). */
    uint32_t mmfar;
    /* BusFault address (valid if BFSR.BFARVALID). */
    uint32_t bfar;
    /* Exception number from xPSR. */
    unsigned ipsr;
} tracer_fault_t;

/* Print build-time configuration (text/stack bounds).  Call once at boot. */
void tracer_init(void);
/* C fault entry: 'exc_frame' points at the hardware exception frame
 * [r0,r1,r2,r3,r12,lr,pc,xpsr] (PSP if the fault came from Thread mode,
 * else MSP), 'exc_return' is the LR value at entry, and 'core_regs' points
 * at r4..r11 captured at handler entry.  Never returns (traps). */
void tracer_fault_handler(uint32_t *exc_frame, uint32_t exc_return,
                          uint32_t *core_regs);

/* Called just before the built-in dump (e.g. print RTOS task name). */
void tracer_on_fault(const tracer_fault_t *func);
/* Upper bound for the call-stack scan.  Default = end of the main stack;
 * on FreeRTOS override with the current task's stack top (pxEndOfStack). */
uint32_t tracer_stack_limit(void);

/* Dump the current call stack by scanning from the live SP for Thumb-2
 * BL/BLX return addresses (replaces fault-dump's fault_dump_callstack).
 * Useful for on-demand backtrace from a debug command. */
void tracer_dump_callstack(void);
/* Fill buf with up to size return addresses of the current call stack
 * (same scan as tracer_dump_callstack, but stores the PCs for e.g. logging
 * to a file or sending over a protocol).  Returns the number stored. */
uint32_t tracer_get_callstack(uint32_t *buf, uint32_t size);
/* Force a UsageFault (sets CCR.UNALIGN_TRP then does a misaligned read)
 * to exercise the fault path (replaces fault-dump's fault_dump_unalign). */
void tracer_trigger_unalign(void);

#ifdef __cplusplus
}
#endif

#endif /* TRACER_H */
