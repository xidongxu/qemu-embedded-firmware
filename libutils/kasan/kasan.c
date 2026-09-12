/* kasan.c -- minimal KASan runtime for Cortex-M.
 *
 * Compiled WITHOUT -fsanitize=kernel-address: this file must touch shadow RAM
 * and the (mostly poisoned) heap arena directly.  Instrumented app code calls
 * the __asan_*_noabort hooks below on every memory access.  The allocator is
 * delegated to a pluggable backend (see kasan_alloc_backend_t); this file only
 * keeps the shadow map and the live-allocation record table.
 *
 * Shadow semantics (ASan): one shadow byte per 8-byte granule.  0x00 = fully
 * addressable, 0x01-0x07 = first N bytes addressable, 0xe1-0xe7 = first N
 * bytes poisoned (partial granule), 0xf1-0xf3 = stack redzone (left/mid/
 * right, fully poisoned), 0xF8 = global-variable redzone, 0xFA = freed
 * (use-after-free), 0xFB = heap redzone (header / free block / unused
 * arena), 0xff = generic poisoned.  Only live-allocation user areas are
 * unpoisoned, so any access to a header, free block or unused byte is
 * caught on the spot.
 */
#include "kasan.h"

volatile uint32_t kasan_reports = 0;
volatile uint32_t kasan_report_type = 0;
volatile uint32_t kasan_report_addr = 0;
volatile uint32_t kasan_report_size = 0;
volatile uint32_t kasan_report_shadow = 0;
volatile uint32_t kasan_report_pc = 0;
volatile uint32_t kasan_report_cause = 0;
volatile uint32_t kasan_report_alloc_pc = 0;
volatile uint32_t kasan_report_free_pc = 0;
volatile uint8_t kasan_report_shadow_dump[KASAN_SHADOW_DUMP];

/* Semantic poison values (cf. Linux KASan 0xFA freed / 0xFB heap redzone /
 * 0xF8 global redzone / 0xF1-0xF3 stack redzone).  The stack redzones are
 * written inline by the compiler (--param asan-stack=1); this file only
 * classifies them. */
#define KASAN_POISON_FREED     0xFAu
#define KASAN_POISON_REDZONE   0xFBu
#define KASAN_POISON_GLOBAL    0xF8u
#define KASAN_POISON_STACK_LEFT  0xF1u
#define KASAN_POISON_STACK_RIGHT 0xF3u
#define KASAN_SHADOW_NO_SHADOW 0xEEu

/* Classify a shadow byte for the report: 0=addressable, 1=redzone,
 * 2=freed (UAF), 3=partial boundary, 4=generic poisoned. */
static uint32_t kasan_cause_of(uint8_t value) {
    if (value == 0) {
        return 0;
    }
    if (value < 8u || (value >= 0xe1u && value <= 0xe7u)) {
        return 3;
    }
    if (value == KASAN_POISON_FREED) {
        return 2;
    }
    if (value == KASAN_POISON_REDZONE || value == KASAN_POISON_GLOBAL ||
        (value >= 0xf1u && value <= 0xf7u)) {
        return 1;
    }
    return 4;
}

/* Unified shadow segment table.  Every covered address range (the primary
 * region and each runtime-registered region / heap) is a row here;
 * kasan_shadow_of() walks it.  kasan_init() resets the table and registers
 * the primary region; further rows are appended at runtime via
 * kasan_register_region(). */
typedef struct {
    uint32_t base;
    uint32_t usable;
    uint32_t shadow;
} kasan_segment_t;

static kasan_segment_t kasan_segments[KASAN_MAX_SEGMENTS];
static uint32_t kasan_segment_count = 0;

static int kasan_segment_add(uint32_t base, uint32_t usable, uint32_t shadow) {
    if (kasan_segment_count >= KASAN_MAX_SEGMENTS) {
        return 0;
    }
    kasan_segments[kasan_segment_count].base = base;
    kasan_segments[kasan_segment_count].usable = usable;
    kasan_segments[kasan_segment_count].shadow = shadow;
    kasan_segment_count++;
    return 1;
}

/* Compute the usable size and shadow base for a region: an explicit
 * shadow_base wins; otherwise the shadow is carved from the region's own
 * tail (1/8). */
static void kasan_region_layout(uint32_t base, uint32_t size,
                                uint32_t shadow_base, uint32_t *usable,
                                uint32_t *shadow) {
    if (shadow_base == 0) {
        *usable = size - size / 8u;
        *shadow = base + *usable;
    } else {
        *usable = size;
        *shadow = shadow_base;
    }
}

