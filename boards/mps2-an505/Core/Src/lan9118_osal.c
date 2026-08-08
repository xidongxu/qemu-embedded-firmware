/**
 * @file    lan9118_osal.c
 * @brief   OSAL backends for the LAN9118 driver.
 *
 * Backend selection:
 *   LAN9118_USE_FREERTOS == 1  -> FreeRTOS backend
 *   otherwise                  -> bare-metal backend
 *
 * The bare-metal time source uses the LAN9118 free-running counter
 * (CSR_FREE_RUN, increments every 40 ns in QEMU) so that no SysTick
 * or application timer is hijacked.
 */
#include "lan9118_osal.h"
#include "lan9118_regs.h"

#if defined(LAN9118_USE_FREERTOS) && (LAN9118_USE_FREERTOS == 1)
#include "FreeRTOS.h"
#include "semphr.h"
#define LAN9118_OSAL_FREERTOS 1
#else
#define LAN9118_OSAL_BAREMETAL 1
#endif

/* CMSIS device header (ARMCM33 with FPU + DSP, matching the board's
 * -mfpu=fpv5-sp-d16 and -DARMCM33_DSP_FP settings).  Provides the
 * PRIMASK intrinsics, NVIC_*() functions and IRQn_Type. */
#include "ARMCM33_DSP_FP.h"

uint32_t lan9118_osal_critical_enter(void) {
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    return state;
}

void lan9118_osal_critical_exit(uint32_t state) {
    __set_PRIMASK(state);
}

/* Nesting-safe lock using the critical section above.  Interrupts are
 * disabled for the (short) duration of the protected region. */
static uint32_t s_lock_depth = 0U;
static uint32_t s_lock_state = 0U;

void lan9118_osal_lock(void) {
    if (s_lock_depth++ == 0U) {
        s_lock_state = lan9118_osal_critical_enter();
    }
}

void lan9118_osal_unlock(void) {
    if (s_lock_depth > 0U) {
        if (--s_lock_depth == 0U) {
            lan9118_osal_critical_exit(s_lock_state);
        }
    }
}

#if LAN9118_OSAL_FREERTOS

static SemaphoreHandle_t s_rx_sem = NULL;

int lan9118_osal_sem_init(void) {
    /* The semaphore is created lazily on the first take() (task context,
     * after the scheduler has started).  It must NOT be allocated here:
     * lan9118_open() runs before vTaskStartScheduler(), and on a
     * TrustZone (ARM_CM33_NTZ) target pvPortMalloc() before the secure
     * context is set up causes a SecureFault/Lockup. */
    return 0;
}

void lan9118_osal_sem_give_from_isr(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (s_rx_sem != NULL) {
        xSemaphoreGiveFromISR(s_rx_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void lan9118_osal_sem_give(void) {
    if (s_rx_sem != NULL) {
        (void)xSemaphoreGive(s_rx_sem);
    }
}

bool lan9118_osal_sem_take(uint32_t timeout_ms) {
    if (s_rx_sem == NULL) {
        /* Lazy creation - task context only (never call from ISR). */
        s_rx_sem = xSemaphoreCreateBinary();
    }
    if (s_rx_sem == NULL) {
        return false;
    }
    return xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) == pdPASS;
}

#else /* bare-metal */

static volatile uint32_t s_rx_flag = 0U;

int lan9118_osal_sem_init(void) {
    s_rx_flag = 0U;
    return 0;
}

void lan9118_osal_sem_give_from_isr(void) {
    s_rx_flag = 1U;
}

void lan9118_osal_sem_give(void) {
    s_rx_flag = 1U;
}

bool lan9118_osal_sem_take(uint32_t timeout_ms) {
    uint32_t start = lan9118_osal_time_ms();

    while (s_rx_flag == 0U) {
        if ((lan9118_osal_time_ms() - start) >= timeout_ms) {
            return false;
        }
    }
    s_rx_flag = 0U;
    return true;
}

#endif /* LAN9118_OSAL_FREERTOS / BAREMETAL */

/* Direct 32-bit read of a LAN9118 CSR (used for the hardware time
 * source; the driver core has its own accessor in lan9118.c). */
static inline uint32_t lan9118_hw_read(uint32_t reg) {
    return *(volatile uint32_t *)(LAN9118_BASE + reg);
}

#if LAN9118_OSAL_FREERTOS

/* The LAN9118 CSR_FREE_RUN counter (40 ns ticks) is usable at any time,
 * including before the scheduler starts (it is a hardware counter inside
 * the NIC, independent of the CPU).  ms = ticks / 25000. */
static inline uint32_t lan9118_hw_time_ms(void) {
    return (uint32_t)(lan9118_hw_read(LAN9118_FREE_RUN) / 25000U);
}

uint32_t lan9118_osal_time_ms(void) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        return (uint32_t)xTaskGetTickCount();
    }
    return lan9118_hw_time_ms();
}

void lan9118_osal_delay_ms(uint32_t ms) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    } else {
        /* Pre-scheduler: vTaskDelay() cannot be used yet, busy-wait on
         * the LAN9118 free-running counter. */
        uint32_t start = lan9118_hw_time_ms();
        while ((lan9118_hw_time_ms() - start) < ms) {}
    }
}
#else /* bare-metal */

/* CSR_FREE_RUN counts in 40 ns units:  ms = ticks * 40 / 1000000
 *                                        = ticks / 25000             */
uint32_t lan9118_osal_time_ms(void) {
    return (uint32_t)(lan9118_hw_read(LAN9118_FREE_RUN) / 25000U);
}

void lan9118_osal_delay_ms(uint32_t ms) {
    uint32_t start = lan9118_osal_time_ms();
    while ((lan9118_osal_time_ms() - start) < ms) {}
}
#endif /* LAN9118_OSAL_FREERTOS / BAREMETAL */

void lan9118_osal_irq_priority(uint32_t prio) {
    NVIC_SetPriority((IRQn_Type)LAN9118_IRQn, (uint32_t)prio);
}

void lan9118_osal_irq_enable(void) {
    NVIC_EnableIRQ((IRQn_Type)LAN9118_IRQn);
}

void lan9118_osal_irq_disable(void) {
    NVIC_DisableIRQ((IRQn_Type)LAN9118_IRQn);
}
