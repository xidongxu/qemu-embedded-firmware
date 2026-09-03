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

/* Feature switches (default off).  Defined up front so the preprocessor
 * never relies on the implicit "undefined macro == 0" rule later. */
#ifndef TRACER_USE_CRASH
#define TRACER_USE_CRASH 0
#endif
#ifndef TRACER_USE_LOG
#define TRACER_USE_LOG 0
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

/* 1. Default output backend: standard printf(), used only when the app has
 *    not defined its own TRACER_PRINTF. */
#ifndef TRACER_PRINTF
#include <stdio.h>
#define TRACER_PRINTF printf
#endif

/* 2. CRASH-SAFE / CRASH-LOG mode: when the app defines TRACER_PUTCHAR (a
 *    lock-free char sink, e.g. a bare UART data register or Segger RTT), the
 *    fault/assert dump must not touch printf or its lock -- so TRACER_PRINTF
 *    is redirected to tracer's own mini-printf (defined in tracer.c), which
 *    renders one char at a time through TRACER_PUTCHAR.  This intentionally
 *    overrides step 1 (and any app-defined TRACER_PRINTF): crash-safety wins
 *    on the fault path.
 *
 *    TRACER_USE_CRASH / TRACER_USE_LOG (the crash "black box" record and the
 *    leveled runtime log) also force the same per-char mini-printf path --
 *    even without TRACER_PUTCHAR (a stdio putchar() fallback is used then) --
 *    because the crash record mirrors the dump one char at a time into a RAM
 *    capture buffer, and the runtime log renders through the same mini-printf
 *    (see tracer_ring_printf / tracer_crash_save / tracer_log below).  A
 *    TRACER_USE_CRASH / TRACER_USE_LOG build therefore redefines TRACER_PRINTF
 *    regardless of TRACER_PUTCHAR. */
#if defined(TRACER_PUTCHAR) || TRACER_USE_CRASH || TRACER_USE_LOG
#undef TRACER_PRINTF
#define TRACER_PRINTF tracer_xprintf
#endif

/* Crash "black box" + leveled runtime log.  Both are opt-in and INDEPENDENT
 * (default off: no ring, no capture, no log code -- a fault dump behaves
 * exactly as before).  The shared infra (mini-printf / PRIMASK / ring) is
 * compiled when EITHER switch is on; a build that touches the ring needs the
 * format subset mini-printf supports (%s %c %d %u %x %X %p with 'l'
 * (long) / 'll' (long long) lengths, the %% escape, '-'/'0'/width).
 *
 *   TRACER_USE_CRASH=1 -- crash "black box":
 *     - tracer_ring_printf() keeps the most recent TRACER_RING_SIZE bytes of
 *       pre-crash events in RAM (lock-free, IRQ-safe).
 *     - When a fault / assert / stack-overflow dump runs, every dump
 *       character is mirrored into an in-RAM capture buffer
 *       (TRACER_CRASH_SIZE); before trapping / auto-resetting, tracer
 *       appends the pre-crash ring tail and a CRC footer and hands the record
 *       to the weak hook tracer_crash_save(), which an app overrides with its
 *       non-volatile backend (reserved flash / SPI NOR).  After a reset, boot
 *       code reads the record back and archives it.  Default weak = no-op.
 *
 *   TRACER_USE_LOG=1 -- leveled runtime log:
 *     - tracer_log() / TRACER_LOGI.. print leveled lines to the serial sink
 *       AND append them to the SAME ring (unified); optional async
 *       persistence via tracer_log_sink() / tracer_log_drain().  When
 *       TRACER_USE_CRASH is also on, a crash record automatically ends with
 *       the recent run log.
 */

/* Shared ring buffer size (bytes, circular): pre-crash events
 * (tracer_ring_printf) and/or the log stream (tracer_log) share it. */
#ifndef TRACER_RING_SIZE
#define TRACER_RING_SIZE 2048u
#endif

/* In-RAM capture buffer size for the crash record (dump text + ring tail +
 * CRC footer).  Must comfortably fit one dump (raw stack + registers). */
#ifndef TRACER_CRASH_SIZE
#define TRACER_CRASH_SIZE 8192u
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
/* System up-time in milliseconds, printed in every dump and as the
 * "[<ms> ms]" prefix of every tracer_log() line.  Weak.  Default: a
 * no-dependency SysTick wrap counter (SysTick running -> monotonic ms with
 * the usual ~1 ms reload; else 0).  RTOS adapters override it with the more
 * accurate tick source (FreeRTOS: xTaskGetTickCount()*portTICK_PERIOD_MS). */
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

#if TRACER_USE_CRASH
/* Append a formatted event line to the pre-crash RAM ring log (the "black
 * box": the most recent TRACER_RING_SIZE bytes of what happened before a
 * crash).  Lock-free and IRQ-safe; call from app code on key state changes
 * (call start/end, registration state, watchdog resets, ...).  The last
 * entries are appended to every crash record. */
void tracer_ring_printf(const char *fmt, ...);

/* Weak non-volatile storage hook.  Called at the very end of a fault /
 * assert / stack-overflow dump, before trapping or auto-reset, with the
 * completed crash record: the full dump text, then the pre-crash ring tail,
 * then a CRC footer:
 *     ==== TRACER CRASHLOG v1 ====
 *     <dump text>
 *     ==== Recent ring log (pre-crash) ====
 *     <...>
 *     ==== TRACER CRASHLOG END crc=xxxxxxxx ====
 * 'data' is valid only for the duration of the call.  The default weak
 * implementation is a no-op: override it with the platform's non-volatile
 * backend (reserved flash sector / SPI NOR ...).  Boot code then reads the
 * record back and archives it (file / report). */
