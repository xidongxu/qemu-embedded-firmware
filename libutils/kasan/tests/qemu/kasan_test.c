/* kasan_test.c -- QEMU firmware test for the kasan library.
 * Instrumented with -fsanitize=kernel-address: every memory access calls the
 * kasan __asan_*_noabort hooks.  KHEAP_CASE injects one fault; the report is
 * parked in kasan_* markers then traps (gdb reads them).
 *
 *   1 = heap overflow   : write past an allocation into the neighbour's
 *                         poisoned header -> caught on the spot
 *   2 = use-after-free  : write after kasan_free() (block re-poisoned)
 *   3 = double-free     : kasan_free() twice (record table check)
 *   4 = heap underflow  : write before an allocation into its own poisoned
 *                         header -> caught on the spot
 *   5 = realloc move    : use-after-free via the old pointer after
 *                         kasan_realloc() relocated the block
 *   6 = realloc shrink  : write past the shrunk block (backend moves it)
 *   7 = partial granule : 20-byte allocation; a[20] hits the poisoned rest
 *                         of the tail granule (4-byte aligned block size)
 *   8 = memcpy overflow : memcpy() reads past a block into the header
 *   9 = memset overflow : memset() writes past a block into the header
 *  10 = quarantine      : a freed block is held (not reused by the next
 *                         same-size malloc), so a stale write still traps
 *  11 = global overflow : a write past a global array hits its compiler-
 *                         generated redzone (--param asan-globals=1)
 *  12 = stack overflow  : a write past a local buffer hits the compiler-
 *                         generated stack redzone (--param asan-stack=1)
 *  13 = runtime region   : a write into a runtime-registered segment
 *                         (0x80100000) is caught by its own shadow map
 *  14 = multi-heap       : a second heap (runtime-registered over
 *                         0x80100000) catches a heap overflow
 */
#include "kasan.h"
#include <stdint.h>
#include <string.h>

volatile uint32_t g_sink;
uint32_t g_arr[10];   /* global; non-volatile so the OOB store is instrumented */

/* Stack overflow must happen in a function called AFTER kasan_init(): the
 * prologue inlines the stack-redzone poison, and kasan_init() zeroes the
 * whole shadow, so a local in main() itself would be un-poisoned again. */
__attribute__((noinline))
static void kasan_stack_oob(void) {
    volatile char buf[16];
    buf[16] = 0x42;                       /* stack OOB -> trap */
}

/* mps2-an505 CMSDK UART @0x40200000.  QEMU's cmsdk-uart needs BAUDDIV set
 * (0x40200010 = 16) or TX is silent. */
#define UART_BASE    0x40200000u
#define UART_DATA    (*(volatile uint32_t *)(UART_BASE + 0x00u))
#define UART_STATE   (*(volatile uint32_t *)(UART_BASE + 0x04u))
#define UART_CTRL    (*(volatile uint32_t *)(UART_BASE + 0x08u))
#define UART_BAUDDIV (*(volatile uint32_t *)(UART_BASE + 0x10u))

static void uart_init(void) {
    UART_BAUDDIV = 16u;
    UART_CTRL = 1u;                       /* TX enable */
}

static void uart_putc(char c) {
    while (UART_STATE & 2u) {             /* TX FIFO full */
    }
    UART_DATA = (uint32_t)c;
}

static void uart_puts(const char *s) {
    while (*s != 0) {
        uart_putc(*s);
        s++;
    }
}

static void uart_put_hex(uint32_t v) {
    uint32_t i = 0;
    uart_puts("0x");
    for (i = 0; i < 8u; i++) {
        uint32_t nib = (v >> (28u - i * 4u)) & 0xFu;
        uart_putc((char)(nib < 10u ? (uint32_t)'0' + nib
                                   : (uint32_t)'a' + nib - 10u));
    }
}

/* Report sink: print a human-readable line over the UART before trapping. */
static void kasan_uart_sink(uint32_t type, uint32_t addr, uint32_t size,
                            uint32_t shadow, uint32_t cause, uint32_t pc,
                            uint32_t alloc_pc, uint32_t free_pc) {
    (void)size;
    (void)pc;
    (void)alloc_pc;
    (void)free_pc;
    uart_puts("\r\nKASAN fault: type=");
    uart_put_hex(type);
    uart_puts(" addr=");
    uart_put_hex(addr);
    uart_puts(" shadow=");
    uart_put_hex(shadow);
    uart_puts(" cause=");
    uart_put_hex(cause);
    uart_puts("\r\n");
}

