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

/* ISR reserve: TINY slots for interrupt context, drop-and-count when empty.
 * Lives in PSRAM, which is fine while no IRAM ISR (ESP_INTR_FLAG_IRAM, cache disabled) acquires from it;
 * before one does, allocate with MALLOC_CAP_INTERNAL. */
#include "event_bus.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define ISR_ALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define ISR_FREE(p)   heap_caps_free(p)
#else
#include <stdlib.h>
#define ISR_ALLOC(sz) malloc(sz)
#define ISR_FREE(p)   free(p)
#endif

static uint8_t  *s_buffers;
static evt_t    *s_slots;
static uint16_t *s_freelist;
static int       s_free_top;
static bool     *s_busy;
static uint16_t  s_in_use;
static uint16_t  s_peak;
static uint32_t  s_dropped;
static bool      s_inited;

bool evt_isr_pool_init(void)
{
    if (s_inited) {
        return true;
    }
    s_buffers  = ISR_ALLOC((size_t) EVT_TINY_SIZE * EVT_ISR_POOL_COUNT);
    s_slots    = ISR_ALLOC(sizeof(evt_t) * EVT_ISR_POOL_COUNT);
    s_freelist = ISR_ALLOC(sizeof(uint16_t) * EVT_ISR_POOL_COUNT);
    s_busy     = ISR_ALLOC(sizeof(bool) * EVT_ISR_POOL_COUNT);
    if (!s_buffers || !s_slots || !s_freelist || !s_busy) {
        evt_isr_pool_teardown();
        return false;
    }
    for (uint16_t i = 0; i < EVT_ISR_POOL_COUNT; i++) {
        s_freelist[i] = (uint16_t) (EVT_ISR_POOL_COUNT - 1 - i);
        s_busy[i] = false;
        s_slots[i].payload    = s_buffers + (size_t) i * EVT_TINY_SIZE;
        s_slots[i].slot_class = EVT_CLASS_TINY;
    }
    s_free_top = EVT_ISR_POOL_COUNT;
    s_in_use   = 0;
    s_peak     = 0;
    s_dropped  = 0;
    s_inited   = true;
    return true;
}

void evt_isr_pool_teardown(void)
{
    ISR_FREE(s_buffers);
    ISR_FREE(s_slots);
    ISR_FREE(s_freelist);
    ISR_FREE(s_busy);
    s_buffers = NULL;
    s_slots = NULL;
    s_freelist = NULL;
    s_busy = NULL;
    s_free_top = 0;
    s_in_use = 0;
    s_peak = 0;
    s_dropped = 0;
    s_inited = false;
}

evt_t *evt_isr_acquire(uint16_t tag)
{
    if (s_free_top == 0) {
        s_dropped++;
        return NULL;
    }
    uint16_t idx = s_freelist[--s_free_top];
    s_busy[idx] = true;
    s_in_use++;
    if (s_in_use > s_peak) {
        s_peak = s_in_use;
    }
    evt_t *e = &s_slots[idx];
    e->tag         = tag;
    e->source      = EVT_SRC_NONE;
    e->flags       = EVT_FLAG_OWNED | EVT_FLAG_FROM_ISR;
    e->depth       = 0;
    e->payload_len = 0;
    return e;
}

void evt_isr_release(evt_t *e)
{
    if (!e) {
        return;
    }
    ptrdiff_t idx = e - s_slots;
    if (idx < 0 || idx >= EVT_ISR_POOL_COUNT || !s_busy[idx]) {
        return;
    }
    s_busy[idx] = false;
    s_freelist[s_free_top++] = (uint16_t) idx;
    s_in_use--;
}

uint32_t evt_isr_dropped(void)  { return s_dropped; }
uint16_t evt_isr_in_use(void)   { return s_in_use; }
uint16_t evt_isr_capacity(void) { return EVT_ISR_POOL_COUNT; }
