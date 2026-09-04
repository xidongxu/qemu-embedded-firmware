/*
 * os_rwmutex_freertos.c - Read/Write mutex emulation for the FreeRTOS port.
 *
 * PJLIB's os_rwmutex.c (emulation on top of pj_mutex + pj_sem) is normally
 * #included by os_core_*.c when PJ_EMULATE_RWMUTEX is set.  This wrapper
 * provides the pjlib headers it needs and compiles it as a standalone TU.
 */
#include <pj/os.h>
#include <pj/pool.h>
#include <pj/assert.h>
#include <pj/errno.h>

#include "os_rwmutex.c"
