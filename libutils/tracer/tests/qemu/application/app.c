/*
 * app.c - shared tracer board test application.
 *
 * Same test body runs on every board; only the board's UART base / core /
 * memory map differ (see config.json / -DBOARD_UART0).  A compile-time
 * TEST_CASE selects which scenario to run:
 *
 *   TEST_CASE=0 (default)  smoke: tracer_init + callstack dump + PASS marker,
 *                          never faults (normal exit -> spin).
 *   TEST_CASE=1            usage fault: tracer_trigger_unalign() -> dump.
 *   TEST_CASE=2            bus fault: write 0xDEADBEEF -> dump.
 *   TEST_CASE=3            assertion: TRACER_ASSERT(0) -> dump.
 *   TEST_CASE=4            PSP fault: re-point PSP at a fake task stack and
 *                          fault in Thread mode -> dump shows
 *                          "Thread mode, PSP" (exercises the PSP frame
 *                          path taken by RTOS task crashes).
 *   TEST_CASE=5            re-entrancy guard: the strong tracer_on_fault()
 *                          hook pends an NMI (ICSR.NMIPENDSET) inside the
 *                          dump; the guard must log "fault while dumping,
 *                          ignored" instead of recursing.
 *   TEST_CASE=6            auto-reset: fault dump then system reset (the
 *                          build adds -DTRACER_AUTO_RESET_MS>0) -> the
 *                          firmware boots a second time; the harness checks
 *                          the second "app: boot" marker.
 *   TEST_CASE=7            FPU extended frame: enable CP10/CP11, run a VFP
 *                          op so the faulting context owns an FPU frame,
 *                          then bus-fault -> dump prints the " FPU
 *                          (extended frame):" S0..S15 + FPSCR block.
 *                          FPU-capable boards only (config.json "fpu").
 *   TEST_CASE=8            PSP + FPU combined: own an FPU context, then
 *                          fault from the PSP (Thread mode) -> dump decodes
 *                          BOTH the PSP frame and the extended FPU frame.
 *                          FPU-capable boards only.
 *   TEST_CASE=9            assert re-entrancy: the strong tracer_on_fault()
 *                          hook fires TRACER_ASSERT(0) inside an on-going
 *                          dump; the guard must log "assert while dumping,
 *                          ignored" instead of recursing.
 *   TEST_CASE=10           assert + auto-reset: TRACER_ASSERT(0) then system
 *                          reset (needs -DTRACER_AUTO_RESET_MS>0) -> the
 *                          firmware boots a second time.
 *
 * Fault cases end in a hard trap inside the tracer (no return), so the QEMU
 * harness kills the machine once the expected dump text is seen.
 */
#include "uart.h"

/* Route tracer's own output through the lock-free UART putc. */
#ifndef TRACER_PUTCHAR
#define TRACER_PUTCHAR board_putc
#endif
#include "tracer.h"

#ifndef TEST_CASE
#define TEST_CASE 0
#endif

/* board_test.py --coverage mode: dump .gcda after the smoke run too (fault
 * cases dump from the strong tracer_halt in gcov_dump.c instead). */
#ifdef TRACER_GCOV
extern void tracer_gcov_dump(void);
#endif

/* Print helper: tracer_log() writes to the serial sink synchronously AND
 * into the shared ring (tracer_ring_printf only fills the ring). */
#define APP_LOG(...) tracer_log(TRACER_LOG_INFO, __VA_ARGS__)

/* noinline so the call stack dump has a few real frames. */
__attribute__((noinline)) static void app_frame_b(void)
{
    tracer_dump_callstack();
    APP_LOG("PASS callstack\r\n");
}

__attribute__((noinline)) static void app_frame_a(void)
{
    app_frame_b();
}

/* BusFault trigger address; a board may override via config 'busfault_addr'
 * (some QEMU machines silently ignore writes to 0xDEADBEEF). */
