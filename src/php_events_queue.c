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

/* Event classes and tags, the reactor queue of slot pointers, the C API producers, releases, warnings. */
#include "php_events_internal.h"
#include "zend_exceptions.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bm_tag_t   bm_tags[EVT_MAX_TAGS];
bm_event_filler_t bm_fillers[EVT_MAX_TAGS];   /* PHP task only */

bool bm_events_set_filler(uint16_t tag, bm_event_filler_t fn)
{
    if (tag >= EVT_MAX_TAGS) {
        return false;
    }
    bm_fillers[tag] = fn;
    return true;
}
bm_class_t bm_classes[EVT_MAX_TAGS];
int        bm_class_n;
bool       bm_pools_ok;
bm_hal_queue_t *bm_queue;

static HashTable bm_class_map;   /* name -> bm_class_t * */
static uint16_t  bm_tag_next = BM_TAG_FIRST;

static bm_hal_queue_t *bm_freed_q;   /* a token when a slot comes back and a task waits for one */
static uint32_t bm_waiters;
static uint32_t bm_q_sent;           /* messages queued, any task */
static uint32_t bm_q_read;           /* messages read, PHP task */

static bool bm_warn_pending;
static bool bm_isr_pending;
static bool bm_isr_warned;
static bool bm_cascade_warned;

static void (*bm_http_handler)(void *handle);
static void (*bm_ws_handler)(void *handle);
static void (*bm_http_release)(void *handle, int status);

_Static_assert(sizeof(bm_payload_t) <= EVT_TINY_SIZE, "the slot payload fits a small slot");

#define BM_QUEUE_LEN (EVT_TINY_COUNT + EVT_RELEASE_RESERVE + EVT_SMALL_COUNT + EVT_MEDIUM_COUNT + EVT_ISR_POOL_COUNT)
#define BM_WAIT_STEP_MS 10

/* ---- classes and tags ---- */

static bm_class_t *bm_class_add(const char *name, size_t len)
{
    if (bm_class_n >= EVT_MAX_TAGS) {
        return NULL;
    }
    bm_class_t *c = &bm_classes[bm_class_n++];
    c->name = zend_string_init(name, len, 1);
    c->tag[0] = c->tag[1] = BM_TAG_NONE;
    c->shown = true;
    zend_hash_add_new_ptr(&bm_class_map, c->name, c);
    return c;
}

static void bm_tag_bind(uint16_t tag, bm_class_t *c, bool coalesce)
{
    c->tag[coalesce] = tag;
    bm_tags[tag].cls = c;
    bm_tags[tag].coalesce = coalesce;
    evt_tag_set_coalesce(tag, coalesce);
}

void bm_classes_init(void)
{
    zend_hash_init(&bm_class_map, 16, NULL, NULL, 1);
    bm_class_t *http = bm_class_add(BM_HTTP_CLASS, sizeof(BM_HTTP_CLASS) - 1);
    bm_class_t *ws = bm_class_add(BM_WS_CLASS, sizeof(BM_WS_CLASS) - 1);
    http->shown = ws->shown = false;   /* until a request or a frame arrives */
    bm_tag_bind(BM_TAG_HTTP, http, false);
    bm_tag_bind(BM_TAG_WS, ws, false);
}

void bm_classes_shutdown(void)
{
    zend_hash_destroy(&bm_class_map);
    for (int i = 0; i < bm_class_n; i++) {
        zend_string_release(bm_classes[i].name);
    }
    bm_class_n = 0;
}

void bm_classes_request_end(void)
{
    for (int t = 0; t < EVT_MAX_TAGS; t++) {
        bm_tags[t].ce = NULL;
    }
}

