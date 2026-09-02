/*
 * tracer.h -- minimal Cortex-M fault dump library
 *
 * Supported cores: Cortex-M3 / M4 / M7 / M23 / M33 / M55 / M85.
 * Cortex-M0/M0+ are NOT supported (Thumb-1 has no multi-register PUSH, which
 * the vector entry needs to preserve r4..r11).
 *
 * Self-contained (no CMSIS / RTOS / printf dependency).  To build for a
 * specific core just compile with that core's -mcpu (e.g. -mcpu=cortex-m85
 * -mthumb).  When the faulting context used the FPU/MVE (M4F/M7/M33/M55/M85)
 * tracer decodes S0..S15 + FPSCR from the extended frame (via FPCAR after
 * forcing the lazy save).  TrustZone secure state is reported from EXC_RETURN.
 *
 * Fault dumps are re-entrancy protected and IRQ-masked: the assembly entry
 * runs `cpsid i`, and a fault that fires while a dump is already in progress
 * (e.g. an NMI, which cannot be masked) is only logged, never recursed.
 *
 * The fault/assert dump output can be made lock-free and crash-safe by
 * defining TRACER_PUTCHAR (see below); otherwise it falls back to
 * TRACER_PRINTF (may deadlock if the fault interrupted printf or its lock).
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
 *       tracer_on_fault()    - e.g. print the current FreeRTOS task name
 *       tracer_stack_limit() - e.g. return the current task's stack top
 *       tracer_uptime_ms()   - e.g. FreeRTOS xTaskGetTickCount()*tick_ms
 *       tracer_watchdog_kick()- feed a hardware IWDG during a long dump/
 *                               pre-reset delay
 *  3. (optional) Redefine TRACER_PRINTF / TRACER_PUTCHAR before including
 *     this header if the project does not provide a standard printf().
 *
 * Output example (serial):
 *   ===== Tracer: Cortex-M33 Fault Dump =====
 *   FW     : v1.2.3
 *   Up     : 123456 ms
 *   Exception : HardFault (IPSR=3)
 *   EXC_RETURN: 0xFFFFFFFD  [Thread mode, PSP, NonSecure]
 *   R0..R11 / R12 SP LR PC / xPSR
 *   FPU (extended frame): S0..S31 / FPSCR
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

/* Output backends.
 *
 * 1. Default / non-fault output uses TRACER_PRINTF (standard printf(), or
 *    your own redefinition before including this header).
 *
 * 2. TRACER_PUTCHAR -- optional CRASH-SAFE character output for the fault /
 *    assert dump.  A fault can strike while the faulting context holds a
 *    printf lock (or dies inside printf), so the dump must not depend on
 *    printf.  If you define TRACER_PUTCHAR(c) -- e.g. a bare UART data
 *    register write or a Segger RTT call -- every tracer print is rendered
 *    by tracer's own small lock-free mini-printf (defined in tracer.c),
 *    stdio/printf are never referenced, and a TRACER_PUTCHAR build has no
 *    printf() dependency at all.  If you do NOT define it, output falls back
 *    to TRACER_PRINTF (simpler, but can deadlock if the fault interrupted
 *    printf or its lock). */
#ifndef TRACER_PUTCHAR
#ifndef TRACER_PRINTF
#include <stdio.h>
#define TRACER_PRINTF printf
#endif
#else
#define TRACER_PRINTF tracer_xprintf /* lock-free mini-printf in tracer.c */
#endif

/* Maximum number of call-stack entries dumped. */
#ifndef TRACER_STACK_DEPTH
#define TRACER_STACK_DEPTH 32u
#endif

/* Number of raw stack bytes dumped after the fault frame, so a host tool
 * (works/tools/tracer_decode.py) can re-walk them with the ELF (symbols +
 * .ARM.exidx) and turn the addresses into a readable call chain -- this is
 * how non-exidx toolchains (IAR / ARMCC5) get an exact backtrace.  0 disables. */
#ifndef TRACER_STACK_DUMP_BYTES
#define TRACER_STACK_DUMP_BYTES 256u
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

/* Record a dynamic function call trace via -finstrument-functions
 * (__cyg_profile_func_enter/exit into a ring buffer) and print the last
 * entries in the fault dump, so you can see how execution reached the fault
 * ("crash trace replay").  On Cortex-M this is far more useful than the FP
 * chain below (which yields only the innermost frame).  Define to 1 to
 * enable; the consuming project must ALSO compile the code to be traced with
 * -finstrument-functions (exclude hot/ISR code with
 * __attribute__((no_instrument_function))).  The hooks themselves are
 * implemented in tracer.c and marked no_instrument_function.
 * Define to 1 to enable (default 0). */
#ifndef TRACER_USE_FINSTRUMENT
#define TRACER_USE_FINSTRUMENT 0
#endif

