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

/* Baremetal\Event, Baremetal\Events, delivery frames, power_hold()/power_release() and the module. */
#include "php_events_internal.h"
#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

#include <stdio.h>

static zend_class_entry *bm_event_ce;
static zend_class_entry *bm_timer_ce;
static zend_class_entry *bm_pin_ce;
static zend_class_entry *bm_events_ce;

static bool      bm_ready;
static bool      bm_frozen;
static bool      bm_draining;
static HashTable bm_listeners;   /* class name (zend_string) -> zend_array of callables */

/* Events::dispatch() FIFO: slots whose payload holds the object (one reference each). */
static evt_t *bm_pend_head, *bm_pend_tail;

static void bm_lazy_init(void)
{
    if (bm_ready) {
        return;
    }
    zend_hash_init(&bm_listeners, 8, NULL, ZVAL_PTR_DTOR, 0);
    bm_ready = true;
}

void bm_events_freeze(void)
{
    bm_frozen = true;
}

/* ---- C watches (bm_events_watch) and idle hooks (bm_events_on_idle): process-wide, PHP task only ---- */

#define BM_WATCH_MAX 8
#define BM_IDLE_MAX  4

static struct { zend_class_entry *ce; bm_events_watch_fn fn; void *arg; } bm_watch[BM_WATCH_MAX];
static struct { void (*cb)(void *arg); void *arg; } bm_idle[BM_IDLE_MAX];

bool bm_events_watch(zend_class_entry *ce, bm_events_watch_fn fn, void *arg)
{
    int free_i = -1;
    for (int i = 0; i < BM_WATCH_MAX; i++) {
        if (bm_watch[i].fn == fn && bm_watch[i].ce == ce && bm_watch[i].arg == arg) {
            return true;
        }
        if (!bm_watch[i].fn && free_i < 0) {
            free_i = i;
        }
    }
    if (!ce || !fn || free_i < 0) {
        return false;
    }
    bm_watch[free_i].ce = ce;
    bm_watch[free_i].fn = fn;
    bm_watch[free_i].arg = arg;
    return true;
}

void bm_events_unwatch(zend_class_entry *ce, bm_events_watch_fn fn, void *arg)
{
    for (int i = 0; i < BM_WATCH_MAX; i++) {
        if (bm_watch[i].fn == fn && bm_watch[i].ce == ce && bm_watch[i].arg == arg) {
            bm_watch[i].fn = NULL;
            bm_watch[i].ce = NULL;
            bm_watch[i].arg = NULL;
        }
    }
}

bool bm_events_on_idle(void (*cb)(void *arg), void *arg)
{
    int free_i = -1;
    for (int i = 0; i < BM_IDLE_MAX; i++) {
        if (bm_idle[i].cb == cb && bm_idle[i].arg == arg) {
            return true;
        }
        if (!bm_idle[i].cb && free_i < 0) {
            free_i = i;
        }
    }
    if (!cb || free_i < 0) {
        return false;
    }
    bm_idle[free_i].cb = cb;
    bm_idle[free_i].arg = arg;
    return true;
}

void bm_events_off_idle(void (*cb)(void *arg), void *arg)
{
    for (int i = 0; i < BM_IDLE_MAX; i++) {
        if (bm_idle[i].cb == cb && bm_idle[i].arg == arg) {
            bm_idle[i].cb = NULL;
            bm_idle[i].arg = NULL;
        }
    }
}

void bm_run_idle(void)
{
    for (int i = 0; i < BM_IDLE_MAX; i++) {
        if (bm_idle[i].cb) {
            bm_idle[i].cb(bm_idle[i].arg);
        }
    }
}

/* The watches of the event's class, in table order; an exception stops them. */
static void bm_run_watches(zval *event)
{
    zend_class_entry *ce = Z_OBJCE_P(event);
    for (int i = 0; i < BM_WATCH_MAX && !EG(exception); i++) {
        if (bm_watch[i].fn && bm_watch[i].ce == ce) {
            bm_watch[i].fn(Z_OBJ_P(event), bm_watch[i].arg);
        }
    }
}