uint16_t bm_class_tag(zend_string *name, bool coalesce)
{
    bm_class_t *c = zend_hash_find_ptr(&bm_class_map, name);
    if (!c && !(c = bm_class_add(ZSTR_VAL(name), ZSTR_LEN(name)))) {
        return BM_TAG_NONE;
    }
    if (c->tag[coalesce] == BM_TAG_NONE && bm_tag_next < EVT_MAX_TAGS) {
        bm_tag_bind(bm_tag_next++, c, coalesce);
    }
    return c->tag[coalesce];
}

/* ---- warnings ---- */

static void bm_flag_set(bool *f)
{
    __atomic_store_n(f, true, __ATOMIC_SEQ_CST);
}

static bool bm_flag_take(bool *f)
{
    return __atomic_exchange_n(f, false, __ATOMIC_SEQ_CST);
}

/* Any task or ISR, after a refused acquire: a drop (not a coalesce) is printed later by the PHP task. */
static void bm_note_refused(uint16_t tag, bool isr)
{
    if (tag < EVT_MAX_TAGS && bm_tags[tag].coalesce) {
        evt_tag_stats_t ts;
        evt_tag_stats(tag, &ts);
        if (ts.queued > 0) {
            return;   /* coalesced */
        }
    }
    if (isr) {
        if (!bm_isr_warned) {
            bm_flag_set(&bm_isr_pending);
            bm_flag_set(&bm_warn_pending);
        }
    } else if (tag < EVT_MAX_TAGS && bm_tags[tag].cls && !bm_tags[tag].cls->warned) {
        bm_flag_set(&bm_tags[tag].drop_pending);
        bm_flag_set(&bm_warn_pending);
    }
}

/* The drop warnings. Raised from the reactor, between two deliveries (inside the runtime's internal frame,
   no script line running), PHP would end the message with " in Unknown on line 0". Then, with no user error handler, it is printed without a
   location; otherwise it is an ordinary E_WARNING. */
static void bm_warn(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (zend_get_executed_filename_ex() != NULL || Z_TYPE(EG(user_error_handler)) != IS_UNDEF) {
        zend_error(E_WARNING, "%s", msg);
        return;
    }
    if (!(EG(error_reporting) & E_WARNING)) {
        return;
    }
    if (PG(display_errors)) {
        php_printf("\nWarning: %s\n", msg);
    }
}

void bm_warn_class_drop(uint16_t tag)
{
    if (tag >= EVT_MAX_TAGS || !bm_tags[tag].cls || bm_tags[tag].cls->warned) {
        return;
    }
    bm_class_t *c = bm_tags[tag].cls;
    c->warned = true;
    bm_warn("events: dropped a %s event (no free small slot); further drops of %s are counted in "
               "Events::stats()", ZSTR_VAL(c->name), ZSTR_VAL(c->name));
}

void bm_warn_cascade(void)
{
    if (!bm_cascade_warned) {
        bm_cascade_warned = true;
        bm_warn("events: Events::dispatch() refused an event nested deeper than %d; further "
                   "refusals are counted in Events::stats()", EVT_DISPATCH_DEPTH_MAX);
    }
}

void bm_flush_warnings(void)
{
    if (!bm_flag_take(&bm_warn_pending)) {
        return;
    }
    if (bm_flag_take(&bm_isr_pending) && !bm_isr_warned) {
        evt_pool_stats_t st;
        evt_isr_stats(&st);
        if (st.dropped > 0) {
            bm_isr_warned = true;
            bm_warn("events: dropped an interrupt event (the interrupt pool is full); further "
                       "drops are counted in Events::stats()");
        }
    }
    for (int t = 0; t < EVT_MAX_TAGS; t++) {
        if (bm_flag_take(&bm_tags[t].drop_pending)) {
            evt_tag_stats_t ts;
            evt_tag_stats((uint16_t) t, &ts);
            if (ts.dropped > 0) {
                bm_warn_class_drop((uint16_t) t);
            }
        }
    }
}

/* ---- the queue ---- */

