/* kasan.h -- minimal KASan (memory-safety checker) for Cortex-M.
 *
 * Targets compiled with `-fsanitize=kernel-address` turn every memory access
 * into a call to __asan_{load,store}{1,2,4,8,16}_noabort(addr).  This library
 * owns the shadow map plus a heap allocator, giving real KASan semantics:
 *
 *   - only the user area of a live allocation is unpoisoned;
 *   - everything else (block headers, free blocks, unused arena, ...) is
 *     poisoned, so heap overflow / underflow and use-after-free are caught
 *     immediately by the compiler-inserted check (not delayed until free);
 *   - block-header magic catches double-free and bad free.
 *
 * The library itself is compiled WITHOUT -fsanitize (it must be able to touch
 * shadow and arena directly); only the code under test is instrumented.
 *
 * Memory map is configurable with -D defines (defaults are the mps2-an505
 * QEMU values):
 *   KASAN_REGION_BASE  0x80000000   instrumented region base (real RAM)
 *   KASAN_REGION_SIZE  0x00040000   instrumented region size (256 KB)
 *   KASAN_HEAP_SIZE    (64 KB)      heap arena size (must fit in region)
 *
 * The shadow map is carved out of the TAIL of the instrumented region itself
 * (the top 1/8) by default, matching fixed MCU memory maps that have no spare
 * RAM bank; define KASAN_SHADOW_BASE to place it in an independent RAM bank
 * instead (then the whole region stays usable).
 *
 * shadow_of(a) = shadow_base + (a - region_base) / 8, valid only inside the
 * usable part of the region; accesses elsewhere pass through unchecked.
 */
#ifndef KASAN_H
#define KASAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KASAN_REGION_BASE
#define KASAN_REGION_BASE 0x80000000u
#endif
#ifndef KASAN_REGION_SIZE
#define KASAN_REGION_SIZE 0x00040000u
#endif
/* Inline shadow by default (tail of the region); an explicit
 * KASAN_SHADOW_BASE switches to an independent shadow RAM bank. */
#ifdef KASAN_SHADOW_BASE
#define KASAN_USABLE_SIZE KASAN_REGION_SIZE
#else
#define KASAN_SHADOW_BASE (KASAN_REGION_BASE + KASAN_REGION_SIZE - \
                           (KASAN_REGION_SIZE / 8u))
#define KASAN_USABLE_SIZE (KASAN_SHADOW_BASE - KASAN_REGION_BASE)
#endif
#define KASAN_SHADOW_SIZE (KASAN_USABLE_SIZE / 8u)
#ifndef KASAN_HEAP_SIZE
#define KASAN_HEAP_SIZE (64u * 1024u)
#endif
/* Leak detection record-table size: every live allocation is logged while
 * kasan_malloc() hands it out.  The table is fixed-size (a debug aid); when
 * it fills up new allocations are still served but not logged. */
#ifndef KASAN_LEAK_MAX
#define KASAN_LEAK_MAX 32u
#endif
/* Host tests may point the heap arena at their own RAM inside a fake region
 * via KASAN_ARENA_EXT (an integer expression giving the arena base); the
 * arena size defaults to KASAN_HEAP_SIZE unless KASAN_ARENA_SIZE is set. */
#ifdef KASAN_ARENA_EXT
#ifndef KASAN_ARENA_SIZE
#define KASAN_ARENA_SIZE KASAN_HEAP_SIZE
#endif
#endif

/* Zero the shadow for the whole region. */
void kasan_init(void);
/* Poison [addr, addr+len) in the shadow map. */
void kasan_poison(uint32_t addr, uint32_t len);
/* Unpoison [addr, addr+len) in the shadow map. */
void kasan_unpoison(uint32_t addr, uint32_t len);
/* Poison the whole arena and seed one big free block. */
void kasan_heap_init(void);
/* Allocate nbytes; the block header and neighbours stay poisoned so overflow
 * or underflow on the returned area are caught immediately. */
void *kasan_malloc(uint32_t nbytes);
/* Free a block: the whole area is re-poisoned (use-after-free caught) and
 * the header magic catches double-free / bad-free. */
void kasan_free(void *p);
/* Recount every allocation still in the leak record table and refresh the
 * kasan_leak_* markers.  A workload that frees everything it should can
 * assert kasan_leak_count == 0 afterwards. */
void kasan_leak_dump(void);
/* Walk the live records: returns the user pointer of the index-th record
 * still held (index 0 = first) and stores its size / callsite in the out
 * pointers (optional); returns 0 past the end. */
uint32_t kasan_leak_live(uint32_t index, uint32_t *size, uint32_t *pc);
/* Fault report side-channel: kasan_report() parks the fault info in the
 * markers below then traps (the QEMU test reads them via gdb; a real port
 * can hook an UART/tracer sink instead).  Define KASAN_TEST_RETURNS to make
 * report() return instead of trapping (host unit tests).  report type:
 * 1=load 2=store 3=double-free 4=bad-free. */
extern volatile uint32_t kasan_reports;
extern volatile uint32_t kasan_report_type;
extern volatile uint32_t kasan_report_addr;
extern volatile uint32_t kasan_report_size;
extern volatile uint32_t kasan_report_shadow;
extern volatile uint32_t kasan_report_pc;
/* Leak-detection markers, refreshed by kasan_leak_dump(): count and total
 * bytes of allocations still live, plus the first live record (user ptr,
 * size, callsite) for a quick gdb read. */
extern volatile uint32_t kasan_leak_count;
extern volatile uint32_t kasan_leak_bytes;
extern volatile uint32_t kasan_leak_ptr;
extern volatile uint32_t kasan_leak_size;
extern volatile uint32_t kasan_leak_pc;
extern volatile uint32_t kasan_leak_lost;

#ifdef __cplusplus
}
#endif

#endif /* KASAN_H */
