/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-events                                            |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/* Platform primitives the event bus needs; implemented by the platform's events HAL. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Monotonic microseconds since boot, on the clock of the platform's CLOCK_MONOTONIC (the base of PHP's hrtime()).
   Callable from an ISR: the event pool stamps each slot with it on acquire. */
int64_t bm_hal_now_us(void);

/* Bounded queue. wait_ms: 0 = never block, UINT32_MAX = forever. */
typedef struct bm_hal_queue bm_hal_queue_t;
bm_hal_queue_t *bm_hal_queue_create(size_t depth, size_t item_size);
bool bm_hal_queue_send(bm_hal_queue_t *q, const void *item, uint32_t wait_ms);
bool bm_hal_queue_send_isr(bm_hal_queue_t *q, const void *item);   /* from an ISR */
bool bm_hal_queue_receive(bm_hal_queue_t *q, void *out, uint32_t wait_ms);

/* Short critical section around the event pools, usable from a task and from an ISR (since events-hal 1.3).
 * Not recursive; create returns NULL when out of memory. */
typedef struct bm_hal_lock bm_hal_lock_t;
bm_hal_lock_t *bm_hal_lock_create(void);
void bm_hal_lock_enter(bm_hal_lock_t *l);
void bm_hal_lock_exit(bm_hal_lock_t *l);

/* Periodic timer: cb(arg) every period_ms, or every period_us (since events-hal 1.2). */
typedef struct bm_hal_timer bm_hal_timer_t;
bm_hal_timer_t *bm_hal_timer_create(void (*cb)(void *arg), void *arg, const char *name);
bool bm_hal_timer_start(bm_hal_timer_t *t, uint32_t period_ms);
bool bm_hal_timer_start_us(bm_hal_timer_t *t, uint64_t period_us);
void bm_hal_timer_stop(bm_hal_timer_t *t);

#define BM_HAL_PULL_UP   0
#define BM_HAL_PULL_DOWN 1
#define BM_HAL_PULL_NONE 2
void bm_hal_gpio_input(int pin, int pull);
/* Callable from an ISR: the GPIO edge handler reads the pin level with it. Must not block. */
int  bm_hal_gpio_level(int pin);
/* Input that calls isr(arg) on each falling edge. */
void bm_hal_gpio_on_falling(int pin, void (*isr)(void *arg), void *arg);
/* Detach the pin's interrupt (the handler set by bm_hal_gpio_on_falling()); the pin stays an input. Called from the PHP task. */
void bm_hal_gpio_off(int pin);

/* Whether automatic light sleep is on (the host configured esp_pm). */
bool bm_hal_power_save(void);

/* Whether the runtime runs every HTTP request as its own PHP request (web-server): event sources make no
 * sense there, at boot included. Since events-hal 1.1. */
bool bm_hal_request_scoped(void);

/* No-light-sleep lock; create returns NULL when power management is off. */
typedef struct bm_hal_pmlock bm_hal_pmlock_t;
bm_hal_pmlock_t *bm_hal_pmlock_create(const char *name);
void bm_hal_pmlock_acquire(bm_hal_pmlock_t *l);
void bm_hal_pmlock_release(bm_hal_pmlock_t *l);
