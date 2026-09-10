/* kasan.c -- minimal KASan runtime for Cortex-M.
 *
 * Compiled WITHOUT -fsanitize=kernel-address: this file must touch shadow RAM
 * and the (mostly poisoned) heap arena directly.  Instrumented app code calls
 * the __asan_*_noabort hooks below on every memory access.  The allocator is
 * delegated to a pluggable backend (see kasan_alloc_backend_t); this file only
 * keeps the shadow map and the live-allocation record table.
 *
 * Shadow semantics (ASan): one shadow byte per 8-byte granule.  0x00 = fully
 * addressable, 0x01-0x07 = first N bytes addressable, 0xf1-0xf7 = first N
 * bytes poisoned, 0xFA = freed (use-after-free), 0xFB = redzone (header /
 * free block / unused arena), 0xff = generic poisoned.  Only live-allocation
 * user areas are unpoisoned, so any access to a header, free block or unused
 * byte is caught on the spot.
 */
#include "kasan.h"

volatile uint32_t kasan_reports = 0;
volatile uint32_t kasan_report_type = 0;
volatile uint32_t kasan_report_addr = 0;
volatile uint32_t kasan_report_size = 0;
volatile uint32_t kasan_report_shadow = 0;
volatile uint32_t kasan_report_pc = 0;
volatile uint32_t kasan_report_cause = 0;
volatile uint8_t kasan_report_shadow_dump[KASAN_SHADOW_DUMP];

/* Semantic poison values (cf. Linux ASan 0xFA freed / 0xFB redzone). */
#define KASAN_POISON_FREED     0xFAu
#define KASAN_POISON_REDZONE   0xFBu
#define KASAN_SHADOW_NO_SHADOW 0xEEu

/* Classify a shadow byte for the report: 0=addressable, 1=redzone,
 * 2=freed (UAF), 3=partial boundary, 4=generic poisoned. */
static uint32_t kasan_cause_of(uint8_t value) {
    if (value == 0) {
        return 0;
    }
    if (value < 8u || (value >= 0xf1u && value <= 0xf7u)) {
        return 3;
    }
    if (value == KASAN_POISON_FREED) {
        return 2;
    }
    if (value == KASAN_POISON_REDZONE) {
        return 1;
    }
    return 4;
}

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

const char *kasan_shadow_name(uint8_t value) {
    if (value == 0) {
        return "addressable";
    }
    if (value < 8u) {
        return "partial-addressable";
    }
    if (value >= 0xf1u && value <= 0xf7u) {
        return "partial-poisoned";
    }
    if (value == KASAN_POISON_FREED) {
        return "freed";
    }
    if (value == KASAN_POISON_REDZONE) {
        return "redzone";
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

static void kasan_report(uint32_t type, uint32_t addr, uint32_t size,
                         uint32_t pc) {
    uint8_t *shadow = kasan_shadow_of(addr);

    kasan_reports++;
    kasan_report_type = type;
    kasan_report_addr = addr;
    kasan_report_size = size;
    kasan_report_shadow = shadow ? *shadow : 0;
    kasan_report_pc = pc;
    kasan_report_cause = kasan_cause_of(shadow ? *shadow : 0);
    kasan_shadow_dump(addr, (uint8_t *)kasan_report_shadow_dump,
                      KASAN_SHADOW_DUMP);
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
     * 0xfN (first N poisoned, rest addressable). */
    cur = addr & ~7u;
    if ((addr & 7u) != 0) {
        uint8_t *shadow = kasan_shadow_of(cur);
        if (shadow) {
            *shadow = (uint8_t)(0xf0u | (addr & 7u));
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

static void kasan_check(uint32_t type, uint32_t addr, uint32_t size,
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
                kasan_report(type, addr, size, pc);
                return;
            }
        } else if (value >= 0xf1u && value <= 0xf7u) {
            /* 0xf1..0xf7: first (value & 7) bytes poisoned, rest addressable. */
            if (acc_start - base < (uint32_t)(value & 7u)) {
                kasan_report(type, addr, size, pc);
                return;
            }
        } else {
            /* 0x80..0xf0 / 0xf8..0xff: fully poisoned. */
            kasan_report(type, addr, size, pc);
            return;
        }
    }
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

typedef struct {
    uint32_t ptr;
    uint32_t size;
    uint32_t state;
} kasan_live_entry_t;

static kasan_live_entry_t kasan_live_table[KASAN_LIVE_MAX];
static const kasan_alloc_backend_t *kasan_backend = 0;

static void kasan_live_reset(void) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        kasan_live_table[i].ptr = 0;
        kasan_live_table[i].size = 0;
        kasan_live_table[i].state = KASAN_LIVE_STATE_EMPTY;
    }
    kasan_live_overflow = 0;
}

static void kasan_live_add(uint32_t ptr, uint32_t size) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_table[i].state != KASAN_LIVE_STATE_LIVE) {
            kasan_live_table[i].ptr = ptr;
            kasan_live_table[i].size = size;
            kasan_live_table[i].state = KASAN_LIVE_STATE_LIVE;
            return;
        }
    }
    kasan_live_overflow = 1;
}

