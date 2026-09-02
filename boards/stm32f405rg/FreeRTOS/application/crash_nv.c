/*
 * crash_nv.c - stm32f405rg crash-store board glue (internal flash).
 *
 * Media primitives for tracer_crash_store over the reserved last 256 KiB of
 * the internal flash (sectors 10 & 11 @ 0x080C0000, 2 x 128 KiB slots), plus
 * crash_nv_boot_report().  The linker script FLASH length is reduced to
 * 768 K so code never overlaps this area (see
 * startup/gcc/STM32F405RGTx_FLASH.ld).
 *
 * HARDWARE-NOTE: internal-flash erase/program cannot be validated under QEMU
 * ("netduinoplus2" models the flash registers but does not persist writes) --
 * the HAL calls below must be validated on real silicon.  The surrounding
 * strategy (double slot / CRC / anti-partial-write / read / clear) is fully
 * covered by host tests and the mps2 (SPI NOR) QEMU path.
 *
 * Flash programming from flash is allowed on STM32F4 as long as the code is
 * not in the sector being erased/programmed (it is far below 0x080C0000);
 * interrupts are masked around the operation for safety.
 */
#include <stdio.h>
#include <string.h>

#include "crash_nv.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"
#include "tracer_crash_store.h"

/* Reserved top-of-flash crash area: sector 10 (0x080C0000) + sector 11
 * (0x080E0000), each 128 KiB. */
#define CRASH_NV_BASE   0x080C0000u
#define CRASH_NV_SLOT   (128u * 1024u)
#define CRASH_NV_SLOTS  2u
/* Boot report buffer: a crash record is at most a few KiB of text, so a
 * small fixed buffer is enough (do NOT size it to the whole 128 KiB slot
 * -- that would not fit in RAM).  Records larger than this are skipped. */
#define CRASH_NV_REPORT_CAP 4096u

/* Map a slot start address to its STM32F405 sector number (-1 if unknown). */
static int crash_nv_sector_of(uint32_t addr) {
    if (addr == 0x080C0000u) {
        return FLASH_SECTOR_10;
    }
    if (addr == 0x080E0000u) {
        return FLASH_SECTOR_11;
    }
    return -1;
}

/* ===== tracer_crash_store media primitives (override the weak defaults) == */

int tracer_crash_store_get_media(tracer_crash_store_media_t *info) {
    if (info == NULL) {
        return -1;
    }
    info->slot_base = CRASH_NV_BASE;
    info->slot_size = CRASH_NV_SLOT;
    info->slot_count = CRASH_NV_SLOTS;
    return 0;
}

int tracer_crash_store_erase(uint32_t addr) {
    int sec = crash_nv_sector_of(addr);
    FLASH_EraseInitTypeDef e;
    uint32_t bad = 0u;
    HAL_StatusTypeDef st;
    if (sec < 0) {
        return -1;
    }
    memset(&e, 0, sizeof(e));
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Sector = (uint32_t)sec;
    e.NbSectors = 1u;
    e.VoltageRange = VOLTAGE_RANGE_3;
    HAL_FLASH_Unlock();
    __disable_irq();
    st = HAL_FLASHEx_Erase(&e, &bad);
    __enable_irq();
    HAL_FLASH_Lock();
    return (st == HAL_OK && bad == 0xFFFFFFFFu) ? 0 : -1;
}

int tracer_crash_store_write(uint32_t addr, const void *buf, uint32_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t i;
    if ((addr & 3u) != 0u) {
        return -1;
    }
    HAL_FLASH_Unlock();
    __disable_irq();
    for (i = 0u; i < len; i += 4u) {
        uint32_t w = 0xFFFFFFFFu;
        uint32_t n = len - i;
        uint32_t k;
        if (n > 4u) {
            n = 4u;
        }
        for (k = 0u; k < n; k++) {
            w |= ((uint32_t)p[i + k]) << (8u * k);
        }
        /* Pad to a full word with 0xFF (beyond 'len' is never CRC-checked). */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) {
            __enable_irq();
            HAL_FLASH_Lock();
            return -1;
        }
    }
    __enable_irq();
    HAL_FLASH_Lock();
    return 0;
}

int tracer_crash_store_read(uint32_t addr, void *buf, uint32_t len) {
    /* Internal flash is memory-mapped: a plain copy reads it back. */
    memcpy(buf, (const void *)addr, len);
    return 0;
}

/* ===== boot-side report ====================================================
 * No filesystem on this board: just print the stored record and clear it. */
void crash_nv_boot_report(void) {
    static uint8_t s_rec[CRASH_NV_REPORT_CAP];
    uint32_t len = tracer_crash_store_read_latest(s_rec,
                                                 (uint32_t)sizeof(s_rec));
    uint32_t i;

    if (len == 0u) {
        return;
    }
    printf("\r\n===== Crash record from last reset =====\r\n");
    for (i = 0u; i < len; i++) {
        putchar((char)s_rec[i]);
    }
    printf("\r\n===== End of stored crash record =====\r\n");
    tracer_crash_store_clear();
    printf("[crash] staging cleared\r\n");
}
