/*
 * config_site.h
 *
 * PJPROJECT site configuration for the FreeRTOS / Cortex-M port.
 * This file is included by pj/config.h. Put this directory first in the
 * include path so this copy shadows any other config_site.h.
 */
#ifndef PJ_CONFIG_SITE_H
#define PJ_CONFIG_SITE_H

/* ---- PJLIB core ------------------------------------------------------ */

/* Cortex-M has no FPU in some configurations and float printf pulls in a
 * lot of code; disable floating point support for the embedded port. */
#define PJ_HAS_FLOATING_POINT           0

/* Keep debug output reasonable on a serial console. */
#define PJ_LOG_MAX_LEVEL                4

/* Embedded: keep the per-log stack buffer small (4000-byte default would
 * eat into the 8 KB FreeRTOS task stack). */
#define PJ_LOG_MAX_SIZE                 512

#define PJ_DEBUG                        0

#define PJ_MAX_OBJ_NAME                 32

/* Default thread stack: 4 KiB (1 K words). Overridable at runtime. */
#define PJ_THREAD_DEFAULT_STACK_SIZE    4096

/* Default thread priority (FreeRTOS, higher = more important). */
#define PJ_FREERTOS_DEFAULT_PRIO        2

/* We implement thread stacks ourselves (FreeRTOS task stack). */
#define PJ_THREAD_ALLOCATE_STACK        0

/* Atomic ops are emulated with a mutex in os_core_freertos.c. */
#define PJ_ATOMIC_VALUE_TYPE            long

/* Pool alignment (must be power of two). */
#ifndef PJ_POOL_ALIGNMENT
#   define PJ_POOL_ALIGNMENT            8
#endif

/* PJLIB is initialized from a FreeRTOS task, no libc startup work needed. */
#define PJ_HAS_MALLOC                   1

/* Stack checking is not available on FreeRTOS tasks. */
#define PJ_OS_HAS_CHECK_STACK           0

/* Suppress the (intentional) unused-label warning from PJ_TODO(). */
#define PJ_TODO(x)

#endif  /* PJ_CONFIG_SITE_H */