static uint8_t *kasan_shadow_of(uint32_t addr) {
    uint32_t i = 0;

    for (i = 0; i < kasan_segment_count; i++) {
        if (addr >= kasan_segments[i].base &&
            addr < kasan_segments[i].base + kasan_segments[i].usable) {
            return (uint8_t *)(uintptr_t)(
                kasan_segments[i].shadow +
                ((addr - kasan_segments[i].base) >> 3));
        }
    }
    return 0;
}

static void kasan_poison_globals(void);

static void kasan_shadow_clear_segment(uint32_t shadow_base, uint32_t size) {
    volatile uint8_t *shadow = (volatile uint8_t *)(uintptr_t)shadow_base;
    uint32_t index = 0;

    for (index = 0; index < size; index++) {
        shadow[index] = 0;
    }
}

void kasan_init(void) {
    uint32_t i = 0;

    /* Reset the segment table and register the primary region (segment 0).
     * Runtime regions must be registered after kasan_init(). */
    kasan_segment_count = 0;
    kasan_segment_add(KASAN_REGION_BASE, KASAN_USABLE_SIZE, KASAN_SHADOW_BASE);

    for (i = 0; i < kasan_segment_count; i++) {
        kasan_shadow_clear_segment(kasan_segments[i].shadow,
                                   kasan_segments[i].usable / 8u);
    }
    kasan_poison_globals();
}

const char *kasan_shadow_name(uint8_t value) {
    if (value == 0) {
        return "addressable";
    }
    if (value < 8u) {
        return "partial-addressable";
    }
    if (value >= 0xe1u && value <= 0xe7u) {
        return "partial-poisoned";
    }
    if (value >= 0xf1u && value <= 0xf7u) {
        return "stack-redzone";
    }
    if (value == KASAN_POISON_FREED) {
        return "freed";
    }
    if (value == KASAN_POISON_REDZONE) {
        return "redzone";
    }
    if (value == KASAN_POISON_GLOBAL) {
        return "global-redzone";
    }
    if (value == KASAN_SHADOW_NO_SHADOW) {
        return "no-shadow";
    }
    if (value == 0xffu) {
        return "poisoned";
    }
    return "unknown";
}

void kasan_shadow_dump(uint32_t addr, uint8_t *out, uint32_t count) {
    uint32_t base = 0;
    uint32_t i = 0;

    base = (addr & ~7u) - (count / 2u) * 8u;
    for (i = 0; i < count; i++) {
        uint8_t *shadow = kasan_shadow_of(base + i * 8u);
        out[i] = shadow ? *shadow : KASAN_SHADOW_NO_SHADOW;
    }
}

static int kasan_live_find_freed(uint32_t addr, uint32_t *alloc_pc,
                                 uint32_t *free_pc);

static kasan_report_sink_fn kasan_report_sink = 0;

void kasan_set_report_sink(kasan_report_sink_fn sink) {
    kasan_report_sink = sink;
}

static void kasan_report(uint32_t type, uint32_t addr, uint32_t size,
                         uint32_t pc, uint32_t fault_addr) {
    uint8_t *shadow = kasan_shadow_of(fault_addr);
    uint32_t alloc_pc = 0;
    uint32_t free_pc = 0;

    if (shadow && *shadow == KASAN_POISON_FREED) {
        kasan_live_find_freed(fault_addr, &alloc_pc, &free_pc);
    }
    kasan_reports++;
    kasan_report_type = type;
    kasan_report_addr = addr;
    kasan_report_size = size;
    kasan_report_shadow = shadow ? *shadow : 0;
    kasan_report_pc = pc;
    kasan_report_cause = kasan_cause_of(shadow ? *shadow : 0);
    kasan_report_alloc_pc = alloc_pc;
    kasan_report_free_pc = free_pc;
    kasan_shadow_dump(fault_addr, (uint8_t *)kasan_report_shadow_dump,
                      KASAN_SHADOW_DUMP);
    if (kasan_report_sink) {
        kasan_report_sink(type, addr, size, shadow ? *shadow : 0,
                          kasan_report_cause, pc, alloc_pc, free_pc);
    }
#ifndef KASAN_TEST_RETURNS
    for (;;) {
    }
#else
    (void)0;
#endif
}

static void kasan_poison_as(uint32_t addr, uint32_t len, uint8_t value) {
    uint32_t end = addr + len;
    uint32_t cur = 0;

    if (len == 0) {
        return;
    }
    /* Head granule: leading bytes (of the previous object) stay addressable,
     * so mark it 0x0N (first N addressable, rest poisoned). */
    cur = addr & ~7u;
    if ((addr & 7u) != 0) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = (uint8_t)(addr & 7u);
        }
        cur += 8u;
    }
    /* Middle granules: fully poisoned with the semantic value. */
    for (; cur + 8u <= end; cur += 8u) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = value;
        }
    }
    /* Tail granule: trailing bytes are the next header (already poisoned), so
     * mark the whole granule with the semantic value. */
    if (cur < end) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = value;
        }
    }
}

