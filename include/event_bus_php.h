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

/* PHP side of the event bus: Baremetal\Event, Baremetal\Events and the reactor queue. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct _zend_class_entry;
struct _zend_string;
struct _zval_struct;

/* After this, Events::listen() throws. */
void bm_events_freeze(void);

/* Reactor queue message. SOURCE: `ptr` is the emitter or NULL; HTTP/WS: `ptr` is the handler's handle. */
#define BM_EVT_SOURCE 0
#define BM_EVT_HTTP   1
#define BM_EVT_WS     2
#define BM_EVT_RELEASE 3   /* `ptr` is a zend_object to release once the messages queued before it are delivered */
typedef struct { uint16_t tag; uint8_t kind; void *ptr; } bm_event_msg_t;

/* UINT32_MAX = forever. False on timeout or no queue. */
bool bm_events_receive(bm_event_msg_t *out, uint32_t ms);

/* Reactor only. */
void bm_events_deliver_msg(const bm_event_msg_t *msg);

/* Drop any power_hold() left after a handler. */
void bm_events_power_autorelease(void);

/* Whether the build enabled power-save (from the HAL, valid once the module started). */
bool bm_events_power_save(void);

/* Baremetal\Event, for extensions that register event subclasses. */
struct _zend_class_entry *bm_events_base_ce(void);

/* False if the class is unknown or the source table is full. */
bool bm_events_source_tag(struct _zend_string *class_name, uint16_t *out_tag);

/* Safe from any task; dropped when the queue is full. */
void bm_events_emit(uint16_t tag, void *device);
/* Release an emitter object passed to bm_events_emit() after the messages already queued (which may
 * carry it) are delivered: now when there is no queue; kept forever if the queue is full. PHP task only. */
struct _zend_object;
void bm_events_release_later(struct _zend_object *obj);

void bm_events_ensure_queue(void);

/* False if there is no queue or it is full. */
bool bm_events_post_http(void *handle);

void bm_events_set_http_handler(void (*fn)(void *handle));

/* Set by the HTTP server: answers a parked request with `status` when no handler takes it. */
void bm_events_set_http_release(void (*fn)(void *handle, int status));

/* Blocks up to wait_ms for room. False: the caller frees the handle. */
bool bm_events_post_ws(void *handle, uint32_t wait_ms);

void bm_events_set_ws_handler(void (*fn)(void *handle));

void bm_events_deliver_object(struct _zval_struct *event);

/* First object returned goes to ret_out. 1 answered, 0 none answered, -1 threw, -2 no listener. */
int bm_events_deliver_answering(struct _zval_struct *event, struct _zval_struct *ret_out);
