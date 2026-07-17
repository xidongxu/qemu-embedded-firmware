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
