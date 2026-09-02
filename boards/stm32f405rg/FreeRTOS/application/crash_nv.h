/*
 * crash_nv.h - stm32f405rg crash-store board glue (internal flash).
 *
 * Board layer of the crash log for the stm32f405rg FreeRTOS project: it
 * implements the weak media primitives of tracer_crash_store over the
 * RESERVED last 256 KiB of the internal flash (sectors 10 & 11 at
 * 0x080C0000), and provides crash_nv_boot_report() which prints a stored
 * crash record at boot and clears it (no filesystem archive on this board).
 *
 * The slot strategy (double slot, header + CRC, anti-partial-write, read /
 * clear) lives in the media-agnostic libutils/tracer/tracer_crash_store.c.
 *
 * NOTE (verification): QEMU "netduinoplus2" does NOT model internal-flash
 * programming (HAL erase/program are accepted but data is not persisted),
 * so the flash primitives below must be validated on real silicon.
 */
#ifndef CRASH_NV_H
#define CRASH_NV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Boot-time handling of a stored crash record: read it back (via
 * tracer_crash_store_read_latest), print it, and clear the staging record
 * (no duplicate report next boot).  Silent when nothing is stored.  Call
 * once early at boot, after the debug UART is up. */
void crash_nv_boot_report(void);

#ifdef __cplusplus
}
#endif

#endif /* CRASH_NV_H */
