/* kasan.h -- minimal KASan (memory-safety checker) for Cortex-M.
 *
 * Targets compiled with `-fsanitize=kernel-address` turn every memory access
 * into a call to __asan_{load,store}{1,2,4,8,16}_noabort(addr).  This library
 * owns the shadow map and a pluggable allocator backend, giving real KASan
 * semantics:
 *
 *   - only the user area of a live allocation is unpoisoned;
 *   - everything else (block headers, free blocks, unused arena, ...) is
 *     poisoned, so heap overflow / underflow and use-after-free are caught
 *     immediately by the compiler-inserted check (not delayed until free);
 *   - a live-allocation record table catches double-free and bad free;
 *   - a freed block is quarantined (kept poisoned) for a while, extending the
 *     use-after-free detection window past block reuse;
 *   - UAF / double-free reports carry the alloc and free call sites.
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
/* Host tests may point the heap arena at their own RAM inside a fake region
 * via KASAN_ARENA_EXT (an integer expression giving the arena base); the
 * arena size defaults to KASAN_HEAP_SIZE unless KASAN_ARENA_SIZE is set. */
#ifdef KASAN_ARENA_EXT
#ifndef KASAN_ARENA_SIZE
#define KASAN_ARENA_SIZE KASAN_HEAP_SIZE
#endif
#endif
/* Live-allocation record table capacity: the maximum number of SIMULTANEOUS
 * live allocations tracked for double-free / bad-free detection.  Each entry
 * is 16 bytes (ptr+state packed + size + alloc_pc + free_pc) on 32-bit, so
 * 4096 entries cost 64 KB of .bss; the 2-bit state is packed into the low
 * bits of the 8-aligned pointer.  alloc_pc / free_pc record the call sites
 * of the allocation and the free for UAF / double-free reports.  When the
 * table fills up, bad-free detection degrades gracefully (kasan_live_overflow
 * is set); double-free detection for tracked pointers keeps working. */
#ifndef KASAN_LIVE_MAX
#define KASAN_LIVE_MAX 4096u
#endif
/* Number of shadow bytes dumped around a faulting address in the report. */
#ifndef KASAN_SHADOW_DUMP
#define KASAN_SHADOW_DUMP 16u
#endif
/* Quarantine: freed blocks are not handed back to the allocator immediately
 * but kept poisoned (0xFA) for a while, so use-after-free via a stale pointer
 * is still caught after the block would otherwise have been reused.
 * KASAN_QUARANTINE_BYTES caps the TOTAL held bytes (0 disables the
 * quarantine: freed blocks return to the allocator at once);
 * KASAN_QUARANTINE_MAX caps the number of held blocks (must be >= 1). */
#ifndef KASAN_QUARANTINE_BYTES
#define KASAN_QUARANTINE_BYTES (8u * 1024u)
#endif
#ifndef KASAN_QUARANTINE_MAX
#define KASAN_QUARANTINE_MAX 64u
#endif

/* Zero the shadow for the whole region. */
void kasan_init(void);
/* Poison [addr, addr+len) in the shadow map. */
void kasan_poison(uint32_t addr, uint32_t len);
/* Unpoison [addr, addr+len) in the shadow map. */
void kasan_unpoison(uint32_t addr, uint32_t len);

/* Allocator backend: kasan keeps only the shadow map and a live-allocation
 * record table; the allocation policy is delegated to a pluggable backend so
 * the same checking logic works over TLSF, newlib malloc, an RTOS heap, ... */
typedef struct kasan_alloc_backend {
    const char *name;
    /* (Re)create the allocator and report the arena it manages as
     * [*base, *base + *size), so kasan can poison the whole arena. */
    void (*init)(uint32_t *base, uint32_t *size);
    /* Allocate bytes and return the user pointer (0 on failure). */
    void *(*malloc)(uint32_t bytes);
    /* Size in bytes of the user area at p; may be NULL (kasan then falls
     * back to the requested size).  Must be >= the requested size. */
    uint32_t (*usable)(void *p);
    /* Release the user pointer p. */
    void (*free)(void *p);
    /* Resize p to bytes (may return p in place or a moved pointer).  NULL
     * means unsupported: kasan_realloc() falls back to malloc+copy+free. */
    void *(*realloc)(void *p, uint32_t bytes);
    /* Allocate bytes aligned to align (a power of two).  NULL means
     * unsupported: kasan_memalign() returns NULL. */
    void *(*memalign)(uint32_t align, uint32_t bytes);
} kasan_alloc_backend_t;

