/*
 * tracer.c -- minimal Cortex-M fault dump library (M3..M85)
 *
 * See tracer.h for wiring.  Design goals:
 *  - no CMSIS / RTOS / printf dependency (all registers via raw addresses)
 *  - Cortex-M wide: SCB at 0xE000ED00 and the 8-word exception frame are
 *    identical across M3..M85; TrustZone/FPU/MVE are compile-time only
 *  - call-stack recovery, best to worst: .ARM.exidx (TRACER_USE_EXIDX),
 *    AAPCS frame-pointer chain (TRACER_USE_FP), or a Thumb-2 BL/BLX stack
 *    scan (always available as the fallback)
 */
#include "tracer.h"

/* Weak-symbol attribute, portable across GCC/Clang/ARMCC/IAR.
 * IAR's __weak keyword needs the extended-language option (--eec) that many
 * projects leave off, so declare the hooks weak via #pragma instead. */
#if defined(__ICCARM__)
  #pragma weak tracer_on_fault
  #pragma weak tracer_stack_limit
  #define TRACER_WEAK
#else
  #define TRACER_WEAK __attribute__((weak))
#endif

/* Memory-map bounds used to constrain the text/stack scan.
 *
 * Defaults come from the linker-script symbols below.  Projects whose linker
 * scripts do not export them can override any macro with -D on the compiler
 * command line, e.g.
 *   -DTRACER_TEXT_START=0x08000000 -DTRACER_TEXT_END=0x08100000
 *   -DTRACER_STACK_BASE=0x20000000 -DTRACER_STACK_TOP=0x20020000
 */
#ifndef TRACER_TEXT_START
#if defined(__GNUC__)
extern uint32_t _stext;
#define TRACER_TEXT_START ((uint32_t)&_stext)
#elif defined(__ARMCC_VERSION)
extern uint32_t Image$$ER_IROM1$$Base;
#define TRACER_TEXT_START ((uint32_t)&Image$$ER_IROM1$$Base)
#elif defined(__ICCARM__)
#pragma section=".text"
#define TRACER_TEXT_START ((uint32_t)__section_begin(".text"))
#else
#error "tracer: unsupported compiler (define TRACER_TEXT_START manually)"
#endif
#endif

#ifndef TRACER_TEXT_END
#if defined(__GNUC__)
extern uint32_t _etext;
#define TRACER_TEXT_END ((uint32_t)&_etext)
#elif defined(__ARMCC_VERSION)
extern uint32_t Image$$ER_IROM1$$Length;
#define TRACER_TEXT_END (TRACER_TEXT_START + (uint32_t)&Image$$ER_IROM1$$Length)
#elif defined(__ICCARM__)
#pragma section=".text"
#define TRACER_TEXT_END ((uint32_t)__section_end(".text"))
#else
#error "tracer: unsupported compiler (define TRACER_TEXT_END manually)"
#endif
#endif

#ifndef TRACER_STACK_BASE
#if defined(__GNUC__)
extern uint32_t _sstack;
#define TRACER_STACK_BASE ((uint32_t)&_sstack)
#elif defined(__ARMCC_VERSION)
extern uint32_t STACK$$Base;
#define TRACER_STACK_BASE ((uint32_t)&STACK$$Base)
#elif defined(__ICCARM__)
#pragma section="CSTACK"
#define TRACER_STACK_BASE ((uint32_t)__section_begin("CSTACK"))
#else
#error "tracer: unsupported compiler (define TRACER_STACK_BASE manually)"
#endif
#endif

#ifndef TRACER_STACK_TOP
#if defined(__GNUC__)
extern uint32_t _estack;
#define TRACER_STACK_TOP ((uint32_t)&_estack)
#elif defined(__ARMCC_VERSION)
extern uint32_t STACK$$Length;
#define TRACER_STACK_TOP (TRACER_STACK_BASE + (uint32_t)&STACK$$Length)
#elif defined(__ICCARM__)
#pragma section="CSTACK"
#define TRACER_STACK_TOP ((uint32_t)__section_end("CSTACK"))
#else
#error "tracer: unsupported compiler (define TRACER_STACK_TOP manually)"
#endif
#endif