void kasan_poison(uint32_t addr, uint32_t len) {
    kasan_poison_as(addr, len, 0xff);
}

void kasan_unpoison(uint32_t addr, uint32_t len) {
    uint32_t end = addr + len;
    uint32_t cur = 0;

    if (len == 0) {
        return;
    }
    /* Head granule: leading bytes (previous header) stay poisoned, so mark it
     * 0xeN (first N poisoned, rest addressable). */
    cur = addr & ~7u;
    if ((addr & 7u) != 0) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = (uint8_t)(0xe0u | (addr & 7u));
        }
        cur += 8u;
    }
    /* Middle granules: fully addressable. */
    for (; cur + 8u <= end; cur += 8u) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = 0;
        }
    }
    /* Tail granule: first (end & 7) bytes addressable, rest poisoned. */
    if ((end & 7u) != 0) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = (uint8_t)(end & 7u);
        }
    }
}

/* ---- global-variable redzone registration --------------------------------
 * The compiler (app built with -fsanitize=kernel-address
 * --param asan-globals=1) emits one struct __asan_global per instrumented
 * global -- layout fixed, eight 4-byte words on a 32-bit target -- plus a
 * .init_array call to __asan_register_globals and a .fini_array call to
 * __asan_unregister_globals.  We remember the table and (re)poison each
 * global's redzone [beg+size, beg+size_with_redzone).  The .init_array call
 * runs before kasan_init(), so it only records; kasan_init() zeroes the
 * shadow and then re-poisons via kasan_poison_globals(). */
struct __asan_global {
    const volatile void *beg;
    uint32_t size;
    uint32_t size_with_redzone;
    const char *name;
    const char *module_name;
    uint32_t has_dynamic_init;
    void *location;
    void *odr_indicator;
};

static const struct __asan_global *kasan_globals = 0;
static uint32_t kasan_globals_n = 0;

static void kasan_poison_globals(void) {
    uint32_t i = 0;

    for (i = 0; i < kasan_globals_n; i++) {
        const struct __asan_global *g = &kasan_globals[i];
        uint32_t beg = (uint32_t)(uintptr_t)g->beg;

        if (g->size_with_redzone > g->size) {
            kasan_poison_as(beg + g->size, g->size_with_redzone - g->size,
                            KASAN_POISON_GLOBAL);
        }
    }
}

void __asan_register_globals(struct __asan_global *globals, uint32_t n) {
    kasan_globals = globals;
    kasan_globals_n = n;
    kasan_poison_globals();
}

void __asan_unregister_globals(struct __asan_global *globals, uint32_t n) {
    uint32_t i = 0;

    for (i = 0; i < n; i++) {
        uint32_t beg = (uint32_t)(uintptr_t)globals[i].beg;

        if (globals[i].size_with_redzone > globals[i].size) {
            kasan_unpoison(beg + globals[i].size,
                           globals[i].size_with_redzone - globals[i].size);
        }
    }
    if (globals == kasan_globals) {
        kasan_globals = 0;
        kasan_globals_n = 0;
    }
}

static int kasan_check(uint32_t type, uint32_t addr, uint32_t size,
                       uint32_t pc) {
    uint32_t end_addr = addr + size;
    uint32_t scan = addr;

    for (scan = addr; scan < end_addr; scan = (scan & ~7u) + 8u) {
        uint8_t *shadow = kasan_shadow_of(scan);
        uint32_t base = 0;
        uint32_t acc_start = 0;
        uint32_t acc_end = 0;
        uint8_t value = 0;

        if (!shadow) {
            continue;
        }
        value = *shadow;
        if (value == 0) {
            continue;
        }
        base = scan & ~7u;
        acc_start = (addr > base) ? addr : base;
        acc_end = ((base + 8u) < end_addr) ? (base + 8u) : end_addr;
        if (value < 8u) {
            /* 1..7: first `value` bytes addressable, rest poisoned. */
            if (acc_end - base > value) {
                uint32_t fault = (acc_start > base + value) ? acc_start
                                                            : (base + value);
                kasan_report(type, addr, size, pc, fault);
                return 1;
            }
        } else if (value >= 0xe1u && value <= 0xe7u) {
            /* 0xe1..0xe7: first (value & 7) bytes poisoned, rest addressable. */
            if (acc_start - base < (uint32_t)(value & 7u)) {
                kasan_report(type, addr, size, pc, acc_start);
                return 1;
            }
        } else {
            /* 0x80..0xe0 / 0xe8..0xff: fully poisoned (incl. 0xf1-0xf3 stack
             * redzone, written inline by the compiler). */
            kasan_report(type, addr, size, pc, acc_start);
            return 1;
        }
    }
    return 0;
}