#ifndef APP_BUSFAULT_ADDR
#define APP_BUSFAULT_ADDR 0xDEADBEEFu
#endif

static void app_busfault(void)
{
    APP_LOG("trigger: write "
            "0x%08lx\r\n", (unsigned long)APP_BUSFAULT_ADDR);
    *(volatile unsigned long *)APP_BUSFAULT_ADDR = 0xDEADBEEFu;
}

#if TEST_CASE == 4 || TEST_CASE == 8
/* Fake "task" stack: the PSP scenarios must fire in Thread mode on the PSP
 * (as an RTOS task crash would).  Shared by TEST_CASE 4 and 8.  Global (not
 * static) so the naked entry can take its address via `ldr rN, =psp_stack`. */
uint32_t psp_stack[256] __attribute__((aligned(8)));

/* Switch to the PSP and jump into @fn WITHOUT returning through the old
 * stack frame (whose return address lives on the now-abandoned MSP).  The
 * clean bare-metal way to swap stacks is a naked tail-jump: point PSP at
 * the fake task stack, flip CONTROL.SPSEL, then `bx fn` -- fn's prologue
 * pushes onto the PSP, and fn must never return (each PSP scenario ends in
 * a fault dump). */
__attribute__((naked)) static void app_run_on_psp(void (*fn)(void))
{
    __asm volatile (
        "mov r4, r0\n\t"
        "ldr r0, =psp_stack\n\t"
        "movw r1, #%c0\n\t"
        "add r0, r0, r1\n\t"
        "msr psp, r0\n\t"
        "movs r0, #2\n\t"        /* CONTROL: Thread mode (priv), use PSP */
        "msr control, r0\n\t"
        "isb\n\t"
        "bx  r4\n" : : "i"((int)sizeof(psp_stack)));
    __builtin_unreachable();
}

#if TEST_CASE == 4
__attribute__((noinline)) static void app_psp_body(void)
{
    /* Still in Thread mode, now running on the PSP. */
    app_busfault();
    for (;;) {
    }
}

static void app_psp_fault(void)
{
    app_run_on_psp(app_psp_body);
}
#endif /* TEST_CASE == 4 */
#endif /* TEST_CASE == 4 || TEST_CASE == 8 */

#if TEST_CASE == 5
/* Re-entrancy guard test: deliberately fire ANOTHER exception from inside
 * an on-going dump.  A second BusFault cannot preempt the BusFault handler
 * it fires from (same priority -> latched, never taken), so we pend an NMI
 * via ICSR.NMIPENDSET: NMI has the fixed highest priority and WILL preempt
 * the running fault dump -- the guard must log "fault while dumping,
 * ignored" instead of recursing.  This STRONG definition overrides
 * tracer.c's weak tracer_on_fault(), so it only exists for this build. */
void tracer_on_fault(const tracer_fault_t *f)
{
    (void)f;
    APP_LOG("hook: pend NMI inside dump\r\n");
    *(volatile uint32_t *)0xE000ED04u = 0x80000000u; /* ICSR.NMIPENDSET */
}
#endif /* TEST_CASE == 5 */

#if TEST_CASE == 9
/* Assert re-entrancy guard: fire TRACER_ASSERT(0) from inside an on-going
 * dump (via the strong tracer_on_fault hook).  tracer_assert_fail() sees the
 * dump already in progress and must log "assert while dumping, ignored"
 * instead of recursing. */
void tracer_on_fault(const tracer_fault_t *f)
{
    (void)f;
    APP_LOG("hook: assert inside dump\r\n");
    TRACER_ASSERT(0);
}
#endif /* TEST_CASE == 9 */

#if TEST_CASE == 7 || TEST_CASE == 8
/* Enable the FPU (CPACR CP10/CP11 full access).  The TrustZone machines
 * (M33/M55) boot Secure, so the plain CPACR (0xE000ED88) is the Secure
 * one -- enough to give the Secure test code an FPU context. */
