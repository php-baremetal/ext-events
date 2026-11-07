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

/* Listener registry, the reference ring queue, now() and the frame stack. One task only. */
#include "event_bus.h"

#include <string.h>

typedef struct {
    uint16_t       tag;
    evt_handler_fn fn;
    void          *ctx;
    bool           used;
} listener_t;

static listener_t s_listeners[EVT_MAX_LISTENERS];

static evt_t *s_queue[EVT_QUEUE_DEPTH];
static int    s_q_head;
static int    s_q_count;

typedef struct {
    evt_t  *event;   /* owned slot, or NULL */
    uint8_t kind;    /* evt_frame_kind_t */
    uint8_t depth;   /* cascade depth inside the frame */
} frame_t;

static frame_t s_frames[EVT_FRAME_DEPTH_MAX];
static int     s_frame_top;
static int     s_now_depth;

static void (*s_on_close)(int index, void *ctx);
static void  *s_on_close_ctx;

static evt_cascade_stats_t s_cascade;

bool evt_listen(uint16_t tag, evt_handler_fn fn, void *ctx)
{
    if (!fn) {
        return false;
    }
    for (int i = 0; i < EVT_MAX_LISTENERS; i++) {
        if (!s_listeners[i].used) {
            s_listeners[i].tag  = tag;
            s_listeners[i].fn   = fn;
            s_listeners[i].ctx  = ctx;
            s_listeners[i].used = true;
            return true;
        }
    }
    return false;
}

static uint8_t current_depth(void)
{
    return s_frame_top > 0 ? s_frames[s_frame_top - 1].depth : 0;
}

bool evt_frame_open(evt_frame_kind_t kind, evt_t *owned)
{
    if (kind == EVT_FRAME_NOW && s_now_depth >= EVT_NOW_DEPTH_MAX) {
        s_cascade.now_refused++;
        return false;
    }
    if (s_frame_top >= EVT_FRAME_DEPTH_MAX) {
        s_cascade.frame_refused++;
        return false;
    }
    frame_t *f = &s_frames[s_frame_top];
    f->event = owned;
    f->kind  = (uint8_t) kind;
    f->depth = kind == EVT_FRAME_DISPATCH && owned ? owned->depth : current_depth();
    s_frame_top++;
    if (kind == EVT_FRAME_NOW) {
        s_now_depth++;
        if ((uint32_t) s_now_depth > s_cascade.now_max_depth) {
            s_cascade.now_max_depth = (uint32_t) s_now_depth;
        }
    }
    return true;
}

static void close_top(void)
{
    s_frame_top--;
    frame_t f = s_frames[s_frame_top];
    if (s_on_close) {
        s_on_close(s_frame_top, s_on_close_ctx);
    }
    if (f.kind == EVT_FRAME_NOW) {
        s_now_depth--;
    }
    if (f.event) {
        evt_pool_release(f.event);
    }
}

void evt_frame_close(void)
{
    if (s_frame_top > 0) {
        close_top();
    }
}

void evt_frames_unwind_to(int base)
{
    if (base < 0) {
        base = 0;
    }
    while (s_frame_top > base) {
        close_top();
    }
}

void evt_frames_on_close(void (*fn)(int index, void *ctx), void *ctx)
{
    s_on_close = fn;
    s_on_close_ctx = ctx;
}

int evt_frame_depth(void)
{
    return s_frame_top;
}

bool evt_dispatch_prepare(evt_t *e)
{
    if (!e) {
        return false;
    }
    if (s_frame_top > 0) {
        int d = current_depth() + 1;
        if (d > EVT_DISPATCH_DEPTH_MAX) {
            s_cascade.dispatch_refused++;
            return false;
        }
        e->depth = (uint8_t) d;
        if ((uint32_t) d > s_cascade.dispatch_max_depth) {
            s_cascade.dispatch_max_depth = (uint32_t) d;
        }
    }
    return true;
}

bool evt_dispatch(evt_t *e)
{
    if (!e || s_q_count == EVT_QUEUE_DEPTH || !evt_dispatch_prepare(e)) {
        return false;
    }
    int tail = (s_q_head + s_q_count) % EVT_QUEUE_DEPTH;
    s_queue[tail] = e;
    s_q_count++;
    return true;
}

static void run_handlers(evt_t *e)
{
    for (int i = 0; i < EVT_MAX_LISTENERS; i++) {
        if (s_listeners[i].used && s_listeners[i].tag == e->tag) {
            s_listeners[i].fn(e, s_listeners[i].ctx);
        }
    }
}

int evt_drain(void)
{
    if (s_frame_top > 0) {
        s_cascade.drain_refused++;
        return -1;
    }
    int ran = 0;
    while (s_q_count > 0) {
        evt_t *e = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % EVT_QUEUE_DEPTH;
        s_q_count--;

        evt_pool_mark_delivered(e);
        if (!evt_frame_open(EVT_FRAME_DISPATCH, e)) {
            evt_pool_release(e);
            continue;
        }
        run_handlers(e);
        evt_frame_close();
        ran++;
    }
    return ran;
}

bool evt_now(evt_t *e)
{
    if (!e || !evt_frame_open(EVT_FRAME_NOW, NULL)) {
        return false;
    }
    evt_pool_mark_delivered(e);
    run_handlers(e);
    evt_frame_close();
    return true;
}

void evt_bus_reset(void)
{
    memset(s_listeners, 0, sizeof s_listeners);
    memset(s_queue, 0, sizeof s_queue);
    s_q_head = 0;
    s_q_count = 0;
    memset(s_frames, 0, sizeof s_frames);
    s_frame_top = 0;
    s_now_depth = 0;
    memset(&s_cascade, 0, sizeof s_cascade);
}

void evt_cascade_stats(evt_cascade_stats_t *out)
{
    *out = s_cascade;
}

uint32_t evt_dispatch_dropped(void)
{
    return s_cascade.dispatch_refused;
}

uint32_t evt_now_rejected(void)
{
    return s_cascade.now_refused;
}

uint32_t evt_frame_refused(void)
{
    return s_cascade.frame_refused;
}

uint32_t evt_drain_refused(void)
{
    return s_cascade.drain_refused;
}

int evt_now_depth(void)
{
    return s_now_depth;
}
