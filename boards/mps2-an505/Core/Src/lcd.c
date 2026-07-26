#include "lcd.h"

static uint32_t lcd_framebuffer[LCD_WIDTH_PIXELS * LCD_HEIGHT_PIXELS];

void lcd_init(void) {
    printf("fb=%p\n",lcd_framebuffer);
    LCD_FB = (uint32_t)lcd_framebuffer;
    LCD_STRIDE = LCD_WIDTH_PIXELS * 4;
    LCD_FORMAT = LCD_FORMAT_ARGB8888;
    LCD_CTRL = LCD_CTRL_ENABLE;
    lcd_clear(0xff000000);
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
    for(int y = 0; y < height; y++) {
        memcpy(&lcd_framebuffer[(y1 + y) * LCD_WIDTH_PIXELS + x1], pixels, width * sizeof(uint32_t));
        pixels += width;
    }
}

void *lcd_get_framebuffer() {
    return (void *)lcd_framebuffer;
}