/* The noabort hooks are the kernel-address entry points: instrumented code
 * calls them before every memory access. */
#define KASAN_DEFINE_HOOK(name, type, size)                 \
void __asan_##name##size##_noabort(uint32_t addr) {         \
    kasan_check(type, addr, size,                           \
                (uint32_t)(uintptr_t)__builtin_return_address(0)); \
}

/* Define the noabort hooks for load operations */
KASAN_DEFINE_HOOK(load, 1, 1)
KASAN_DEFINE_HOOK(load, 1, 2)
KASAN_DEFINE_HOOK(load, 1, 4)
KASAN_DEFINE_HOOK(load, 1, 8)
KASAN_DEFINE_HOOK(load, 1, 16)

/* Define the noabort hooks for store operations */
KASAN_DEFINE_HOOK(store, 2, 1)
KASAN_DEFINE_HOOK(store, 2, 2)
KASAN_DEFINE_HOOK(store, 2, 4)
KASAN_DEFINE_HOOK(store, 2, 8)
KASAN_DEFINE_HOOK(store, 2, 16)

/* 
 * kasan does NOT implement an allocator.  It owns only the shadow map and a
 * live-allocation record table (for double-free / bad-free detection).  The
 * allocation policy is delegated to a pluggable backend so the same checking
 * logic works over TLSF, newlib malloc, an RTOS heap, ...  Register one with
 * kasan_set_alloc_backend() before kasan_heap_init().
 *
 * The whole arena is poisoned up front; only the user area of a live
 * allocation is unpoisoned.  Backend bookkeeping (block headers, free lists,
 * control blocks) stays poisoned, so instrumented code touching it traps --
 * while the backend itself (compiled without -fsanitize) reads/writes it
 * freely.
 */

volatile uint32_t kasan_live_overflow = 0;

#define KASAN_LIVE_STATE_EMPTY 0u
#define KASAN_LIVE_STATE_LIVE  1u
#define KASAN_LIVE_STATE_FREED 2u
#define KASAN_LIVE_STATE_MASK  7u

/* 16-byte record: state (2 bits) is packed into the low bits of ptr, which is
 * always 8-aligned (tlsf_memalign(tlsf, 8, ...)), so the low 3 bits are free. */
typedef struct {
    uint32_t ptr_state;
    uint32_t size;
    uint32_t alloc_pc;
    uint32_t free_pc;
} kasan_live_entry_t;

static uint32_t kasan_live_ptr(const kasan_live_entry_t *e) {
    return e->ptr_state & ~KASAN_LIVE_STATE_MASK;
}

static uint32_t kasan_live_state(const kasan_live_entry_t *e) {
    return e->ptr_state & KASAN_LIVE_STATE_MASK;
}

static void kasan_live_set(kasan_live_entry_t *e, uint32_t ptr, uint32_t state) {
    e->ptr_state = (ptr & ~KASAN_LIVE_STATE_MASK) |
                   (state & KASAN_LIVE_STATE_MASK);
}

static kasan_live_entry_t kasan_live_table[KASAN_LIVE_MAX];

/* Heap table: heap #0 is the default heap; kasan_heap_register() fills the
 * rest.  Each heap owns a backend instance and an arena [base, base+size). */
struct kasan_heap {
    const kasan_alloc_backend_t *backend;
    uint32_t arena_base;
    uint32_t arena_size;
    uint32_t in_use;
};

static kasan_heap_t kasan_heaps[KASAN_MAX_HEAPS];

/* Quarantine: freed blocks are held (still 0xFA-poisoned) instead of being
 * returned to the allocator, extending the UAF detection window.  A FIFO of
 * {ptr,size}; KASAN_QUARANTINE_BYTES caps the total held bytes. */
typedef struct {
    uint32_t ptr;
    uint32_t size;
} kasan_quarantine_entry_t;

static kasan_quarantine_entry_t kasan_quarantine[KASAN_QUARANTINE_MAX];
static uint32_t kasan_quarantine_head = 0;
static uint32_t kasan_quarantine_count = 0;
static uint32_t kasan_quarantine_bytes = 0;

static void kasan_live_reset(void) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        kasan_live_table[i].ptr_state = 0;
        kasan_live_table[i].size = 0;
        kasan_live_table[i].alloc_pc = 0;
        kasan_live_table[i].free_pc = 0;
    }
    kasan_live_overflow = 0;
}