/* System control block base is 0xE000ED00 on every Cortex-M (M3..M85). */
#define TRACER_SCB_BASE   0xE000ED00u
#define TRACER_CCR        (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x14u))
#define TRACER_CFSR       (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x28u))
#define TRACER_UFSR       (*(volatile uint16_t *)(TRACER_SCB_BASE + 0x2Au))
#define TRACER_HFSR       (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x2Cu))
#define TRACER_DFSR       (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x30u))
#define TRACER_MMFAR      (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x34u))
#define TRACER_BFAR       (*(volatile uint32_t *)(TRACER_SCB_BASE + 0x38u))

/* CCR flags used by tracer_trigger_unalign(). */
#define TRACER_CCR_UNALIGN_TRP  0x00000008u

static const char *tracer_exc_name(unsigned ipsr) {
    switch (ipsr) {
    case 2:
        return "NMI";
    case 3:
        return "HardFault";
    case 4:
        return "MemManage";
    case 5:
        return "BusFault";
    case 6:
        return "UsageFault";
    case 7:
        return "SecureFault";
    case 11:
        return "SVCall";
    case 12:
        return "DebugMonitor";
    case 14:
        return "PendSV";
    case 15:
        return "SysTick";
    default:
        return "IRQn";
    }
}

/* Best-effort fault name decoded from the fault status registers.
 * In a Thread-mode fault IPSR is 0, so the status registers are the truth.
 * A precise BusFault with the BusFault exception not enabled escalates to
 * HardFault (HFSR.FORCED); report the root cause. */
static const char *tracer_fault_name(const tracer_fault_t *f) {
    if (f->mmfsr) {
        return "MemManage";
    }
    if (f->bfsr) {
        return "BusFault";
    }
    if (f->ufsr) {
        return "UsageFault";
    }
    if (f->hfsr) {
        return "HardFault";
    }
    return tracer_exc_name(f->ipsr);
}

/* Fetch a 32-bit word byte-wise.  This never triggers an unaligned-access
 * fault, which matters when CCR.UNALIGN_TRP is set (e.g. re-entered from a
 * fault that tracer_trigger_unalign() caused). */
