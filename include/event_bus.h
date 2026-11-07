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

/* Event bus core: event slots, pools, per-tag counters, listeners, frames, dispatch.
 *
 * Threads: the pool, ISR-pool and tag functions (evt_pool_*, evt_isr_*, evt_tag_*) are safe from any task and
 * from an ISR (one short bm_hal_lock section each). Everything else (listeners, frames, dispatch, drain, now,
 * the cascade counters) belongs to one task, the PHP task.
 *
 * Bailout: the core has no setjmp of its own. The caller delivers inside its own catch (zend_try); on a bailout
 * it calls evt_frames_unwind_to(base) with the evt_frame_depth() taken before the delivery, which closes the
 * frames opened since (owned slots released, close hook called) and restores the now() depth.
 *
 * Transport: evt_dispatch()/evt_drain() run a small single-task ring queue; they are the reference used by the
 * host cases. A platform transport (a FreeRTOS queue of evt_t pointers) uses instead: acquire, queue the
 * pointer, then on the PHP task evt_pool_mark_delivered(), evt_frame_open(EVT_FRAME_DISPATCH, e), the
 * listeners, evt_frame_close(); a dispatch from a listener calls evt_dispatch_prepare() before queueing. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__has_include)
#if __has_include("phpbm_options.h")
#include "phpbm_options.h"
#endif
#endif

enum {
    EVT_FLAG_OWNED    = 1u << 0,   /* the event owns its slot */
    EVT_FLAG_FROM_ISR = 1u << 1,   /* emitted from an ISR -> the ISR pool */
};

/* No device origin: timer tick, GPIO edge, system event. */
#define EVT_SRC_NONE   ((uint16_t) 0xFFFF)

/* The tag must not change while the slot is out: the counters are per tag. */
typedef struct {
    uint16_t tag;          /* event type */
    uint16_t source;       /* emitting device (registry index), or EVT_SRC_NONE */
    uint16_t flags;        /* EVT_FLAG_* */
    uint8_t  slot_class;   /* evt_class_t */
    uint8_t  depth;        /* cascade depth */
    uint16_t payload_len;
    uint8_t  kind;         /* the caller's message kind; 0 on acquire */
    uint32_t merged;       /* events coalesced into this slot while it waited, saturating; 0 on acquire */
    int64_t  time_us;      /* bm_hal_now_us() at acquire */
    void    *payload;      /* the slot buffer, slot-class capacity */
} evt_t;

/* TINY, SMALL, MEDIUM are the "small", "medium", "large" classes of the package options. */
typedef enum {
    EVT_CLASS_TINY = 0,
    EVT_CLASS_SMALL,
    EVT_CLASS_MEDIUM,
    EVT_CLASS_COUNT,
} evt_class_t;

/* Slot counts: the events-small/-medium/-large options when the build has them, at most 65535 each. */
#ifndef EVT_TINY_SIZE
#define EVT_TINY_SIZE     32
#endif
#ifndef EVT_TINY_COUNT
#ifdef PHPBM_OPTION_EVENTS_SMALL
#define EVT_TINY_COUNT    PHPBM_OPTION_EVENTS_SMALL
#else
#define EVT_TINY_COUNT    64
#endif
#endif
#ifndef EVT_SMALL_SIZE
#define EVT_SMALL_SIZE    256
#endif
#ifndef EVT_SMALL_COUNT
#ifdef PHPBM_OPTION_EVENTS_MEDIUM
#define EVT_SMALL_COUNT   PHPBM_OPTION_EVENTS_MEDIUM
#else
#define EVT_SMALL_COUNT   16
#endif
#endif
#ifndef EVT_MEDIUM_SIZE
#define EVT_MEDIUM_SIZE   2048
#endif
#ifndef EVT_MEDIUM_COUNT
#ifdef PHPBM_OPTION_EVENTS_LARGE
#define EVT_MEDIUM_COUNT  PHPBM_OPTION_EVENTS_LARGE
#else
#define EVT_MEDIUM_COUNT  8
#endif
#endif

/* TINY slots on top of EVT_TINY_COUNT that only evt_pool_acquire_reserve() takes (object releases). */
#ifndef EVT_RELEASE_RESERVE
#define EVT_RELEASE_RESERVE  4
#endif

/* Tags 0..EVT_MAX_TAGS-1 have counters and the coalesce flag; higher tags get slots but no counters. */
#ifndef EVT_MAX_TAGS
#define EVT_MAX_TAGS  64
#endif

/* Idempotent; false if an allocation failed (everything taken is freed, see evt_pool_init_error()). */
bool evt_pool_init(void);

/* After a failed evt_pool_init(): writes "events: cannot allocate <n> <class> event slots (<KB> KB): lower
 * events-<class>" (or the interrupt-pool / lock variant) and returns its length (snprintf-style); 0 when the
 * last init did not fail. */
size_t evt_pool_init_error(char *buf, size_t len);

/* Frees the pools and zeroes every pool and tag counter and the coalesce flags. */
void evt_pool_teardown(void);

/* NULL when the class is exhausted (a drop of `tag`) or `tag` coalesces and has an event queued (coalesced).
 * A slot counts as queued for its tag until evt_pool_mark_delivered() or its release. */
evt_t *evt_pool_acquire(evt_class_t cls, uint16_t tag);

/* A TINY slot that may come from the release reserve; NULL (a drop) when that is empty too. No coalesce. */
evt_t *evt_pool_acquire_reserve(uint16_t tag);

/* NULL-safe; routes ISR-pool slots back to the ISR pool. A foreign pointer or a release of a free slot is
 * ignored; a stale release after the slot went to a new owner cannot be detected (one owner per slot). */
