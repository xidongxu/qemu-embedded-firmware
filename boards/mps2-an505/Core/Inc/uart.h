#ifndef UART_H
#define UART_H

#include <stdio.h>
#include <stdint.h>

void uart_init(void);
void uart_send(const char* string);
size_t uart_recv(uint8_t* buffer, size_t size);
/* Lock-free single-character UART output (busy-waits on TX empty).  This is
 * the crash-safe sink used by libutils/tracer via TRACER_PUTCHAR: it never
 * takes a lock, so a fault dump can print even if it interrupted printf. */
int put_char(int ch);

#endif
