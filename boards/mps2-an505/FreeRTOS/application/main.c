#include <stdint.h>

#include "lcd.h"
#include "touch.h"
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
static void main_task_entry(void *parameters) {
    lcd_init();
    touch_init();
    lv_task_init();
    while(1) {
        vTaskDelay(1000);
    }
}

static void main_task_init(void) {
    static TaskHandle_t main_task = NULL;
    BaseType_t xReturn = pdPASS;
    xReturn = xTaskCreate(main_task_entry, "main_task", 2048, NULL, 1U, &main_task);
    if (xReturn == pdPASS) {
        vTaskStartScheduler();
    } else {
        printf("main task create failed(%d).\r\n", (int)(xReturn));
    }
}

int main(void) {
    int count = 0;
    uart_init();

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
