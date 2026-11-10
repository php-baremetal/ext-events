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

/* Shared by the PHP layer: php_events.c (Events, delivery, frames, power), php_events_queue.c (tags, slots,
 * queue, warnings), php_events_sources.c (every, watch_gpio, watch_button), php_events_source.c (Source, Timer) and php_events_stats.c. */
#pragma once

#include "php.h"
#include "event_bus.h"
#include "event_bus_php.h"
#include "bm_events_hal.h"

/* Fixed tags; the event classes take the next ones. Tags from EVT_MAX_TAGS on have no counters. */
#define BM_TAG_RELEASE 0
#define BM_TAG_HTTP    1
#define BM_TAG_WS      2
#define BM_TAG_FIRST   3
#define BM_TAG_NONE    ((uint16_t) EVT_MAX_TAGS)

#define BM_MAX_SOURCES 32

#define BM_HTTP_CLASS "Baremetal\\Http\\Request"
#define BM_WS_CLASS   "Baremetal\\Http\\Message"

/* What a small slot carries. */
typedef struct {
    void  *ptr;    /* emitter, handle, or the zend_object of a dispatch() */
    union {
        evt_t  *next;                     /* the dispatch() FIFO (dispatch() events carry no data) */
        uint8_t data[BM_EVENTS_DATA_MAX]; /* bm_events_emit_data(): data_len bytes */
    };
    int16_t pin;      /* GPIO sources: the pin, -1 when there is none */
    bool    level;    /* GPIO sources: the level read when the event was raised */
    uint8_t data_len; /* bytes in data, 0 for any other event */
} bm_payload_t;

/* An event class, by name (persistent: the counters live from boot). tag[0] plain, tag[1] coalescing. */
typedef struct {
    zend_string *name;
    uint16_t     tag[2];
    bool         warned;
    bool         shown;   /* listed by Events::stats() */
} bm_class_t;

typedef struct {
    bm_class_t       *cls;
    zend_class_entry *ce;        /* source tags: the class delivered, request lifetime */
    bool              coalesce;
    bool              source;
    bool              drop_pending;
} bm_tag_t;

extern bm_tag_t   bm_tags[EVT_MAX_TAGS];
extern bm_class_t bm_classes[EVT_MAX_TAGS];
extern int        bm_class_n;
extern bool       bm_pools_ok;
extern bm_hal_queue_t *bm_queue;

void bm_classes_init(void);
void bm_classes_shutdown(void);
void bm_classes_request_end(void);

/* The tag of a class name, created on first use; BM_TAG_NONE beyond the counted tags. PHP task only. */
uint16_t bm_class_tag(zend_string *name, bool coalesce);

/* From an ISR: an event of `tag` from the interrupt pool. */
void bm_events_emit_isr(uint16_t tag);

/* The same, with the GPIO the event is about: $pin and $level of a PinEvent (pin -1: none). */
bool bm_events_emit_pin(uint16_t tag, int pin, bool level);
bool bm_events_emit_isr_pin(uint16_t tag, int pin, bool level);

/* PHP task: prints the warnings recorded by other tasks, warns now for a drop of `tag` or for the cascade. */
void bm_flush_warnings(void);
void bm_warn_class_drop(uint16_t tag);
void bm_warn_cascade(void);

/* A small slot was given back on the PHP task: wakes a task waiting for one. */
void bm_slot_freed(void);

/* Runs fn(arg) in a DISPATCH frame owning `e`, inside zend_try; false (slot released) when refused. */
bool bm_run_frame(evt_t *e, void (*fn)(void *arg), void *arg);

/* Fillers per source tag, read by the PHP task only (bm_events_set_filler()). */
extern bm_event_filler_t bm_fillers[EVT_MAX_TAGS];

/* Writes a readonly property of an event object straight in its slot (also over an earlier value). */
bool bm_event_set_prop_long(zend_object *obj, zend_class_entry *ce, const char *name, zend_long v);
bool bm_event_set_prop_bool(zend_object *obj, zend_class_entry *ce, const char *name, bool v);
bool bm_event_set_prop_double(zend_object *obj, zend_class_entry *ce, const char *name, double v);
/* $time, in nanoseconds (int on a 64-bit zend_long, float otherwise), from bm_hal_now_us() microseconds. */
void bm_event_set_time(zend_object *obj, zend_class_entry *ce, int64_t us);

/* Delivers to the exact-class listeners. */
void bm_deliver(zval *event);
/* The hooks of bm_events_on_idle(), in table order (bm_events_receive(), bm_events_service()). */
void bm_run_idle(void);

/* Object releases whose messages have all been read. */
void bm_release_due(void);
void bm_release_request_end(void);

/* Baremetal\Source handles (php_events_source.c). The object a handle is: the C state of the source (`ops`,
 * `rec`) before the zend_object. */
typedef struct {
    const bm_source_ops_t *ops;
    void                  *rec;
    zend_object            std;
} bm_source_obj_t;

static inline bm_source_obj_t *bm_source_from(zend_object *o)
{
    return (bm_source_obj_t *) ((char *) o - XtOffsetOf(bm_source_obj_t, std));
}

/* MINIT: registers Source and Timer. MSHUTDOWN: frees the records given to bm_source_keep(). */
void bm_source_minit(void);
void bm_source_shutdown(void);

/* Registers a final subclass of Source (same create_object and handlers as Source): `name` is
 * "Baremetal\\Timer"-style, with the namespace separator as in the PHP name. */
void bm_source_register_class(zend_class_entry **out, const char *name, const zend_function_entry *methods);

/* Hands a persistent record (pemalloc'ed) to the module: at MSHUTDOWN ops->stop(rec) runs, then the record is
 * freed with pefree(rec, 1). */
void bm_source_keep(void *rec, const bm_source_ops_t *ops);

#define BM_GPIO_PINS 64   /* GPIO numbers 0..63 (the highest GPIO of any supported chip is 54) */

/* The GPIO ISR (php_events_sources.c) and watch_gpio()/watch_gpios(): a started watch of `pins` (`n` of them, in
 * the order of the call) with its handle. The ISR argument packs tag (bits 0-15), pin (16-23), bit 24 set when
 * the debounce is per pin. */
void bm_gpio_isr(void *arg);
zend_object *bm_gpio_watch_new(uint16_t tag, bool per_pin, const uint8_t *pins, int n);

/* MINIT (php_events_sources.c): registers Baremetal\Button, a Source subclass on the record of a button. */
void bm_button_minit(void);

/* The period every() and Timer::setPeriod() take (int|float ms): validated (ValueError) and converted.
 * False when it threw. */
bool bm_timer_period(zval *zms, uint64_t *period_us, double *period_ms);

/* every(): a started timer of `tag`; its handle, or NULL with an Exception thrown. */
zend_object *bm_timer_new(uint16_t tag, uint64_t period_us, double period_ms);

extern const zend_function_entry bm_events_functions[];
PHP_FUNCTION(power_hold);
PHP_FUNCTION(power_release);
PHP_METHOD(Events, stats);
PHP_MINFO_FUNCTION(events);

#define BM_POOLS_FAILED "events: the event pools could not be allocated"
