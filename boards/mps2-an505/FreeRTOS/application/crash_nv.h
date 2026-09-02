/*
 * crash_nv.h - mps2-an505 SPI NOR media + littlefs archive for the crash log
 * (FreeRTOS application).
 *
 * This is the BOARD layer of the crash log (see libutils/tracer
 * tracer_crash_store.h): it implements the weak media primitives over the
 * reserved TOP of the external SPI NOR (w25q02jvm) and archives a stored
 * crash record into littlefs files at boot.  The slot strategy (alternating
 * 2 x 4 KiB slots, header + CRC, anti-partial-write, read/clear) lives in
 * the media-agnostic tracer_crash_store.c.
 *
 * The reserved area does not overlap the SPI NOR FAT/LittleFS test volume
 * because the PJ_PHONE build never formats the device (main.c skips
 * fatfs_test()/spi_flash_init() there).
 */
#ifndef CRASH_NV_H
#define CRASH_NV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot-time handling of a stored crash record: read it back (via
 * tracer_crash_store_read_latest), print it, archive it into littlefs files
 * and clear the staging record (no duplicate report next boot).  Silent when
 * there is nothing stored.  Call once early at boot, before normal op. */
void crash_nv_boot_report(void);

#ifdef __cplusplus
}
#endif

#endif /* CRASH_NV_H */
