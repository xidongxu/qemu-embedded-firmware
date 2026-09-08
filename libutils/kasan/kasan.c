/* kasan.c -- minimal KASan runtime and heap allocator for Cortex-M.
 *
 * Compiled WITHOUT -fsanitize=kernel-address: this file must touch shadow RAM
 * and the (mostly poisoned) heap arena directly.  Instrumented app code calls
 * the __asan_*_noabort hooks below on every memory access.
 *
 * Shadow semantics (ASan): a shadow byte of 0 means the 8-byte granule is
 * accessible, non-zero means poisoned.  Only live-allocation user areas are
 * unpoisoned, so any access to a header, free block or unused byte is caught
 * on the spot.
 */
#include "kasan.h"

volatile uint32_t kasan_reports = 0;
volatile uint32_t kasan_report_type = 0;
volatile uint32_t kasan_report_addr = 0;
volatile uint32_t kasan_report_size = 0;
volatile uint32_t kasan_report_shadow = 0;
volatile uint32_t kasan_report_pc = 0;
volatile uint32_t kasan_leak_count = 0;
volatile uint32_t kasan_leak_bytes = 0;
volatile uint32_t kasan_leak_ptr = 0;
volatile uint32_t kasan_leak_size = 0;
volatile uint32_t kasan_leak_pc = 0;
volatile uint32_t kasan_leak_lost = 0;

static uint8_t *kasan_shadow_of(uint32_t addr) {
    uint8_t *shadow = 0;

    if (addr < KASAN_REGION_BASE ||
        addr >= KASAN_REGION_BASE + KASAN_USABLE_SIZE) {
        return 0;
    }
    shadow = (uint8_t *)(uintptr_t)(KASAN_SHADOW_BASE +
                                    ((addr - KASAN_REGION_BASE) >> 3));
    return shadow;
}

void kasan_init(void) {
    volatile uint8_t *shadow = (volatile uint8_t *)(uintptr_t)KASAN_SHADOW_BASE;
    uint32_t index = 0;

    for (index = 0; index < KASAN_SHADOW_SIZE; index++) {
        shadow[index] = 0;
    }
}

static void kasan_report(uint32_t type, uint32_t addr, uint32_t size) {
    uint8_t *shadow = kasan_shadow_of(addr);

    kasan_reports++;
    kasan_report_type = type;
    kasan_report_addr = addr;
    kasan_report_size = size;
    kasan_report_shadow = shadow ? *shadow : 0;
    kasan_report_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
#ifndef KASAN_TEST_RETURNS
    for (;;) {
    }
#else
    (void)0;
#endif
}

void kasan_poison(uint32_t addr, uint32_t len) {
    uint32_t scan = addr;

    for (scan = addr; scan < addr + len; scan += 8) {
        uint8_t *shadow = kasan_shadow_of(scan);
        if (shadow) {
            *shadow = 0xff;
        }
    }
}

void kasan_unpoison(uint32_t addr, uint32_t len) {
    uint32_t scan = addr;

    for (scan = addr; scan < addr + len; scan += 8) {
        uint8_t *shadow = kasan_shadow_of(scan);
        if (shadow) {
            *shadow = 0;
        }
    }
}

static void kasan_check(uint32_t type, uint32_t addr, uint32_t size) {
    uint32_t end_addr = addr + size;
    uint32_t scan = addr;

    for (scan = addr; scan < end_addr; scan = (scan & ~7u) + 8u) {
        uint8_t *shadow = kasan_shadow_of(scan);
        if (shadow && *shadow != 0) {
            kasan_report(type, addr, size);
            return;
        }
    }
}

/* The noabort hooks are the kernel-address entry points: instrumented code
 * calls them before every memory access. */
#define KASAN_DEFINE_HOOK(name, type, size)                  \
    void __asan_##name##size##_noabort(uint32_t addr) {      \
        kasan_check(type, addr, size);                       \
    }

KASAN_DEFINE_HOOK(load, 1, 1)
KASAN_DEFINE_HOOK(load, 1, 2)
KASAN_DEFINE_HOOK(load, 1, 4)
KASAN_DEFINE_HOOK(load, 1, 8)
KASAN_DEFINE_HOOK(load, 1, 16)
KASAN_DEFINE_HOOK(store, 2, 1)
KASAN_DEFINE_HOOK(store, 2, 2)
KASAN_DEFINE_HOOK(store, 2, 4)
KASAN_DEFINE_HOOK(store, 2, 8)
KASAN_DEFINE_HOOK(store, 2, 16)