static void kasan_live_add(uint32_t ptr, uint32_t size, uint32_t alloc_pc) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_state(&kasan_live_table[i]) != KASAN_LIVE_STATE_LIVE) {
            kasan_live_set(&kasan_live_table[i], ptr, KASAN_LIVE_STATE_LIVE);
            kasan_live_table[i].size = size;
            kasan_live_table[i].alloc_pc = alloc_pc;
            kasan_live_table[i].free_pc = 0;
            return;
        }
    }
    kasan_live_overflow = 1;
}

static int kasan_live_find(uint32_t ptr, uint32_t *size, uint32_t *state) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_ptr(&kasan_live_table[i]) == ptr &&
            kasan_live_state(&kasan_live_table[i]) != KASAN_LIVE_STATE_EMPTY) {
            *size = kasan_live_table[i].size;
            *state = kasan_live_state(&kasan_live_table[i]);
            return 1;
        }
    }
    return 0;
}

static void kasan_live_mark_freed(uint32_t ptr, uint32_t free_pc) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_ptr(&kasan_live_table[i]) == ptr &&
            kasan_live_state(&kasan_live_table[i]) == KASAN_LIVE_STATE_LIVE) {
            kasan_live_set(&kasan_live_table[i], ptr, KASAN_LIVE_STATE_FREED);
            kasan_live_table[i].free_pc = free_pc;
            return;
        }
    }
}

static void kasan_live_update_size(uint32_t ptr, uint32_t size) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_ptr(&kasan_live_table[i]) == ptr &&
            kasan_live_state(&kasan_live_table[i]) == KASAN_LIVE_STATE_LIVE) {
            kasan_live_table[i].size = size;
            return;
        }
    }
}

/* Overwrite the allocation call site of a freshly added record (used when a
 * public wrapper such as kasan_calloc routes through kasan_malloc and wants
 * the wrapper's caller, not kasan_malloc's caller). */
static void kasan_live_set_alloc_pc(uint32_t ptr, uint32_t alloc_pc) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_ptr(&kasan_live_table[i]) == ptr &&
            kasan_live_state(&kasan_live_table[i]) == KASAN_LIVE_STATE_LIVE) {
            kasan_live_table[i].alloc_pc = alloc_pc;
            return;
        }
    }
}

/* Look up the freed record whose [ptr, ptr+size) contains addr; return the
 * alloc / free call sites for the UAF / double-free report. */
static int kasan_live_find_freed(uint32_t addr, uint32_t *alloc_pc,
                                 uint32_t *free_pc) {
    uint32_t i = 0;
    uint32_t p = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        p = kasan_live_ptr(&kasan_live_table[i]);
        if (kasan_live_state(&kasan_live_table[i]) == KASAN_LIVE_STATE_FREED &&
            addr >= p && addr < p + kasan_live_table[i].size) {
            *alloc_pc = kasan_live_table[i].alloc_pc;
            *free_pc = kasan_live_table[i].free_pc;
            return 1;
        }
    }
    return 0;
}

void kasan_set_alloc_backend(const kasan_alloc_backend_t *backend) {
    kasan_heaps[0].backend = backend;
    kasan_heaps[0].arena_base = 0;
    kasan_heaps[0].arena_size = 0;
    kasan_heaps[0].in_use = 0;
}

static kasan_heap_t *kasan_heap_of_ptr(uint32_t ptr) {
    uint32_t i = 0;

    for (i = 0; i < KASAN_MAX_HEAPS; i++) {
        if (kasan_heaps[i].in_use &&
            ptr >= kasan_heaps[i].arena_base &&
            ptr < kasan_heaps[i].arena_base + kasan_heaps[i].arena_size) {
            return &kasan_heaps[i];
        }
    }
    return 0;
}

static void kasan_quarantine_reset(void) {
    kasan_quarantine_head = 0;
    kasan_quarantine_count = 0;
    kasan_quarantine_bytes = 0;
}

static void kasan_quarantine_pop_oldest(void) {
    uint32_t idx = kasan_quarantine_head;
    uint32_t ptr = kasan_quarantine[idx].ptr;
    kasan_heap_t *heap = kasan_heap_of_ptr(ptr);

    kasan_quarantine_bytes -= kasan_quarantine[idx].size;
    kasan_quarantine_head =
        (kasan_quarantine_head + 1u) % KASAN_QUARANTINE_MAX;
    kasan_quarantine_count--;
    if (heap && heap->backend->free) {
        heap->backend->free(heap->backend->ctx, (void *)(uintptr_t)ptr);
    }
}

/* Hand a freed block to the quarantine, releasing the oldest held blocks if
 * needed to stay under the byte / entry caps. */