static int kasan_live_find(uint32_t ptr, uint32_t *size, uint32_t *state) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_table[i].ptr == ptr &&
            kasan_live_table[i].state != KASAN_LIVE_STATE_EMPTY) {
            *size = kasan_live_table[i].size;
            *state = kasan_live_table[i].state;
            return 1;
        }
    }
    return 0;
}

static void kasan_live_mark_freed(uint32_t ptr) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_table[i].ptr == ptr &&
            kasan_live_table[i].state == KASAN_LIVE_STATE_LIVE) {
            kasan_live_table[i].state = KASAN_LIVE_STATE_FREED;
            return;
        }
    }
}

static void kasan_live_update_size(uint32_t ptr, uint32_t size) {
    uint32_t i = 0;
    for (i = 0; i < KASAN_LIVE_MAX; i++) {
        if (kasan_live_table[i].ptr == ptr &&
            kasan_live_table[i].state == KASAN_LIVE_STATE_LIVE) {
            kasan_live_table[i].size = size;
            return;
        }
    }
}

void kasan_set_alloc_backend(const kasan_alloc_backend_t *backend) {
    kasan_backend = backend;
}

void kasan_heap_init(void) {
    uint32_t base = 0;
    uint32_t size = 0;

    kasan_live_reset();
    if (!kasan_backend || !kasan_backend->init) {
        return;
    }
    kasan_backend->init(&base, &size);
    if (base != 0 && size != 0) {
        kasan_poison_as(base, size, KASAN_POISON_REDZONE);
    }
}

static uint32_t kasan_usable_of(void *p, uint32_t requested) {
    uint32_t size = kasan_backend->usable ? kasan_backend->usable(p) : requested;

    if (size == 0) {
        size = requested;
    }
    return size;
}

void *kasan_malloc(uint32_t nbytes) {
    void *p = 0;
    uint32_t size = 0;

    if (!kasan_backend || !kasan_backend->malloc) {
        return 0;
    }
    p = kasan_backend->malloc(nbytes);
    if (p == 0) {
        return 0;
    }
    size = kasan_usable_of(p, nbytes);
    kasan_live_add((uint32_t)(uintptr_t)p, size);
    kasan_unpoison((uint32_t)(uintptr_t)p, size);
    return p;
}

