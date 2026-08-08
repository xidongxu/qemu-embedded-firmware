/**
 * @file    lan9118_osal.h
 * @brief   Minimal OS abstraction layer for the LAN9118 Ethernet driver.
 *
 * The LAN9118 driver core is kept completely free of RTOS / library
 * dependencies.  Everything that touches the operating environment is
 * routed through this tiny interface, which makes the driver portable
 * across:
 *   - FreeRTOS   (compiled with -DLAN9118_USE_FREERTOS=1)
 *   - bare-metal / polled main-loop  (default)
 *   - ThreadX or any other RTOS      (add a small backend)
 *
 * The abstraction intentionally exposes only what a NIC driver needs:
 *   - nesting-safe critical sections
 *   - a mutex-like lock for the transmit path
 *   - a binary semaphore used to defer RX processing to a stack thread
 *   - a millisecond time source
 *   - NVIC IRQ control (Cortex-M / CMSIS)
 */
#ifndef LAN9118_OSAL_H
#define LAN9118_OSAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Critical section.  Returns the previous interrupt mask so that
 * nested sections can restore it.  Safe in both task and ISR context. */
uint32_t lan9118_osal_critical_enter(void);
void lan9118_osal_critical_exit(uint32_t state);

/* Lock / unlock the transmit path.  On bare-metal this maps to the
 * critical section; on RTOS it maps to a critical section as well so
 * that it is safe from any context (the protected region is short). */
void lan9118_osal_lock(void);
void lan9118_osal_unlock(void);

/* Binary semaphore used to signal "RX data available".
 * Must be initialised once via lan9118_osal_sem_init() before use. */
int lan9118_osal_sem_init(void);
/* ISR context */
void lan9118_osal_sem_give_from_isr(void);
/* task context */
void lan9118_osal_sem_give(void);
/* true if acquired */
bool lan9118_osal_sem_take(uint32_t timeout_ms);

/* Time source.  ms since an arbitrary epoch (monotonic). */
uint32_t lan9118_osal_time_ms(void);
void lan9118_osal_delay_ms(uint32_t ms);

/* NVIC IRQ control for the LAN9118 line (LAN9118_IRQn). */
void lan9118_osal_irq_priority(uint32_t prio);
void lan9118_osal_irq_enable(void);
void lan9118_osal_irq_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* LAN9118_OSAL_H */
