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

/* Slot pools: one fixed-size free-list per size class. */
#include "event_bus.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define POOL_ALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define POOL_FREE(p)   heap_caps_free(p)
#else
#include <stdlib.h>
#define POOL_ALLOC(sz) malloc(sz)
#define POOL_FREE(p)   free(p)
#endif

typedef struct {
    uint16_t  size;
    uint16_t  count;
    uint8_t  *buffers;   /* count * size */
    evt_t    *slots;     /* slot i -> buffers + i*size */
    uint16_t *freelist;  /* stack of free indices */
    int       free_top;
    bool     *busy;      /* per-slot: currently out? */
    uint16_t  in_use;
    uint16_t  peak;
} slot_pool_t;

static slot_pool_t s_pools[EVT_CLASS_COUNT];
static bool s_inited;

static const uint16_t k_size[EVT_CLASS_COUNT]  = { EVT_TINY_SIZE,  EVT_SMALL_SIZE,  EVT_MEDIUM_SIZE  };
static const uint16_t k_count[EVT_CLASS_COUNT] = { EVT_TINY_COUNT, EVT_SMALL_COUNT, EVT_MEDIUM_COUNT };

bool evt_pool_init(void)
{
    if (s_inited) {
        return true;
    }

    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        slot_pool_t *p = &s_pools[c];
        p->size     = k_size[c];
        p->count    = k_count[c];
        p->buffers  = POOL_ALLOC((size_t) p->size * p->count);
        p->slots    = POOL_ALLOC(sizeof(evt_t) * p->count);
        p->freelist = POOL_ALLOC(sizeof(uint16_t) * p->count);
        p->busy     = POOL_ALLOC(sizeof(bool) * p->count);
        if (!p->buffers || !p->slots || !p->freelist || !p->busy) {
            evt_pool_teardown();
            return false;
        }

        for (uint16_t i = 0; i < p->count; i++) {
            p->freelist[i] = (uint16_t) (p->count - 1 - i);
            p->busy[i] = false;
            p->slots[i].payload    = p->buffers + (size_t) i * p->size;
            p->slots[i].slot_class = (uint8_t) c;
        }
        p->free_top = p->count;
        p->in_use   = 0;
        p->peak     = 0;
    }

    if (!evt_isr_pool_init()) {
        evt_pool_teardown();
        return false;
    }

    s_inited = true;
    return true;
}

void evt_pool_teardown(void)
{
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        slot_pool_t *p = &s_pools[c];
        POOL_FREE(p->buffers);
        POOL_FREE(p->slots);
        POOL_FREE(p->freelist);
        POOL_FREE(p->busy);
        memset(p, 0, sizeof *p);
    }
    evt_isr_pool_teardown();
    s_inited = false;
}

evt_t *evt_pool_acquire(evt_class_t cls, uint16_t tag)
{
    if (cls >= EVT_CLASS_COUNT) {
        return NULL;
    }
    slot_pool_t *p = &s_pools[cls];
    if (p->free_top == 0) {
        return NULL;
    }

    uint16_t idx = p->freelist[--p->free_top];
    p->busy[idx] = true;
    p->in_use++;
    if (p->in_use > p->peak) {
        p->peak = p->in_use;
    }

    evt_t *e = &p->slots[idx];
    e->tag         = tag;
    e->source      = EVT_SRC_NONE;
    e->flags       = EVT_FLAG_OWNED;
    e->depth       = 0;
    e->payload_len = 0;
    return e;
}

void evt_pool_release(evt_t *e)
{
    if (!e) {
        return;
    }
    if (e->flags & EVT_FLAG_FROM_ISR) {
        evt_isr_release(e);
        return;
    }
    evt_class_t cls = (evt_class_t) e->slot_class;
    if (cls >= EVT_CLASS_COUNT) {
        return;
    }
    slot_pool_t *p = &s_pools[cls];

    ptrdiff_t idx = e - p->slots;
    if (idx < 0 || idx >= p->count || !p->busy[idx]) {
        return;
    }

    p->busy[idx] = false;
    p->freelist[p->free_top++] = (uint16_t) idx;
    p->in_use--;
}

uint16_t evt_pool_in_use(evt_class_t cls)
{
    return cls < EVT_CLASS_COUNT ? s_pools[cls].in_use : 0;
}

uint16_t evt_pool_peak(evt_class_t cls)
{
    return cls < EVT_CLASS_COUNT ? s_pools[cls].peak : 0;
}

uint16_t evt_pool_capacity(evt_class_t cls)
{
    return cls < EVT_CLASS_COUNT ? s_pools[cls].count : 0;
}

uint32_t evt_pool_in_use_by_tag(uint16_t tag)
{
    uint32_t n = 0;
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        slot_pool_t *p = &s_pools[c];
        for (uint16_t i = 0; i < p->count; i++) {
            if (p->busy[i] && p->slots[i].tag == tag) {
                n++;
            }
        }
    }
    return n;
}
