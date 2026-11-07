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

/* The event sources: every(), watch_gpio(), watch_gpios(), watch_button(), button_sampling(), and the source tags of the C API. */
#include <stdatomic.h>
#include "php_events_internal.h"
#include "SAPI.h"
#include "zend_exceptions.h"

#define BM_GPIO_DEBOUNCE_US 200000   /* 200 ms */

static volatile int64_t bm_gpio_last[EVT_MAX_TAGS];       /* watch_gpio(): per class */
static volatile int64_t bm_gpio_last_pin[BM_GPIO_PINS];   /* watch_gpios(): per pin, shared by every class watching the pin */
static int              bm_source_n;

/* Class name -> source tag, allocated on first use. -1 on failure. */
static int bm_source_tag(zend_string *class_name, bool coalesce)
{
    zend_class_entry *ce = zend_lookup_class(class_name);
    if (!ce || (ce->ce_flags & (ZEND_ACC_ABSTRACT | ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT | ZEND_ACC_ENUM))) {
        return -1;   /* no object of it can be created at delivery */
    }
    uint16_t tag = bm_class_tag(ce->name, coalesce);
    if (tag == BM_TAG_NONE) {
        return -1;
    }
    if (!bm_tags[tag].source) {
        if (bm_source_n >= BM_MAX_SOURCES) {
            return -1;
        }
        bm_events_ensure_queue();
        if (!bm_queue) {
            return -1;
        }
        bm_tags[tag].source = true;
        bm_source_n++;
    }
    bm_tags[tag].ce = ce;
    return tag;
}

static bool bm_sources_refused(const char *fn);

/* The pools, and the queue the sources post to, could not be allocated: throws. */
static bool bm_pools_missing(void)
{
    if (bm_pools_ok) {
        bm_events_ensure_queue();
        if (bm_queue) {
            return false;
        }
    }
    zend_throw_exception(zend_ce_exception, BM_POOLS_FAILED, 0);
    return true;
}

bool bm_events_source_tag_ex(zend_string *class_name, bool coalesce, uint16_t *out_tag)
{
    /* The message names the PHP function that asked (poll(), wifi_events(), onFlushed(), ...). */
    const char *fn = get_active_function_name();
    if (bm_sources_refused(fn != NULL && fn[0] != '\0' ? fn : "poll") || bm_pools_missing()) {   /* throws: the caller must not throw again */
        return false;
    }
    int tag = bm_source_tag(class_name, coalesce);
    if (tag < 0) {
        return false;
    }
    *out_tag = (uint16_t) tag;
    return true;
}

bool bm_events_source_tag(zend_string *class_name, uint16_t *out_tag)
{
    return bm_events_source_tag_ex(class_name, false, out_tag);
}

void bm_gpio_isr(void *arg)
{
    uint32_t a = (uint32_t) (uintptr_t) arg;
    uint16_t tag = (uint16_t) (a & 0xFFFF);
    int pin = (int) ((a >> 16) & 0xFF);
    volatile int64_t *last = (a & (1u << 24)) ? &bm_gpio_last_pin[pin] : &bm_gpio_last[tag];
    int64_t now = bm_hal_now_us();
    if (now - *last < BM_GPIO_DEBOUNCE_US) {
        return;
    }
    *last = now;
    bm_events_emit_isr_pin(tag, pin, bm_hal_gpio_level(pin) != 0);
}

/* ---- Buttons: a debounced per-pin state machine run by one sampler timer ---- */

#define BM_MAX_BUTTONS 4
#define BM_BTN_TICK_MS 10

enum { BTN_PRESSED, BTN_RELEASED, BTN_HELD, BTN_CLICK, BTN_REPEAT, BTN_DOUBLE, BTN_EVENTS };

typedef struct {
    bool    in_use;
    int     pin;
    bool    active_low;
    int     tag[BTN_EVENTS];      /* -1: not wired */
    int     hold_ms, repeat_ms, double_ms, debounce_ms;
    bool    pressed;              /* debounced level */
    int     stable_ms;
    int64_t press_t, last_repeat, last_click;
    bool    held_fired, click_pending;
    atomic_bool enabled;          /* stop()/start() of the handle; the sampler skips a disabled button */
    bool    ignore_until_release; /* a press held at start(): nothing is raised until it is released */
} bm_button_t;

static bm_button_t        bm_buttons[BM_MAX_BUTTONS];
static int                bm_button_n;
static bm_hal_timer_t    *bm_btn_timer;
static bool               bm_btn_running;

