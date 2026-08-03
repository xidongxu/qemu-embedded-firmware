#ifndef LAN9118_H
#define LAN9118_H

#include "lan9118_regs.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

bool lan9118_probe(void);
void lan9118_reset(void);

uint32_t lan9118_mac_read(uint8_t reg);
void lan9118_mac_write(uint8_t reg, uint32_t value);
void lan9118_dump_mac(void);

int16_t lan9118_phy_read(uint8_t reg);
void lan9118_phy_write(uint8_t reg, uint16_t value);
void lan9118_dump_phy(void);

void lan9118_mac_enable(void);
void lan9118_rx_init(void);

int lan9118_send(const uint8_t *data, uint32_t len);
void lan9118_test(void);

#endif