void tracer_crash_save(const void *data, uint32_t len);
#endif /* TRACER_USE_CRASH */

#if TRACER_USE_LOG
/* ---- Leveled runtime log (unified with the pre-crash ring) ----
 *
 * tracer_log() is a plain leveled log call for application code.  It shares
 * the SAME RAM ring as tracer_ring_printf() ("unified", one buffer): when
 * TRACER_USE_CRASH is also enabled a crash record automatically ends with
 * the recent run log -- no separate "persist the log" step is ever needed
 * (with only TRACER_USE_LOG, persistence goes through tracer_log_sink /
 * tracer_log_drain instead).
 *
 * Filtering is a RUNTIME switch (initial value TRACER_LOG_DEFAULT_LEVEL):
 * lines below tracer_log_get_level() are dropped.  Change it at any time
 * with tracer_log_set_level() (a shell command, a debugger, or raise it to
 * TRACE while chasing a bug).  All levels are always compiled in -- there is
 * no compile-time filter, so the runtime level may move freely in both
 * directions.
 *
 * Each record ("[<ms> ms]X: <body>\r\n", X = level letter; <ms> from the weak
 * tracer_uptime_ms(), which by default counts SysTick wraps -- so every log
 * already carries a monotonic time stamp, no extra wiring) is STREAMED
 * character-by-character -- printf-like: there is NO line-length limit and
 * the caller never has to split long output; put '\n' in the format string
 * for explicit line breaks, and a final CRLF is added automatically.
 * tracer_log() is IRQ-safe and re-entrant (it streams under a short PRIMASK
 * section):
 *   1. every character is printed synchronously to the serial sink (visible
 *      immediately on the console when no async backend is attached);
 *   2. every character is appended to the shared pre-crash ring (the log);
 * and the output is pushed in blocks to the weak tracer_log_sink() hook.
 *
 * Asynchronous persistence (file / flash / network) is left to the app with
 * TWO independent interfaces -- use either, both, or neither:
 *   - tracer_log_sink(): push model.  Override the weak hook; it is called
 *     with a block of <= TRACER_LOG_SINK_CHUNK_SIZE bytes every time that
 *     many bytes accumulate and once more at the end of each tracer_log()
 *     call with the remainder (blocks may split lines, so just append the
 *     bytes to your storage).  Default is a no-op (pure sync output).
 *   - tracer_log_drain(): pull model.  Call from a low-priority background
 *     task / idle hook to copy the incremental byte stream (everything
 *     written since the previous call) and write it out.  Independent,
 *     lock-free read cursor. */
typedef enum {
    TRACER_LOG_TRACE = 0,
    TRACER_LOG_DEBUG = 1,
    TRACER_LOG_INFO  = 2,
    TRACER_LOG_WARN  = 3,
    TRACER_LOG_ERROR = 4
} tracer_log_level_t;

/* Runtime log level at startup (TRACER_LOG_INFO by default). */
#ifndef TRACER_LOG_DEFAULT_LEVEL
#define TRACER_LOG_DEFAULT_LEVEL TRACER_LOG_INFO
#endif

/* Block size handed to the weak tracer_log_sink() hook: tracer_log() pushes
 * a block every time this many bytes accumulate, and the (< block) remainder
 * at the end of each call.  A file/flash backend just appends the blocks. */
#ifndef TRACER_LOG_SINK_CHUNK_SIZE
#define TRACER_LOG_SINK_CHUNK_SIZE 128u
#endif

/* Log one record at 'level' (streamed, printf-like, no length limit).
 * Returns the number of bytes emitted (0 when filtered out by the current
 * runtime level). */
uint32_t tracer_log(tracer_log_level_t level, const char *fmt, ...);
void tracer_log_set_level(tracer_log_level_t level);
tracer_log_level_t tracer_log_get_level(void);

/* Leveled convenience macros: the level is baked into the macro NAME, so a
 * call carries no level argument -- e.g. TRACER_LOGI("call %u", n) expands
 * to tracer_log(TRACER_LOG_INFO, "call %u", n).  Every one still respects
 * the runtime level (TRACER_LOG_DEFAULT_LEVEL / tracer_log_set_level):
 * a TRACER_LOGI(...) below the current runtime level is dropped just like
 * tracer_log(TRACER_LOG_INFO, ...). */
#define TRACER_LOGT(...) tracer_log(TRACER_LOG_TRACE, __VA_ARGS__)
#define TRACER_LOGD(...) tracer_log(TRACER_LOG_DEBUG, __VA_ARGS__)
#define TRACER_LOGI(...) tracer_log(TRACER_LOG_INFO,  __VA_ARGS__)
#define TRACER_LOGW(...) tracer_log(TRACER_LOG_WARN,  __VA_ARGS__)
#define TRACER_LOGE(...) tracer_log(TRACER_LOG_ERROR, __VA_ARGS__)

/* Weak block-push persistence hook.  Called inside tracer's critical section
 * with up to TRACER_LOG_SINK_CHUNK_SIZE bytes (once per filled block and once
 * per tracer_log() call with the remainder).  Blocks may split lines -- just
 * append them to your file / flash log; 'data' is not NUL-terminated.  Keep
 * it quick (copy into your own queue) since it runs with IRQs masked.
 * Default no-op. */
void tracer_log_sink(const void *data, uint32_t len);

/* Incremental pull: copy up to 'max' bytes of the log stream produced since
 * the previous call into 'out' and advance the internal read cursor.
 * Returns the bytes copied (0 when nothing new).  If the ring already
 * overwrote part of the stream (consumer slower than the writer), the copy
 * starts at the oldest bytes still available. */
uint32_t tracer_log_drain(uint8_t *out, uint32_t max);
#endif /* TRACER_USE_LOG */

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