/* Register the allocator backend; call before kasan_heap_init(). */
void kasan_set_alloc_backend(const kasan_alloc_backend_t *backend);
/* TLSF backend (libmem/tlsf): returns a static descriptor. */
const kasan_alloc_backend_t *kasan_tlsf_backend(void);

/* Poison the whole arena and (re)create the allocator via the registered
 * backend.  Only the user area of a live allocation is unpoisoned; block
 * headers, free blocks and the unused arena stay poisoned, so overflow /
 * underflow / use-after-free are caught immediately by the instrumented
 * check. */
void kasan_heap_init(void);
/* Allocate nbytes via the backend; the returned user area is unpoisoned and
 * tracked for double-free / bad-free detection. */
void *kasan_malloc(uint32_t nbytes);
/* Free a block: the whole user area is re-poisoned (use-after-free caught);
 * the record table catches double-free / bad-free. */
void kasan_free(void *p);
/* Allocate zeroed memory for nmemb elements of size bytes (calloc). */
void *kasan_calloc(uint32_t nmemb, uint32_t size);
/* Resize an allocation (NULL p -> malloc, 0 size -> free).  A moved block
 * leaves the old pointer re-poisoned, so use-after-free via it is caught. */
void *kasan_realloc(void *p, uint32_t size);
/* Allocate bytes aligned to align (a power of two); NULL if unsupported by
 * the backend. */
void *kasan_memalign(uint32_t align, uint32_t bytes);
/* Human-readable name for a shadow byte value ("addressable", "freed",
 * "redzone", ...); for reports / a UART-tracer sink. */
const char *kasan_shadow_name(uint8_t value);
/* Copy `count` shadow bytes centred on addr into out; a granule outside the
 * instrumented region reads KASAN_SHADOW_NO_SHADOW (0xEE). */
void kasan_shadow_dump(uint32_t addr, uint8_t *out, uint32_t count);
/* Release every quarantined block back to the allocator immediately (useful
 * before a memory-critical section, or in tests). */
void kasan_quarantine_drain(void);

/* Optional report sink: invoked by kasan_report() with the fault details
 * before it traps.  Register a UART / tracer dump here; the default (no sink)
 * only parks the kasan_* markers and traps. */
typedef void (*kasan_report_sink_fn)(uint32_t type, uint32_t addr,
                                     uint32_t size, uint32_t shadow,
                                     uint32_t cause, uint32_t pc,
                                     uint32_t alloc_pc, uint32_t free_pc);
void kasan_set_report_sink(kasan_report_sink_fn sink);

/* Global-variable redzone registration: called by the compiler-generated
 * .init_array when the app is built with -fsanitize=kernel-address
 * --param asan-globals=1.  The struct layout is fixed by the compiler (one
 * 32-byte record per instrumented global on a 32-bit target). */
struct __asan_global;
void __asan_register_globals(struct __asan_global *globals, uint32_t n);
void __asan_unregister_globals(struct __asan_global *globals, uint32_t n);

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
/* Fault classification derived from the shadow byte at the fault address:
 * 0=unknown 1=redzone(overflow/underflow) 2=freed(use-after-free)
 * 3=partial-granule boundary 4=generic poisoned. */
extern volatile uint32_t kasan_report_cause;
/* For freed-block faults (use-after-free / double-free): the call sites of
 * the allocation and of the free, looked up from the live record table.
 * 0 when unknown (bad-free, non-freed fault, or record-table overflow). */
extern volatile uint32_t kasan_report_alloc_pc;
extern volatile uint32_t kasan_report_free_pc;
/* Shadow bytes around the fault address (see kasan_shadow_dump). */
extern volatile uint8_t kasan_report_shadow_dump[KASAN_SHADOW_DUMP];
/* Set when the record table overflowed (more than KASAN_LIVE_MAX live
 * allocations at once); bad-free detection is then disabled to avoid false
 * positives. */
extern volatile uint32_t kasan_live_overflow;

#ifdef __cplusplus
}
#endif

#endif /* KASAN_H */
