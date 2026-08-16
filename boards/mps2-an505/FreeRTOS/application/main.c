#include <stdint.h>

#include "audio.h"
#include "mic.h"
#include "lcd.h"
#include "lan9118.h"
#include "touch.h"
#include "spi_flash.h"
#include "fatfs_test.h"
#include "mic_test.h"
#include "pj_test.h"
#include "pj_net_test.h"
#include "uart.h"
#include "printf.h"
#include "ARMCM33_DSP_FP.h"
#include "fault-dump.h"
#include "FreeRTOSConfig.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>

uint8_t ucHeap[configTOTAL_HEAP_SIZE];

void HardFault_Handler_Legency(void) {
    printf("%s\n", __func__);
}

void Default_Handler(void) {
    printf("%s\n", __func__);
}

void vApplicationIdleHook(void) {
    __WFI();
}

void dump_callstack(void) {
    unsigned int buffer[FD_STACK_DUMP_DEPTH_MAX] = {0};
    unsigned int point = fault_dump_bm_stack_point();
    unsigned int start = fault_dump_bm_stack_start();
    int count = fault_dump_callstack(buffer, FD_STACK_DUMP_DEPTH_MAX, (unsigned int*)point, (unsigned int*)start);
    if (count < 0) {
        printf("CallStack dump error: %d\r\n", count);
    } else {
        printf("CallStack:[ ");
        for (int i = 0; i < count; i++) {
            printf("%08X ", buffer[i]);
        }
        printf("] \r\n");
    }
}

void test0(void) {
    printf("this is %s.\r\n", __func__);
    dump_callstack();
    extern void fault_dump_unalign(void);
    fault_dump_unalign();
}

void test1(void) {
    printf("this is %s.\r\n", __func__);
    test0();
}

void test2(void) {
    printf("this is %s.\r\n", __func__);
    test1();
}

void test3(void) {
    printf("this is %s.\r\n", __func__);
    test2();
}

void test4(void) {
    printf("this is %s.\r\n", __func__);
    test3();
}

void test5(void) {
    printf("this is %s.\r\n", __func__);
    test4();
}

extern void lv_task_init(void);
#ifdef LWIP_USE_FREERTOS
extern void lwip_os_task_init(void);
#else
extern void lwip_task_init(void);
#endif
static void main_task_entry(void *parameters) {
    spi_flash_err_t rc;
    spi_flash_info_t fi = { 0 };

    lcd_init();
    touch_init();
    audio_init();
    audio_test();
    mic_init();
    mic_test();

    /* PJLIB (pjsip stack foundation) FreeRTOS port self-test. */
    pj_test_run();

    /* SPI NOR flash (w25q02jvm) - probe and report geometry */
    rc = spi_flash_init(NULL);
    if (rc == SPI_FLASH_OK) {
        spi_flash_get_info(&fi);
        printf("spi_flash: JEDEC %02X %02X %02X, size=%uMiB, page=%u, "
               "sector=%u, 4B-addr=%d\r\n",
               fi.jedec[0], fi.jedec[1], fi.jedec[2],
               (unsigned)(fi.size >> 20), (unsigned)fi.page_size,
               (unsigned)fi.sector_size, fi.four_byte_addr ? 1 : 0);
    } else {
        printf("spi_flash: init failed (%d)\r\n", (int)rc);
    }

    /* FatFS over the SPI NOR flash - mount/format, write/read a file. */
    fatfs_test();

    lv_task_init();
#ifdef LWIP_USE_FREERTOS
    lwip_os_task_init();
#else
    lwip_task_init();
#endif

    /* PJLIB socket/ioqueue over lwIP self-test (netif is up now). */
    pj_net_test_run();

    while(1) {
        vTaskDelay(1000);
    }
}

static void main_task_init(void) {
    static TaskHandle_t main_task = NULL;
    BaseType_t xReturn = pdPASS;
    xReturn = xTaskCreate(main_task_entry, "main_task", 4096, NULL, 1U, &main_task);
    if (xReturn == pdPASS) {
        vTaskStartScheduler();
    } else {
        printf("main task create failed(%d).\r\n", (int)(xReturn));
    }
}

int main(void) {
    int count = 0;
    uart_init();
    lan9118_open();
    printf("Start\r\n");
    fault_dump_init();
    extern int freertos_stack_parser(unsigned int *buffer, size_t length, unsigned int *stack_point, unsigned int *stack_start);
    fault_dump_psp_stack_parser(freertos_stack_parser);
    main_task_init();

    while (1) {
        __NOP();
        printf("hello world - %d.\r\n", count++);
    }
    return 0;
}
