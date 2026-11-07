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

/* Slot pools: one fixed-size free-list per size class, per-tag counters, all under one lock. */
#include "evt_internal.h"

#include <stdio.h>
#include <string.h>

enum { SLOT_FREE = 0, SLOT_OUT, SLOT_QUEUED };

typedef struct {
    uint32_t queued;
    uint32_t delivered;
    uint32_t dropped;
    uint32_t coalesced;
    bool     coalesce;
    evt_t   *waiting;     /* the queued slot of a coalescing tag, which counts the merges */
} tag_state_t;

bm_hal_lock_t *evt_lock;

static slot_pool_t  s_pools[EVT_CLASS_COUNT];
static tag_state_t  s_tags[EVT_MAX_TAGS];
static bool         s_pools_inited;

/* The last failed init: pool index (EVT_CLASS_COUNT = the ISR pool, FAIL_LOCK = the lock), slots and bytes. */
#define FAIL_LOCK (EVT_CLASS_COUNT + 1)
static int      s_fail_pool = -1;
static uint32_t s_fail_count;
static size_t   s_fail_bytes;

static const uint16_t k_size[EVT_CLASS_COUNT]  = { EVT_TINY_SIZE,  EVT_SMALL_SIZE,  EVT_MEDIUM_SIZE  };
static const uint32_t k_count[EVT_CLASS_COUNT] = { EVT_TINY_COUNT, EVT_SMALL_COUNT, EVT_MEDIUM_COUNT };

_Static_assert(EVT_TINY_COUNT <= 65535 && EVT_SMALL_COUNT <= 65535 && EVT_MEDIUM_COUNT <= 65535,
               "event slot counts are 16-bit");
_Static_assert(EVT_TINY_SIZE % 8 == 0 && EVT_SMALL_SIZE % 8 == 0 && EVT_MEDIUM_SIZE % 8 == 0,
               "event slot sizes keep the headers aligned");

size_t evt_slots_bytes(uint16_t size, uint32_t total)
{
    return (size_t) total * (size + sizeof(evt_t) + sizeof(uint32_t) + 1);
}

bool evt_slots_alloc(slot_pool_t *p, evt_class_t cls, uint16_t size, uint32_t count, uint32_t reserve,
                     uint16_t flags)
{
    memset(p, 0, sizeof *p);
    p->count = count;
    p->total = count + reserve;
    p->size  = size;
    p->cls   = (uint8_t) cls;
    p->flags = flags;
    if (p->total == 0) {
        return true;
    }

    p->block = EVT_POOL_ALLOC(evt_slots_bytes(size, p->total));
    if (!p->block) {
        memset(p, 0, sizeof *p);
        return false;
    }
    p->buffers  = p->block;
    p->slots    = (evt_t *) (void *) (p->buffers + (size_t) size * p->total);
    p->freelist = (uint32_t *) (void *) (p->slots + p->total);
    p->state    = (uint8_t *) (p->freelist + p->total);

    for (uint32_t i = 0; i < p->total; i++) {
        p->freelist[i] = p->total - 1 - i;   /* index 0 pops first */
        p->state[i] = SLOT_FREE;
        memset(&p->slots[i], 0, sizeof p->slots[i]);
        p->slots[i].payload    = p->buffers + (size_t) i * size;
        p->slots[i].slot_class = (uint8_t) cls;
    }
    p->free_top = p->total;
    return true;
}

void evt_slots_free(slot_pool_t *p)
{
    if (p->block) {
        EVT_POOL_FREE(p->block);
    }
    memset(p, 0, sizeof *p);
}

static tag_state_t *tag_state(uint16_t tag)
{
    return tag < EVT_MAX_TAGS ? &s_tags[tag] : NULL;
}

evt_t *evt_slots_take_locked(slot_pool_t *p, uint16_t tag, bool use_reserve, bool coalesce)
{
    tag_state_t *t = tag_state(tag);
    if (coalesce && t && t->coalesce && t->queued > 0) {
        t->coalesced++;
        if (t->waiting && t->waiting->merged < UINT32_MAX) {
            t->waiting->merged++;
        }
        return NULL;
    }
    uint32_t keep = use_reserve ? 0 : p->total - p->count;
    if (p->free_top <= keep) {
        p->dropped++;
        if (t) {
            t->dropped++;
        }
        return NULL;
    }

    uint32_t idx = p->freelist[--p->free_top];
    p->state[idx] = SLOT_QUEUED;
    p->in_use++;
    if (p->in_use > p->peak) {
        p->peak = p->in_use;
    }
    evt_t *e = &p->slots[idx];
    if (t) {
        t->queued++;
        if (t->coalesce && t->queued == 1) {
            t->waiting = e;
        }
    }
    e->tag         = tag;
    e->source      = EVT_SRC_NONE;
    e->flags       = p->flags;
    e->depth       = 0;
    e->payload_len = 0;
    e->kind        = 0;
    e->merged      = 0;
    e->time_us     = bm_hal_now_us();
    return e;
}

