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

/* Events::stats() and the phpinfo() section: the same figures, counted since boot. */
#include "php_events_internal.h"
#include "ext/standard/info.h"

static const char *const bm_pool_names[EVT_CLASS_COUNT] = { "small", "medium", "large" };

typedef struct {
    evt_pool_stats_t pool[EVT_CLASS_COUNT];
    evt_pool_stats_t isr;
    uint32_t         reserve_in_use;
    evt_cascade_stats_t cascade;
} bm_snapshot_t;

/* The release reserve sits on top of the small slots: what is in use beyond their size is the reserve. */
static void bm_snapshot(bm_snapshot_t *s)
{
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        evt_pool_stats((evt_class_t) c, &s->pool[c]);
    }
    evt_pool_stats_t *small = &s->pool[EVT_CLASS_TINY];
    s->reserve_in_use = small->in_use > small->size ? small->in_use - small->size : 0;
    small->in_use -= s->reserve_in_use;
    if (small->peak > small->size) {
        small->peak = small->size;
    }
    evt_isr_stats(&s->isr);
    evt_cascade_stats(&s->cascade);
}

/* A class's counters over its tags; false when it has none to show. */
static bool bm_class_stats(const bm_class_t *c, evt_tag_stats_t *out)
{
    memset(out, 0, sizeof *out);
    if (!c->shown) {
        return false;
    }
    bool any = false;
    for (int k = 0; k < 2; k++) {
        if (c->tag[k] == BM_TAG_NONE) {
            continue;
        }
        evt_tag_stats_t ts;
        evt_tag_stats(c->tag[k], &ts);
        out->queued    += ts.queued;
        out->delivered += ts.delivered;
        out->dropped   += ts.dropped;
        out->coalesced += ts.coalesced;
        any = true;
    }
    return any;
}

static void bm_add_pool(zval *pools, const char *name, const evt_pool_stats_t *st, bool dropped)
{
    zval z;
    array_init(&z);
    add_assoc_long(&z, "size", (zend_long) st->size);
    add_assoc_long(&z, "in_use", (zend_long) st->in_use);
    add_assoc_long(&z, "peak", (zend_long) st->peak);
    if (dropped) {
        add_assoc_long(&z, "dropped", (zend_long) st->dropped);
    }
    add_assoc_zval(pools, name, &z);
}

/* Events::stats(): array */
PHP_METHOD(Events, stats)
{
    ZEND_PARSE_PARAMETERS_NONE();
    bm_snapshot_t s;
    bm_snapshot(&s);

    zval pools, classes, cascade, reserve;
    array_init(return_value);
    array_init(&pools);
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        bm_add_pool(&pools, bm_pool_names[c], &s.pool[c], false);
    }
    bm_add_pool(&pools, "isr", &s.isr, true);
    array_init(&reserve);
    add_assoc_long(&reserve, "size", EVT_RELEASE_RESERVE);
    add_assoc_long(&reserve, "in_use", (zend_long) s.reserve_in_use);
    add_assoc_zval(&pools, "reserve", &reserve);
    add_assoc_zval(return_value, "pools", &pools);

    array_init(&classes);
    for (int i = 0; i < bm_class_n; i++) {
        evt_tag_stats_t ts;
        if (!bm_class_stats(&bm_classes[i], &ts)) {
            continue;
        }
        zval z;
        array_init(&z);
        add_assoc_long(&z, "queued", (zend_long) ts.queued);
        add_assoc_long(&z, "delivered", (zend_long) ts.delivered);
        add_assoc_long(&z, "dropped", (zend_long) ts.dropped);
        add_assoc_long(&z, "coalesced", (zend_long) ts.coalesced);
        zend_hash_str_update(Z_ARRVAL(classes), ZSTR_VAL(bm_classes[i].name), ZSTR_LEN(bm_classes[i].name), &z);
    }
    add_assoc_zval(return_value, "classes", &classes);

    array_init(&cascade);
    add_assoc_long(&cascade, "dispatch_max_depth", (zend_long) s.cascade.dispatch_max_depth);
    add_assoc_long(&cascade, "dispatch_refused", (zend_long) s.cascade.dispatch_refused);
    add_assoc_long(&cascade, "now_max_depth", (zend_long) s.cascade.now_max_depth);
    add_assoc_long(&cascade, "now_refused", (zend_long) s.cascade.now_refused);
    add_assoc_zval(return_value, "cascade", &cascade);
}

PHP_MINFO_FUNCTION(events)
{
    char buf[160];
    bm_snapshot_t s;
    bm_snapshot(&s);

    php_info_print_table_start();
    php_info_print_table_header(2, "ext-events", bm_pools_ok ? "enabled" : "event pools not allocated");
    for (int c = 0; c < EVT_CLASS_COUNT; c++) {
        char label[24];
        snprintf(label, sizeof label, "%s slots", bm_pool_names[c]);
        snprintf(buf, sizeof buf, "size %lu, in use %lu, peak %lu", (unsigned long) s.pool[c].size,
                 (unsigned long) s.pool[c].in_use, (unsigned long) s.pool[c].peak);
        php_info_print_table_row(2, label, buf);
    }
    snprintf(buf, sizeof buf, "size %lu, in use %lu, peak %lu, dropped %lu", (unsigned long) s.isr.size,
             (unsigned long) s.isr.in_use, (unsigned long) s.isr.peak, (unsigned long) s.isr.dropped);
    php_info_print_table_row(2, "interrupt slots", buf);
    snprintf(buf, sizeof buf, "size %d, in use %lu", EVT_RELEASE_RESERVE, (unsigned long) s.reserve_in_use);
    php_info_print_table_row(2, "release reserve", buf);
    for (int i = 0; i < bm_class_n; i++) {
        evt_tag_stats_t ts;
        if (!bm_class_stats(&bm_classes[i], &ts)) {
            continue;
        }
        snprintf(buf, sizeof buf, "queued %lu, delivered %lu, dropped %lu, coalesced %lu",
                 (unsigned long) ts.queued, (unsigned long) ts.delivered, (unsigned long) ts.dropped,
                 (unsigned long) ts.coalesced);
        php_info_print_table_row(2, ZSTR_VAL(bm_classes[i].name), buf);
    }
    snprintf(buf, sizeof buf, "max depth %lu, refused %lu", (unsigned long) s.cascade.dispatch_max_depth,
             (unsigned long) s.cascade.dispatch_refused);
    php_info_print_table_row(2, "dispatch cascade", buf);
    snprintf(buf, sizeof buf, "max depth %lu, refused %lu", (unsigned long) s.cascade.now_max_depth,
             (unsigned long) s.cascade.now_refused);
    php_info_print_table_row(2, "now() nesting", buf);
    php_info_print_table_end();
}