void evt_pool_release(evt_t *e);

/* A delivery of `e` starts: its slot stops counting as queued, `delivered` of its tag grows. Takes stack
 * (slotless) events too. */
void evt_pool_mark_delivered(evt_t *e);

/* Saturate at 65535; evt_pool_stats() has the full values. `in_use` includes reserve slots, so for TINY it
 * can exceed the capacity by EVT_RELEASE_RESERVE. */
uint16_t evt_pool_in_use(evt_class_t cls);
uint16_t evt_pool_peak(evt_class_t cls);
uint16_t evt_pool_capacity(evt_class_t cls);

/* Diagnostics and tests only: scans every slot with the lock held. */
uint32_t evt_pool_in_use_by_tag(uint16_t tag);

/* Since init; dropped: acquisitions refused for an empty pool. */
typedef struct {
    uint32_t size;
    uint32_t in_use;
    uint32_t peak;
    uint32_t dropped;
} evt_pool_stats_t;

void evt_pool_stats(evt_class_t cls, evt_pool_stats_t *out);

/* Coalesce `tag`: false if tag >= EVT_MAX_TAGS. */
bool evt_tag_set_coalesce(uint16_t tag, bool on);

/* queued: acquired, neither delivered nor released yet. Zero for tag >= EVT_MAX_TAGS. */
typedef struct {
    uint32_t queued;
    uint32_t delivered;
    uint32_t dropped;
    uint32_t coalesced;
} evt_tag_stats_t;

void evt_tag_stats(uint16_t tag, evt_tag_stats_t *out);

/* ISR pool: TINY slots for interrupt context, drop-and-count when empty. Coalesce applies. */
#ifndef EVT_ISR_POOL_COUNT
#define EVT_ISR_POOL_COUNT  8
#endif

evt_t *evt_isr_acquire(uint16_t tag);

void evt_isr_release(evt_t *e);

uint32_t evt_isr_dropped(void);
uint16_t evt_isr_in_use(void);
uint16_t evt_isr_capacity(void);
void     evt_isr_stats(evt_pool_stats_t *out);

#ifndef EVT_QUEUE_DEPTH
#define EVT_QUEUE_DEPTH      64
#endif
#ifndef EVT_MAX_LISTENERS
#define EVT_MAX_LISTENERS    32
#endif
#ifndef EVT_FRAME_DEPTH_MAX
#define EVT_FRAME_DEPTH_MAX  16
#endif

/* Cascade limits: queue hops for dispatch(), nested frames for now(). */
#ifndef EVT_DISPATCH_DEPTH_MAX
#define EVT_DISPATCH_DEPTH_MAX  16
#endif
#ifndef EVT_NOW_DEPTH_MAX
#define EVT_NOW_DEPTH_MAX       8
#endif

typedef void (*evt_handler_fn)(evt_t *e, void *ctx);

/* Handlers fire in registration order. False if the registry is full. */
bool evt_listen(uint16_t tag, evt_handler_fn fn, void *ctx);

/* Sets e->depth for a dispatch from inside a frame (one hop deeper than the frame); false, counted in
 * dispatch_refused, beyond EVT_DISPATCH_DEPTH_MAX. Outside any frame e->depth is kept. */
bool evt_dispatch_prepare(evt_t *e);

/* evt_dispatch_prepare() and the ring queue, which takes the slot. False, slot kept, when refused or full. */
bool evt_dispatch(evt_t *e);

/* Runs the handlers now in a NOW frame; the caller keeps `e`. False when refused: beyond EVT_NOW_DEPTH_MAX
 * (now_refused) or EVT_FRAME_DEPTH_MAX (frame_refused). */
bool evt_now(evt_t *e);

/* Runs every queued event, each in a DISPATCH frame; returns how many. -1 (drain_refused) when called inside
 * a frame: a drain never nests. */
int evt_drain(void);

/* Listeners, ring queue, frames (without releasing) and the cascade counters; keeps the close hook. */
void evt_bus_reset(void);

typedef enum {
    EVT_FRAME_DISPATCH = 0,   /* delivery of a queued event; its depth is the event's */
    EVT_FRAME_NOW,            /* a now() call; counts toward EVT_NOW_DEPTH_MAX, no cascade hop */
} evt_frame_kind_t;

/* Opens a frame owning `owned` (released when the frame closes) or none (NULL). False, counted, beyond
 * EVT_FRAME_DEPTH_MAX or, for NOW, beyond EVT_NOW_DEPTH_MAX; the caller keeps `owned` then. */
bool evt_frame_open(evt_frame_kind_t kind, evt_t *owned);

/* Closes the innermost frame. */
void evt_frame_close(void);

int evt_frame_depth(void);

/* Closes the frames opened since `base`, innermost first. */
void evt_frames_unwind_to(int base);

/* Called for every frame that closes (close or unwind) with its index (0 = outermost), before its slot is
 * released: the caller's per-frame state (power holds) goes with it. NULL to remove. */
void evt_frames_on_close(void (*fn)(int index, void *ctx), void *ctx);

typedef struct {
    uint32_t dispatch_max_depth;
    uint32_t dispatch_refused;
    uint32_t now_max_depth;
    uint32_t now_refused;
    uint32_t frame_refused;
    uint32_t drain_refused;
} evt_cascade_stats_t;

void evt_cascade_stats(evt_cascade_stats_t *out);

uint32_t evt_dispatch_dropped(void);
uint32_t evt_now_rejected(void);
uint32_t evt_frame_refused(void);
uint32_t evt_drain_refused(void);
int      evt_now_depth(void);