static void kasan_quarantine_push(kasan_heap_t *heap, uint32_t ptr,
                                  uint32_t size) {
    uint32_t idx = 0;

    if (KASAN_QUARANTINE_BYTES == 0u) {
        heap->backend->free(heap->backend->ctx, (void *)(uintptr_t)ptr);
        return;
    }
    if (size >= KASAN_QUARANTINE_BYTES) {
        heap->backend->free(heap->backend->ctx, (void *)(uintptr_t)ptr);
        return;
    }
    while (kasan_quarantine_count > 0u &&
           kasan_quarantine_bytes + size > KASAN_QUARANTINE_BYTES) {
        kasan_quarantine_pop_oldest();
    }
    if (kasan_quarantine_count >= KASAN_QUARANTINE_MAX) {
        kasan_quarantine_pop_oldest();
    }
    idx = (kasan_quarantine_head + kasan_quarantine_count) %
          KASAN_QUARANTINE_MAX;
    kasan_quarantine[idx].ptr = ptr;
    kasan_quarantine[idx].size = size;
    kasan_quarantine_count++;
    kasan_quarantine_bytes += size;
}

void kasan_quarantine_drain(void) {
    while (kasan_quarantine_count > 0u) {
        kasan_quarantine_pop_oldest();
    }
}

void kasan_heap_init(void) {
    kasan_heap_t *heap = &kasan_heaps[0];
    uint32_t base = 0;
    uint32_t size = 0;

    kasan_live_reset();
    kasan_quarantine_reset();
    if (!heap->backend || !heap->backend->init) {
        return;
    }
    heap->backend->init(heap->backend->ctx, &base, &size);
    heap->arena_base = base;
    heap->arena_size = size;
    heap->in_use = (base != 0 && size != 0) ? 1u : 0u;
    if (heap->in_use) {
        kasan_poison_as(base, size, KASAN_POISON_REDZONE);
    }
}

uint32_t kasan_register_region(uint32_t base, uint32_t size,
                               uint32_t shadow_base) {
    uint32_t usable = 0;
    uint32_t shadow = 0;

    if (base == 0 || size == 0) {
        return 0;
    }
    kasan_region_layout(base, size, shadow_base, &usable, &shadow);
    if (!kasan_segment_add(base, usable, shadow)) {
        return 0;
    }
    kasan_shadow_clear_segment(shadow, usable / 8u);
    return usable;
}

kasan_heap_t *kasan_heap_register(const kasan_alloc_backend_t *backend,
                                  void *arena, uint32_t arena_size,
                                  uint32_t shadow_base) {
    uint32_t base = (uint32_t)(uintptr_t)arena;
    uint32_t usable = 0;
    uint32_t shadow = 0;
    uint32_t i = 0;
    kasan_heap_t *heap = 0;

    if (!backend || !backend->init_pool || !backend->malloc ||
        !backend->free) {
        return 0;
    }
    if (base == 0 || arena_size == 0) {
        return 0;
    }
    kasan_region_layout(base, arena_size, shadow_base, &usable, &shadow);
    for (i = 0; i < KASAN_MAX_HEAPS; i++) {
        if (!kasan_heaps[i].in_use) {
            heap = &kasan_heaps[i];
            break;
        }
    }
    if (heap == 0) {
        return 0;
    }
    /* Take a segment slot first; roll it back if the backend cannot use the
     * pool, so a failed registration leaves no dangling segment. */
    if (!kasan_segment_add(base, usable, shadow)) {
        return 0;
    }
    if (backend->init_pool(backend->ctx, arena, usable) != 0) {
        kasan_segment_count--;
        return 0;
    }
    heap->backend = backend;
    heap->arena_base = base;
    heap->arena_size = usable;
    heap->in_use = 1;
    kasan_shadow_clear_segment(shadow, usable / 8u);
    kasan_poison_as(base, usable, KASAN_POISON_REDZONE);
    return heap;
}

static uint32_t kasan_usable_of(const kasan_heap_t *heap, void *p,
                                uint32_t requested) {
    uint32_t size = heap->backend->usable
                        ? heap->backend->usable(heap->backend->ctx, p)
                        : requested;

    if (size == 0) {
        size = requested;
    }
    return size;
}

static void *kasan_malloc_impl(kasan_heap_t *heap, uint32_t nbytes) {
    void *p = 0;
    uint32_t size = 0;
    uint32_t alloc_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    if (!heap || !heap->in_use || !heap->backend->malloc) {
        return 0;
    }
    p = heap->backend->malloc(heap->backend->ctx, nbytes);
    if (p == 0) {
        return 0;
    }
    size = kasan_usable_of(heap, p, nbytes);
    kasan_live_add((uint32_t)(uintptr_t)p, size, alloc_pc);
    kasan_unpoison((uint32_t)(uintptr_t)p, size);
    return p;
}

void *kasan_malloc(uint32_t nbytes) {
    return kasan_malloc_impl(&kasan_heaps[0], nbytes);
}

