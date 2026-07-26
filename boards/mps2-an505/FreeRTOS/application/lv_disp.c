#include <stdint.h>

#include "lcd.h"
#include "printf.h"
#include "FreeRTOSConfig.h"
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>
#include "lvgl.h"
#include "lv_demos.h"

void vApplicationTickHook(void) {
    lv_tick_inc(1);
}

static void lv_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)disp;
    (void)area;
    (void)px_map;
    lcd_update();
    lcd_wait_done();
    lv_display_flush_ready(disp);
}

static void lv_task_entry(void *parameters) {
    lv_init();
    printf("LVGL %d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());
    lv_display_t *disp = lv_display_create(LCD_WIDTH_PIXELS, LCD_HEIGHT_PIXELS);
    lv_display_set_buffers(
        disp,
        lcd_get_framebuffer(),
        NULL,
        LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS * sizeof(uint32_t),
        LV_DISPLAY_RENDER_MODE_DIRECT
    );
    lv_display_set_flush_cb(disp, lv_flush);
    lv_demo_benchmark();
    while(1) {
        lv_timer_handler();
        vTaskDelay(5);
    }
}

void lv_task_init(void) {
    static TaskHandle_t lv_task = NULL;
    xTaskCreate(lv_task_entry, "lv_task", 4096, NULL, 1U, &lv_task);
}