/* Ring-buffer depth for the function trace (TRACER_USE_FINSTRUMENT). */
#ifndef TRACER_TRACE_DEPTH
#define TRACER_TRACE_DEPTH 128u
#endif

/* Use a frame-pointer-chain backtrace (via __builtin_return_address) instead
 * of the BL/BLX scan.  Requires the WHOLE project to be compiled with
 * -fno-omit-frame-pointer.  GCC/armclang only.
 *
 * NOT RECOMMENDED on Cortex-M: GCC/armclang keep r7 (not AAPCS r11) as the
 * frame pointer with a non-standard per-frame layout, and on ARM
 * __builtin_return_address is only fully reliable at level 0 -- so on
 * Cortex-M this yields only the innermost frame(s).  Prefer TRACER_USE_EXIDX
 * (complete exact backtrace) or TRACER_USE_FINSTRUMENT (trace replay) over
 * this option.  It only makes sense on A32 (r11 standard chain) or IAR.
 * Precedence when several are enabled: TRACER_USE_EXIDX > TRACER_USE_FP >
 * scan.  Define to 1 to enable (default 0). */
#ifndef TRACER_USE_FP
#define TRACER_USE_FP 0
#endif

/* Firmware version string printed by tracer_init() and at the top of every
 * dump, so a crash log can be matched to a specific build.  Override with
 * -DTRACER_FW_VERSION="v1.2.3" (or in a header included before this one). */
#ifndef TRACER_FW_VERSION
#define TRACER_FW_VERSION "0.0.0"
#endif

/* Milliseconds to wait after a dump/assert before issuing a system reset
 * (SCB->AIRCR.SYSRESETREQ).  0 = trap forever (default).  Set >0 so a field
 * unit recovers automatically instead of hanging until the watchdog fires.
 * The delay lets the dump finish flushing on the output line first. */
#ifndef TRACER_AUTO_RESET_MS
#define TRACER_AUTO_RESET_MS 0u
#endif

/* Assertion that routes through the tracer: a failure prints the expression,
 * file:line and the current call stack, then auto-resets (if
 * TRACER_AUTO_RESET_MS>0) or traps forever -- instead of a bare hardfault.
 * Define TRACER_ASSERT to your own macro to override. */
#ifndef TRACER_ASSERT
#define TRACER_ASSERT(expr) \
    ((expr) ? (void)0 : tracer_assert_fail(#expr, __FILE__, __LINE__))
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
    /* FPU/MVE context.  When the faulting context had the FPU/MVE active
     * (EXC_RETURN bit4 is CLEAR) the hardware reserved the 26-word extended
     * frame (S0..S15 + FPSCR) above the basic frame.  Under lazy stacking
     * (FPCCR.LSPEN=1, the RTOS default) the values are only written when the
     * FPU is touched, so tracer forces the lazy save and reads the context
     * from FPCAR: 'fpu' points at S0 (fpu[0..15] = S0..S15, fpu[16] =
     * FPSCR) and fpu_words is 17.  NULL/0 when there is no extended frame or
     * tracer.c is compiled without FPU support.
     *
     * S16..S31 are caller-saved and are NOT preserved across exception
     * entry on ARMv8-M with lazy stacking (verified on QEMU M33: they read
     * as 0 inside the fault handler), so tracer does not capture them. */
    const uint32_t *fpu;
    unsigned fpu_words;
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
/* Called after tracer_on_fault, e.g. to list all RTOS tasks (state / stack
 * high-water).  Weak, default empty: the core stays RTOS-agnostic, RTOS
 * adapters override it (FreeRTOS: vTaskList). */
void tracer_dump_tasks(void);
/* System up-time in milliseconds, printed in every dump.  Weak, default 0:
 * RTOS adapters override it (FreeRTOS: xTaskGetTickCount()*portTICK_PERIOD_MS). */
uint32_t tracer_uptime_ms(void);
/* Feed the hardware watchdog during a long dump / pre-reset delay, so it
 * does not cut the dump short.  Weak, default no-op: apps with an IWDG
 * override it.  Called periodically while tracer_delay_ms() runs (i.e. during
 * TRACER_AUTO_RESET_MS).  A plain trap (TRACER_AUTO_RESET_MS=0) does NOT
 * feed, so the watchdog remains the final recovery backstop. */
void tracer_watchdog_kick(void);
/* Print a full on-demand diagnostic snapshot (version, tasks, call stack and,
 * if enabled, the function trace).  Call from a debug command / shell. */
void tracer_dump_all(void);
/* Assertion failure path used by TRACER_ASSERT: prints the expression,
 * file:line and the current call stack, then auto-resets (if
 * TRACER_AUTO_RESET_MS>0) or traps forever.  Never returns. */
void tracer_assert_fail(const char *expr, const char *file, int line);

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