void *kasan_heap_malloc(kasan_heap_t *heap, uint32_t nbytes) {
    return kasan_malloc_impl(heap, nbytes);
}

static void kasan_free_impl(kasan_heap_t *heap, void *p) {
    uint32_t size = 0;
    uint32_t state = 0;
    uint32_t free_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    if (p == 0) {
        return;
    }
    if (!heap || !heap->in_use || !heap->backend->free) {
        return;
    }
    if (!kasan_live_find((uint32_t)(uintptr_t)p, &size, &state)) {
        /* Not tracked: normally an invalid pointer (bad-free).  If the
         * record table overflowed earlier, detection is off; free
         * best-effort to avoid leaking. */
        if (kasan_live_overflow) {
            size = heap->backend->usable
                       ? heap->backend->usable(heap->backend->ctx, p)
                       : 0;
            if (size != 0) {
                kasan_poison_as((uint32_t)(uintptr_t)p, size,
                                KASAN_POISON_FREED);
            }
            heap->backend->free(heap->backend->ctx, p);
        } else {
            kasan_report(4, (uint32_t)(uintptr_t)p, 0, free_pc,
                         (uint32_t)(uintptr_t)p);
        }
        return;
    }
    if (state == KASAN_LIVE_STATE_FREED) {
        kasan_report(3, (uint32_t)(uintptr_t)p, 0, free_pc,
                     (uint32_t)(uintptr_t)p);
        return;
    }
    kasan_live_mark_freed((uint32_t)(uintptr_t)p, free_pc);
    kasan_poison_as((uint32_t)(uintptr_t)p, size, KASAN_POISON_FREED);
    kasan_quarantine_push(heap, (uint32_t)(uintptr_t)p, size);
}

void kasan_free(void *p) {
    kasan_heap_t *heap = kasan_heap_of_ptr((uint32_t)(uintptr_t)p);

    if (heap == 0) {
        heap = &kasan_heaps[0];
    }
    kasan_free_impl(heap, p);
}

void kasan_heap_free(kasan_heap_t *heap, void *p) {
    kasan_free_impl(heap, p);
}

static void kasan_memcpy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    uint32_t i = 0;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void kasan_zero(uint8_t *dst, uint32_t n) {
    uint32_t i = 0;
    for (i = 0; i < n; i++) {
        dst[i] = 0;
    }
}

/* ---- memcpy / memmove / memset interceptors ----------------------------
 * Link with -Wl,--wrap=memcpy,--wrap=memset,--wrap=memmove so out-of-line
 * (variable-size) memcpy/memset/memmove calls from instrumented code are
 * range-checked against the shadow map before the copy.  The copy itself is
 * a raw loop here (this file is not instrumented).  Sizes are uint32_t
 * (Cortex-M: size_t == uint32_t). */
void *__wrap_memcpy(void *dst, const void *src, uint32_t n) {
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    if (kasan_check(1, (uint32_t)(uintptr_t)src, n, pc)) {
        return dst;
    }
    if (kasan_check(2, (uint32_t)(uintptr_t)dst, n, pc)) {
        return dst;
    }
    kasan_memcpy((uint8_t *)dst, (const uint8_t *)src, n);
    return dst;
}

