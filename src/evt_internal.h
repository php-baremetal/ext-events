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

/* Shared by pool.c and isr_pool.c: the slot-pool type, its helpers and the lock. */
#pragma once

#include "event_bus.h"
#include "bm_events_hal.h"

/* PSRAM when the chip has it, internal RAM otherwise. Overridable (the host cases inject failures). */
#ifndef EVT_POOL_ALLOC
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define EVT_POOL_ALLOC(sz) heap_caps_malloc_prefer((sz), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)
#define EVT_POOL_FREE(p)   heap_caps_free(p)
#else
#include <stdlib.h>
#define EVT_POOL_ALLOC(sz) malloc(sz)
#define EVT_POOL_FREE(p)   free(p)
#endif
#endif

typedef struct {
    uint32_t  count;      /* ordinary slots */
    uint32_t  total;      /* count + reserve */
    uint16_t  size;
    uint8_t   cls;
    uint16_t  flags;      /* EVT_FLAG_* set on acquire */
    void     *block;      /* buffers, headers, free stack, states: one allocation */
    uint8_t  *buffers;
    evt_t    *slots;
    uint32_t *freelist;
    uint8_t  *state;      /* SLOT_* */
    uint32_t  free_top;
    uint32_t  in_use;
    uint32_t  peak;
    uint32_t  dropped;
} slot_pool_t;

/* Created by evt_pool_init(), never freed. */
extern bm_hal_lock_t *evt_lock;

size_t evt_slots_bytes(uint16_t size, uint32_t total);
bool   evt_slots_alloc(slot_pool_t *p, evt_class_t cls, uint16_t size, uint32_t count, uint32_t reserve,
                       uint16_t flags);
void   evt_slots_free(slot_pool_t *p);

/* With evt_lock held. NULL: coalesced (when asked) or exhausted, counted. */
evt_t *evt_slots_take_locked(slot_pool_t *p, uint16_t tag, bool use_reserve, bool coalesce);
/* False if the slot is not out of this pool. */
bool   evt_slots_give_locked(slot_pool_t *p, evt_t *e);
uint32_t evt_slots_count_tag_locked(const slot_pool_t *p, uint16_t tag);
void   evt_slots_stats_locked(const slot_pool_t *p, evt_pool_stats_t *out);

/* The ISR pool, for pool.c. */
extern slot_pool_t evt_isr_pool;
bool evt_isr_pool_init(void);
void evt_isr_pool_teardown(void);
