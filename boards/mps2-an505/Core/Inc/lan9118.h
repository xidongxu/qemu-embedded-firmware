#ifndef LAN9118_H
#define LAN9118_H

#include "lan9118_regs.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

int lan9118_open(void);
int lan9118_send(const uint8_t *data, uint32_t len);
int lan9118_receive(uint8_t *data, uint32_t *len);
int lan9118_xmit(const uint8_t *data, uint32_t len);
int lan9118_poll(uint8_t *data, uint32_t *len);
void lan9118_poll_loop(void);

void lan9118_isr(void);
void lan9118_test(void);

#endif
