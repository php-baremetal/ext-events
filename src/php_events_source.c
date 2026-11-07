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

/* Baremetal\Source, the handle of a controllable event source, and Baremetal\Timer, the handle every() returns. */
#include <stdatomic.h>
#include "php_events_internal.h"
#include "zend_exceptions.h"

static zend_class_entry *bm_source_ce;
static zend_class_entry *bm_timer_ce;
static zend_class_entry *bm_gpio_ce;
static zend_object_handlers bm_source_handlers;

/* ---- Records of the sources, freed at MSHUTDOWN ---- */

typedef struct { void *rec; const bm_source_ops_t *ops; } bm_kept_t;
static bm_kept_t *bm_kept;
static size_t bm_kept_n, bm_kept_cap;

void bm_source_keep(void *rec, const bm_source_ops_t *ops)
{
    if (bm_kept_n == bm_kept_cap) {
        bm_kept_cap = bm_kept_cap ? bm_kept_cap * 2 : 8;
        bm_kept = perealloc(bm_kept, bm_kept_cap * sizeof(bm_kept_t), 1);   /* persistent: never NULL */
    }
    bm_kept[bm_kept_n].rec = rec;
    bm_kept[bm_kept_n].ops = ops;
    bm_kept_n++;
}

void bm_source_shutdown(void)
{
    /* A hardware callback may outlive the module (the board waits before it restarts): stop every source
       before its record goes. */
    for (size_t i = 0; i < bm_kept_n; i++) {
        if (bm_kept[i].ops && bm_kept[i].ops->stop) {
            bm_kept[i].ops->stop(bm_kept[i].rec);
        }
        pefree(bm_kept[i].rec, 1);
    }
    pefree(bm_kept, 1);
    bm_kept = NULL;
    bm_kept_n = bm_kept_cap = 0;
}

/* ---- The handle object ---- */

static zend_object *bm_source_create(zend_class_entry *ce)
{
    bm_source_obj_t *o = zend_object_alloc(sizeof(bm_source_obj_t), ce);
    o->ops = NULL;
    o->rec = NULL;
    zend_object_std_init(&o->std, ce);
    object_properties_init(&o->std, ce);
    o->std.handlers = &bm_source_handlers;
    return &o->std;
}

static void bm_source_free(zend_object *obj)
{
    bm_source_obj_t *o = bm_source_from(obj);
    if (o->ops && o->ops->release) {
        o->ops->release(o->rec);
    }
    zend_object_std_dtor(obj);
}

static zend_function *bm_source_get_constructor(zend_object *obj)
{
    zend_throw_error(NULL, "Cannot directly construct %s", ZSTR_VAL(obj->ce->name));
    return NULL;
}

zend_class_entry *bm_events_source_ce(void)
{
    return bm_source_ce;
}

void *bm_events_source_rec(zend_object *obj)
{
    if (!obj || !bm_source_ce || !instanceof_function(obj->ce, bm_source_ce)) {
        return NULL;
    }
    return bm_source_from(obj)->rec;
}

zend_object *bm_events_source_new(zend_class_entry *ce, const bm_source_ops_t *ops, void *rec)
{
    zval zv;
    if (object_init_ex(&zv, ce) != SUCCESS) {
        return NULL;
    }
    bm_source_obj_t *o = bm_source_from(Z_OBJ(zv));
    o->ops = ops;
    o->rec = rec;
    return Z_OBJ(zv);
}

void bm_source_register_class(zend_class_entry **out, const char *name, const zend_function_entry *methods)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY_EX(ce, name, strlen(name), methods);
    *out = zend_register_internal_class_ex(&ce, bm_source_ce);
    (*out)->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
}

/* ---- Baremetal\Source ---- */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_source_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_source_bool, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Source, stop)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_source_obj_t *o = bm_source_from(Z_OBJ_P(ZEND_THIS));
    if (o->ops && o->ops->stop) {
        o->ops->stop(o->rec);
    }
}

PHP_METHOD(Source, start)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_source_obj_t *o = bm_source_from(Z_OBJ_P(ZEND_THIS));
    if (o->ops && o->ops->start) {
        o->ops->start(o->rec);
    }
}

PHP_METHOD(Source, running)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_source_obj_t *o = bm_source_from(Z_OBJ_P(ZEND_THIS));
    RETURN_BOOL(o->ops && o->ops->running && o->ops->running(o->rec));
}

