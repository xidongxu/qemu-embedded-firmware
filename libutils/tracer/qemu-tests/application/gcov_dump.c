/* gcov_dump.c - QEMU-test coverage export (only linked by board_test.py in
 * --coverage mode).
 *
 * Each instrumented TU compiled with -fprofile-info-section contributes one
 * `struct gcov_info *` to the .gcov_info output section (no __gcov_init /
 * libgcov file backend needed).  tracer_gcov_dump() serializes every TU's
 * .gcda over the UART as:
 *     [0xA5 'G' 'C'] [u16 LE filename len] [filename]  <.gcda bytes>
 * ... one frame per TU.  The host splits on the 0xA5'G''C' magic.
 *
 * It ALSO provides a strong tracer_halt() (overriding tracer.c's weak trap),
 * so every fault/assert case dumps its coverage right before trapping.  The
 * auto-reset cases (TEST_CASE 6/10) are excluded from coverage runs because
 * a system reset would wipe the counters.
 */
#include "uart.h"

/* Ships with the ARM toolchain (defines struct gcov_info + exporter). */
#include "gcov.h"

extern struct gcov_info *const __gcov_info_start[];
extern struct gcov_info *const __gcov_info_end[];

static void gd_putc(int c)
{
    board_putc(c);
}

static void gd_bytes(const void *p, unsigned int n)
{
    const unsigned char *q = (const unsigned char *)p;
    while (n-- != 0u) {
        gd_putc(*q++);
    }
}

static void gd_u16(unsigned int v)
{
    gd_putc((int)(v & 0xFFu));
    gd_putc((int)((v >> 8) & 0xFFu));
}

/* Dump callback: raw .gcda bytes out of the UART. */
static void gd_dump_cb(const void *data, unsigned int len, void *arg)
{
    (void)arg;
    gd_bytes(data, len);
}

/* Called once per TU, before its .gcda: emit the frame header + source name
 * so the host can name the .gcda to match the right .gcno. */
static void gd_filename_cb(const char *name, void *arg)
{
    unsigned int l = 0u;
    (void)arg;
    if (name != 0) {
        while (name[l] != '\0') {
            l++;
        }
    }
    gd_putc(0xA5);
    gd_putc('G');
    gd_putc('C');
    gd_u16(l);
    gd_bytes((name != 0) ? name : "", l);
}

/* Single growing pool; exporter is called once per TU at trap time. */
static char s_pool[32768];
static unsigned int s_used = 0u;

static void *gd_alloc_cb(unsigned int size, void *arg)
{
    void *p;
    (void)arg;
    size = (size + 3u) & ~3u;
    if (s_used + size > sizeof(s_pool)) {
        return 0;
    }
    p = s_pool + s_used;
    s_used += size;
    return p;
}

/* Stream every instrumented TU's .gcda to the UART. */
void tracer_gcov_dump(void)
{
    struct gcov_info *const *it;
    for (it = __gcov_info_start; it < __gcov_info_end; ++it) {
        if (*it != 0) {
            __gcov_info_to_gcda(*it, gd_filename_cb, gd_dump_cb, gd_alloc_cb, 0);
        }
    }
}

/* Strong override of tracer.c's weak tracer_halt(): dump coverage, then the
 * real infinite trap (so the harness still sees the trap / markers). */
void tracer_halt(void)
{
    tracer_gcov_dump();
    for (;;) {
    }
}

/* QEMU coverage-only: make tracer_stack_limit() report "unknown" so the PSP
 * call-stack scan takes tracer.c's `limit == 0 -> TRACER_STACK_TOP` fallback
 * line (unreachable with the default &_estack answer).  Coverage-only build:
 * this file is only linked by the coverage harness, never the main matrix. */
uint32_t tracer_stack_limit(void)
{
    return 0u;
}