#define KASAN_HDR_MAGIC_ALLOC 0xAB5A11C0u
#define KASAN_HDR_MAGIC_FREE 0xF2EEA100u
#define KASAN_MIN_BLOCK 16u

typedef struct {
    uint32_t magic;
    uint32_t size;
} kasan_hdr_t;

/* Leak record table: every live allocation is logged (user pointer, size and
 * the caller's return address) when kasan_malloc() hands it out, and the
 * entry is dropped by kasan_free().  A fixed-size debug aid; when it fills
 * up new allocations are still served but not logged (kasan_leak_lost). */
typedef struct {
    uint32_t ptr;
    uint32_t size;
    uint32_t callsite;
} kasan_leak_t;

static kasan_leak_t kasan_leak_recs[KASAN_LEAK_MAX];

static void kasan_leak_clear(void) {
    uint32_t index = 0;

    for (index = 0; index < KASAN_LEAK_MAX; index++) {
        kasan_leak_recs[index].ptr = 0;
        kasan_leak_recs[index].size = 0;
        kasan_leak_recs[index].callsite = 0;
    }
    kasan_leak_count = 0;
    kasan_leak_bytes = 0;
    kasan_leak_ptr = 0;
    kasan_leak_size = 0;
    kasan_leak_pc = 0;
}

static void kasan_leak_add(uint32_t ptr, uint32_t size, uint32_t callsite) {
    uint32_t index = 0;

    for (index = 0; index < KASAN_LEAK_MAX; index++) {
        if (kasan_leak_recs[index].ptr == 0) {
            kasan_leak_recs[index].ptr = ptr;
            kasan_leak_recs[index].size = size;
            kasan_leak_recs[index].callsite = callsite;
            return;
        }
    }
    kasan_leak_lost++;
}

static void kasan_leak_remove(uint32_t ptr) {
    uint32_t index = 0;

    for (index = 0; index < KASAN_LEAK_MAX; index++) {
        if (kasan_leak_recs[index].ptr == ptr) {
            kasan_leak_recs[index].ptr = 0;
            kasan_leak_recs[index].size = 0;
            kasan_leak_recs[index].callsite = 0;
            return;
        }
    }
}

/* Arena backing store.  Default is a static array inside the instrumented
 * region; host tests point KASAN_ARENA_EXT at their own RAM.  It is prepared
 * at kasan_heap_init() time (the external base is not a C constant
 * expression).  Only ever touched by this (non-instrumented) file, so the
 * poisoned-header trick is safe: instrumented code hitting a header, free or
 * unused byte traps. */
#ifdef KASAN_ARENA_EXT
#define KASAN_ARENA_PREP()                                      \
    do {                                                        \
        kasan_arena = (uint8_t *)(uintptr_t)KASAN_ARENA_EXT;    \
        kasan_arena_size = KASAN_ARENA_SIZE;                    \
    } while (0)
#else
static uint8_t kasan_arena_storage[KASAN_HEAP_SIZE] __attribute__((aligned(8)));
#define KASAN_ARENA_PREP()                                      \
    do {                                                        \
        kasan_arena = kasan_arena_storage;                      \
        kasan_arena_size = sizeof(kasan_arena_storage);         \
    } while (0)
#endif

static uint8_t *kasan_arena = 0;
static uint32_t kasan_arena_size = 0;
static kasan_hdr_t *kasan_free_head = 0;

void kasan_heap_init(void) {
    kasan_hdr_t *header = 0;

    KASAN_ARENA_PREP();
    kasan_leak_clear();
    header = (kasan_hdr_t *)(void *)kasan_arena;
    kasan_poison((uint32_t)(uintptr_t)kasan_arena, kasan_arena_size);
    header->magic = KASAN_HDR_MAGIC_FREE;
    header->size = kasan_arena_size - sizeof(kasan_hdr_t);
    *(uint32_t *)(void *)((uint8_t *)header + sizeof(kasan_hdr_t)) = 0;
    kasan_free_head = header;
}

static kasan_hdr_t *kasan_free_next(kasan_hdr_t *header) {
    return (kasan_hdr_t *)(uintptr_t)
        *(uint32_t *)(void *)((uint8_t *)header + sizeof(kasan_hdr_t));
}

static void kasan_free_set_next(kasan_hdr_t *header, kasan_hdr_t *next) {
    *(uint32_t *)(void *)((uint8_t *)header + sizeof(kasan_hdr_t)) =
        (uint32_t)(uintptr_t)next;
}

