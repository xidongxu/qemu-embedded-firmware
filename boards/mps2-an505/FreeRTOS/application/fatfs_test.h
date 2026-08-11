/*
 * fatfs_test.h - FatFS on SPI NOR flash smoke test (mps2-an505 / QEMU).
 */
#ifndef FATFS_TEST_H
#define FATFS_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run the FatFS-over-SPI-flash test.  Returns 0 on success. */
int fatfs_test(void);

#ifdef __cplusplus
}
#endif

#endif /* FATFS_TEST_H */
