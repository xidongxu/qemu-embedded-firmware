#include <stdint.h>

#include "audio.h"
#include "mic.h"
#include "lcd.h"
#include "lan9118.h"
#include "touch.h"
#include "spi_flash.h"
#include "fatfs_test.h"
#include "crash_nv.h"
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
#include "tracer.h"
#include "FreeRTOSConfig.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>

uint8_t ucHeap[configTOTAL_HEAP_SIZE];

void HardFault_Handler_Legency(void) {
    TRACER_LOGI("%s", __func__);
}

void Default_Handler(void) {
    TRACER_LOGI("%s", __func__);
}

void vApplicationIdleHook(void) {
    __WFI();
}

/* tracer hook: print the faulting FreeRTOS task name before the dump.  Goes
 * through tracer_log() (not printf) so the line is also kept in the log
 * ring / sink and survives a crash (0 printf policy: everything must flow
 * through the tracer log pipeline to be persistable). */
void tracer_on_fault(const tracer_fault_t *f) {
    (void)f;
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (h != NULL) {
        TRACER_LOGI("  CurrentTask: %s", pcTaskGetName(h));
    }
}

/* tracer hook: the faulting task's stack top (Thread-mode faults scan the
 * task's PSP stack).  Returning 0 makes tracer fall back to the main-stack
 * top when no task is active. */
uint32_t tracer_stack_limit(void) {
    TaskStatus_t st = {0};
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    if (h == NULL) {
        return 0u;
    }
    vTaskGetInfo(h, &st, pdTRUE, eInvalid);
    return (uint32_t)st.pxEndOfStack;
}

/* tracer hook: list all FreeRTOS tasks (state / stack high-water mark).
 * V11 vTaskList() formats into a caller buffer (no internal print), so we log
 * it as one streamed record -- tracer_log() is printf-like with no line-length
 * limit, so the whole table flows through the log pipeline (0-printf policy:
 * task state is kept in the log ring / sink).  Stack-high-water is in
 * StackType_t words. */
void tracer_dump_tasks(void) {
    static char buf[1024];
    vTaskList(buf);
    TRACER_LOGI("\r\n %s \r\n", buf);
}

/* tracer hook: system up-time in ms, printed in every dump (crash-to-boot
 * matching).  Tick rate is usually 1 kHz here, so tick == ms. */
uint32_t tracer_uptime_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);
}

void dump_callstack(void) {
    tracer_dump_callstack();
}

void test0(void) {
    TRACER_LOGI("this is %s", __func__);
    dump_callstack();
    tracer_trigger_unalign();
}

void test1(void) {
    TRACER_LOGI("this is %s", __func__);
    test0();
}

void test2(void) {
    TRACER_LOGI("this is %s", __func__);
    test1();
}

void test3(void) {
    TRACER_LOGI("this is %s", __func__);
    test2();
}

void test4(void) {
    TRACER_LOGI("this is %s", __func__);
    test3();
}

void test5(void) {
    TRACER_LOGI("this is %s", __func__);
    test4();
}

extern void lv_task_init(void);
#ifdef LWIP_USE_FREERTOS
extern void lwip_os_task_init(void);
#else
extern void lwip_task_init(void);
#endif
static void main_task_entry(void *parameters) {
    /* Record key runtime events in the crash-log ring ("black box"): they are
     * replayed at the end of any fault/assert dump before persistence. */
    tracer_ring_printf("app: main_task_entry enter\r\n");
    /* Report / archive any crash record left by the previous reset. */
    crash_nv_boot_report();
    lcd_init();
    touch_init();
    audio_init();
    audio_irq_enable();
    mic_init();
#if defined(PJ_PHONE_OPUS_BENCH)
    /* Standalone Opus encode/decode benchmark: dedicated CPU, no pjsua /
     * lwIP / LVGL, so the raw libopus per-frame cost on this M33/TCG is
     * measured in isolation (see opus_bench.c). */
    {
        extern void opus_bench_start(void);
        opus_bench_start();
    }
    while (1) {
        vTaskDelay(1000);
    }
#endif
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
        TRACER_LOGI("spi_flash: JEDEC %02X %02X %02X, size=%uMiB, page=%u, "
                    "sector=%u, 4B-addr=%d",
                    fi.jedec[0], fi.jedec[1], fi.jedec[2],
                    (unsigned)(fi.size >> 20), (unsigned)fi.page_size,
                    (unsigned)fi.sector_size, fi.four_byte_addr ? 1 : 0);
    } else {
        TRACER_LOGI("spi_flash: init failed (%d)", (int)rc);
    }

    /* FatFS over the SPI NOR flash - mount/format, write/read a file. */
    fatfs_test();