void *__wrap_memmove(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t i = 0;

    if (kasan_check(1, (uint32_t)(uintptr_t)src, n, pc)) {
        return dst;
    }
    if (kasan_check(2, (uint32_t)(uintptr_t)dst, n, pc)) {
        return dst;
    }
    if (d < s) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (i = n; i > 0; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

void *__wrap_memset(void *ptr, int value, uint32_t n) {
    uint8_t *d = (uint8_t *)ptr;
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    uint32_t i = 0;

    if (kasan_check(2, (uint32_t)(uintptr_t)ptr, n, pc)) {
        return ptr;
    }
    for (i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }
    return ptr;
}

static void *kasan_calloc_impl(kasan_heap_t *heap, uint32_t nmemb,
                               uint32_t size) {
    uint32_t total = 0;
    void *p = 0;
    uint32_t alloc_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    if (nmemb != 0 && size > 0xFFFFFFFFu / nmemb) {
        return 0;
    }
    total = nmemb * size;
    p = kasan_malloc_impl(heap, total);
    if (p != 0) {
        kasan_zero((uint8_t *)p, total);
        /* kasan_malloc recorded its own (internal) call site; point the
         * record at this wrapper's caller instead. */
        kasan_live_set_alloc_pc((uint32_t)(uintptr_t)p, alloc_pc);
    }
    return p;
}

void *kasan_calloc(uint32_t nmemb, uint32_t size) {
    return kasan_calloc_impl(&kasan_heaps[0], nmemb, size);
}

void *kasan_heap_calloc(kasan_heap_t *heap, uint32_t nmemb, uint32_t size) {
    return kasan_calloc_impl(heap, nmemb, size);
}

static void *kasan_memalign_impl(kasan_heap_t *heap, uint32_t align,
                                 uint32_t bytes) {
    void *p = 0;
    uint32_t size = 0;
    uint32_t alloc_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);

    if (!heap || !heap->in_use || !heap->backend->memalign) {
        return 0;
    }
    p = heap->backend->memalign(heap->backend->ctx, align, bytes);
    if (p == 0) {
        return 0;
    }
    size = kasan_usable_of(heap, p, bytes);
    kasan_live_add((uint32_t)(uintptr_t)p, size, alloc_pc);
    kasan_unpoison((uint32_t)(uintptr_t)p, size);
    return p;
}

void *kasan_memalign(uint32_t align, uint32_t bytes) {
    return kasan_memalign_impl(&kasan_heaps[0], align, bytes);
}

void *kasan_heap_memalign(kasan_heap_t *heap, uint32_t align, uint32_t bytes) {
    return kasan_memalign_impl(heap, align, bytes);
}

static void *kasan_realloc_impl(kasan_heap_t *heap, void *p, uint32_t size) {
    uint32_t old_size = 0;
    uint32_t state = 0;
    uint32_t new_usable = 0;
    uint32_t pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    void *newp = 0;

    if (p == 0) {
        return kasan_malloc_impl(heap, size);
    }
    if (size == 0) {
        kasan_free_impl(heap, p);
        return 0;
    }
    if (!heap || !heap->in_use || !heap->backend->malloc ||
        !heap->backend->free) {
        return 0;
    }
    if (!kasan_live_find((uint32_t)(uintptr_t)p, &old_size, &state)) {
        /* Record table overflowed: forward best-effort without the old size
         * (the old area cannot be re-poisoned, so UAF via it may slip). */
        if (kasan_live_overflow && heap->backend->realloc) {
            newp = heap->backend->realloc(heap->backend->ctx, p, size);
            if (newp != 0) {
                new_usable = kasan_usable_of(heap, newp, size);
                kasan_live_add((uint32_t)(uintptr_t)newp, new_usable, pc);
                kasan_unpoison((uint32_t)(uintptr_t)newp, new_usable);
            }
            return newp;
        }
        kasan_report(4, (uint32_t)(uintptr_t)p, 0, pc,
                     (uint32_t)(uintptr_t)p);
        return 0;
    }
    if (state == KASAN_LIVE_STATE_FREED) {
        kasan_report(3, (uint32_t)(uintptr_t)p, 0, pc,
                     (uint32_t)(uintptr_t)p);
        return 0;
    }
    /* state == LIVE: old_size is the current user area size. */
    if (!heap->backend->realloc) {
        /* Emulate: allocate a new block, copy the live bytes, free the old. */
        uint32_t copy = (old_size < size) ? old_size : size;

        newp = kasan_malloc_impl(heap, size);
        if (newp != 0) {
            kasan_memcpy((uint8_t *)newp, (const uint8_t *)p, copy);
            kasan_free_impl(heap, p);
            /* kasan_malloc recorded its own call site; point the record at
             * this wrapper's caller instead. */
            kasan_live_set_alloc_pc((uint32_t)(uintptr_t)newp, pc);
        }
        return newp;
    }
    newp = heap->backend->realloc(heap->backend->ctx, p, size);
    if (newp == 0) {
        return 0;
    }
    new_usable = kasan_usable_of(heap, newp, size);
    if (newp == p) {
        /* In place: re-poison a shrunk tail, then unpoison the new area. */
        if (new_usable < old_size) {
            kasan_poison_as((uint32_t)(uintptr_t)p + new_usable,
                            old_size - new_usable, KASAN_POISON_REDZONE);
        }
        kasan_unpoison((uint32_t)(uintptr_t)p, new_usable);
        kasan_live_update_size((uint32_t)(uintptr_t)p, new_usable);
    } else {
        /* Moved: re-poison the old block (UAF via the old pointer is caught)
         * and register the new one. */
        kasan_poison_as((uint32_t)(uintptr_t)p, old_size, KASAN_POISON_FREED);
        kasan_live_mark_freed((uint32_t)(uintptr_t)p, pc);
        kasan_live_add((uint32_t)(uintptr_t)newp, new_usable, pc);
        kasan_unpoison((uint32_t)(uintptr_t)newp, new_usable);
    }
    return newp;
}

void *kasan_realloc(void *p, uint32_t size) {
    return kasan_realloc_impl(&kasan_heaps[0], p, size);
}

void *kasan_heap_realloc(kasan_heap_t *heap, void *p, uint32_t size) {
    return kasan_realloc_impl(heap, p, size);
}