static bool slot_index(const slot_pool_t *p, const evt_t *e, uint32_t *idx)
{
    if (!p->slots) {
        return false;
    }
    uintptr_t a = (uintptr_t) e, lo = (uintptr_t) p->slots, hi = (uintptr_t) (p->slots + p->total);
    if (a < lo || a >= hi || (a - lo) % sizeof(evt_t) != 0) {
        return false;
    }
    *idx = (uint32_t) ((a - lo) / sizeof(evt_t));
    return true;
}

static void unqueue_locked(slot_pool_t *p, uint32_t idx)
{
    if (p->state[idx] == SLOT_QUEUED) {
        tag_state_t *t = tag_state(p->slots[idx].tag);
        if (t && t->queued > 0) {
            t->queued--;
        }
        if (t && (t->queued == 0 || t->waiting == &p->slots[idx])) {
            t->waiting = NULL;
        }
        p->state[idx] = SLOT_OUT;
    }
}

bool evt_slots_give_locked(slot_pool_t *p, evt_t *e)
{
    uint32_t idx;
    if (!slot_index(p, e, &idx) || p->state[idx] == SLOT_FREE) {
        return false;
    }
    unqueue_locked(p, idx);
    p->state[idx] = SLOT_FREE;
    p->freelist[p->free_top++] = idx;
    p->in_use--;
    return true;
}

uint32_t evt_slots_count_tag_locked(const slot_pool_t *p, uint16_t tag)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->total; i++) {
        if (p->state[i] != SLOT_FREE && p->slots[i].tag == tag) {
            n++;
        }
    }
    return n;
}

void evt_slots_stats_locked(const slot_pool_t *p, evt_pool_stats_t *out)
{
    out->size    = p->count;
    out->in_use  = p->in_use;
    out->peak    = p->peak;
    out->dropped = p->dropped;
}

bool evt_pool_init(void)
{
    if (s_pools_inited) {
        return true;
    }
    s_fail_pool = -1;
    if (!evt_lock && !(evt_lock = bm_hal_lock_create())) {
        s_fail_pool = FAIL_LOCK;
        return false;
    }

    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        uint32_t reserve = c == EVT_CLASS_TINY ? EVT_RELEASE_RESERVE : 0;
        if (!evt_slots_alloc(&s_pools[c], (evt_class_t) c, k_size[c], k_count[c], reserve, EVT_FLAG_OWNED)) {
            s_fail_pool  = c;
            s_fail_count = k_count[c];
            s_fail_bytes = evt_slots_bytes(k_size[c], k_count[c] + reserve);
            evt_pool_teardown();
            return false;
        }
    }
    if (!evt_isr_pool_init()) {
        s_fail_pool  = EVT_CLASS_COUNT;
        s_fail_count = EVT_ISR_POOL_COUNT;
        s_fail_bytes = evt_slots_bytes(EVT_TINY_SIZE, EVT_ISR_POOL_COUNT);
        evt_pool_teardown();
        return false;
    }

    s_pools_inited = true;
    return true;
}

size_t evt_pool_init_error(char *buf, size_t len)
{
    static const char *const names[EVT_CLASS_COUNT] = { "small", "medium", "large" };
    int n;
    if (s_fail_pool < 0) {
        if (buf && len) {
            buf[0] = '\0';
        }
        return 0;
    } else if (s_fail_pool == FAIL_LOCK) {
        n = snprintf(buf, len, "events: cannot create the event lock");
    } else if (s_fail_pool == EVT_CLASS_COUNT) {
        n = snprintf(buf, len, "events: cannot allocate %lu interrupt event slots (%lu KB): the interrupt pool is "
                     "fixed-size; lower events-small, events-medium or events-large to free memory",
                     (unsigned long) s_fail_count, (unsigned long) ((s_fail_bytes + 1023) / 1024));
    } else {
        n = snprintf(buf, len, "events: cannot allocate %lu %s event slots (%lu KB): lower events-%s",
                     (unsigned long) s_fail_count, names[s_fail_pool],
                     (unsigned long) ((s_fail_bytes + 1023) / 1024), names[s_fail_pool]);
    }
    return n > 0 ? (size_t) n : 0;
}

