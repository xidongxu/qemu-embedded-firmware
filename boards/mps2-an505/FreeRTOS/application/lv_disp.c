#include <stdint.h>

#include "lcd.h"
#include "touch.h"
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
    (void)area;
    lcd_set_framebuffer((void *)px_map);
    lcd_update();
    lcd_wait_done();
    lv_display_flush_ready(disp);
}

static void lv_touch_read(lv_indev_t * indev, lv_indev_data_t * data) {
    touch_point_t point;
    touch_read(&point);
    data->point.x = (uint32_t)point.x * (LCD_WIDTH_PIXELS - 1) / 4095;
    data->point.y = (uint32_t)point.y * (LCD_HEIGHT_PIXELS - 1) / 4095;
    data->state = point.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void lv_task_entry(void *parameters) {
    lv_init();
    printf("LVGL %d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());
    lv_display_t *disp = lv_display_create(LCD_WIDTH_PIXELS, LCD_HEIGHT_PIXELS);
    lv_display_set_buffers(
        disp,
        lcd_get_framebuffer0(),
        lcd_get_framebuffer1(),
        LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS * sizeof(uint32_t),
        LV_DISPLAY_RENDER_MODE_FULL
    );
    lv_display_set_flush_cb(disp, lv_flush);

    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lv_touch_read);
    lv_demo_benchmark();
    int time = 0;
    while(1) {
        time = lv_timer_handler();
        if (time < 1) {
            time = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(time));
    }
}

void lv_task_init(void) {
    static TaskHandle_t lv_task = NULL;
    xTaskCreate(lv_task_entry, "lv_task", 4096, NULL, 1U, &lv_task);
}
