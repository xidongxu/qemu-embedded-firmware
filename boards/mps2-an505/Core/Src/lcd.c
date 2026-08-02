#include "lcd.h"

#define LCD_FB_SIZE (LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS)
static uint32_t lcd_framebuffer0[LCD_FB_SIZE] __attribute__((aligned(64)));
static uint32_t lcd_framebuffer1[LCD_FB_SIZE] __attribute__((aligned(64)));

static uint32_t *lcd_draw_fb = NULL;
static uint32_t *lcd_disp_fb = NULL;

void lcd_init(void) {
    printf("lcd fb0=%p fb1=%p\n", lcd_framebuffer0, lcd_framebuffer1);
    memset(lcd_framebuffer0, 0xff, sizeof(lcd_framebuffer0));
    memset(lcd_framebuffer1, 0xff, sizeof(lcd_framebuffer1));
    lcd_draw_fb = lcd_framebuffer0;
    lcd_disp_fb = lcd_framebuffer1;
    LCD_FB = (uint32_t)lcd_disp_fb;
    LCD_STRIDE = LCD_WIDTH_PIXELS * 4;
    LCD_FORMAT = LCD_FORMAT_ARGB8888;
    LCD_CTRL = LCD_CTRL_ENABLE;
    lcd_update();
    lcd_wait_done();
}

void lcd_update(void) {
    LCD_CTRL = LCD_CTRL_ENABLE | LCD_CTRL_UPDATE;
}

void lcd_wait_done(void) {
    while(!(LCD_STATUS & LCD_STATUS_DONE)) {}
}

void lcd_clear(uint32_t color) {
    uint32_t i;
    uint32_t *lcd_framebuffer = lcd_draw_fb;
    for(i = 0; i < LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS; i++) {
        lcd_framebuffer[i] = color;
    }
}

void lcd_draw_pixel(int x, int y, uint32_t color) {
    if(x < 0 || x >= LCD_WIDTH_PIXELS) {
        return;
    }
    if(y < 0 || y >= LCD_HEIGHT_PIXELS) {
        return;
    }
    uint32_t *lcd_framebuffer = lcd_draw_fb;
    lcd_framebuffer[y * LCD_WIDTH_PIXELS + x] = color;
}

void lcd_draw(int x1, int y1, int x2, int y2, const uint32_t *pixels) {
    if(x1 < 0) {
        x1 = 0;
    }
    if(y1 < 0) {
        y1 = 0;
    }
    if(x2 >= LCD_WIDTH_PIXELS) {
        x2 = LCD_WIDTH_PIXELS - 1;
    }
    if(y2 >= LCD_HEIGHT_PIXELS) {
        y2 = LCD_HEIGHT_PIXELS - 1;
    }
    int width  = x2 - x1 + 1;
    int height = y2 - y1 + 1;
    uint32_t *lcd_framebuffer = lcd_draw_fb;
    for(int y = 0; y < height; y++) {
        memcpy(&lcd_framebuffer[(y1 + y) * LCD_WIDTH_PIXELS + x1], pixels, width * sizeof(uint32_t));
        pixels += width;
    }
}

void lcd_swap_framebuffer(void) {
    uint32_t *tmp = NULL;
    lcd_wait_done();
    tmp = lcd_disp_fb;
    lcd_disp_fb = lcd_draw_fb;
    lcd_draw_fb = tmp;
    LCD_FB = (uint32_t)lcd_disp_fb;
    lcd_update();
}

void *lcd_get_framebuffer0(void) {
    return (void *)lcd_framebuffer0;
}

void *lcd_get_framebuffer1(void) {
    return (void *)lcd_framebuffer1;
}

void lcd_set_framebuffer(void *addr) {
    LCD_FB = (uint32_t)addr;
}