/* C watches, then the exact-class listeners; a false return stops the chain. */
void bm_deliver(zval *event)
{
    bm_run_watches(event);
    if (!bm_ready || EG(exception)) {
        return;
    }
    zval *list = zend_hash_find(&bm_listeners, Z_OBJCE_P(event)->name);
    if (!list || Z_TYPE_P(list) != IS_ARRAY) {
        return;
    }
    zval *cb;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(list), cb) {
        zval ret;
        ZVAL_UNDEF(&ret);
        if (call_user_function(NULL, NULL, cb, &ret, 1, event) != SUCCESS) {
            break;
        }
        bool stop = (Z_TYPE(ret) == IS_FALSE);
        zval_ptr_dtor(&ret);
        if (stop || EG(exception)) {
            break;
        }
    } ZEND_HASH_FOREACH_END();
}

/* ---- power_hold / power_release: owned by the outermost delivery, released when its frame closes ---- */

static bm_hal_pmlock_t *bm_pm_lock;
static bool             bm_pm_tried;
static bool             bm_power_save;
static int              bm_pm_base;    /* outside any delivery */
static int              bm_pm_outer;   /* during a delivery: the outermost frame's */
static bool             bm_pm_cascade; /* a dispatch() cascade started outside any frame owns bm_pm_outer */

static void bm_pm_ensure(void)
{
    if (!bm_pm_tried) {
        bm_pm_tried = true;
        /* NULL when power management is off: hold/release become no-ops */
        bm_pm_lock = bm_hal_pmlock_create("php_hold");
    }
}

static void bm_pm_drop(int *count)
{
    while (*count > 0) {
        bm_hal_pmlock_release(bm_pm_lock);
        (*count)--;
    }
}

static void bm_frame_closed(int index, void *ctx)
{
    (void) ctx;
    if (index == 0 && !bm_pm_cascade) {
        bm_pm_drop(&bm_pm_outer);
    }
}

void bm_events_power_autorelease(void)
{
    bm_pm_drop(&bm_pm_base);
    bm_pm_drop(&bm_pm_outer);
}

bool bm_events_power_save(void)
{
    return bm_power_save;
}

/* power_hold(): void -- until power_release() or the end of the outermost delivery (the reactor event, or
   the outermost dispatch()/now() call), also when taken inside a nested one */
PHP_FUNCTION(power_hold)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_pm_ensure();
    if (bm_pm_lock) {
        bm_hal_pmlock_acquire(bm_pm_lock);
        if (evt_frame_depth() > 0) {
            bm_pm_outer++;
        } else {
            bm_pm_base++;
        }
    }
}

/* power_release(): void -- the most recent hold */
PHP_FUNCTION(power_release)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (!bm_pm_lock) {
        return;
    }
    if (evt_frame_depth() > 0 && bm_pm_outer > 0) {
        bm_hal_pmlock_release(bm_pm_lock);
        bm_pm_outer--;
    } else if (bm_pm_base > 0) {
        bm_hal_pmlock_release(bm_pm_lock);
        bm_pm_base--;
    }
}

/* ---- frames and bailout ---- */

/* Gives back the slots of the events still waiting in the dispatch() FIFO; the objects too, unless the
   program is bailing out (no destructor runs then). */
static void bm_pending_discard(bool release_objects)
{
    while (bm_pend_head) {
        evt_t *e = bm_pend_head;
        bm_payload_t *p = e->payload;
        bm_pend_head = p->next;
        if (!bm_pend_head) {
            bm_pend_tail = NULL;   /* before the release: a destructor that dispatches starts a new list, */
        }                          /* which this loop then discards too */
        zend_object *obj = p->ptr;
        evt_pool_release(e);
        if (release_objects) {
            OBJ_RELEASE(obj);
        }
    }
    bm_slot_freed();
}

/* A fatal error (zend_bailout) inside a delivery: the program stops. Close the frames opened since `base`
   (their slots and holds go back), drop the waiting events and every hold, then let the bailout go on. */
static ZEND_NORETURN void bm_bailout(int base)
{
    evt_frames_unwind_to(base);
    bm_pending_discard(false);
    bm_draining = false;
    bm_pm_cascade = false;
    bm_events_power_autorelease();
    bm_slot_freed();
    zend_bailout();
}

static void bm_deliver_zv(void *arg)
{
    bm_deliver(arg);
}

