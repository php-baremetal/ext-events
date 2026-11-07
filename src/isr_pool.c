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

/* ISR reserve: TINY slots for interrupt context, drop-and-count when empty; acquire and release are in pool.c.
 * Lives in PSRAM when there is one, which is fine while no IRAM ISR (ESP_INTR_FLAG_IRAM, cache disabled)
 * acquires from it; before one does, allocate it in internal RAM. */
#include "evt_internal.h"

slot_pool_t evt_isr_pool;

bool evt_isr_pool_init(void)
{
    if (evt_isr_pool.block) {
        return true;
    }
    return evt_slots_alloc(&evt_isr_pool, EVT_CLASS_TINY, EVT_TINY_SIZE, EVT_ISR_POOL_COUNT, 0,
                           EVT_FLAG_OWNED | EVT_FLAG_FROM_ISR);
}

void evt_isr_pool_teardown(void)
{
    evt_slots_free(&evt_isr_pool);
}

uint32_t evt_isr_dropped(void)
{
    evt_pool_stats_t st;
    evt_isr_stats(&st);
    return st.dropped;
}

uint16_t evt_isr_in_use(void)
{
    evt_pool_stats_t st;
    evt_isr_stats(&st);
    return (uint16_t) st.in_use;
}

uint16_t evt_isr_capacity(void)
{
    return EVT_ISR_POOL_COUNT;
}
