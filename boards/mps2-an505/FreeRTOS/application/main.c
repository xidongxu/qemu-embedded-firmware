#include <stdint.h>

#include "uart.h"
#include "printf.h"
#include "ARMCM33_DSP_FP.h"
#include "fault-dump.h"
#include "FreeRTOSConfig.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>

volatile uint32_t *lcd = (volatile uint32_t *)0x51000000;
static uint32_t framebuffer[320 * 240] = { 0 };
uint8_t ucHeap[configTOTAL_HEAP_SIZE];

#define TOUCH_BASE   (0x51001000UL)
#define TOUCH_STATUS (*(volatile uint32_t *)(TOUCH_BASE + 0x00))
#define TOUCH_X      (*(volatile uint32_t *)(TOUCH_BASE + 0x04))
#define TOUCH_Y      (*(volatile uint32_t *)(TOUCH_BASE + 0x08))
#define TOUCH_CTRL   (*(volatile uint32_t *)(TOUCH_BASE + 0x0c))
#define TOUCH_ID     (*(volatile uint32_t *)(TOUCH_BASE + 0x10))
#define TOUCH_RES_X  (*(volatile uint32_t *)(TOUCH_BASE + 0x14))
#define TOUCH_RES_Y  (*(volatile uint32_t *)(TOUCH_BASE + 0x18))

#define TOUCH_STATUS_PRESSED (1 << 0)
#define TOUCH_STATUS_READY   (1 << 1)
#define TOUCH_CTRL_CLEAR_INT (1 << 0)

static void touch_clear_irq(void) {
    TOUCH_CTRL = TOUCH_CTRL_CLEAR_INT;
}

void Interrupt32_Handler(void) {
    uint32_t x;
    uint32_t y;
    uint32_t status;
    status = TOUCH_STATUS;
    printf("status=0x%08lx\n", status);
    if (status & TOUCH_STATUS_READY) {
        x = TOUCH_X;
        y = TOUCH_Y;
        printf("touch x=%lu y=%lu\n", x, y);
    }
    touch_clear_irq();
}

void test_touch(void) {
    NVIC_EnableIRQ(32);
    TOUCH_CTRL = 1;
    printf("TOUCH_ID = %08lx\n", TOUCH_ID);
    printf("TOUCH_RES = %lux%lu\n", TOUCH_RES_X, TOUCH_RES_Y);
    printf("TOUCH_STATUS = %08lx\n", TOUCH_STATUS);
}

void HardFault_Handler_Legency(void) {
    printf("%s\n", __func__);
}

void Default_Handler(void) {
    printf("%s\n", __func__);
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

#define ARGB8888(a,r,g,b) \
    (((uint32_t)(a) << 24) | \
     ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) << 8)  | \
     ((uint32_t)(b)))

void test_lcd(void) { 
    for (int y = 0; y < 240; y++) { 
        for (int x = 0; x < 320; x++) { 
            uint8_t r = x * 255 / 319; 
            uint8_t g = y * 255 / 239; 
            uint8_t b = 0; 
            framebuffer[y * 320 + x] = ARGB8888(0xff, r, g, b); 
        } 
    } 
    lcd[2] = (uint32_t)framebuffer; 
    printf("lcd update started\n");
    lcd[3] = 1; 
    printf("lcd update done\n");
}

static void main_task_entry(void *parameters) {
    int counter = 0;
    uint32_t width;
    uint32_t height;
    width  = lcd[0];
    height = lcd[1];
    printf("lcd init %d %d\n", width, height);
    test_lcd();
    test_touch();
    
    while(1) {
        vTaskDelay(1000);
        printf("hello this is FreeRTOS: %d, %dx%d.\r\n", counter++, width, height);
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