static inline uint32_t tracer_load32(uint32_t addr) {
    const uint8_t *p = (const uint8_t *)addr;
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* True if a 32-bit Thumb-2 instruction is BL or BLX (return-address
 * candidates for the stack walk).
 * BL  = 11110 S imm10 | 11 J1 1 J2 imm11
 * BLX = 11110 S imm10 | 11 J1 0 J2 imm11
 * Only the fixed bits matter: J1(bit13)/J2(bit10) encode the target address
 * and must NOT be part of the test, otherwise ~half of the calls are missed. */
static int tracer_is_bl_or_blx(uint32_t ins) {
    uint16_t hig = (uint16_t)(ins & 0xFFFFu);
    uint16_t low = (uint16_t)(ins >> 16);
    /* halfword1: 11110xxxxx (BL and BLX share it). */
    if ((hig & 0xF800u) != 0xF000u) {
        return 0;
    }
    /* halfword2: keep bit15,14,12,11 (mask 0xD800), ignore J1/J2. */
    low &= 0xD800u;
    /* BL (bit11=1) or BLX (bit11=0). */
    return (low == 0xD800u) || (low == 0xD000u);
}

void TRACER_WEAK tracer_on_fault(const tracer_fault_t *func) {
    (void)func;
}

uint32_t TRACER_WEAK tracer_stack_limit(void) {
    return TRACER_STACK_TOP;
}

void TRACER_WEAK tracer_dump_tasks(void) {
    /* RTOS adapter hook: override to list all tasks (e.g. FreeRTOS
     * vTaskList) right after the faulting task name.  Core is RTOS-agnostic. */
}

void tracer_init(void) {
    TRACER_PRINTF("Tracer: Cortex-M fault dump ready\r\n");
    TRACER_PRINTF("  text  [%08lX - %08lX]\r\n",
                  (unsigned long)TRACER_TEXT_START, (unsigned long)TRACER_TEXT_END);
    TRACER_PRINTF("  stack [%08lX - %08lX]\r\n",
                  (unsigned long)TRACER_STACK_BASE, (unsigned long)TRACER_STACK_TOP);
}

/* Walk [scan, limit] for Thumb-2 BL/BLX return addresses.
 * If buf is non-NULL, up to size addresses are stored and the count is
 * returned; otherwise each hit is printed (size caps the depth). */
static uint32_t tracer_walk_callstack(uint32_t scan, uint32_t limit,
                                      uint32_t *buf, uint32_t size) {
    uint32_t depth = 0;
    /* No lower-bound clamp on purpose: RTOS task stacks live in the FreeRTOS
     * heap, which is usually far BELOW TRACER_STACK_BASE (the main/MSP
     * stack).  scan always comes from a real SP / exception frame, so it is
     * a valid stack address; the (scan+8 <= limit) check caps the walk. */
    while ((scan + 8u) <= limit && depth < size) {
        uint32_t ret = *(volatile uint32_t *)scan;
        uint32_t pc = 0u;
        if ((ret & 1u) && ret >= TRACER_TEXT_START && ret < TRACER_TEXT_END) {
            /* Return address minus the Thumb bit and the 4-byte BL. */
            pc = ret - 1u - 4u;
            if (pc >= TRACER_TEXT_START &&
                tracer_is_bl_or_blx(tracer_load32(pc))) {
                if (buf != NULL) {
                    buf[depth] = pc;
                } else {
                    TRACER_PRINTF(" %08lX", (unsigned long)pc);
                }
                depth++;
            }
        }
        scan += 4u;
    }
    return depth;
}

/* Best-effort current SP: inline asm on GCC/Clang (accurate), address of a
 * local elsewhere (approximate).  `&sp` alone is unreliable under -O2 where
 * the local may be hoisted to the top of the frame. */
static inline uint32_t tracer_current_sp(void) {
    uint32_t sp = 0u;
#if defined(__GNUC__) || defined(__clang__)
    __asm volatile ("mov %0, sp" : "=r" (sp));
#else
    sp = (uint32_t)&sp;
#endif
    return sp;
}

/* ===== .ARM.exidx backtrace (optional, requires -funwind-tables) =====
 * GNU EHABI unwind tables (.ARM.exidx) + _Unwind_Backtrace are provided by
 * GCC and armclang (AC6) only.  IAR and ARMCC5 lack <unwind.h>, so
 * TRACER_USE_EXIDX there silently falls back to the BL/BLX scan. */
#if TRACER_USE_EXIDX && (defined(__GNUC__) || defined(__clang__))
#define TRACER_HAVE_EXIDX 1
#else
#define TRACER_HAVE_EXIDX 0
#endif

/* Frame-pointer-chain backtrace: same toolchain scope as exidx.  Enabled
 * separately by TRACER_USE_FP; the exidx path takes precedence when both
 * are on.  See the implementation note below. */
#if TRACER_USE_FP && (defined(__GNUC__) || defined(__clang__))
#define TRACER_HAVE_FP 1
#else
#define TRACER_HAVE_FP 0
#endif

#if TRACER_HAVE_EXIDX
#include <unwind.h>

typedef struct {
    uint32_t *buf;
    uint32_t size;
    uint32_t count;
} tracer_unwind_ctx_t;

static _Unwind_Reason_Code tracer_unwind_cb(struct _Unwind_Context *ctx,
                                            void *arg) {
    tracer_unwind_ctx_t *c = (tracer_unwind_ctx_t *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (c->count >= c->size) {
        return _URC_END_OF_STACK;
    }
    if (pc != 0u) {
        c->buf[c->count++] = (uint32_t)pc;
    }
    return _URC_NO_REASON;
}

static uint32_t tracer_backtrace_exidx(uint32_t *buf, uint32_t size) {
    tracer_unwind_ctx_t c;
    c.buf = buf;
    c.size = size;
    c.count = 0;
    _Unwind_Backtrace(tracer_unwind_cb, &c);
    return c.count;
}
#endif /* TRACER_HAVE_EXIDX */

/* ===== AAPCS frame-pointer-chain backtrace (requires
 *       -fno-omit-frame-pointer) =====
 * On Cortex-M (Thumb) GCC/armclang keep r7 as the frame pointer with a batch
 * `push {r4..r11, lr}` prologue -- NOT the AAPCS r11 chain with a fixed
 * [fp]=prev, [fp+4]=lr layout (that only holds for A32).  The per-frame
 * offset varies with how many registers each function pushes, so there is no
 * fixed chain to walk by hand; __builtin_return_address() lets the compiler
 * resolve the real layout.  Requires -fno-omit-frame-pointer on the WHOLE
 * project, and on ARM only level 0 is fully reliable -- so on Cortex-M this
 * yields the innermost frame(s); use TRACER_USE_EXIDX for a complete
 * backtrace.  Precedence: exidx > FP chain > BL/BLX scan. */
#if TRACER_HAVE_FP
/* __builtin_return_address requires a compile-time constant level, and on
 * ARM GCC only guarantees level 0 is fully reliable (deeper levels are
 * best-effort even with -fno-omit-frame-pointer).  Keep this function
 * noinline so each level maps 1:1 to a real call frame.  Expand a fixed
 * number of levels and validate each against the .text range, so garbage
 * can neither crash nor pollute the output. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static uint32_t tracer_backtrace_fp(uint32_t *buf, uint32_t size) {
    uint32_t count = 0u;
#define TRACER_FP_PUSH(N)                                                     \
    do {                                                                      \
        if (count >= size) {                                                  \
            return count;                                                     \
        }                                                                     \
        uintptr_t ra = (uintptr_t)__builtin_return_address(N);                \
        if (ra == 0u || ra == (uintptr_t)-1) {                                \
            return count;                                                     \
        }                                                                     \
        if ((ra & 1u) && (ra & ~1u) >= TRACER_TEXT_START &&                   \
            (ra & ~1u) < TRACER_TEXT_END) {                                   \
            buf[count++] = (uint32_t)(ra & ~1u);                              \
        } else {                                                              \
            return count;                                                     \
        }                                                                     \
    } while (0)
    TRACER_FP_PUSH(0);
    TRACER_FP_PUSH(1);
    TRACER_FP_PUSH(2);
    TRACER_FP_PUSH(3);
    TRACER_FP_PUSH(4);
    TRACER_FP_PUSH(5);
    TRACER_FP_PUSH(6);
    TRACER_FP_PUSH(7);
#undef TRACER_FP_PUSH
    return count;
}
#endif /* TRACER_HAVE_FP */

void tracer_dump_callstack(void) {
#if TRACER_HAVE_EXIDX
    uint32_t buf[TRACER_STACK_DEPTH];
    uint32_t n = tracer_backtrace_exidx(buf, TRACER_STACK_DEPTH);
    TRACER_PRINTF(" Call stack:");
    for (uint32_t i = 0; i < n; i++) {
        TRACER_PRINTF(" %08lX", (unsigned long)buf[i]);
    }
    TRACER_PRINTF("\r\n");
#elif TRACER_HAVE_FP
    uint32_t buf[TRACER_STACK_DEPTH];
    uint32_t n = tracer_backtrace_fp(buf, TRACER_STACK_DEPTH);
    TRACER_PRINTF(" Call stack (FP chain):");
    for (uint32_t i = 0; i < n; i++) {
        TRACER_PRINTF(" %08lX", (unsigned long)buf[i]);
    }
    TRACER_PRINTF("\r\n");
#else
    /* The scan limit reuses the same hook as the fault path, so a call from
     * an RTOS task is confined to that task's stack. */
    uint32_t sp = tracer_current_sp();
    uint32_t limit = tracer_stack_limit();
    if (limit == 0u) {
        limit = TRACER_STACK_TOP;
    }
    TRACER_PRINTF(" Call stack (SP=0x%08lX):", (unsigned long)sp);
    tracer_walk_callstack(sp, limit, NULL, TRACER_STACK_DEPTH);
    TRACER_PRINTF("\r\n");
#endif
}

/* Fill buf with up to size return addresses of the current call stack
 * (for e.g. logging to a file or sending over a protocol).  Returns the
 * number stored.  With TRACER_HAVE_EXIDX (TRACER_USE_EXIDX=1 on GCC/
 * armclang) this is an exact .ARM.exidx walk; otherwise it falls back to
 * the heuristic BL/BLX stack scan. */
uint32_t tracer_get_callstack(uint32_t *buf, uint32_t size) {
    if (buf == NULL || size == 0u) {
        return 0u;
    }
#if TRACER_HAVE_EXIDX
    return tracer_backtrace_exidx(buf, size);
#elif TRACER_HAVE_FP
    return tracer_backtrace_fp(buf, size);
#else
    uint32_t sp = tracer_current_sp();
    uint32_t limit = tracer_stack_limit();
    if (limit == 0u) {
        limit = TRACER_STACK_TOP;
    }
    return tracer_walk_callstack(sp, limit, buf, size);
#endif
}

/* ===== Dynamic function trace (optional, -finstrument-functions) =====
 * When TRACER_USE_FINSTRUMENT is on and the consuming project compiles code
 * with -finstrument-functions, GCC/Clang call these two hooks at every
 * function entry/exit.  We record (fn address, direction) in a small ring
 * buffer; the fault dump prints the last TRACER_TRACE_DEPTH entries so you
 * can replay how execution reached the fault.  bit0 = 0 entered / 1 exited,
 * upper bits = function address (Thumb bit cleared).  Both hooks are marked
 * no_instrument_function to avoid re-entrancy, and they only touch the ring
 * buffer (no printf / malloc), so they are safe from IRQ context too. */
#if TRACER_USE_FINSTRUMENT
/* Time source: SysTick current value (counts DOWN).  The delta between two
 * consecutive entries is the elapsed SysTick cycles -- a cheap relative
 * timing of each call step.  Requires the SysTick timer to be running (as it
 * is under FreeRTOS).  Raw cycles; convert with configCPU_CLOCK_HZ. */
#define TRACER_SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018u)
#define TRACER_SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)

typedef struct {
    /* bit0 = 0 entered / 1 exited, upper = function address */
    uint32_t fn;
    /* SysTick->VAL at the moment of the event */
    uint32_t ts;
} tracer_trace_t;

static volatile tracer_trace_t s_trace[TRACER_TRACE_DEPTH];
/* next free slot */
static volatile uint32_t s_trace_head = 0u;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_instrument_function))
#endif
static uint32_t tracer_trace_ts(void) {
    return TRACER_SYSTICK_VAL;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_instrument_function))
