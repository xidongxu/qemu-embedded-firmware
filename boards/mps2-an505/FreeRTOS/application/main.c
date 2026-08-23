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
#include "pj_sip_test.h"
#include "pj_sip_inv_test.h"
#include "pj_sip_dual_test.h"
#include "pj_media_full_test.h"
#include "pj_media_dsp_test.h"
#include "pj_rtp_test.h"
#include "pj_call_test.h"
#include "pj_phone.h"
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

    lcd_init();
    touch_init();
    audio_init();
    audio_irq_enable();
    mic_init();
#if defined(PJ_PHONE)
    /* PJSUA phone: the looping arpeggio test and the 1s blocking mic test
     * would fight the real call audio (mpsx_dev reconfigures both devices
     * to S16/8k/frame-size), so skip them.  The mpsx audiodev backend
     * enables the IRQs when the stream starts. */
#else
    audio_test();
    mic_test();
#endif

#if defined(PJ_PHONE)
    /* PJSUA 高层电话应用：必须在 lwIP 网络（tcpip_thread）就绪后再启动，
     * 见下方 lwip_os_task_init() 之后的 PJ_PHONE 分支。 */
#endif

    /* PJLIB (pjsip stack foundation) FreeRTOS port self-test. */
    pj_test_run();

#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE) || defined(PJ_PHONE)
    /* Dual-QEMU mode: SKIP the SPI-flash + FatFS self-test.  fatfs_test()
     * formats the file system and blocks for a long time; running it before
     * the dual media test would leave the peer waiting at the media
     * handshake while this instance is stuck formatting (2026-08-22). */
#else
    /* SPI NOR flash (w25q02jvm) - probe and report geometry */
    spi_flash_err_t rc;
    spi_flash_info_t fi = { 0 };
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
#endif

#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE) || defined(PJ_PHONE)
    /* Dual-QEMU call mode: SKIP the LVGL task + benchmark so the guest CPU
     * is free for the SIP/RTP media path.  Hypothesis (2026-08-20): the LVGL
     * benchmark saturates CPU, worsening QEMU TCG virtual-clock bursts and
     * amplifying RTP loss.  Controlled A/B: compare dual-call loss with
     * LVGL on (default build) vs off (dual build). */
#else
    lv_task_init();
#endif
#ifdef LWIP_USE_FREERTOS
    lwip_os_task_init();
#else
    lwip_task_init();
#endif

#if defined(PJ_PHONE)
    /* PJSUA 高层电话应用：lwIP/tcpip 已就绪，启动 pjsua 并拨号到宿主。
     * 稍等让 tcpip_init 完成，再启动 pjsua。 */
    vTaskDelay(1000);
    {
        /* High-prio watchdog to observe system state if pjsua stalls. */
        extern void phone_watchdog(void *arg);
        xTaskCreate(phone_watchdog, "wd", 2048, NULL, 5U, NULL);
    }
    pj_phone_start();
    while (1) {
        vTaskDelay(1000);
    }
#endif

    /* Dual-QEMU inter-instance call mode: run the pjmedia SIP call test.
     * (net_burst_test.c proved the base network is loss-free on 2026-08-22,
     * so any pjmedia loss is an integration/ioqueue issue, not the network.) */
#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE)
    pj_sip_dual_test_run();
    while (1) {
        vTaskDelay(1000);
    }
#endif

    /* PJLIB socket/ioqueue over lwIP self-test (netif is up now). */
    pj_net_test_run();

    /* PJSIP REGISTER loopback self-test (stage 3). */
    pj_sip_test_run();

    /* Full-pjmedia framework self-test (stage 14): endpoint / codec mgr /
     * G.711 encode-decode / event mgr / RTCP session. */
    pj_media_full_test_run();

    /* PJMEDIA DSP self-test (stage 16): AEC echo suppression + conference
     * multi-port mixing. */
    pj_media_dsp_test_run();

    /* PJSIP INVITE loopback self-test (stage 4/5, INVITE session + SDP). */
    pj_sip_inv_test_run();

    /* PJMEDIA RTP/PCMU media loopback self-test (stage 6). */
    pj_rtp_test_run();

    /* FULL CALL media test (stage 7): mic->PCMU->RTP->decode->speaker. */
    pj_call_test_run();

    while(1) {
        vTaskDelay(1000);
    }
}

static void main_task_init(void) {
    static TaskHandle_t main_task = NULL;
    BaseType_t xReturn = pdPASS;
    xReturn = xTaskCreate(main_task_entry, "main_task", 8192, NULL, 1U, &main_task);
    if (xReturn == pdPASS) {
        vTaskStartScheduler();
    } else {
        printf("main task create failed(%d).\r\n", (int)(xReturn));
    }
}

/* FreeRTOS failure hooks: surface stack overflow / heap exhaustion instead of
 * silently corrupting memory (pjsua deep-call-chain debugging aid). */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("FATAL: stack overflow in task %s\r\n",
           pcTaskName ? pcTaskName : "?");
    while (1) { }
}

void vApplicationMallocFailedHook(void)
{
    printf("FATAL: FreeRTOS heap exhausted\r\n");
    while (1) { }
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