/* fn(arg) with the frames opened since `base` closed on a bailout. Its own function: no caller local is
   live across the setjmp. */
static void bm_guarded(void (*fn)(void *arg), void *arg, int base)
{
    zend_try {
        fn(arg);
    } zend_catch {
        bm_bailout(base);
    } zend_end_try();
}

bool bm_run_frame(evt_t *e, void (*fn)(void *arg), void *arg)
{
    int base = evt_frame_depth();
    if (!evt_frame_open(EVT_FRAME_DISPATCH, e)) {
        evt_pool_release(e);
        bm_slot_freed();
        return false;
    }
    bm_guarded(fn, arg, base);
    evt_frame_close();
    bm_slot_freed();
    return true;
}

/* Breadth-first, in the call that started it; an exception drops what still waits. Started outside any
   frame, the whole cascade is one delivery for the power holds: each event has its own frame 0. */
static void bm_drain(void)
{
    bool outermost = evt_frame_depth() == 0;
    bm_pm_cascade = outermost;
    bm_draining = true;
    while (bm_pend_head && !EG(exception)) {
        evt_t *e = bm_pend_head;
        bm_payload_t *p = e->payload;
        bm_pend_head = p->next;
        if (!bm_pend_head) {
            bm_pend_tail = NULL;
        }
        zend_object *obj = p->ptr;
        zval event;
        ZVAL_OBJ(&event, obj);
        evt_pool_mark_delivered(e);
        bm_run_frame(e, bm_deliver_zv, &event);
        OBJ_RELEASE(obj);
    }
    if (bm_pend_head) {
        bm_pending_discard(true);
    }
    bm_draining = false;
    if (outermost) {
        bm_pm_cascade = false;
        bm_pm_drop(&bm_pm_outer);
    }
}

/* ---- Events ---- */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_events_listen, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, event, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, listener, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_events_now, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, event, Baremetal\\Event, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_events_dispatch, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_OBJ_INFO(0, event, Baremetal\\Event, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_events_stats, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Events::listen(string $event, callable $listener): void */
PHP_METHOD(Events, listen)
{
    zend_string *event;
    zval *cb;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(event)
        Z_PARAM_ZVAL(cb)
    ZEND_PARSE_PARAMETERS_END();

    if (bm_frozen) {
        zend_throw_exception(zend_ce_exception,
            "Events::listen() must be called in setup(); the listener table is frozen afterwards", 0);
        RETURN_THROWS();
    }
    if (!zend_is_callable(cb, 0, NULL)) {
        zend_argument_type_error(2, "must be a valid callback");
        RETURN_THROWS();
    }
    bm_lazy_init();

    zval *list = zend_hash_find(&bm_listeners, event);
    if (!list) {
        zval arr;
        array_init(&arr);
        list = zend_hash_add(&bm_listeners, event, &arr);
    }
    Z_TRY_ADDREF_P(cb);
    add_next_index_zval(list, cb);
}

/* Events::now(Event $event): void -- the listeners now, without a slot; at most 8 nested */
PHP_METHOD(Events, now)
{
    zval *event;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(event, bm_event_ce)
    ZEND_PARSE_PARAMETERS_END();

    int base = evt_frame_depth();
    /* Refused at 8 nested now(); a full frame stack (16) is unreachable today (reactor + drain + 8 now frames
       at most, drains never nest), so it shares the message. */
    if (!evt_frame_open(EVT_FRAME_NOW, NULL)) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "Events::now(): nested too deep (limit %d)",
                                EVT_NOW_DEPTH_MAX);
        RETURN_THROWS();
    }
    evt_t counted = { .tag = bm_class_tag(Z_OBJCE_P(event)->name, false) };
    evt_pool_mark_delivered(&counted);
    bm_guarded(bm_deliver_zv, event, base);
    evt_frame_close();
}

/* Events::dispatch(Event $event): bool -- breadth-first, in this call or in the drain already running;
   false (counted) when no small slot is free or beyond 16 levels of cascade */