void bm_events_ensure_queue(void)
{
    if (!bm_queue && bm_pools_ok) {
        bm_queue = bm_hal_queue_create(BM_QUEUE_LEN, sizeof(evt_t *));
    }
    if (bm_queue && !bm_freed_q) {
        bm_freed_q = bm_hal_queue_create(1, 1);
    }
}

static evt_t *bm_take(uint16_t tag, bool isr)
{
    evt_t *e = isr ? evt_isr_acquire(tag) : evt_pool_acquire(EVT_CLASS_TINY, tag);
    if (!e) {
        bm_note_refused(tag, isr);
    } else {
        ((bm_payload_t *) e->payload)->data_len = 0;
    }
    return e;
}

/* Never full: the queue is as long as all the slots together. */
static bool bm_send(evt_t *e, uint8_t kind, void *ptr, bool isr, int pin, bool level)
{
    e->kind = kind;
    bm_payload_t *p = (bm_payload_t *) e->payload;
    p->ptr = ptr;
    p->pin = (int16_t) pin;
    p->level = level;
    /* Counted before the send: a deferred release asked meanwhile waits for this message too. */
    __atomic_fetch_add(&bm_q_sent, 1, __ATOMIC_SEQ_CST);
    bool ok = isr ? bm_hal_queue_send_isr(bm_queue, &e) : bm_hal_queue_send(bm_queue, &e, 0);
    if (!ok) {
        __atomic_fetch_sub(&bm_q_sent, 1, __ATOMIC_SEQ_CST);
        evt_pool_release(e);
    }
    return ok;
}

static bool bm_small_full(void)
{
    evt_pool_stats_t st;
    evt_pool_stats(EVT_CLASS_TINY, &st);
    return st.in_use >= st.size;
}

/* Another task: waits up to wait_ms for a small slot, counting one drop at most. */
static evt_t *bm_take_wait(uint16_t tag, uint32_t wait_ms)
{
    if (wait_ms > 0 && bm_freed_q) {
        int64_t deadline = bm_hal_now_us() + (int64_t) wait_ms * 1000;
        while (bm_small_full() && bm_hal_now_us() < deadline) {
            uint8_t token;
            __atomic_fetch_add(&bm_waiters, 1, __ATOMIC_SEQ_CST);
            bm_hal_queue_receive(bm_freed_q, &token, BM_WAIT_STEP_MS);
            __atomic_fetch_sub(&bm_waiters, 1, __ATOMIC_SEQ_CST);
        }
    }
    return bm_take(tag, false);
}

void bm_slot_freed(void)
{
    if (bm_freed_q && __atomic_load_n(&bm_waiters, __ATOMIC_SEQ_CST) > 0) {
        uint8_t token = 0;
        bm_hal_queue_send(bm_freed_q, &token, 0);
    }
}

void bm_events_emit(uint16_t tag, void *device)
{
    if (!bm_queue) {
        return;
    }
    evt_t *e = bm_take(tag, false);
    if (e) {
        bm_send(e, BM_EVT_SOURCE, device, false, -1, false);
    }
}

bool bm_events_emit_data(uint16_t tag, void *device, const void *data, size_t len)
{
    if (!bm_queue || len > BM_EVENTS_DATA_MAX || (len && !data)) {
        return false;
    }
    evt_t *e = bm_take(tag, false);
    if (!e) {
        return false;
    }
    bm_payload_t *p = (bm_payload_t *) e->payload;
    if (len) {
        memcpy(p->data, data, len);
    }
    p->data_len = (uint8_t) len;
    return bm_send(e, BM_EVT_SOURCE, device, false, -1, false);
}

const void *bm_events_slot_data(const evt_t *slot, size_t *len)
{
    const bm_payload_t *p = (const bm_payload_t *) slot->payload;
    if (slot->kind != BM_EVT_SOURCE || p->data_len == 0) {
        *len = 0;
        return NULL;
    }
    *len = p->data_len;
    return p->data;
}

