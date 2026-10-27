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

/* Listener registry, dispatch queue, now() and the frame stack. */
#include "event_bus.h"

#include <string.h>
#include <setjmp.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define EVT_LOGW(...) ESP_LOGW("event_bus", __VA_ARGS__)
#else
#define EVT_LOGW(...) ((void) 0)
#endif

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
    evt_t *event;   /* owned slot, or NULL */
} frame_t;

static frame_t s_frames[EVT_FRAME_DEPTH_MAX];
static int     s_frame_top;

static jmp_buf s_jb;
static bool    s_protected;   /* inside a drain/now */

static uint8_t  s_current_depth;
static int      s_now_depth;
static uint32_t s_dispatch_dropped;
static uint32_t s_now_rejected;

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

bool evt_dispatch(evt_t *e)
{
    if (!e || s_q_count == EVT_QUEUE_DEPTH) {
        return false;
    }
    /* from a handler: a cascade hop */
    if (s_protected) {
        int d = s_current_depth + 1;
        if (d > EVT_DISPATCH_DEPTH_MAX) {
            s_dispatch_dropped++;
            EVT_LOGW("dispatch cascade too deep (tag %u): dropped", (unsigned) e->tag);
            return false;
        }
        e->depth = (uint8_t) d;
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

void evt_frames_unwind_to(int base)
{
    if (base < 0) {
        base = 0;
    }
    while (s_frame_top > base) {
        s_frame_top--;
        evt_t *ev = s_frames[s_frame_top].event;
        if (ev) {
            evt_pool_release(ev);
        }
    }
}

static void frame_push(evt_t *owned)
{
    if (s_frame_top >= EVT_FRAME_DEPTH_MAX) {
        return;
    }
    s_frames[s_frame_top].event = owned;
    s_frame_top++;
}

static void frame_pop(void)
{
    if (s_frame_top > 0) {
        evt_frames_unwind_to(s_frame_top - 1);
    }
}

int evt_drain(void)
{
    volatile int ran = 0;

    while (s_q_count > 0) {
        evt_t *e = s_queue[s_q_head];
        s_q_head = (s_q_head + 1) % EVT_QUEUE_DEPTH;
        s_q_count--;

        volatile int base = s_frame_top;
        volatile int base_now = s_now_depth;
        s_current_depth = e->depth;
        if (setjmp(s_jb) == 0) {
            s_protected = true;
            frame_push(e);
            run_handlers(e);
            frame_pop();
        } else {
            evt_frames_unwind_to(base);
            s_now_depth = base_now;
        }
        s_protected = false;
        ran++;
    }

    return ran;
}

void evt_now(evt_t *e)
{
    if (!e) {
        return;
    }
    if (s_now_depth >= EVT_NOW_DEPTH_MAX) {
        s_now_rejected++;
        EVT_LOGW("now cascade too deep (tag %u): refused", (unsigned) e->tag);
        return;
    }

    if (s_protected) {
        /* nested: the outer setjmp catches a bail */
        s_now_depth++;
        frame_push(NULL);
        run_handlers(e);
        frame_pop();
        s_now_depth--;
        return;
    }

    volatile int base = s_frame_top;
    volatile int base_now = s_now_depth;
    if (setjmp(s_jb) == 0) {
        s_protected = true;
        s_now_depth++;
        frame_push(NULL);
        run_handlers(e);
        frame_pop();
        s_now_depth--;
    } else {
        evt_frames_unwind_to(base);
        s_now_depth = base_now;
    }
    s_protected = false;
}

void evt_bailout(void)
{
    if (s_protected) {
        longjmp(s_jb, 1);
    }
}

void evt_bus_reset(void)
{
    memset(s_listeners, 0, sizeof s_listeners);
    memset(s_queue, 0, sizeof s_queue);
    s_q_head = 0;
    s_q_count = 0;
    memset(s_frames, 0, sizeof s_frames);
    s_frame_top = 0;
    s_protected = false;
    s_current_depth = 0;
    s_now_depth = 0;
    s_dispatch_dropped = 0;
    s_now_rejected = 0;
}

int evt_frame_depth(void)
{
    return s_frame_top;
}

uint32_t evt_dispatch_dropped(void)
{
    return s_dispatch_dropped;
}

uint32_t evt_now_rejected(void)
{
    return s_now_rejected;
}

int evt_now_depth(void)
{
    return s_now_depth;
}
