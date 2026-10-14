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

/* Event bus core: event slots, pools, listeners, dispatch. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    EVT_FLAG_OWNED    = 1u << 0,   /* the event owns its slot */
    EVT_FLAG_FROM_ISR = 1u << 1,   /* emitted from an ISR -> the ISR pool */
    EVT_FLAG_COALESCE = 1u << 2,   /* latest-wins */
};

/* No device origin: timer tick, GPIO edge, system event. */
#define EVT_SRC_NONE   ((uint16_t) 0xFFFF)

typedef struct {
    uint16_t tag;          /* event type; index into the listener table */
    uint16_t source;       /* emitting device (registry index), or EVT_SRC_NONE */
    uint16_t flags;        /* EVT_FLAG_* */
    uint8_t  slot_class;   /* evt_class_t */
    uint8_t  depth;        /* cascade depth */
    uint16_t payload_len;
    void    *payload;      /* the slot buffer, slot-class capacity */
} evt_t;

typedef enum {
    EVT_CLASS_TINY = 0,
    EVT_CLASS_SMALL,
    EVT_CLASS_MEDIUM,
    EVT_CLASS_COUNT,
} evt_class_t;

/* Overridable at build time. */
#ifndef EVT_TINY_SIZE
#define EVT_TINY_SIZE     32
#endif
#ifndef EVT_TINY_COUNT
#define EVT_TINY_COUNT    32
#endif
#ifndef EVT_SMALL_SIZE
#define EVT_SMALL_SIZE    256
#endif
#ifndef EVT_SMALL_COUNT
#define EVT_SMALL_COUNT   16
#endif
#ifndef EVT_MEDIUM_SIZE
#define EVT_MEDIUM_SIZE   2048
#endif
#ifndef EVT_MEDIUM_COUNT
#define EVT_MEDIUM_COUNT  8
#endif

/* Idempotent; false if an allocation failed. */
bool evt_pool_init(void);

void evt_pool_teardown(void);

/* NULL when the class is exhausted. */
evt_t *evt_pool_acquire(evt_class_t cls, uint16_t tag);

/* NULL-safe; ignores double or foreign releases. */
void evt_pool_release(evt_t *e);

uint16_t evt_pool_in_use(evt_class_t cls);
uint16_t evt_pool_peak(evt_class_t cls);
uint16_t evt_pool_capacity(evt_class_t cls);
uint32_t evt_pool_in_use_by_tag(uint16_t tag);

/* ISR reserve: TINY slots, drop-and-count when empty. */
#ifndef EVT_ISR_POOL_COUNT
#define EVT_ISR_POOL_COUNT  8
#endif

evt_t *evt_isr_acquire(uint16_t tag);

void evt_isr_release(evt_t *e);

uint32_t evt_isr_dropped(void);
uint16_t evt_isr_in_use(void);
uint16_t evt_isr_capacity(void);

/* Internal. */
bool evt_isr_pool_init(void);
void evt_isr_pool_teardown(void);

#ifndef EVT_QUEUE_DEPTH
#define EVT_QUEUE_DEPTH      64
#endif
#ifndef EVT_MAX_LISTENERS
#define EVT_MAX_LISTENERS    32
#endif
#ifndef EVT_FRAME_DEPTH_MAX
#define EVT_FRAME_DEPTH_MAX  16
#endif

/* Cascade limits: queue hops for dispatch(), nested C frames for now(). */
#ifndef EVT_DISPATCH_DEPTH_MAX
#define EVT_DISPATCH_DEPTH_MAX  16
#endif
#ifndef EVT_NOW_DEPTH_MAX
#define EVT_NOW_DEPTH_MAX       8
#endif

typedef void (*evt_handler_fn)(evt_t *e, void *ctx);

/* Handlers fire in registration order. False if the registry is full. */
bool evt_listen(uint16_t tag, evt_handler_fn fn, void *ctx);

/* Queue for later; the queue takes the slot. False, slot kept, when full. */
bool evt_dispatch(evt_t *e);

/* Run the handlers now; the caller keeps `e`. */
void evt_now(evt_t *e);

/* Run every queued event; returns how many. */
int evt_drain(void);

void evt_bus_reset(void);

int evt_frame_depth(void);

/* Release the frames opened since `base`. */
void evt_frames_unwind_to(int base);

/* Leave the current drain()/now(); no-op outside one. */
void evt_bailout(void);

uint32_t evt_dispatch_dropped(void);
uint32_t evt_now_rejected(void);
int      evt_now_depth(void);