/* Interrupt context: the interrupt pool. */
void bm_events_emit_from_isr(uint16_t tag, void *device)
{
    if (!bm_queue) {
        return;
    }
    evt_t *e = bm_take(tag, true);
    if (e) {
        bm_send(e, BM_EVT_SOURCE, device, true, -1, false);
    }
}

void bm_events_emit_isr(uint16_t tag)
{
    bm_events_emit_from_isr(tag, NULL);
}

bool bm_events_emit_pin(uint16_t tag, int pin, bool level)
{
    if (!bm_queue) {
        return false;
    }
    evt_t *e = bm_take(tag, false);
    return e && bm_send(e, BM_EVT_SOURCE, NULL, false, pin, level);
}

bool bm_events_emit_isr_pin(uint16_t tag, int pin, bool level)
{
    if (!bm_queue) {
        return false;
    }
    evt_t *e = bm_take(tag, true);
    return e && bm_send(e, BM_EVT_SOURCE, NULL, true, pin, level);
}

bool bm_events_post_http(void *handle)
{
    if (!bm_queue) {
        return false;
    }
    bm_tags[BM_TAG_HTTP].cls->shown = true;
    evt_t *e = bm_take(BM_TAG_HTTP, false);
    return e && bm_send(e, BM_EVT_HTTP, handle, false, -1, false);
}

bool bm_events_post_ws(void *handle, uint32_t wait_ms)
{
    if (!bm_queue) {
        return false;
    }
    bm_tags[BM_TAG_WS].cls->shown = true;
    evt_t *e = bm_take_wait(BM_TAG_WS, wait_ms);
    return e && bm_send(e, BM_EVT_WS, handle, false, -1, false);
}

void bm_events_set_http_handler(void (*fn)(void *handle))
{
    bm_http_handler = fn;
}

void bm_events_set_http_release(void (*fn)(void *handle, int status))
{
    bm_http_release = fn;
}

void bm_events_set_ws_handler(void (*fn)(void *handle))
{
    bm_ws_handler = fn;
}

/* ---- object releases ---- */

/* Waiting for the messages queued before them to be read (no reserve slot was free). Request memory. */
typedef struct bm_release {
    zend_object       *obj;
    uint32_t           after;   /* bm_q_sent when it was asked */
    struct bm_release *next;
} bm_release_t;

static bm_release_t *bm_rel_head, *bm_rel_tail;

void bm_events_release_later(zend_object *obj)
{
    if (!bm_queue) {
        OBJ_RELEASE(obj);                             /* nothing can have been queued */
        return;
    }
    evt_t *e = evt_pool_acquire_reserve(BM_TAG_RELEASE);
    if (e && bm_send(e, BM_EVT_RELEASE, obj, false, -1, false)) {
        return;
    }
    bm_release_t *r = emalloc(sizeof *r);
    r->obj = obj;
    r->after = __atomic_load_n(&bm_q_sent, __ATOMIC_SEQ_CST);
    r->next = NULL;
    if (bm_rel_tail) {
        bm_rel_tail->next = r;
    } else {
        bm_rel_head = r;
    }
    bm_rel_tail = r;
    bm_release_due();   /* nothing queued and unread: now */
}

void bm_release_due(void)
{
    while (bm_rel_head && (int32_t) (bm_q_read - bm_rel_head->after) >= 0) {
        bm_release_t *r = bm_rel_head;
        bm_rel_head = r->next;
        if (!bm_rel_head) {
            bm_rel_tail = NULL;
        }
        zend_object *obj = r->obj;
        efree(r);
        OBJ_RELEASE(obj);
    }
}

void bm_release_request_end(void)
{
    bm_rel_head = bm_rel_tail = NULL;   /* request memory: gone with the request, objects included */
}

/* ---- the reactor side ---- */

