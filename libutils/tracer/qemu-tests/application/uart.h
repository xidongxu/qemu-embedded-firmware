/*
 * uart.h - minimal lock-free output for the QEMU MPS2/MPS3 CMSDK APB UART.
 *
 * Pure bare registers (no CMSIS / RTOS / stdio).  Each board defines the
 * UART base via -DBOARD_UART0=<addr>:
 *   mps2-an385/386/500/511 (non-TZ)  -> 0x40004000
 *   mps2-an505/an521 (SSE-200 TZ)    -> 0x40200000  (NS view)
 *   mps3-an547 (SSE-300 M55)         -> 0x49303000
 *   mps3-an555 (SSE-310 M85)         -> <see board/README, pending QEMU>
 *
 * CMSDK APB UART registers: DATA=+0x0, STATE=+0x4 (bit0=TXFULL),
 * CTRL=+0x8 (bit0=TX_EN, bit1=RX_EN), INTSTATUS=+0xC, BAUDDIV=+0x10.
 * This driver is wired as the tracer crash-safe sink (TRACER_PUTCHAR).
 */
#ifndef BOARD_UART_H
#define BOARD_UART_H

#include <stdint.h>

#ifndef BOARD_UART0
#error "BOARD_UART0 base address must be defined (e.g. -DBOARD_UART0=0x40004000)"
#endif

#define UART_DATA    (*(volatile uint32_t *)(BOARD_UART0 + 0x00))
#define UART_STATE   (*(volatile uint32_t *)(BOARD_UART0 + 0x04))
#define UART_CTRL    (*(volatile uint32_t *)(BOARD_UART0 + 0x08))

static inline void board_uart_init(void)
{
    UART_CTRL = 0x03u;   /* TX_EN | RX_EN */
}

/* Lock-free single character out; matches the TRACER_PUTCHAR(int) signature. */
static inline int board_putc(int c)
{
    while (UART_STATE & 1u) {
    }
    UART_DATA = (uint32_t)(uint8_t)c;
    return c;
}

#endif /* BOARD_UART_H */