static const zend_function_entry bm_source_methods[] = {
    PHP_ME(Source, stop,    arginfo_source_void, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_ME(Source, start,   arginfo_source_void, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_ME(Source, running, arginfo_source_bool, ZEND_ACC_PUBLIC | ZEND_ACC_FINAL)
    PHP_FE_END
};

/* ---- Baremetal\Timer ---- */

typedef struct {
    uint16_t         tag;
    bm_hal_timer_t  *h;
    uint64_t         period_us;
    double           period_ms;
    atomic_bool      active;
} bm_timer_rec_t;

/* The timer task: a tick of a stopped timer is dropped (it may already have been in flight). */
static void bm_timer_cb(void *arg)
{
    bm_timer_rec_t *rec = arg;
    if (atomic_load(&rec->active)) {
        bm_events_emit(rec->tag, NULL);
    }
}

static void bm_timer_stop(void *r)
{
    bm_timer_rec_t *rec = r;
    if (atomic_load(&rec->active)) {
        atomic_store(&rec->active, false);
        if (rec->h) {
            bm_hal_timer_stop(rec->h);
        }
    }
}

static void bm_timer_start(void *r)
{
    bm_timer_rec_t *rec = r;
    if (!atomic_load(&rec->active)) {
        atomic_store(&rec->active, true);   /* before the start: the first tick must not be dropped */
        if (!bm_hal_timer_start_us(rec->h, rec->period_us)) {
            atomic_store(&rec->active, false);
        }
    }
}

static bool bm_timer_running(void *r)
{
    return atomic_load(&((bm_timer_rec_t *) r)->active);
}

static const bm_source_ops_t bm_timer_ops = { bm_timer_stop, bm_timer_start, bm_timer_running, NULL };

/* The period every() accepts, in milliseconds: from 100 us (each event costs a PHP call of tens of us) to the
   largest one events-hal 1.1 took (UINT32_MAX ms). */
#define BM_EVERY_MIN_MS 0.1
#define BM_EVERY_MAX_MS 4294967295.0

bool bm_timer_period(zval *zms, uint64_t *period_us, double *period_ms)
{
    double ms = Z_TYPE_P(zms) == IS_LONG ? (double) Z_LVAL_P(zms) : Z_DVAL_P(zms);
    if (!(ms >= BM_EVERY_MIN_MS)) {                  /* NaN included */
        zend_argument_value_error(1, "must be at least 0.1");
        return false;
    }
    if (ms > BM_EVERY_MAX_MS) {
        zend_argument_value_error(1, "must be at most 4294967295");
        return false;
    }
    *period_us = Z_TYPE_P(zms) == IS_LONG ? (uint64_t) Z_LVAL_P(zms) * 1000 : (uint64_t) (ms * 1000.0 + 0.5);
    *period_ms = ms;
    return true;
}

zend_object *bm_timer_new(uint16_t tag, uint64_t period_us, double period_ms)
{
    bm_timer_rec_t *rec = pecalloc(1, sizeof(*rec), 1);
    rec->tag = tag;
    rec->period_us = period_us;
    rec->period_ms = period_ms;
    bm_source_keep(rec, &bm_timer_ops);
    rec->h = bm_hal_timer_create(bm_timer_cb, rec, "every");
    atomic_store(&rec->active, true);
    if (!rec->h || !bm_hal_timer_start_us(rec->h, period_us)) {
        atomic_store(&rec->active, false);
        zend_throw_exception(zend_ce_exception, "every(): cannot start the timer", 0);
        return NULL;
    }
    return bm_events_source_new(bm_timer_ce, &bm_timer_ops, rec);
}

#define BM_TIMER_THIS(execute_data) ((bm_timer_rec_t *) bm_source_from(Z_OBJ_P(ZEND_THIS))->rec)

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_timer_setperiod, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_MASK(0, ms, MAY_BE_LONG|MAY_BE_DOUBLE, NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_timer_period, 0, 0, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Timer, setPeriod)
{
    zval *zms;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_NUMBER(zms)
    ZEND_PARSE_PARAMETERS_END();
    uint64_t us;
    double ms;
    if (!bm_timer_period(zms, &us, &ms)) {
        RETURN_THROWS();
    }
    bm_timer_rec_t *rec = BM_TIMER_THIS(execute_data);
    rec->period_us = us;
    rec->period_ms = ms;
    if (atomic_load(&rec->active)) {
        /* A started periodic timer cannot be started again: stop it first. */
        bm_hal_timer_stop(rec->h);
        if (!bm_hal_timer_start_us(rec->h, us)) {
            atomic_store(&rec->active, false);
            zend_throw_exception(zend_ce_exception, "every(): cannot start the timer", 0);
            RETURN_THROWS();
        }
    }
}

PHP_METHOD(Timer, period)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_DOUBLE(BM_TIMER_THIS(execute_data)->period_ms);
}

static const zend_function_entry bm_timer_methods[] = {
    PHP_ME(Timer, setPeriod, arginfo_timer_setperiod, ZEND_ACC_PUBLIC)
    PHP_ME(Timer, period,    arginfo_timer_period,    ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ---- Baremetal\GpioWatch ---- */

#define BM_GPIO_ARG(tag, pin, per_pin) \
    ((void *) (uintptr_t) ((uint32_t) (tag) | ((uint32_t) (pin) << 16) | ((per_pin) ? (1u << 24) : 0u)))

typedef struct {
    uint16_t tag;
    bool     per_pin;
    bool     active;
    int      n;
    uint8_t  pins[];
} bm_gpio_rec_t;

/* Who watches each pin (PHP task): the last watch of a pin owns it. */
static bm_gpio_rec_t *bm_gpio_owner[BM_GPIO_PINS];

static void bm_gpio_attach(bm_gpio_rec_t *rec)
{
    for (int i = 0; i < rec->n; i++) {
        int pin = rec->pins[i];
        bm_gpio_owner[pin] = rec;
        bm_hal_gpio_on_falling(pin, bm_gpio_isr, BM_GPIO_ARG(rec->tag, pin, rec->per_pin));
    }
}

static bool bm_gpio_running(void *r)
{
    bm_gpio_rec_t *rec = r;
    if (!rec->active) {
        return false;
    }
    for (int i = 0; i < rec->n; i++) {
        if (bm_gpio_owner[rec->pins[i]] == rec) {
            return true;
        }
    }
    return false;
}

static void bm_gpio_stop(void *r)
{
    bm_gpio_rec_t *rec = r;
    if (rec->active) {
        for (int i = 0; i < rec->n; i++) {
            int pin = rec->pins[i];
            if (bm_gpio_owner[pin] == rec) {
                bm_hal_gpio_off(pin);
                bm_gpio_owner[pin] = NULL;
            }
        }
        rec->active = false;
    }
}

static void bm_gpio_start(void *r)
{
    bm_gpio_rec_t *rec = r;
    if (!bm_gpio_running(rec)) {
        bm_gpio_attach(rec);
        rec->active = true;
    }
}

static const bm_source_ops_t bm_gpio_ops = { bm_gpio_stop, bm_gpio_start, bm_gpio_running, NULL };

zend_object *bm_gpio_watch_new(uint16_t tag, bool per_pin, const uint8_t *pins, int n)
{
    bm_gpio_rec_t *rec = pecalloc(1, sizeof(*rec) + (size_t) n, 1);
    rec->tag = tag;
    rec->per_pin = per_pin;
    rec->n = n;
    memcpy(rec->pins, pins, (size_t) n);
    bm_source_keep(rec, &bm_gpio_ops);
    bm_gpio_attach(rec);
    rec->active = true;
    return bm_events_source_new(bm_gpio_ce, &bm_gpio_ops, rec);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gpio_pins, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(GpioWatch, pins)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_gpio_rec_t *rec = bm_source_from(Z_OBJ_P(ZEND_THIS))->rec;
    array_init_size(return_value, (uint32_t) rec->n);
    for (int i = 0; i < rec->n; i++) {
        add_next_index_long(return_value, rec->pins[i]);
    }
}

static const zend_function_entry bm_gpio_methods[] = {
    PHP_ME(GpioWatch, pins, arginfo_gpio_pins, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void bm_source_minit(void)
{
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal", "Source", bm_source_methods);
    bm_source_ce = zend_register_internal_class(&ce);
    bm_source_ce->ce_flags |= ZEND_ACC_ABSTRACT | ZEND_ACC_NOT_SERIALIZABLE;
    bm_source_ce->create_object = bm_source_create;

    memcpy(&bm_source_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    bm_source_handlers.offset = XtOffsetOf(bm_source_obj_t, std);
    bm_source_handlers.free_obj = bm_source_free;
    bm_source_handlers.clone_obj = NULL;
    bm_source_handlers.get_constructor = bm_source_get_constructor;

    bm_source_register_class(&bm_timer_ce, "Baremetal\\Timer", bm_timer_methods);
    bm_source_register_class(&bm_gpio_ce, "Baremetal\\GpioWatch", bm_gpio_methods);
    bm_button_minit();
}
