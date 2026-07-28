#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
} touch_point_t;

void touch_init(void);
bool touch_read(touch_point_t *point);

#endif
