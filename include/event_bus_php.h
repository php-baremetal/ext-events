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

/* PHP side of the event bus: Baremetal\Event, Baremetal\Events and the reactor queue. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "event_bus.h"

struct _zend_class_entry;
struct _zend_string;
struct _zval_struct;
struct _zend_object;

/* After this, Events::listen() throws. */
void bm_events_freeze(void);

/* Reactor message, read from an event slot. SOURCE: `ptr` is the emitter or NULL; HTTP/WS: `ptr` is the
 * handler's handle; RELEASE: `ptr` is a zend_object to release once the messages queued before it are
 * delivered. DISPATCH marks the slots of Events::dispatch(), which never reach the queue. `slot` is the
 * event slot, given back when bm_events_deliver_msg() is done. */
#define BM_EVT_SOURCE   0
#define BM_EVT_HTTP     1
#define BM_EVT_WS       2
#define BM_EVT_RELEASE  3
#define BM_EVT_DISPATCH 4
typedef struct { uint16_t tag; uint8_t kind; void *ptr; void *slot; } bm_event_msg_t;

/* Since 1.2: C work that must run each time the PHP task returns to the event loop (Slate hands over the scene
 * changes a listener made). cb(arg) runs on the PHP task at the start of every bm_events_receive() (so after each
 * delivered message, before the reactor takes the next one or waits) and once at the end of bm_events_service()
 * (init-loop, after each loop()). It runs outside any PHP frame: plain C only, no PHP calls, no exceptions, short.
 * At most 4 hooks; the same (cb, arg) twice is kept once. False when the table is full. PHP task only. */
bool bm_events_on_idle(void (*cb)(void *arg), void *arg);
/* Removes a hook of bm_events_on_idle() (no-op when absent). PHP task only. */
void bm_events_off_idle(void (*cb)(void *arg), void *arg);

/* Since 1.2: a C listener of one event class (exact class, as Events::listen()), for an extension that reacts to
 * events without a PHP callable. fn(event, arg) runs on the PHP task before the PHP listeners of the class, in the
 * delivery frame (it may call PHP; an exception it leaves stops the delivery like a listener's). Unlike
 * Events::listen() it is never frozen, and it stays until bm_events_unwatch(). At most 8 watches; the same
 * (ce, fn, arg) twice is kept once. False when the table is full. PHP task only. */
typedef void (*bm_events_watch_fn)(struct _zend_object *event, void *arg);
bool bm_events_watch(struct _zend_class_entry *ce, bm_events_watch_fn fn, void *arg);
/* Removes a watch of bm_events_watch() (no-op when absent). PHP task only. */
void bm_events_unwatch(struct _zend_class_entry *ce, bm_events_watch_fn fn, void *arg);

/* UINT32_MAX = forever. False on timeout or no queue. */
bool bm_events_receive(bm_event_msg_t *out, uint32_t ms);

/* Reactor only, for every message received: delivers it inside a frame (its slot and the power_hold()s taken
 * meanwhile go back at the end, or on a fatal error before the bailout goes on), then runs the releases due
 * and prints the pending warnings. */
void bm_events_deliver_msg(const bm_event_msg_t *msg);

/* Runtimes that never deliver events (init-loop), once per loop on the PHP task: reads the queue, runs the
 * object releases, gives the slots of source events back without delivering them (HTTP requests get 503),
 * and prints the pending warnings. */
void bm_events_service(void);

/* Drop any power_hold() left after a handler. */
void bm_events_power_autorelease(void);

/* Whether the build enabled power-save (from the HAL, valid once the module started). */
bool bm_events_power_save(void);

/* Baremetal\Event, for extensions that register event subclasses. */
struct _zend_class_entry *bm_events_base_ce(void);

/* Baremetal\TimerEvent (`public readonly int $ticks`), the base of the classes every() fills with the ticks
 * coalesced into the event. */
struct _zend_class_entry *bm_events_timer_ce(void);

/* Baremetal\PinEvent (`public readonly int $pin`, `public readonly bool $level`), the base of the classes
 * watch_gpio(), watch_gpios() and watch_button() fill with the GPIO and its level. */
struct _zend_class_entry *bm_events_pin_ce(void);

/* A per-tag filler: runs on the PHP task for each event of a source `tag` just before it is delivered, to
 * write the payload properties of the event object from the slot (`slot->merged`, `slot->time_us`, the slot
 * payload) and from the emitter `device` (NULL when there is none). Read-only for the slot. */
typedef void (*bm_event_filler_t)(struct _zend_object *event, struct _zend_object *device, const evt_t *slot);

/* Registers (or with NULL removes) the filler of `tag`. A tag has one filler: the last registration wins. False when tag >= EVT_MAX_TAGS. PHP task only. */
bool bm_events_set_filler(uint16_t tag, bm_event_filler_t fn);