#endif

#if defined(PJ_DUAL_ROLE_CALLER) || defined(PJ_DUAL_ROLE_CALLEE)
    /* Dual-QEMU call mode: SKIP the LVGL task so the guest CPU is free for
     * the SIP/RTP media path (the LVGL benchmark saturated CPU under TCG).
     * Normal and PJ_PHONE builds run the LVGL task (phone UI). */
#else
    lv_task_init();
#endif
#ifdef LWIP_USE_FREERTOS
    lwip_os_task_init();
#else
    lwip_task_init();
#endif

#if defined(PJ_PHONE)
    /* PJSUA 高层电话应用：lwIP/tcpip 已就绪，启动 pjsua（注册到 FreeSWITCH，
     * 不自动拨号 - 由 LVGL 电话 UI 通过触摸屏发起呼叫/接听/挂断）。
     * 稍等让 tcpip_init 完成，再启动 pjsua。 */
    vTaskDelay(1000);
    /* Boot-time NTP sync so mbedtls X509 certificate validity checks use the
     * real clock (host NTP server at 172.16.23.1:123; falls back to a fixed
     * epoch until the first SNTP response arrives). */
    {
        extern void sntp_sync_init(void);
        sntp_sync_init();
    }
    {
        /* High-prio watchdog to observe system state if pjsua stalls.
         * Priority 4 = highest legal (configMAX_PRIORITIES-1) so it can
         * preempt a runaway media thread and still print task states. */
        extern void phone_watchdog(void *arg);
        xTaskCreate(phone_watchdog, "wd", 2048, NULL, 4U, NULL);
    }
    pj_phone_init();
    /* UDP command server (hostfwd udp::15000-:15000): lets a host script
     * drive the phone (dial/hangup/status/stat) reliably over the slirp
     * network - this is the primary debug/automation channel. */
    {
        extern void phone_net_task(void *arg);
        xTaskCreate(phone_net_task, "netcmd", 2048, NULL, 5U, NULL);
    }
    /* UI-driven phone: keep this task suspended; the LVGL task drives the
     * phone, pjsua threads + watchdog handle the media/call state. */
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
        TRACER_LOGI("main task create failed(%d)", (int)(xReturn));
    }
}

/* FreeRTOS failure hooks: surface stack overflow / heap exhaustion instead of
 * silently corrupting memory (pjsua deep-call-chain debugging aid). */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    /* Route through the tracer: prints the message + current call stack and
     * then auto-resets (TRACER_AUTO_RESET_MS) or traps.  The overflowed task's
     * own stack is already blown, so the backtrace is best-effort. */
    tracer_assert_fail(pcTaskName ? pcTaskName : "stack overflow",
                       "vApplicationStackOverflowHook", __LINE__);
}

void vApplicationMallocFailedHook(void)
{
    TRACER_LOGI("FATAL: FreeRTOS heap exhausted");
    while (1) { }
}

int main(void) {
    int count = 0;
    uart_init();
    lan9118_open();
    TRACER_LOGI("Start");
    tracer_init();
    main_task_init();

    while (1) {
        __NOP();
        TRACER_LOGI("hello world - %d", count++);
    }
    return 0;
}