static void app_fp_enable(void)
{
    volatile uint32_t *cpacr = (volatile uint32_t *)0xE000ED88u;
    *cpacr = (*cpacr | 0x00F00000u);
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb");
}

__attribute__((noinline)) static void app_fp_touch(void)
{
    /* Real VFP work so the faulting context owns an FPU (extended) frame. */
    volatile float a = 1.0f, b = 2.0f;
    volatile float c = a * b + 3.0f;
    (void)c;
}

#if TEST_CASE == 7
static void app_fpu_fault(void)
{
    app_fp_enable();
    APP_LOG("FPU enabled, touching VFP\r\n");
    app_fp_touch();
    app_busfault();
}
#endif /* TEST_CASE == 7 */

#if TEST_CASE == 8
/* PSP + FPU combined: like an RTOS task that uses the FPU and then crashes
 * -- run on the PSP (Thread mode), own an FPU context THERE, then fault so
 * the dump decodes BOTH the PSP frame and the extended FPU frame.  (Own the
 * FPU after the stack switch: a switch to another PSP is treated like a
 * task switch, which clears FPU ownership on these cores.) */
__attribute__((noinline)) static void app_psp_fp_body(void)
{
    app_fp_enable();
    APP_LOG("FPU enabled, touching VFP\r\n");
    app_fp_touch();      /* FPU context owned by this PSP task */
    app_busfault();
    for (;;) {
    }
}

static void app_psp_fp_fault(void)
{
    app_run_on_psp(app_psp_fp_body); /* enable FPU + touch on the PSP */
    for (;;) {
    }
}
#endif /* TEST_CASE == 8 */
#endif /* TEST_CASE == 7 || TEST_CASE == 8 */

int main(void)
{
    board_uart_init();

    APP_LOG("app: boot (TEST_CASE=%d)\r\n", (int)TEST_CASE);
    tracer_init();

#ifdef TRACER_GCOV
    /* QEMU coverage-only warm-up: hit paths the fault scenarios never reach
     * so the exported .gcda still counts them.  A log record +
     * tracer_log_drain() exercises the pull model.  (SysTick arm + wrap
     * polling is temporarily disabled while bisecting a hang.)
     * This block is compiled out of the normal board matrix
     * (-DTRACER_GCOV is coverage-only). */
    {
        APP_LOG("cov warm-up\r\n");
        {
            uint8_t dbuf[64];
            (void)tracer_log_drain(dbuf, (uint32_t)sizeof(dbuf));
        }
    }
#endif

    switch (TEST_CASE) {
    case 1:
        APP_LOG("trigger: unaligned access\r\n");
        tracer_trigger_unalign();
        break;
    case 2:
        app_busfault();
        break;
    case 3:
        TRACER_ASSERT(0);
        break;
#if TEST_CASE == 4
    case 4:
        app_psp_fault();
        break;
#endif
#if TEST_CASE == 5
    case 5:
        app_busfault(); /* dump starts; tracer_on_fault re-fires inside it */
        break;
#endif
#if TEST_CASE == 6
    case 6:
        app_busfault(); /* dump then system reset -> second boot */
        break;
#endif
#if TEST_CASE == 7
    case 7:
        app_fpu_fault();
        break;
#endif
#if TEST_CASE == 8
    case 8:
        app_psp_fp_fault();
        break;
#endif
#if TEST_CASE == 9
    case 9:
        app_busfault(); /* dump starts; hook fires TRACER_ASSERT inside it */
        break;
#endif
#if TEST_CASE == 10
    case 10:
        TRACER_ASSERT(0); /* assert dump, then auto-reset -> second boot */
        break;
#endif
    case 0:
    default:
        app_frame_a();
        APP_LOG("PASS smoke\r\n");
        break;
    }

#ifdef TRACER_GCOV
    /* smoke path: no fault -> dump here (fault cases dump in tracer_halt). */
    tracer_gcov_dump();
#endif

    for (;;) {
    }
    return 0;
}