/* For fillers: initialise the public readonly property `name` of `event` (an object of `ce` or a subclass)
 * straight in its slot, bypassing the readonly scope rules; a later PHP write still throws. `event` must be an
 * instance of `ce`. False when the class has no such property or its declared type does not accept the value
 * (int for _long, bool for _bool). The property is looked up once per (class, name) and cached. */
bool bm_events_set_long(struct _zend_object *event, struct _zend_class_entry *ce, const char *name, long v);
bool bm_events_set_bool(struct _zend_object *event, struct _zend_class_entry *ce, const char *name, bool v);
/* As bm_events_set_long(), for a value of any type (a string, an array, an enum case or another object, null):
 * the property gets its own copy of `v` (one more reference), `v` stays the caller's. False when the class has no
 * such property, `v` is undefined or a reference, or the declared type does not take `v` as it is (no coercion). */
bool bm_events_set_zval(struct _zend_object *event, struct _zend_class_entry *ce, const char *name,
                        struct _zval_struct *v);

/* False if the class is unknown or the source table is full (or the event pools could not be allocated:
 * then with an exception). bm_events_source_tag() does not coalesce; with bm_events_source_tag_ex() and
 * `coalesce`, an event of the class is not queued while one from a coalescing source already waits. */
/* While serving an HTTP request: false and an exception naming the calling PHP function. */
bool bm_events_source_tag(struct _zend_string *class_name, uint16_t *out_tag);
bool bm_events_source_tag_ex(struct _zend_string *class_name, bool coalesce, uint16_t *out_tag);

/* Safe from any task; when no small slot is free the event is dropped and counted for its class (the first
 * drop prints a warning on the PHP task). */
void bm_events_emit(uint16_t tag, void *device);

/* Interrupt context: the interrupt pool, the tag's coalesce, time_us at acquire. device as for bm_events_emit(). */
void bm_events_emit_from_isr(uint16_t tag, void *device);

/* Bytes an event can carry next to its device (bm_events_emit_data()). */
#define BM_EVENTS_DATA_MAX 16
/* As bm_events_emit() (any task, not an ISR), with len <= BM_EVENTS_DATA_MAX bytes copied into the slot.
 * False when dropped (counted as for bm_events_emit()) or len is too large. */
bool bm_events_emit_data(uint16_t tag, void *device, const void *data, size_t len);

/* In a filler: the bytes given to bm_events_emit_data(), and their length (0 for any other event). */
const void *bm_events_slot_data(const evt_t *slot, size_t *len);
/* Release an emitter object passed to bm_events_emit() after the messages already queued (which may
 * carry it) are delivered: now when there is no queue. Takes a slot of the release reserve; with none free,
 * the release waits for the reactor to read the messages queued so far. PHP task only. */
struct _zend_object;
void bm_events_release_later(struct _zend_object *obj);

void bm_events_ensure_queue(void);

/* False if there is no queue or no small slot is free (the caller answers 503). */
bool bm_events_post_http(void *handle);

void bm_events_set_http_handler(void (*fn)(void *handle));

/* Set by the HTTP server: answers a parked request with `status` when no handler takes it. */
void bm_events_set_http_release(void (*fn)(void *handle, int status));

/* Blocks up to wait_ms for a free small slot. False: the caller frees the handle. */
bool bm_events_post_ws(void *handle, uint32_t wait_ms);

void bm_events_set_ws_handler(void (*fn)(void *handle));

void bm_events_deliver_object(struct _zval_struct *event);

/* First object returned goes to ret_out. 1 answered, 0 none answered, -1 threw, -2 no listener. */
int bm_events_deliver_answering(struct _zval_struct *event, struct _zval_struct *ret_out);

/* Baremetal\Source: the abstract base of the handles the sources return (every(), and the packages' own).
 * A handle controls its source; it neither keeps it alive nor stops it when it is freed (`release`, when set,
 * is called then, and the source keeps running). `rec` belongs to the source and must outlive the handle: a
 * persistent allocation, or one registered with the package's own shutdown. */
typedef struct {
    void (*stop)(void *rec);
    void (*start)(void *rec);
    bool (*running)(void *rec);
    void (*release)(void *rec);   /* NULL, or called when the handle is freed (the source keeps running) */
} bm_source_ops_t;

struct _zend_class_entry *bm_events_source_ce(void);

/* A new handle of ce (a subclass of Baremetal\Source) on rec; rec belongs to the source, never to the handle.
 * Returns the object with one reference. */
struct _zend_object *bm_events_source_new(struct _zend_class_entry *ce, const bm_source_ops_t *ops, void *rec);

/* The rec of a Baremetal\Source handle, or NULL if obj is not one. */
void *bm_events_source_rec(struct _zend_object *obj);