#endif
void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    uint32_t i = s_trace_head;
    /* bit0 = 0 -> entered */
    s_trace[i].fn = (uint32_t)this_fn & ~1u;
    s_trace[i].ts = tracer_trace_ts();
    s_trace_head = (i + 1u) % TRACER_TRACE_DEPTH;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_instrument_function))
#endif
void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)call_site;
    uint32_t i = s_trace_head;
    /* bit0 = 1 -> exited */
    s_trace[i].fn = ((uint32_t)this_fn & ~1u) | 1u;
    s_trace[i].ts = tracer_trace_ts();
    s_trace_head = (i + 1u) % TRACER_TRACE_DEPTH;
}
#endif /* TRACER_USE_FINSTRUMENT */

void tracer_trigger_unalign(void) {
    TRACER_CCR |= TRACER_CCR_UNALIGN_TRP;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    volatile uint32_t *addr = (volatile uint32_t *)0x3u;
    (void)*addr;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

void tracer_fault_handler(uint32_t *exc_frame, uint32_t exc_return,
                          uint32_t *core_regs) {
    const tracer_exc_frame_t *exc = (const tracer_exc_frame_t *)exc_frame;
    const tracer_core_frame_t *core = (const tracer_core_frame_t *)core_regs;
    tracer_fault_t f = {0};
    uint32_t cfsr = 0u;
    uint32_t mmfsr = 0u;
    uint32_t bfsr = 0u;
    uint32_t scan = 0u;
    uint32_t limit = 0u;

    /* Decode context. */
    f.r0 = exc->r0;
    f.r1 = exc->r1;
    f.r2 = exc->r2;
    f.r3 = exc->r3;
    f.r4 = core->r4;
    f.r5 = core->r5;
    f.r6 = core->r6;
    f.r7 = core->r7;
    f.r8 = core->r8;
    f.r9 = core->r9;
    f.r10 = core->r10;
    f.r11 = core->r11;
    f.r12 = exc->r12;
    f.lr = exc->lr;
    f.pc = exc->pc;
    f.xpsr = exc->xpsr;
    f.exc_return = exc_return;
    /* Fault-time SP: the assembly entry located the exception frame on the
     * PSP (Thread-mode fault) or on the MSP below the r4..r11 it pushed
     * (Handler-mode fault).  Deriving it from exc_frame avoids inline asm,
     * so this file compiles identically with GCC/ARMCC/IAR. */
    f.sp = (uint32_t)exc_frame;
    if (!(exc_return & 0x4u)) {
        /* MSP case: skip the pushed r4..r11. */
        f.sp -= sizeof(tracer_core_frame_t);
    }
    f.ipsr = exc->xpsr & 0x1FFu;
    cfsr = TRACER_CFSR;
    mmfsr = cfsr & 0xFFu;
    bfsr = (cfsr >> 8) & 0xFFu;
    f.cfsr = cfsr;
    f.mmfsr = mmfsr;
    f.bfsr = bfsr;
    f.ufsr = TRACER_UFSR;
    f.hfsr = TRACER_HFSR;
    f.dfsr = TRACER_DFSR;
    f.mmfar = TRACER_MMFAR;
    f.bfar = TRACER_BFAR;

    /* User hooks first: faulting task name, then all-tasks list. */
    tracer_on_fault(&f);
    tracer_dump_tasks();

    /* Dump. */
    {
        const char *name = tracer_fault_name(&f);
        TRACER_PRINTF("\r\n===== Tracer: %s Fault Dump =====\r\n", name);
        TRACER_PRINTF("Exception : %s (IPSR=%u)\r\n", name, f.ipsr);
        TRACER_PRINTF("EXC_RETURN: 0x%08lX  [%s mode, %s, %s]\r\n",
                      (unsigned long)exc_return,
                      (exc_return & 0x8u) ? "Thread" : "Handler",
                      (exc_return & 0x4u) ? "PSP" : "MSP",
                      (exc_return & 0x1u) ? "Secure" : "NonSecure");

        TRACER_PRINTF(" R0 =%08lX  R1 =%08lX  R2 =%08lX  R3 =%08lX\r\n",
                      (unsigned long)f.r0, (unsigned long)f.r1,
                      (unsigned long)f.r2, (unsigned long)f.r3);
        TRACER_PRINTF(" R4 =%08lX  R5 =%08lX  R6 =%08lX  R7 =%08lX\r\n",
                      (unsigned long)f.r4, (unsigned long)f.r5,
                      (unsigned long)f.r6, (unsigned long)f.r7);
        TRACER_PRINTF(" R8 =%08lX  R9 =%08lX  R10=%08lX  R11=%08lX\r\n",
                      (unsigned long)f.r8, (unsigned long)f.r9,
                      (unsigned long)f.r10, (unsigned long)f.r11);
        TRACER_PRINTF(" R12=%08lX  SP =%08lX  LR =%08lX  PC =%08lX\r\n",
                      (unsigned long)f.r12, (unsigned long)f.sp,
                      (unsigned long)f.lr, (unsigned long)f.pc);
        TRACER_PRINTF(" xPSR=%08lX\r\n", (unsigned long)f.xpsr);
    }

    /* Fault status.  CFSR = MMFSR[7:0] | BFSR[15:8] | UFSR[31:16]. */
    TRACER_PRINTF(" CFSR=%08lX  MMFSR=%02lX  BFSR=%02lX  UFSR=%04lX\r\n",
                  (unsigned long)cfsr, (unsigned long)mmfsr,
                  (unsigned long)bfsr, (unsigned long)f.ufsr);
    TRACER_PRINTF(" HFSR=%08lX  DFSR=%08lX\r\n",
                  (unsigned long)f.hfsr, (unsigned long)f.dfsr);
    TRACER_PRINTF(" MMFAR=%08lX%s  BFAR=%08lX%s\r\n",
                  (unsigned long)f.mmfar, (mmfsr & (1u << 7)) ? " [VALID]" : "",
                  (unsigned long)f.bfar, (bfsr & (1u << 7)) ? " [VALID]" : "");

    /* Call stack: scan the fault-time stack for BL/BLX return addresses.
     * Thread-mode fault -> scan the task's PSP stack (the app hook returns
     * the current task's stack top); Handler-mode fault -> main stack. */
    TRACER_PRINTF(" Call stack:");
    scan = (uint32_t)(exc_frame + 8);
    if (exc_return & 0x4u) {
        limit = tracer_stack_limit();
        if (limit == 0u) {
            limit = TRACER_STACK_TOP;
        }
    } else {
        limit = TRACER_STACK_TOP;
    }
    tracer_walk_callstack(scan, limit, NULL, TRACER_STACK_DEPTH);
    TRACER_PRINTF("\r\n");

#if TRACER_USE_FINSTRUMENT
    /* Dynamic function trace: replay the last calls before the fault.
     * The +delta column is the SysTick cycles elapsed since the previous
     * entry (SysTick counts down, so prev - current). */
    TRACER_PRINTF(" Function trace (last %lu, +delta = SysTick cycles):\r\n",
                  (unsigned long)TRACER_TRACE_DEPTH);
    {
        uint32_t start = s_trace_head;
        uint32_t load = TRACER_SYSTICK_LOAD;
        uint32_t prev = 0u;
        int have_prev = 0;
        for (uint32_t n = 0u; n < TRACER_TRACE_DEPTH; n++) {
            uint32_t idx = (start + n) % TRACER_TRACE_DEPTH;
            uint32_t v = s_trace[idx].fn;
            uint32_t ts = s_trace[idx].ts;
            if (v == 0u && ts == 0u) {
                continue;
            }
            if (have_prev) {
                /* SysTick counts down; correct for one reload (prev < ts). */
                uint32_t d = (prev >= ts) ? (prev - ts) : (prev + (load - ts));
                TRACER_PRINTF("  %s %08lX  +%lu\r\n", (v & 1u) ? "<-" : "->",
                              (unsigned long)(v & ~1u), (unsigned long)d);
            } else {
                TRACER_PRINTF("  %s %08lX\r\n", (v & 1u) ? "<-" : "->",
                              (unsigned long)(v & ~1u));
            }
            prev = ts;
            have_prev = 1;
        }
    }
#endif /* TRACER_USE_FINSTRUMENT */

    /* Raw stack dump for offline symbol resolution by
     * works/tools/tracer_decode.py: dump the words under the faulting frame
     * so a host tool can re-walk them with the ELF (symbols + .ARM.exidx)
     * and produce a readable chain (IAR/ARMCC5 path to exact backtraces). */
#if TRACER_STACK_DUMP_BYTES
    {
        uint32_t base = f.sp;
        uint32_t bytes = TRACER_STACK_DUMP_BYTES;
        if (base >= limit) {
            bytes = 0u;
        } else if (base + bytes > limit) {
            bytes = limit - base;
        }
        TRACER_PRINTF(" Raw stack (0x%08lX, %lu bytes):\r\n",
                      (unsigned long)base, (unsigned long)bytes);
        for (uint32_t i = 0u; i < bytes; i += 16u) {
            uint32_t a = base + i;
            TRACER_PRINTF("  %08lX:", (unsigned long)a);
            for (uint32_t j = 0u; j < 16u && (i + j) < bytes; j++) {
                TRACER_PRINTF(" %02X",
                              (unsigned)*(volatile uint8_t *)(a + j));
            }
            TRACER_PRINTF("\r\n");
        }
    }
#endif /* TRACER_STACK_DUMP_BYTES */
    TRACER_PRINTF("===== End of dump =====\r\n");

    /* Trap. */
    for (;;) {}
}