bool bm_events_receive(bm_event_msg_t *out, uint32_t ms)
{
    evt_t *e;
    if (!bm_queue || !bm_hal_queue_receive(bm_queue, &e, ms)) {
        return false;
    }
    bm_q_read++;
    out->tag  = e->tag;
    out->kind = e->kind;
    out->ptr  = ((bm_payload_t *) e->payload)->ptr;
    out->slot = e;
    return true;
}

static void bm_deliver_http(void *handle)
{
    if (bm_http_handler) {
        bm_http_handler(handle);
    } else if (bm_http_release) {
        bm_http_release(handle, 503);                 /* the server task waits on this request */
    }
}

static void bm_deliver_ws(void *handle)
{
    if (bm_ws_handler) {
        bm_ws_handler(handle);
    } else {
        free(handle);                                 /* a malloc'd ws_msg_t nobody will take */
    }
}

static void bm_deliver_source(void *arg)
{
    const bm_event_msg_t *msg = arg;
    zend_class_entry *ce = msg->tag < EVT_MAX_TAGS ? bm_tags[msg->tag].ce : NULL;
    if (!ce) {
        return;
    }
    zval event;
    if (object_init_ex(&event, ce) != SUCCESS) {      /* refused at registration; never crash here */
        zend_clear_exception();
        return;
    }
    const evt_t *slot = msg->slot;
    /* $time and the filler write into Baremetal\Event's layout: a plain class gets neither */
    bool is_event = instanceof_function(ce, bm_events_base_ce());
    if (slot && is_event) {                           /* $e->time: when the source raised it */
        bm_event_set_time(Z_OBJ(event), ce, slot->time_us);
    }
    if (msg->ptr) {                                   /* $e->device */
        zval zdev;
        ZVAL_OBJ(&zdev, (zend_object *) msg->ptr);
        zend_update_property(ce, Z_OBJ(event), "device", sizeof("device") - 1, &zdev);
    }
    if (slot && is_event && bm_fillers[msg->tag]) {
        bm_fillers[msg->tag](Z_OBJ(event), (zend_object *) msg->ptr, slot);
    }
    bm_deliver(&event);
    zval_ptr_dtor(&event);
}

/* A message nobody delivers: its slot goes back, an HTTP request gets 503, a WebSocket frame is freed. */
static void bm_discard(evt_t *e, uint8_t kind, void *ptr)
{
    evt_pool_release(e);
    bm_slot_freed();
    if (kind == BM_EVT_RELEASE) {
        OBJ_RELEASE((zend_object *) ptr);
    } else if (kind == BM_EVT_HTTP && bm_http_release) {
        bm_http_release(ptr, 503);
    } else if (kind == BM_EVT_WS) {
        free(ptr);
    }
}

void bm_events_deliver_msg(const bm_event_msg_t *msg)
{
    evt_t *e = msg->slot;
    if (!e) {
        return;
    }
    if (msg->kind == BM_EVT_RELEASE) {
        bm_discard(e, msg->kind, msg->ptr);
    } else {
        evt_pool_mark_delivered(e);
        bool ran;
        if (msg->kind == BM_EVT_HTTP) {
            ran = bm_run_frame(e, bm_deliver_http, msg->ptr);
        } else if (msg->kind == BM_EVT_WS) {
            ran = bm_run_frame(e, bm_deliver_ws, msg->ptr);
        } else {
            ran = bm_run_frame(e, bm_deliver_source, (void *) msg);
        }
        if (!ran && msg->kind == BM_EVT_HTTP && bm_http_release) {
            bm_http_release(msg->ptr, 503);
        } else if (!ran && msg->kind == BM_EVT_WS) {
            free(msg->ptr);
        }
    }
    bm_release_due();
    bm_flush_warnings();
}

void bm_events_service(void)
{
    bm_event_msg_t msg;
    while (bm_events_receive(&msg, 0)) {
        bm_discard(msg.slot, msg.kind, msg.ptr);
    }
    bm_release_due();
    bm_flush_warnings();
}