static void bm_btn_emit(bm_button_t *b, int ev)
{
    if (b->tag[ev] >= 0 && !b->ignore_until_release) {
        /* The electrical level after debounce: `pressed` is logical, an active-low button pressed is low. */
        bm_events_emit_pin((uint16_t) b->tag[ev], b->pin, b->pressed != b->active_low);
    }
}

static void bm_btn_tick(void *arg)
{
    (void) arg;
    int64_t now = bm_hal_now_us();
    for (int i = 0; i < bm_button_n; i++) {
        bm_button_t *b = &bm_buttons[i];
        if (!b->in_use || !atomic_load(&b->enabled)) {
            continue;
        }
        int lvl = bm_hal_gpio_level(b->pin);
        bool raw = b->active_low ? (lvl == 0) : (lvl != 0);

        if (raw == b->pressed) {
            b->stable_ms = 0;
        } else {
            b->stable_ms += BM_BTN_TICK_MS;
            if (b->stable_ms >= b->debounce_ms) {
                b->pressed = raw;
                b->stable_ms = 0;
                if (raw) {
                    bm_btn_emit(b, BTN_PRESSED);
                    b->press_t = now;
                    b->held_fired = false;
                    b->last_repeat = now;
                } else if (b->ignore_until_release) {
                    b->ignore_until_release = false;      /* the press seen at start() ends: nothing to raise */
                    b->held_fired = true;
                    b->click_pending = false;
                } else {
                    bm_btn_emit(b, BTN_RELEASED);
                    if (!b->held_fired && (now - b->press_t) < (int64_t) b->hold_ms * 1000) {
                        if (b->double_ms > 0) {
                            if (b->click_pending && (now - b->last_click) < (int64_t) b->double_ms * 1000) {
                                bm_btn_emit(b, BTN_DOUBLE);
                                b->click_pending = false;
                            } else {
                                b->click_pending = true;
                                b->last_click = now;
                            }
                        } else {
                            bm_btn_emit(b, BTN_CLICK);
                        }
                    }
                }
            }
        }

        if (b->pressed) {
            if (!b->held_fired && (now - b->press_t) >= (int64_t) b->hold_ms * 1000) {
                bm_btn_emit(b, BTN_HELD);
                b->held_fired = true;
                b->last_repeat = now;
            } else if (b->held_fired && b->repeat_ms > 0 &&
                       (now - b->last_repeat) >= (int64_t) b->repeat_ms * 1000) {
                bm_btn_emit(b, BTN_REPEAT);
                b->last_repeat = now;
            }
        }
        if (b->click_pending && (now - b->last_click) >= (int64_t) b->double_ms * 1000) {
            bm_btn_emit(b, BTN_CLICK);                        /* no second click */
            b->click_pending = false;
        }
    }
}

static void bm_btn_run(void)
{
    if (!bm_btn_timer) {
        bm_btn_timer = bm_hal_timer_create(bm_btn_tick, NULL, "buttons");
        if (!bm_btn_timer) {
            return;
        }
    }
    if (!bm_btn_running) {
        bm_hal_timer_start(bm_btn_timer, BM_BTN_TICK_MS);
        bm_btn_running = true;
    }
}

static void bm_btn_pause(void)
{
    if (bm_btn_timer && bm_btn_running) {
        bm_hal_timer_stop(bm_btn_timer);
        bm_btn_running = false;
    }
}

/* ---- Baremetal\Button: the handle watch_button() returns. The record is the entry of bm_buttons[]. ---- */

static zend_class_entry *bm_button_ce;

static void bm_button_stop(void *r)
{
    atomic_store(&((bm_button_t *) r)->enabled, false);
}

static void bm_button_start(void *r)
{
    bm_button_t *b = r;
    if (atomic_load(&b->enabled)) {
        return;
    }
    b->stable_ms = 0;
    b->held_fired = false;
    b->click_pending = false;
    b->last_click = 0;
    b->last_repeat = 0;
    int lvl = bm_hal_gpio_level(b->pin);
    b->pressed = b->active_low ? (lvl == 0) : (lvl != 0);
    b->ignore_until_release = b->pressed;
    atomic_store(&b->enabled, true);
}

static bool bm_button_running(void *r)
{
    return atomic_load(&((bm_button_t *) r)->enabled);
}