void kasan_free(void *p) {
    uint32_t size = 0;
    uint32_t state = 0;

    if (p == 0) {
        return;
    }
    if (!kasan_backend || !kasan_backend->free) {
        return;
    }
    if (!kasan_live_find((uint32_t)(uintptr_t)p, &size, &state)) {
        /* Not tracked: normally an invalid pointer (bad-free).  If the
         * record table overflowed earlier, detection is off; free
         * best-effort to avoid leaking. */
        if (kasan_live_overflow) {
            size = kasan_backend->usable ? kasan_backend->usable(p) : 0;
            if (size != 0) {
                kasan_poison_as((uint32_t)(uintptr_t)p, size,
                                KASAN_POISON_FREED);
            }
            kasan_backend->free(p);
        } else {
            kasan_report(4, (uint32_t)(uintptr_t)p, 0,
                         (uint32_t)(uintptr_t)__builtin_return_address(0));
        }
        return;
    }
    if (state == KASAN_LIVE_STATE_FREED) {
        kasan_report(3, (uint32_t)(uintptr_t)p, 0,
                     (uint32_t)(uintptr_t)__builtin_return_address(0));
        return;
    }
    kasan_live_mark_freed((uint32_t)(uintptr_t)p);
    kasan_poison_as((uint32_t)(uintptr_t)p, size, KASAN_POISON_FREED);
    kasan_backend->free(p);
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

void *kasan_calloc(uint32_t nmemb, uint32_t size) {
    uint32_t total = 0;
    void *p = 0;

    if (nmemb != 0 && size > 0xFFFFFFFFu / nmemb) {
        return 0;
    }
    total = nmemb * size;
    p = kasan_malloc(total);
    if (p != 0) {
        kasan_zero((uint8_t *)p, total);
    }
    return p;
}

void *kasan_memalign(uint32_t align, uint32_t bytes) {
    void *p = 0;
    uint32_t size = 0;

    if (!kasan_backend || !kasan_backend->memalign) {
        return 0;
    }
    p = kasan_backend->memalign(align, bytes);
    if (p == 0) {
        return 0;
    }
    size = kasan_usable_of(p, bytes);
    kasan_live_add((uint32_t)(uintptr_t)p, size);
    kasan_unpoison((uint32_t)(uintptr_t)p, size);
    return p;
}

void *kasan_realloc(void *p, uint32_t size) {
    uint32_t old_size = 0;
    uint32_t state = 0;
    uint32_t new_usable = 0;
    void *newp = 0;

    if (p == 0) {
        return kasan_malloc(size);
    }
    if (size == 0) {
        kasan_free(p);
        return 0;
    }
    if (!kasan_backend || !kasan_backend->malloc || !kasan_backend->free) {
        return 0;
    }
    if (!kasan_live_find((uint32_t)(uintptr_t)p, &old_size, &state)) {
        /* Record table overflowed: forward best-effort without the old size
         * (the old area cannot be re-poisoned, so UAF via it may slip). */
        if (kasan_live_overflow && kasan_backend->realloc) {
            newp = kasan_backend->realloc(p, size);
            if (newp != 0) {
                new_usable = kasan_usable_of(newp, size);
                kasan_live_add((uint32_t)(uintptr_t)newp, new_usable);
                kasan_unpoison((uint32_t)(uintptr_t)newp, new_usable);
            }
            return newp;
        }
        kasan_report(4, (uint32_t)(uintptr_t)p, 0,
                     (uint32_t)(uintptr_t)__builtin_return_address(0));
        return 0;
    }
    if (state == KASAN_LIVE_STATE_FREED) {
        kasan_report(3, (uint32_t)(uintptr_t)p, 0,
                     (uint32_t)(uintptr_t)__builtin_return_address(0));
        return 0;
    }
    /* state == LIVE: old_size is the current user area size. */
    if (!kasan_backend->realloc) {
        /* Emulate: allocate a new block, copy the live bytes, free the old. */
        uint32_t copy = (old_size < size) ? old_size : size;

        newp = kasan_malloc(size);
        if (newp != 0) {
            kasan_memcpy((uint8_t *)newp, (const uint8_t *)p, copy);
            kasan_free(p);
        }
        return newp;
    }
    newp = kasan_backend->realloc(p, size);
    if (newp == 0) {
        return 0;
    }
    new_usable = kasan_usable_of(newp, size);
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
        kasan_live_mark_freed((uint32_t)(uintptr_t)p);
        kasan_live_add((uint32_t)(uintptr_t)newp, new_usable);
        kasan_unpoison((uint32_t)(uintptr_t)newp, new_usable);
    }
    return newp;
}
