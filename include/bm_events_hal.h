/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-events                                            |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@aurecchia.com>             |
   +----------------------------------------------------------------------+
*/

/* Platform primitives the event bus needs; implemented by the platform's events HAL. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Monotonic microseconds since boot. */
int64_t bm_hal_now_us(void);

/* Bounded queue. wait_ms: 0 = never block, UINT32_MAX = forever. */
typedef struct bm_hal_queue bm_hal_queue_t;
bm_hal_queue_t *bm_hal_queue_create(size_t depth, size_t item_size);
bool bm_hal_queue_send(bm_hal_queue_t *q, const void *item, uint32_t wait_ms);
bool bm_hal_queue_send_isr(bm_hal_queue_t *q, const void *item);   /* from an ISR */
bool bm_hal_queue_receive(bm_hal_queue_t *q, void *out, uint32_t wait_ms);

/* Periodic timer: cb(arg) every period_ms. */
typedef struct bm_hal_timer bm_hal_timer_t;
bm_hal_timer_t *bm_hal_timer_create(void (*cb)(void *arg), void *arg, const char *name);
bool bm_hal_timer_start(bm_hal_timer_t *t, uint32_t period_ms);
void bm_hal_timer_stop(bm_hal_timer_t *t);

#define BM_HAL_PULL_UP   0
#define BM_HAL_PULL_DOWN 1
#define BM_HAL_PULL_NONE 2
void bm_hal_gpio_input(int pin, int pull);
int  bm_hal_gpio_level(int pin);
/* Input that calls isr(arg) on each falling edge. */
void bm_hal_gpio_on_falling(int pin, void (*isr)(void *arg), void *arg);

/* Whether automatic light sleep is on (the host configured esp_pm). */
bool bm_hal_power_save(void);

/* No-light-sleep lock; create returns NULL when power management is off. */
typedef struct bm_hal_pmlock bm_hal_pmlock_t;
bm_hal_pmlock_t *bm_hal_pmlock_create(const char *name);
void bm_hal_pmlock_acquire(bm_hal_pmlock_t *l);
void bm_hal_pmlock_release(bm_hal_pmlock_t *l);
