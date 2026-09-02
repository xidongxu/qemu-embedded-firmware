/*
 * crash_nv.h - non-volatile crash-record staging for tracer crash-log
 * (mps2-an505 FreeRTOS).
 *
 * Phase-2 backend: when a fault / assert dump finishes, tracer calls the
 * weak tracer_crash_save(); this file overrides it and writes the record
 * (dump text + pre-crash ring + CRC footer) into a reserved area at the TOP
 * of the external SPI NOR (w25q02jvm).  Two 4 KiB slots are alternated so a
 * power loss in the middle of a write never destroys the previous record.
 *
 * At boot, crash_nv_boot_report() reads back the most recent valid record
 * and prints it (phase 3 will archive it into a file / mark it consumed).
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

/* Read the most recent valid crash record into @buf (up to @cap bytes).
 * Returns the number of bytes copied, or 0 if none is stored. */
uint32_t crash_nv_read_latest(uint8_t *buf, uint32_t cap);

/* Boot-time handling of a stored crash record: print it (and, in a later
 * phase, archive it to a file).  Silent when there is nothing stored.
 * Call once early at boot, before normal operation. */
void crash_nv_boot_report(void);

#ifdef __cplusplus
}
#endif

#endif /* CRASH_NV_H */