static const bm_source_ops_t bm_button_ops = { bm_button_stop, bm_button_start, bm_button_running, NULL };

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_button_pin, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Button, pin)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(((bm_button_t *) bm_events_source_rec(Z_OBJ_P(ZEND_THIS)))->pin);
}

static const zend_function_entry bm_button_methods[] = {
    PHP_ME(Button, pin, arginfo_button_pin, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void bm_button_minit(void)
{
    bm_source_register_class(&bm_button_ce, "Baremetal\\Button", bm_button_methods);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_every, 0, 2, Baremetal\\Timer, 0)
    ZEND_ARG_TYPE_MASK(0, ms, MAY_BE_LONG|MAY_BE_DOUBLE, NULL)
    ZEND_ARG_TYPE_INFO(0, event, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, coalesce, _IS_BOOL, 0, "true")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_watch_gpio, 0, 2, Baremetal\\GpioWatch, 0)
    ZEND_ARG_TYPE_INFO(0, pin, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, event, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, coalesce, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

/* The web-server runtime serves each HTTP request as its own PHP request: nothing delivers source events there,
   and a source would outlive the request that made it. The runtime says so for its whole run (its init script
   included); a request being served (any SAPI) says so too. */
static bool bm_sources_refused(const char *fn)
{
    if (!bm_hal_request_scoped() && SG(request_info).request_method == NULL) {
        return false;
    }
    zend_throw_exception_ex(zend_ce_exception, 0,
        "%s() is not available in the web-server runtime: event sources need the event-driven runtime", fn);
    return true;
}

/* TimerEvent::$ticks: this tick plus the ticks coalesced into the event while it waited. */
static void bm_timer_fill(zend_object *event, zend_object *device, const evt_t *slot)
{
    (void) device;
    /* Saturates at 2^32-1 (merged is a saturating uint32_t); where zend_long is 32-bit signed (the board) the
       bound is ZEND_LONG_MAX, so the clamp is the smaller of the two on every build. */
    uint64_t n = (uint64_t) 1 + slot->merged;
    uint64_t max = (uint64_t) ZEND_LONG_MAX < (uint64_t) UINT32_MAX ? (uint64_t) ZEND_LONG_MAX : (uint64_t) UINT32_MAX;
    bm_event_set_prop_long(event, bm_events_timer_ce(), "ticks", (zend_long) (n > max ? max : n));
}

/* PinEvent::$pin and $level, from the slot payload. */
static void bm_pin_fill(zend_object *event, zend_object *device, const evt_t *slot)
{
    (void) device;
    const bm_payload_t *p = (const bm_payload_t *) slot->payload;
    if (p->pin < 0) {
        return;
    }
    bm_events_set_long(event, bm_events_pin_ce(), "pin", p->pin);
    bm_events_set_bool(event, bm_events_pin_ce(), "level", p->level);
}

/* Registers the filler on a tag whose class extends PinEvent (a tag has one filler, so never write NULL). */
static void bm_pin_filler_for(int tag)
{
    if (instanceof_function(bm_tags[tag].ce, bm_events_pin_ce())) {
        bm_events_set_filler((uint16_t) tag, bm_pin_fill);
    }
}

/* every(int|float $ms, string $event, bool $coalesce = true): Baremetal\Timer -- the period is rounded to the nearest
   microsecond; with $coalesce a tick is not queued while one of the class already waits */
PHP_FUNCTION(every)
{
    zval *zms;
    zend_string *event;
    bool coalesce = true;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_NUMBER(zms)
        Z_PARAM_STR(event)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(coalesce)
    ZEND_PARSE_PARAMETERS_END();
    if (bm_sources_refused("every")) {
        RETURN_THROWS();
    }
    uint64_t period_us;
    double period_ms;
    if (!bm_timer_period(zms, &period_us, &period_ms)) {
        RETURN_THROWS();
    }
    if (bm_pools_missing()) {
        RETURN_THROWS();
    }
    int tag = bm_source_tag(event, coalesce);
    if (tag < 0) {
        zend_throw_exception_ex(zend_ce_exception, 0, "every(): unknown class '%s' or too many sources",
                                ZSTR_VAL(event));
        RETURN_THROWS();
    }
    /* A tag is shared by every source of its class, so never clear a filler another source registered. */
    if (instanceof_function(bm_tags[tag].ce, bm_events_timer_ce())) {
        bm_events_set_filler((uint16_t) tag, bm_timer_fill);
    }
    zend_object *handle = bm_timer_new((uint16_t) tag, period_us, period_ms);
    if (!handle) {
        RETURN_THROWS();
    }
    RETURN_OBJ(handle);
}

/* watch_gpio(int $pin, string $event, bool $coalesce = false): Baremetal\GpioWatch -- falling edge, debounced */
PHP_FUNCTION(watch_gpio)
{
    zend_long pin;
    zend_string *event;
    bool coalesce = false;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(pin)
        Z_PARAM_STR(event)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(coalesce)
    ZEND_PARSE_PARAMETERS_END();
    if (bm_sources_refused("watch_gpio") || bm_pools_missing()) {
        RETURN_THROWS();
    }
    if (pin < 0 || pin >= BM_GPIO_PINS) {
        zend_value_error("watch_gpio(): $pin must be a GPIO number between 0 and %d", BM_GPIO_PINS - 1);
        RETURN_THROWS();
    }
    int tag = bm_source_tag(event, coalesce);
    if (tag < 0) {
        zend_throw_exception_ex(zend_ce_exception, 0,
                                "watch_gpio(): unknown class '%s' or too many sources", ZSTR_VAL(event));
        RETURN_THROWS();
    }
    bm_pin_filler_for(tag);
    uint8_t p = (uint8_t) pin;
    RETURN_OBJ(bm_gpio_watch_new((uint16_t) tag, false, &p, 1));
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_watch_gpios, 0, 2, Baremetal\\GpioWatch, 0)
    ZEND_ARG_TYPE_INFO(0, pins, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, event, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, coalesce, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

/* watch_gpios(array $pins, string $event, bool $coalesce = false): Baremetal\GpioWatch -- falling edge on each pin, one
   PinEvent class for all of them, debounced per pin (an edge on another pin is never swallowed) */
PHP_FUNCTION(watch_gpios)
{
    HashTable *pins;
    zend_string *event;
    bool coalesce = false;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_ARRAY_HT(pins)
        Z_PARAM_STR(event)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL(coalesce)
    ZEND_PARSE_PARAMETERS_END();
    if (bm_sources_refused("watch_gpios") || bm_pools_missing()) {
        RETURN_THROWS();
    }
    bool ok = zend_hash_num_elements(pins) > 0 && zend_array_is_list(pins);
    zval *z;
    if (ok) {
        ZEND_HASH_FOREACH_VAL(pins, z) {
            if (Z_TYPE_P(z) != IS_LONG || Z_LVAL_P(z) < 0 || Z_LVAL_P(z) >= BM_GPIO_PINS) {
                ok = false;
                break;
            }
        } ZEND_HASH_FOREACH_END();
    }
    if (!ok) {
        zend_value_error("watch_gpios(): $pins must be a non-empty list of GPIO numbers");
        RETURN_THROWS();
    }
    zend_class_entry *ce = zend_lookup_class(event);
    if (ce && !instanceof_function(ce, bm_events_pin_ce())) {
        zend_type_error("watch_gpios(): %s must extend Baremetal\\PinEvent", ZSTR_VAL(event));
        RETURN_THROWS();
    }
    int tag = bm_source_tag(event, coalesce);
    if (tag < 0) {
        zend_throw_exception_ex(zend_ce_exception, 0,
                                "watch_gpios(): unknown class '%s' or too many sources", ZSTR_VAL(event));
        RETURN_THROWS();
    }
    bm_pin_filler_for(tag);
    int n = 0;
    uint8_t *buf = safe_emalloc(zend_hash_num_elements(pins), 1, 0);   /* a pin may be listed twice */
    ZEND_HASH_FOREACH_VAL(pins, z) {
        buf[n++] = (uint8_t) Z_LVAL_P(z);
    } ZEND_HASH_FOREACH_END();
    zend_object *handle = bm_gpio_watch_new((uint16_t) tag, true, buf, n);
    efree(buf);
    RETURN_OBJ(handle);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_watch_button, 0, 2, Baremetal\\Button, 0)
    ZEND_ARG_TYPE_INFO(0, pin, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, on, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, opts, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_button_sampling, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* watch_button(int $pin, array $on, array $opts = []): Baremetal\Button */
PHP_FUNCTION(watch_button)
{
    zend_long pin;
    zval *on, *opts = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(pin)
        Z_PARAM_ARRAY(on)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(opts)
    ZEND_PARSE_PARAMETERS_END();
    if (bm_sources_refused("watch_button") || bm_pools_missing()) {
        RETURN_THROWS();
    }

    if (bm_button_n >= BM_MAX_BUTTONS) {
        zend_throw_exception(zend_ce_exception, "watch_button(): too many buttons", 0);
        RETURN_THROWS();
    }
    bm_button_t *b = &bm_buttons[bm_button_n];
    memset(b, 0, sizeof(*b));
    b->pin = (int) pin;
    b->active_low = true;
    b->hold_ms = 800;
    b->debounce_ms = 30;
    for (int i = 0; i < BTN_EVENTS; i++) {
        b->tag[i] = -1;
    }

    static const struct { const char *k; size_t n; int ev; } gestures[] = {
        { "pressed", sizeof("pressed") - 1, BTN_PRESSED },
        { "released", sizeof("released") - 1, BTN_RELEASED },
        { "held", sizeof("held") - 1, BTN_HELD },
        { "click", sizeof("click") - 1, BTN_CLICK },
        { "repeat", sizeof("repeat") - 1, BTN_REPEAT },
        { "double", sizeof("double") - 1, BTN_DOUBLE },
    };
    HashTable *ht = Z_ARRVAL_P(on);
    for (size_t g = 0; g < sizeof(gestures) / sizeof(gestures[0]); g++) {
        zval *z = zend_hash_str_find(ht, gestures[g].k, gestures[g].n);
        if (z && Z_TYPE_P(z) == IS_STRING) {
            int tag = bm_source_tag(Z_STR_P(z), false);
            if (tag < 0) {
                zend_throw_exception_ex(zend_ce_exception, 0,
                    "watch_button(): unknown class '%s' or too many sources", Z_STRVAL_P(z));
                RETURN_THROWS();
            }
            bm_pin_filler_for(tag);
            b->tag[gestures[g].ev] = tag;
        }
    }

    if (opts) {
        HashTable *o = Z_ARRVAL_P(opts);
        zval *z;
        if ((z = zend_hash_str_find(o, "holdMs", sizeof("holdMs") - 1))) b->hold_ms = (int) zval_get_long(z);
        if ((z = zend_hash_str_find(o, "repeatMs", sizeof("repeatMs") - 1))) b->repeat_ms = (int) zval_get_long(z);
        if ((z = zend_hash_str_find(o, "doubleMs", sizeof("doubleMs") - 1))) b->double_ms = (int) zval_get_long(z);
        if ((z = zend_hash_str_find(o, "debounceMs", sizeof("debounceMs") - 1))) b->debounce_ms = (int) zval_get_long(z);
        if ((z = zend_hash_str_find(o, "activeLow", sizeof("activeLow") - 1))) b->active_low = zend_is_true(z);
    }
    /* A wired `double` without doubleMs would never fire: give it a window. An explicit 0 still turns it off. */
    if (b->tag[BTN_DOUBLE] >= 0 && !(opts && zend_hash_str_exists(Z_ARRVAL_P(opts), "doubleMs", sizeof("doubleMs") - 1))) {
        b->double_ms = 300;
    }

    bm_hal_gpio_input((int) pin, b->active_low ? BM_HAL_PULL_UP : BM_HAL_PULL_DOWN);
    b->in_use = true;
    atomic_store(&b->enabled, true);
    bm_button_n++;
    bm_btn_run();
    RETURN_OBJ(bm_events_source_new(bm_button_ce, &bm_button_ops, b));
}

/* button_sampling(bool $on): void */
PHP_FUNCTION(button_sampling)
{
    zend_bool on;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(on)
    ZEND_PARSE_PARAMETERS_END();
    if (bm_sources_refused("button_sampling")) {
        RETURN_THROWS();
    }
    if (on) {
        bm_btn_run();
    } else {
        bm_btn_pause();
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_power_void, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* The module's functions; power_hold() and power_release() are in php_events.c. */
const zend_function_entry bm_events_functions[] = {
    PHP_FE(every,           arginfo_every)
    PHP_FE(watch_gpio,      arginfo_watch_gpio)
    PHP_FE(watch_gpios,     arginfo_watch_gpios)
    PHP_FE(watch_button,    arginfo_watch_button)
    PHP_FE(button_sampling, arginfo_button_sampling)
    PHP_FE(power_hold,      arginfo_power_void)
    PHP_FE(power_release,   arginfo_power_void)
    PHP_FE_END
};
