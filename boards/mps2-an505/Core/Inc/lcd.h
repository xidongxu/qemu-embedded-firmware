#ifndef LCD_H
#define LCD_H

#include <stdint.h>

#define LCD_BASE            (0x51000000UL)
#define LCD_WIDTH_ADDR      (LCD_BASE + 0x00)
#define LCD_HEIGHT_ADDR     (LCD_BASE + 0x04)
#define LCD_FB_ADDR         (LCD_BASE + 0x08)
#define LCD_CTRL_ADDR       (LCD_BASE + 0x0C)
#define LCD_STATUS_ADDR     (LCD_BASE + 0x10)
#define LCD_FORMAT_ADDR     (LCD_BASE + 0x14)
#define LCD_STRIDE_ADDR     (LCD_BASE + 0x18)
#define LCD_INT_EN_ADDR     (LCD_BASE + 0x1c)
#define LCD_INT_STATUS_ADDR (LCD_BASE + 0x20)

#define REG32(addr)         (*(volatile uint32_t *)(addr))

#define LCD_CTRL            REG32(LCD_CTRL_ADDR)
#define LCD_STATUS          REG32(LCD_STATUS_ADDR)
#define LCD_WIDTH           REG32(LCD_WIDTH_ADDR)
#define LCD_HEIGHT          REG32(LCD_HEIGHT_ADDR)
#define LCD_FB              REG32(LCD_FB_ADDR)
#define LCD_STRIDE          REG32(LCD_STRIDE_ADDR)
#define LCD_FORMAT          REG32(LCD_FORMAT_ADDR)
#define LCD_INT_EN          REG32(LCD_INT_EN_ADDR)
#define LCD_INT_STATUS      REG32(LCD_INT_STATUS_ADDR)

#define LCD_CTRL_ENABLE     (1 << 0)
#define LCD_CTRL_UPDATE     (1 << 1)
#define LCD_CTRL_RESET      (1 << 2)

#define LCD_STATUS_BUSY     (1 << 0)
#define LCD_STATUS_DONE     (1 << 1)

#define LCD_FORMAT_ARGB8888 0
#define LCD_FORMAT_RGB888   1
#define LCD_FORMAT_RGB565   2

#define LCD_WIDTH_PIXELS    450
#define LCD_HEIGHT_PIXELS   450

void lcd_init(void);
void lcd_update(void);
void lcd_wait_done(void);
void lcd_clear(uint32_t color);
void lcd_draw_pixel(int x, int y, uint32_t color);

#endif