/* Quiescent only: frees without the lock (no freeing inside a critical section). */
void evt_pool_teardown(void)
{
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        evt_slots_free(&s_pools[c]);
    }
    evt_isr_pool_teardown();
    memset(s_tags, 0, sizeof s_tags);
    s_pools_inited = false;
}

static evt_t *acquire(slot_pool_t *p, uint16_t tag, bool use_reserve, bool coalesce)
{
    if (!evt_lock) {
        return NULL;
    }
    bm_hal_lock_enter(evt_lock);
    evt_t *e = evt_slots_take_locked(p, tag, use_reserve, coalesce);
    bm_hal_lock_exit(evt_lock);
    return e;
}

evt_t *evt_pool_acquire(evt_class_t cls, uint16_t tag)
{
    return cls < EVT_CLASS_COUNT ? acquire(&s_pools[cls], tag, false, true) : NULL;
}

evt_t *evt_pool_acquire_reserve(uint16_t tag)
{
    return acquire(&s_pools[EVT_CLASS_TINY], tag, true, false);
}

evt_t *evt_isr_acquire(uint16_t tag)
{
    return acquire(&evt_isr_pool, tag, false, true);
}

void evt_pool_release(evt_t *e)
{
    if (!e || !evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    if (!evt_slots_give_locked(&evt_isr_pool, e)) {
        for (int c = 0; c < EVT_CLASS_COUNT; c++) {
            if (evt_slots_give_locked(&s_pools[c], e)) {
                break;
            }
        }
    }
    bm_hal_lock_exit(evt_lock);
}

void evt_isr_release(evt_t *e)
{
    if (!e || !evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    evt_slots_give_locked(&evt_isr_pool, e);
    bm_hal_lock_exit(evt_lock);
}

void evt_pool_mark_delivered(evt_t *e)
{
    if (!e || !evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    uint32_t idx;
    if (slot_index(&evt_isr_pool, e, &idx)) {
        unqueue_locked(&evt_isr_pool, idx);
    } else {
        for (int c = 0; c < EVT_CLASS_COUNT; c++) {
            if (slot_index(&s_pools[c], e, &idx)) {
                unqueue_locked(&s_pools[c], idx);
                break;
            }
        }
    }
    tag_state_t *t = tag_state(e->tag);
    if (t) {
        t->delivered++;
    }
    bm_hal_lock_exit(evt_lock);
}

void evt_pool_stats(evt_class_t cls, evt_pool_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (cls >= EVT_CLASS_COUNT || !evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    evt_slots_stats_locked(&s_pools[cls], out);
    bm_hal_lock_exit(evt_lock);
}

void evt_isr_stats(evt_pool_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    evt_slots_stats_locked(&evt_isr_pool, out);
    bm_hal_lock_exit(evt_lock);
}

static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFF ? 0xFFFF : (uint16_t) v;
}

uint16_t evt_pool_in_use(evt_class_t cls)
{
    evt_pool_stats_t st;
    evt_pool_stats(cls, &st);
    return sat16(st.in_use);
}

uint16_t evt_pool_peak(evt_class_t cls)
{
    evt_pool_stats_t st;
    evt_pool_stats(cls, &st);
    return sat16(st.peak);
}

uint16_t evt_pool_capacity(evt_class_t cls)
{
    evt_pool_stats_t st;
    evt_pool_stats(cls, &st);
    return sat16(st.size);
}

uint32_t evt_pool_in_use_by_tag(uint16_t tag)
{
    if (!evt_lock) {
        return 0;
    }
    uint32_t n = 0;
    bm_hal_lock_enter(evt_lock);
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        n += evt_slots_count_tag_locked(&s_pools[c], tag);
    }
    n += evt_slots_count_tag_locked(&evt_isr_pool, tag);
    bm_hal_lock_exit(evt_lock);
    return n;
}

bool evt_tag_set_coalesce(uint16_t tag, bool on)
{
    tag_state_t *t = tag_state(tag);
    if (!t || !evt_lock) {
        return false;
    }
    bm_hal_lock_enter(evt_lock);
    t->coalesce = on;
    bm_hal_lock_exit(evt_lock);
    return true;
}

void evt_tag_stats(uint16_t tag, evt_tag_stats_t *out)
{
    memset(out, 0, sizeof *out);
    tag_state_t *t = tag_state(tag);
    if (!t || !evt_lock) {
        return;
    }
    bm_hal_lock_enter(evt_lock);
    out->queued    = t->queued;
    out->delivered = t->delivered;
    out->dropped   = t->dropped;
    out->coalesced = t->coalesced;
    bm_hal_lock_exit(evt_lock);
}