void *kasan_malloc(uint32_t nbytes) {
    uint32_t user_size = (nbytes + 7u) & ~7u;
    uint32_t caller_pc = 0;
    kasan_hdr_t *current = kasan_free_head;
    kasan_hdr_t *previous = 0;

    caller_pc = (uint32_t)(uintptr_t)__builtin_return_address(0);
    if (user_size < 8u) {
        user_size = 8u;
    }
    while (current) {
        kasan_hdr_t *next = kasan_free_next(current);
        uint32_t usable_size = current->size;

        if (usable_size >= sizeof(kasan_hdr_t) + user_size) {
            kasan_hdr_t *remainder = (kasan_hdr_t *)(void *)((uint8_t *)current +
                                                             sizeof(kasan_hdr_t) +
                                                             user_size);
            uint32_t remainder_size = usable_size - sizeof(kasan_hdr_t) - user_size;

            if (remainder_size >= KASAN_MIN_BLOCK) {
                remainder->magic = KASAN_HDR_MAGIC_FREE;
                remainder->size = remainder_size;
                kasan_free_set_next(remainder, next);
                if (previous) {
                    kasan_free_set_next(previous, remainder);
                } else {
                    kasan_free_head = remainder;
                }
            } else {
                user_size = usable_size - sizeof(kasan_hdr_t);
                if (previous) {
                    kasan_free_set_next(previous, next);
                } else {
                    kasan_free_head = next;
                }
            }
            current->magic = KASAN_HDR_MAGIC_ALLOC;
            current->size = user_size;
            kasan_unpoison((uint32_t)(uintptr_t)current + sizeof(kasan_hdr_t),
                           user_size);
            kasan_leak_add((uint32_t)(uintptr_t)((uint8_t *)current +
                                                 sizeof(kasan_hdr_t)),
                           user_size, caller_pc);
            return (uint8_t *)current + sizeof(kasan_hdr_t);
        }
        if (usable_size >= user_size) {
            if (previous) {
                kasan_free_set_next(previous, next);
            } else {
                kasan_free_head = next;
            }
            current->magic = KASAN_HDR_MAGIC_ALLOC;
            current->size = usable_size;
            kasan_unpoison((uint32_t)(uintptr_t)current + sizeof(kasan_hdr_t),
                           usable_size);
            kasan_leak_add((uint32_t)(uintptr_t)((uint8_t *)current +
                                                 sizeof(kasan_hdr_t)),
                           usable_size, caller_pc);
            return (uint8_t *)current + sizeof(kasan_hdr_t);
        }
        previous = current;
        current = next;
    }
    return 0;
}

void kasan_free(void *p) {
    kasan_hdr_t *header = 0;
    uint32_t total_size = 0;

    header = (kasan_hdr_t *)(void *)((uint8_t *)p - sizeof(kasan_hdr_t));
    if (header->magic == KASAN_HDR_MAGIC_FREE) {
        kasan_report(3, (uint32_t)(uintptr_t)p, 0);
        return;
    }
    if (header->magic != KASAN_HDR_MAGIC_ALLOC) {
        kasan_report(4, (uint32_t)(uintptr_t)p, 0);
        return;
    }
    total_size = sizeof(kasan_hdr_t) + header->size;
    header->magic = KASAN_HDR_MAGIC_FREE;
    kasan_leak_remove((uint32_t)(uintptr_t)p);
    kasan_free_set_next(header, kasan_free_head);
    kasan_free_head = header;
    kasan_poison((uint32_t)(uintptr_t)header, total_size);
}

void kasan_leak_dump(void) {
    uint32_t index = 0;
    uint32_t first = 0;

    kasan_leak_count = 0;
    kasan_leak_bytes = 0;
    kasan_leak_ptr = 0;
    kasan_leak_size = 0;
    kasan_leak_pc = 0;
    for (index = 0; index < KASAN_LEAK_MAX; index++) {
        kasan_leak_t *record = &kasan_leak_recs[index];

        if (record->ptr == 0) {
            continue;
        }
        if (first == 0) {
            kasan_leak_ptr = record->ptr;
            kasan_leak_size = record->size;
            kasan_leak_pc = record->callsite;
            first = 1;
        }
        kasan_leak_count++;
        kasan_leak_bytes += record->size;
    }
}

uint32_t kasan_leak_live(uint32_t index, uint32_t *size, uint32_t *pc) {
    uint32_t scan = 0;
    uint32_t live = 0;

    for (scan = 0; scan < KASAN_LEAK_MAX; scan++) {
        kasan_leak_t *record = &kasan_leak_recs[scan];

        if (record->ptr == 0) {
            continue;
        }
        if (live == index) {
            if (size) {
                *size = record->size;
            }
            if (pc) {
                *pc = record->callsite;
            }
            return record->ptr;
        }
        live++;
    }
    return 0;
}
