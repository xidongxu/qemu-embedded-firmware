#include "ARMCM33_DSP_FP.h"
#include "touch.h"

static volatile touch_point_t g_touch;

static void touch_clear_irq(void) {
    TOUCH_CTRL = TOUCH_CTRL_CLEAR_INT;
}

void Interrupt32_Handler(void) {
    uint32_t status = TOUCH_STATUS;
    if(status & TOUCH_STATUS_READY) {
        g_touch.x = TOUCH_X;
        g_touch.y = TOUCH_Y;
        g_touch.pressed = (status & TOUCH_STATUS_PRESSED) != 0;
        touch_clear_irq();
    }
}

void touch_init(void) {
    g_touch.pressed = false;
    g_touch.x = 0;
    g_touch.y = 0;
    printf("TOUCH_ID = %08lx\n", TOUCH_ID);
    printf("TOUCH_RES = %lux%lu\n", TOUCH_RES_X, TOUCH_RES_Y);
    TOUCH_CTRL = 1;
    NVIC_ClearPendingIRQ((IRQn_Type)32);
    NVIC_EnableIRQ((IRQn_Type)32);
}

bool touch_read(touch_point_t *point) {
    if(point == NULL) {
        return false;
    }
    *point = g_touch;
    return point->pressed;
}