int main(void) {
    kasan_set_alloc_backend(kasan_tlsf_backend());
    uart_init();
    kasan_set_report_sink(kasan_uart_sink);
    kasan_init();
    kasan_heap_init();

#if KHEAP_CASE == 1
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        uint8_t *b = (uint8_t *)kasan_malloc(64);
        uint32_t i = 0;
        for (i = 0; i < 32; i++) {
            a[i] = (uint8_t)i;
        }
        b[0] = 0xEE;
        a[32] = 0xAA;                       /* OOB -> hits b's header */
        g_sink = a[0] + b[0];
    }
#elif KHEAP_CASE == 2
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        a[0] = 0x11;
        kasan_free(a);
        a[0] = 0x22;                        /* use-after-free -> trap  */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 3
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        kasan_free(a);
        kasan_free(a);                      /* double free -> trap     */
    }
#elif KHEAP_CASE == 4
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        a[0] = 0x11;
        a[-1] = 0xAA;                       /* underflow -> hits own hdr */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 5
    {
        /* Occupy the next block so kasan_realloc() cannot grow in place and
         * must move; the old pointer then traps on use-after-free. */
        uint8_t *a = (uint8_t *)kasan_malloc(16);
        uint8_t *c = (uint8_t *)kasan_malloc(16);
        uint8_t *b = 0;
        a[0] = 0x11;
        c[0] = 0x22;
        b = (uint8_t *)kasan_realloc(a, 256);
        if (b != a) {
            a[0] = 0x33;                    /* UAF via old pointer -> trap */
        }
        g_sink = b[0] + c[0];
    }
#elif KHEAP_CASE == 6
    {
        uint8_t *a = (uint8_t *)kasan_malloc(64);
        a[0] = 0x11;
        a = (uint8_t *)kasan_realloc(a, 32);    /* shrink (backend moves) */
        a[32] = 0x22;                       /* past shrunk block -> trap */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 7
    {
        uint8_t *a = (uint8_t *)kasan_malloc(20);
        a[0] = 0x11;
        a[20] = 0xAA;                       /* tail partial granule -> trap */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 8
    {
        /* Variable size forces an out-of-line memcpy -> __wrap_memcpy; the
         * source runs past a (16 bytes) into the neighbour's header. */
        uint8_t *a = (uint8_t *)kasan_malloc(16);
        uint8_t *b = (uint8_t *)kasan_malloc(64);
        volatile uint32_t n = 32;
        a[0] = 0x11;
        b[0] = 0x22;
        memcpy(b, a, n);                    /* reads a[16..31] -> trap */
        g_sink = a[0] + b[0];
    }
#elif KHEAP_CASE == 9
    {
        uint8_t *a = (uint8_t *)kasan_malloc(16);
        volatile uint32_t n = 32;
        a[0] = 0x11;
        memset(a, 0, n);                    /* writes a[16..31] -> trap */
        g_sink = a[0];
    }
#elif KHEAP_CASE == 10
    {
        uint8_t *a = (uint8_t *)kasan_malloc(32);
        uint8_t *b = 0;
        a[0] = 0x11;
        kasan_free(a);                    /* quarantined -> stays 0xFA */
        b = (uint8_t *)kasan_malloc(32);  /* must NOT reuse a's block   */
        if (b != a) {
            a[0] = 0x33;                  /* UAF via old pointer -> trap */
        }
        g_sink = b[0];
    }
#elif KHEAP_CASE == 11
    {
        g_arr[10] = 0xAA;                 /* global OOB -> trap */
        g_sink = g_arr[0];
    }
#elif KHEAP_CASE == 12
    {
        kasan_stack_oob();                /* stack OOB -> trap */
        g_sink = 0;
    }
#elif KHEAP_CASE == 13
    {
        /* A runtime-registered shadow segment (0x80100000, 128 KB; its shadow
         * map is carved from its own tail).  Poison a granule there: the
         * write traps via the segment table. */
        volatile uint8_t *seg1 = (volatile uint8_t *)0x80100000u;
        kasan_region_register(0x80100000u, 0x20000u, 0);
        kasan_poison(0x80100000u, 16u);
        seg1[8] = 0xAA;                   /* poisoned -> trap */
        g_sink = seg1[0];
    }
#elif KHEAP_CASE == 14
    {
        /* Multi-heap: register a second TLSF heap over 0x80100000 (128 KB)
         * and overflow an allocation from it.  The heap's own shadow map
         * (carved from its tail by kasan_heap_register) catches the write. */
        const kasan_alloc_backend_t *be = kasan_tlsf_create();
        kasan_heap_t *h2 = kasan_heap_register(be, (void *)0x80100000u,
                                               0x20000u, 0);
        volatile uint8_t *a = 0;
        if (h2 != 0) {
            a = (volatile uint8_t *)kasan_heap_malloc(h2, 32u);
        }
        if (a != 0) {
            a[32] = 0xAA;                 /* heap #2 overflow -> trap */
        }
        g_sink = 0;
    }
#endif

    for (;;) {
    }
    return 0;
}
