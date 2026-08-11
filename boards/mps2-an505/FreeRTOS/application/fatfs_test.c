/*
 * fatfs_test.c - FatFS (FAT) on the SPI NOR flash smoke test (mps2-an505/QEMU).
 *
 * Exercises the whole stack: FatFs core -> disk port (ports/spi_flash)
 * -> spi_flash driver -> w25q02jvm.
 *   - mounts drive 0, formats the volume if it has no FAT filesystem yet
 *   - creates a directory and a file, writes and reads it back
 *   - lists the directory and reports volume usage
 *
 * The disk port handles NOR erase-before-program (it erases a 4 KiB flash
 * sector and rewrites the block when a 512-byte sector is not blank), so
 * FatFs writes are safe on the flash.  The first format explicitly erases
 * the whole chip for a clean slate.
 */
#include "fatfs_test.h"

#include <string.h>

#include "ff.h"
#include "diskio_spi_flash.h"
#include "printf.h"

static FATFS s_fatfs;

#define TEST_DIR     "SPIFS"
#define TEST_FILE    "SPIFS/hello.txt"

static const char TEST_TEXT[] =
    "Hello FatFS over SPI flash! (mps2-an505 / QEMU)\r\n"
    "0123456789abcdef0123456789abcdef\r\n";

/* Human-readable name for a FRESULT code. */
static const char *fres_str(FRESULT fres)
{
    static const char *const names[] = {
        "FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE",
        "FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST",
        "FR_INVALID_OBJECT", "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE",
        "FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
        "FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
        "FR_INVALID_PARAMETER"
    };
    return (fres < 20) ? names[fres] : "FR_?";
}

/* Print the result of a FatFs call; returns 0 when it succeeded. */
static int report(const char *op, FRESULT fres)
{
    if (fres != FR_OK) {
        printf("fatfs_test: %-12s FAILED, %s (%d)\r\n", op, fres_str(fres), (int)fres);
        return -1;
    }
    printf("fatfs_test: %-12s OK\r\n", op);
    return 0;
}

/* List a directory ("" = root). */
static void list_dir(const char *path)
{
    DIR dir;
    FILINFO fno;
    FRESULT fres = f_opendir(&dir, path);
    if (fres != FR_OK) {
        printf("fatfs_test:   (cannot open '%s': %s)\r\n", path, fres_str(fres));
        return;
    }
    for (;;) {
        fres = f_readdir(&dir, &fno);
        if (fres != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        printf("fatfs_test:   [%c] %-12s %8u\r\n",
               (fno.fattrib & AM_DIR) ? 'D' : 'F',
               fno.fname, (unsigned)fno.fsize);
    }
    f_closedir(&dir);
}

int fatfs_test(void)
{
    FRESULT fres;
    DIR dir;
    FILINFO fno;
    FIL file;
    FATFS *pfs;
    DWORD free_clst;
    UINT n, len = (UINT)strlen(TEST_TEXT);
    BYTE work[FF_MAX_SS];
    char buf[sizeof(TEST_TEXT)];

    printf("\r\n===== FatFS over SPI flash test =====\r\n");

    /* 1) Mount drive 0 (immediate).  Format when there is no FAT yet. */
    fres = f_mount(&s_fatfs, "", 1);
    if (fres == FR_NO_FILESYSTEM) {
        printf("fatfs_test: no FAT found, formatting (chip erase first)...\r\n");
        if (spi_flash_erase_chip() != SPI_FLASH_OK) {
            printf("fatfs_test: spi_flash_erase_chip FAILED\r\n");
            return -1;
        }
        MKFS_PARM mopt = { 0 };
        mopt.fmt = FM_FAT;      /* auto-select FAT12/16/32 */
        if (report("f_mkfs", f_mkfs("", &mopt, work, sizeof(work))) != 0) {
            return -1;
        }
        fres = f_mount(&s_fatfs, "", 1);
    }
    if (report("f_mount", fres) != 0) {
        return -1;
    }
    printf("fatfs_test: drive 0 mounted\r\n");

    /* 2) Volume geometry / free space (sector = 512 B, report in KB). */
    fres = f_getfree("", &free_clst, &pfs);
    if (fres == FR_OK) {
        DWORD total_kb = ((pfs->n_fatent - 2) * pfs->csize) >> 1;
        DWORD free_kb  = (free_clst * pfs->csize) >> 1;
        printf("fatfs_test: volume %u KB, free %u KB, cluster %u sectors\r\n",
               (unsigned)total_kb, (unsigned)free_kb, (unsigned)pfs->csize);
    }

    /* 3) Create a directory and write a file. */
    fres = f_mkdir(TEST_DIR);
    if (fres == FR_EXIST) {
        printf("fatfs_test: f_mkdir      OK (already exists)\r\n");
    } else if (report("f_mkdir", fres) != 0) {
        return -1;
    }
    if (report("f_open(w)", f_open(&file, TEST_FILE, FA_CREATE_ALWAYS | FA_WRITE)) != 0) {
        return -1;
    }
    fres = f_write(&file, TEST_TEXT, len, &n);
    printf("fatfs_test: f_write      %u/%u bytes\r\n", (unsigned)n, (unsigned)len);
    if (report("f_close", f_close(&file)) != 0) {
        return -1;
    }
    if (fres != FR_OK || n != len) {
        printf("fatfs_test: write MISMATCH\r\n");
        return -1;
    }

    /* 4) Read the file back and compare. */
    if (report("f_open(r)", f_open(&file, TEST_FILE, FA_READ)) != 0) {
        return -1;
    }
    fres = f_read(&file, buf, sizeof(buf), &n);
    if (report("f_close", f_close(&file)) != 0) {
        return -1;
    }
    if (fres == FR_OK && n == len && memcmp(buf, TEST_TEXT, n) == 0) {
        buf[n] = '\0';
        printf("fatfs_test: readback OK, %u bytes: \"%s\"\r\n", (unsigned)n, buf);
    } else {
        printf("fatfs_test: readback MISMATCH: %s (read %u/%u)\r\n",
               fres_str(fres), (unsigned)n, (unsigned)len);
        return -1;
    }

    /* 5) List the directory. */
    if (report("f_opendir", f_opendir(&dir, TEST_DIR)) != 0) {
        return -1;
    }
    printf("fatfs_test: contents of '%s':\r\n", TEST_DIR);
    for (;;) {
        fres = f_readdir(&dir, &fno);
        if (fres != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        printf("fatfs_test:   [%c] %-12s %8u\r\n",
               (fno.fattrib & AM_DIR) ? 'D' : 'F',
               fno.fname, (unsigned)fno.fsize);
    }
    f_closedir(&dir);

    /* 6) Detach the volume. */
    f_unmount("");

    printf("fatfs_test: PASSED\r\n");
    return 0;
}