PHP_METHOD(Events, dispatch)
{
    zval *event;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(event, bm_event_ce)
    ZEND_PARSE_PARAMETERS_END();

    bm_lazy_init();
    if (!bm_pools_ok) {
        RETURN_FALSE;
    }
    uint16_t tag = bm_class_tag(Z_OBJCE_P(event)->name, false);
    evt_t *e = evt_pool_acquire(EVT_CLASS_TINY, tag);
    if (!e) {
        bm_warn_class_drop(tag);
        RETURN_FALSE;
    }
    if (!evt_dispatch_prepare(e)) {
        evt_pool_release(e);
        bm_warn_cascade();
        RETURN_FALSE;
    }
    e->kind = BM_EVT_DISPATCH;
    bm_payload_t *p = e->payload;
    p->ptr = Z_OBJ_P(event);
    p->next = NULL;
    GC_ADDREF(Z_OBJ_P(event));
    if (bm_pend_tail) {
        ((bm_payload_t *) bm_pend_tail->payload)->next = e;
    } else {
        bm_pend_head = e;
    }
    bm_pend_tail = e;
    if (!bm_draining) {
        bm_drain();
    }
    RETURN_TRUE;
}

static const zend_function_entry events_methods[] = {
    PHP_ME(Events, listen,   arginfo_events_listen,   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Events, dispatch, arginfo_events_dispatch, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Events, now,      arginfo_events_now,      ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Events, stats,    arginfo_events_stats,    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* ---- C API for other extensions ---- */

void bm_events_deliver_object(zval *event)
{
    bm_deliver(event);
}

int bm_events_deliver_answering(zval *event, zval *ret_out)
{
    ZVAL_UNDEF(ret_out);
    if (!bm_ready) {
        return -2;
    }
    zval *list = zend_hash_find(&bm_listeners, Z_OBJCE_P(event)->name);
    if (!list || Z_TYPE_P(list) != IS_ARRAY || zend_hash_num_elements(Z_ARRVAL_P(list)) == 0) {
        return -2;
    }
    zval *cb;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(list), cb) {
        zval r;
        ZVAL_UNDEF(&r);
        if (call_user_function(NULL, NULL, cb, &r, 1, event) != SUCCESS) {
            zval_ptr_dtor(&r);
            break;
        }
        if (EG(exception)) {
            zval_ptr_dtor(&r);
            return -1;
        }
        if (Z_TYPE(r) == IS_OBJECT) {          /* first answer wins */
            ZVAL_COPY_VALUE(ret_out, &r);
            return 1;
        }
        zval_ptr_dtor(&r);
    } ZEND_HASH_FOREACH_END();
    return 0;
}

zend_class_entry *bm_events_base_ce(void)
{
    return bm_event_ce;
}

zend_class_entry *bm_events_timer_ce(void)
{
    return bm_timer_ce;
}

zend_class_entry *bm_events_pin_ce(void)
{
    return bm_pin_ce;
}

/* ---- $time, readonly payload properties ---- */

/* Property infos found once per (class, name): a small cache, emptied at request end because user classes (and
   their infos) are freed then. Internal classes are looked up again after that, which is cheap. */
#define BM_PROP_CACHE 16
static struct { zend_class_entry *ce; zend_property_info *info; } bm_prop_cache[BM_PROP_CACHE];
static int bm_prop_cache_n;

static void bm_prop_cache_reset(void)
{
    memset(bm_prop_cache, 0, sizeof bm_prop_cache);
    bm_prop_cache_n = 0;
}

static zend_property_info *bm_prop_info(zend_class_entry *ce, const char *name)
{
    for (int i = 0; i < bm_prop_cache_n; i++) {
        if (bm_prop_cache[i].ce == ce && strcmp(ZSTR_VAL(bm_prop_cache[i].info->name), name) == 0) {
            return bm_prop_cache[i].info;
        }
    }
    zend_property_info *info = zend_hash_str_find_ptr(&ce->properties_info, name, strlen(name));
    if (!info || (info->flags & ZEND_ACC_STATIC)) {
        return NULL;
    }
    if (bm_prop_cache_n < BM_PROP_CACHE) {
        bm_prop_cache[bm_prop_cache_n].ce = ce;
        bm_prop_cache[bm_prop_cache_n].info = info;
        bm_prop_cache_n++;
    }
    return info;
}

/* The slot of a property whose declared type accepts one of the MAY_BE_* bits in `mask` (or that is untyped);
   NULL otherwise. */
static zval *bm_prop_slot(zend_object *obj, zend_class_entry *ce, const char *name, uint32_t mask)
{
    zend_property_info *info = bm_prop_info(ce, name);
    ZEND_ASSERT(instanceof_function(obj->ce, ce));
    if (!info || (ZEND_TYPE_IS_SET(info->type) && !(ZEND_TYPE_PURE_MASK(info->type) & mask))) {
        return NULL;
    }
    zval *slot = OBJ_PROP(obj, info->offset);
    if (Z_TYPE_P(slot) != IS_UNDEF) {
        zval_ptr_dtor(slot);
    }
    return slot;
}

bool bm_event_set_prop_long(zend_object *obj, zend_class_entry *ce, const char *name, zend_long v)
{
    zval *slot = bm_prop_slot(obj, ce, name, MAY_BE_LONG);
    if (!slot) {
        return false;
    }
    ZVAL_LONG(slot, v);
    return true;
}

bool bm_event_set_prop_bool(zend_object *obj, zend_class_entry *ce, const char *name, bool v)
{
    zval *slot = bm_prop_slot(obj, ce, name, v ? MAY_BE_TRUE : MAY_BE_FALSE);
    if (!slot) {
        return false;
    }
    ZVAL_BOOL(slot, v);
    return true;
}

bool bm_event_set_prop_double(zend_object *obj, zend_class_entry *ce, const char *name, double v)
{
    zval *slot = bm_prop_slot(obj, ce, name, MAY_BE_DOUBLE);
    if (!slot) {
        return false;
    }
    ZVAL_DOUBLE(slot, v);
    return true;
}

bool bm_events_set_long(zend_object *event, zend_class_entry *ce, const char *name, long v)
{
    return bm_event_set_prop_long(event, ce, name, (zend_long) v);
}

bool bm_events_set_bool(zend_object *event, zend_class_entry *ce, const char *name, bool v)
{
    return bm_event_set_prop_bool(event, ce, name, v);
}

/* Whether a property of type `t` takes `v` as it is (no coercion): a type bit of the value, or an object of a
   class the type names. */
static bool bm_type_accepts(zend_type t, const zval *v)
{
    if (!ZEND_TYPE_IS_SET(t) || (ZEND_TYPE_PURE_MASK(t) & (1u << Z_TYPE_P(v)))) {
        return true;
    }
    if (Z_TYPE_P(v) != IS_OBJECT || !ZEND_TYPE_IS_COMPLEX(t)) {
        return false;
    }
    const zend_type *single;
    ZEND_TYPE_FOREACH(t, single) {
        if (ZEND_TYPE_HAS_NAME(*single)) {
            zend_class_entry *ce = zend_lookup_class_ex(ZEND_TYPE_NAME(*single), NULL, ZEND_FETCH_CLASS_NO_AUTOLOAD);
            if (ce && instanceof_function(Z_OBJCE_P(v), ce)) {
                return true;
            }
        }
    } ZEND_TYPE_FOREACH_END();
    return false;
}

bool bm_events_set_zval(zend_object *event, zend_class_entry *ce, const char *name, zval *v)
{
    zend_property_info *info = bm_prop_info(ce, name);
    ZEND_ASSERT(instanceof_function(event->ce, ce));
    if (!info || Z_TYPE_P(v) == IS_UNDEF || Z_ISREF_P(v) || !bm_type_accepts(info->type, v)) {
        return false;
    }
    zval *slot = OBJ_PROP(event, info->offset);
    zval old;
    ZVAL_COPY_VALUE(&old, slot);
    ZVAL_COPY(slot, v);
    zval_ptr_dtor(&old);   /* after the copy: v may be the old value itself */
    return true;
}

/* Event::$time, found once at MINIT: its offset is the same in every subclass. */
static zend_property_info *bm_time_info;

void bm_event_set_time(zend_object *obj, zend_class_entry *ce, int64_t us)
{
    (void) ce;
    zval *slot = OBJ_PROP(obj, bm_time_info->offset);
    if (Z_TYPE_P(slot) != IS_UNDEF) {
        zval_ptr_dtor(slot);
    }
#if SIZEOF_ZEND_LONG == 8
    ZVAL_LONG(slot, (zend_long) (us * 1000));
#else
    ZVAL_DOUBLE(slot, (double) us * 1000.0);
#endif
}

/* Every Event, also a PHP subclass and one made with `new`, starts with $time = now. */
static zend_object *bm_event_create(zend_class_entry *ce)
{
    zend_object *obj = zend_objects_new(ce);
    object_properties_init(obj, ce);
    bm_event_set_time(obj, bm_event_ce, bm_hal_now_us());
    return obj;
}

/* ---- module ---- */

/* The listener table, the dispatch queue and the pending releases live in request memory: drop them when the
   request ends (the web-server runtime runs one request per HTTP request), and release a power_hold() left. */
PHP_RSHUTDOWN_FUNCTION(events)
{
    bm_prop_cache_reset();
    evt_frames_unwind_to(0);
    bm_pending_discard(false);
    bm_release_request_end();
    bm_classes_request_end();
    if (bm_ready) {
        zend_hash_destroy(&bm_listeners);
        bm_ready = false;
    }
    bm_frozen = false;
    bm_draining = false;
    bm_pm_cascade = false;
    bm_events_power_autorelease();
    return SUCCESS;
}

PHP_MINIT_FUNCTION(events)
{
    zend_class_entry ce;

    bm_power_save = bm_hal_power_save();

    /* Baremetal\Event; `device` is the emitter, null for PHP-raised events */
    INIT_NS_CLASS_ENTRY(ce, "Baremetal", "Event", NULL);
    bm_event_ce = zend_register_internal_class(&ce);
    zend_declare_property_null(bm_event_ce, "device", sizeof("device") - 1, ZEND_ACC_PUBLIC);
    {
        zval undef;
        ZVAL_UNDEF(&undef);
        bm_time_info = zend_declare_typed_property(bm_event_ce, zend_string_init_interned("time", 4, 1), &undef,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_LONG | MAY_BE_DOUBLE));
        bm_event_ce->create_object = bm_event_create;

        /* Baremetal\TimerEvent: `ticks` is filled by every() */
        INIT_NS_CLASS_ENTRY(ce, "Baremetal", "TimerEvent", NULL);
        bm_timer_ce = zend_register_internal_class_ex(&ce, bm_event_ce);
        zend_declare_typed_property(bm_timer_ce, zend_string_init_interned("ticks", 5, 1), &undef,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type) ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));

        /* Baremetal\PinEvent: `pin` and `level` are filled by watch_gpio(), watch_gpios() and watch_button() */
        INIT_NS_CLASS_ENTRY(ce, "Baremetal", "PinEvent", NULL);
        bm_pin_ce = zend_register_internal_class_ex(&ce, bm_event_ce);
        zend_declare_typed_property(bm_pin_ce, zend_string_init_interned("pin", 3, 1), &undef,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type) ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
        zend_declare_typed_property(bm_pin_ce, zend_string_init_interned("level", 5, 1), &undef,
                                    ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL,
                                    (zend_type) ZEND_TYPE_INIT_CODE(_IS_BOOL, 0, 0));
    }

    INIT_NS_CLASS_ENTRY(ce, "Baremetal", "Events", events_methods);
    bm_events_ce = zend_register_internal_class(&ce);

    bm_source_minit();

    bm_ready = false;
    bm_frozen = false;
    bm_draining = false;

    bm_pools_ok = evt_pool_init();
    if (!bm_pools_ok) {
        char msg[256];
        evt_pool_init_error(msg, sizeof msg);
        printf("%s\n", msg);
        fflush(stdout);
    }
    bm_classes_init();
    evt_frames_on_close(bm_frame_closed, NULL);
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(events)
{
    memset(bm_watch, 0, sizeof bm_watch);
    memset(bm_idle, 0, sizeof bm_idle);
    bm_classes_shutdown();
    bm_source_shutdown();
    return SUCCESS;
}

zend_module_entry events_module_entry = {
    STANDARD_MODULE_HEADER,
    "events",
    bm_events_functions,
    PHP_MINIT(events),
    PHP_MSHUTDOWN(events),
    NULL,   /* RINIT */
    PHP_RSHUTDOWN(events),
    PHP_MINFO(events),
    "1.0",
    STANDARD_MODULE_PROPERTIES
};
