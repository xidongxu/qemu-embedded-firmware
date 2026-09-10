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
 */
#include "kasan.h"
#include <stdint.h>
#include <string.h>

volatile uint32_t g_sink;
uint32_t g_arr[10];   /* global; non-volatile so the OOB store is instrumented */

int main(void) {
    kasan_set_alloc_backend(kasan_tlsf_backend());
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
#endif

    for (;;) {
    }
    return 0;
}
