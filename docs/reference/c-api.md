---
eyebrow: 'ext-events · Reference'
lede:    'The C functions another extension calls to raise events through the event slots and the reactor queue, plus the primitives an events HAL implements.'
see_also:
  - { href: './runtimes-and-delivery.md', meta: 'Reference' }
  - { href: 'package:php-baremetal/ext-i2c', meta: 'Extension', label: 'ext-i2c (a source)' }
  - { href: 'package:php-baremetal/esp32-events-hal', meta: 'HAL', label: 'esp32-events-hal' }
prev: { label: 'Errors and limits', href: './errors-and-limits.md' }
next: { label: 'Run code periodically', href: '../recipes/periodic-tick.md' }
---

# C API for extensions

An extension that raises events requires `php-baremetal/ext-events` in its `composer.json` and includes
`event_bus_php.h` from ext-events' `include/`. The rules of the firmware apply: only the PHP task calls Zend,
so code on a timer, an interrupt or the executor only takes a small event slot and posts it to the queue, and
PHP objects are built when the reactor delivers the message.

## Raising events of your own class

| Function | Use |
| --- | --- |
| `zend_class_entry *bm_events_base_ce(void)` | `Baremetal\Event`, to register event subclasses at module startup (ext-events starts first, priority 10). |
| `bm_source_ops_t` | `{ stop, start, running, release }`: the callbacks of a source, each given the source's `rec`. `release` may be NULL; otherwise it is called when a handle is freed (the source keeps running). |
| `zend_class_entry *bm_events_source_ce(void)` | `Baremetal\Source`, the abstract base of the handles. A package registers its handle class as a final subclass of it at module startup (after ext-events). |
| `zend_object *bm_events_source_new(zend_class_entry *ce, const bm_source_ops_t *ops, void *rec)` | A new handle of `ce` (a subclass of `Source`) on `rec`, returned with one reference; `return` it from the PHP function. `rec` belongs to the source, never to the handle, so it must outlive every handle (a persistent allocation). `ops` must stay valid for the same time. |
| `void *bm_events_source_rec(zend_object *obj)` | The `rec` of a `Baremetal\Source` handle (what was given to `bm_events_source_new()`), or NULL when `obj` is not one. Use it in the methods of your handle class. |
| `zend_class_entry *bm_events_timer_ce(void)` | `Baremetal\TimerEvent`, the base of the timer events `every()` fills with `$ticks`. |
| `zend_class_entry *bm_events_pin_ce(void)` | `Baremetal\PinEvent`, the base of the pin events `watch_gpio()`, `watch_button()` and `watch_gpios()` fill with `$pin` and `$level`. |
| `typedef void (*bm_event_filler_t)(zend_object *event, zend_object *device, const evt_t *slot)` | A filler: runs on the PHP task for each event of its tag, just before delivery, to set the payload properties of `event` from the slot (`slot->merged`, `slot->time_us`, the payload) and the emitter `device` (NULL if none). |
| `bool bm_events_set_filler(uint16_t tag, bm_event_filler_t fn)` | PHP task. Registers the filler of a source tag (NULL removes it); false when `tag >= EVT_MAX_TAGS`. A package that raises its own event class calls it once after `bm_events_source_tag()`. `$time` and `$device` are set before the filler runs. A tag has one filler and the last registration wins, so register one only on a tag whose class you own; `every()` registers its own only for classes that extend `Baremetal\TimerEvent`, and never clears another. The properties are looked up once per class and name; pass string literals. |
| `bool bm_events_set_long(zend_object *event, zend_class_entry *ce, const char *name, long v)`, `bool bm_events_set_bool(...)` | For fillers: initialise a public readonly property of `event` straight in its slot (a later PHP write still throws). False when the class has no such property. |
| `bool bm_events_set_zval(zend_object *event, zend_class_entry *ce, const char *name, zval *v)` | The same for a value of any other type: a string, an array, an enum case or another object, `null`. The property takes its own reference to `v`; the caller still owns `v` and releases it. False when the class has no such property, `v` is undefined or a reference, or the declared type does not accept `v` as it is (no coercion: an `int` is not accepted by a `float` property). |
| `bool bm_events_source_tag(zend_string *class_name, uint16_t *out_tag)` | PHP task. Maps an event class to a tag, allocating one of the 32 source tags on first use, and creates the queue. False if the class is unknown or the table is full; false with an exception pending in the `web-server` runtime or when the event pools could not be allocated. The tag does not coalesce. |
| `bool bm_events_source_tag_ex(zend_string *class_name, bool coalesce, uint16_t *out_tag)` | As `bm_events_source_tag()`; with `coalesce` the tag is a coalescing one: no event is queued while one from a coalescing source of that class waits (counted as `coalesced`). |
| `void bm_events_emit(uint16_t tag, void *device)` | Any task. Takes a small slot and posts an event of the tag's class; `device` (a `zend_object *` or NULL) becomes `$e->device`. Never blocks; with no free slot the event is dropped and counted for its class, and the first drop prints a warning on the PHP task. |
| `void bm_events_emit_from_isr(uint16_t tag, void *device)` | Interrupt context. As `bm_events_emit()`, but takes the slot from the interrupt pool (it never blocks and never touches the small pool); the tag's coalesce applies and `time_us` is set when the slot is acquired. With no free interrupt slot the event is dropped and counted. `watch_gpio()` uses the same pool. |
| `bool bm_events_emit_data(uint16_t tag, void *device, const void *data, size_t len)` | Any task, not an interrupt. As `bm_events_emit()`, and copies `len` bytes (`len <= BM_EVENTS_DATA_MAX`, 16) into the slot. False when the event is dropped (counted as for `bm_events_emit()`) or `len` is too large. The bytes travel with the event, so a filler sees each event's own data even when several wait for delivery. |
| `const void *bm_events_slot_data(const evt_t *slot, size_t *len)` | In a filler: the bytes given to `bm_events_emit_data()` for this slot and their length; `NULL` and 0 for any other event. The pointer is valid only while the filler runs. |
| `void bm_events_release_later(zend_object *obj)` | PHP task. Releases an emitter object only after the messages already queued (which may point at it) are delivered. Uses the 4-slot release reserve; when it is taken too, the release runs once the reactor has read the messages queued before the call. Never leaks. |
| `bool bm_events_power_save(void)` | Whether the build runs with power-save, valid once ext-events has started. `ext-i2c` uses it to refuse `Bus::CORE1`. |

A filler that reads the data of an event (here a region of four `uint16_t`):

```c
typedef struct { uint16_t x0, y0, x1, y1; } region_t;

static void fill_flushed(zend_object *event, zend_object *device, const evt_t *slot)
{
    size_t len;
    const region_t *r = bm_events_slot_data(slot, &len);
    if (r && len == sizeof *r) {
        bm_events_set_long(event, event->ce, "x0", r->x0);
        bm_events_set_long(event, event->ce, "x1", r->x1);
    }
}

/* any task: */
region_t r = { 0, 0, 359, 59 };
bm_events_emit_data(tag, device, &r, sizeof r);
```

`ext-i2c` is the reference user: `Device::poll(..., event: X::class)` resolves the tag on the PHP task, and the
executor's sample callback calls `bm_events_emit(tag, device)`.

## Request/response sources

Used by `ext-web` and its HAL; another server-style source can use them the same way.

| Function | Use |
| --- | --- |
| `void bm_events_ensure_queue(void)` | Create the queue now. |
| `bool bm_events_post_http(void *handle)` | Post a request handle without waiting; false when no small slot is free (the caller answers 503). |
| `void bm_events_set_http_handler(void (*fn)(void *handle))` | Called on the PHP task for each posted request. |
| `void bm_events_set_http_release(void (*fn)(void *handle, int status))` | Called instead with status 503 when no request handler is set. |
| `bool bm_events_post_ws(void *handle, uint32_t wait_ms)` | Post a frame, waiting up to `wait_ms` for a free small slot; on false the caller frees the handle. |
| `void bm_events_set_ws_handler(void (*fn)(void *handle))` | Called on the PHP task for each posted frame. |
| `void bm_events_deliver_object(zval *event)` | PHP task. Runs the listeners of a ready event object. |
| `int bm_events_deliver_answering(zval *event, zval *ret_out)` | PHP task. Runs listeners until one returns an object, stored in `ret_out`. Returns 1 answered, 0 none answered, -1 a listener threw, -2 no listener. |

## For runtimes

| Function | Use |
| --- | --- |
| `bool bm_events_receive(bm_event_msg_t *out, uint32_t ms)` | The reactor: the next message (its slot in `out->slot`). |
| `void bm_events_deliver_msg(const bm_event_msg_t *msg)` | The reactor, for every message received: delivers it in a frame inside `zend_try` (on a fatal error the frames close, their slots and holds go back, and the bailout goes on), then runs the releases due and prints the pending warnings. |
| `void bm_events_service(void)` | A runtime that never delivers events (`init-loop`), once per loop inside `phpbm_invoke()`: gives the slots of waiting events back, runs object releases, answers HTTP requests 503, prints the pending warnings. The SDK declares it weak, so a firmware without ext-events still links. |
| `void bm_events_freeze(void)`, `void bm_events_power_autorelease(void)` | The `event-driven` runtime: freeze the listener table; release any `power_hold()` left. |

### Back in the loop, and C listeners (since 1.2)

| Function | Use |
| --- | --- |
| `bool bm_events_on_idle(void (*cb)(void *arg), void *arg)` | PHP task. `cb(arg)` runs each time the PHP task returns to the event loop: at the start of every `bm_events_receive()` (after each delivered message, before the reactor takes the next one or waits) and at the end of `bm_events_service()` (`init-loop`, after each `loop()`). It runs outside any PHP frame: plain C only, short, no PHP calls. At most 4 hooks; false when the table is full. |
| `void bm_events_off_idle(void (*cb)(void *arg), void *arg)` | PHP task. Removes the hook. |
| `bool bm_events_watch(zend_class_entry *ce, bm_events_watch_fn fn, void *arg)` | PHP task. `fn(event, arg)` (`void (*)(zend_object *event, void *arg)`) runs for every delivered event of exactly class `ce`, before its PHP listeners, inside the delivery frame (it may call PHP; an exception stops the delivery as a listener's would). Never frozen; it stays until removed. At most 8 watches; false when the table is full. |
| `void bm_events_unwatch(zend_class_entry *ce, bm_events_watch_fn fn, void *arg)` | PHP task. Removes the watch. |

The runtimes need no change for these: the `event-driven` reactor calls `bm_events_receive()` after every
message, and `init-loop` calls `bm_events_service()` after every `loop()`.

## The events HAL

`bm_events_hal.h` is what a platform implements to provide `php-baremetal/events-hal`:

| Group | Functions |
| --- | --- |
| Clock | `bm_hal_now_us()` |
| Queue | `bm_hal_queue_create()`, `bm_hal_queue_send()`, `bm_hal_queue_send_isr()`, `bm_hal_queue_receive()` (wait in ms; `UINT32_MAX` waits forever) |
| Timer | `bm_hal_timer_create()`, `bm_hal_timer_start()` (periodic, ms), `bm_hal_timer_start_us()` (periodic, µs; interface version 1.2, used by `every()`), `bm_hal_timer_stop()` |
| GPIO | `bm_hal_gpio_input()` (`BM_HAL_PULL_UP` / `_DOWN` / `_NONE`), `bm_hal_gpio_level()`, `bm_hal_gpio_on_falling()`, `bm_hal_gpio_off()` (detach the pin's interrupt, the pin stays an input; interface version 1.4, used by `GpioWatch::stop()`) |
| Power | `bm_hal_power_save()`, `bm_hal_pmlock_create()` (NULL when power management is off), `bm_hal_pmlock_acquire()`, `bm_hal_pmlock_release()` |
| Lock | `bm_hal_lock_create()`, `bm_hal_lock_enter()`, `bm_hal_lock_exit()`: a short, non-recursive critical section usable from a task and from an ISR (a spinlock on ESP32, a mutex on the PC). Interface version 1.3 |
| Task | `bm_hal_task_start(fn, arg, name, stack, priority, core)`: runs `fn(arg)` in a new task that ends by itself when `fn` returns, for a package that needs its own task without the platform headers. `stack` in bytes, `priority` relative to the PHP task (0 = same, +1 above, -1 below), `core` a `bm_hal_core_t` (`BM_HAL_CORE_ANY`, `BM_HAL_CORE_0`, `BM_HAL_CORE_1`); false when it cannot start. Call it from the PHP task. Interface version 1.5 |
| Memory | `bm_hal_alloc(bytes, where)` and `bm_hal_free(p)`: `where` is `BM_HAL_MEM_INTERNAL` (fast internal RAM) or `BM_HAL_MEM_EXTERNAL` (external RAM such as PSRAM when the board has it, otherwise internal RAM) or `BM_HAL_MEM_EXTERNAL_ONLY` (external RAM or NULL, for optional memory that must not take internal RAM); NULL when there is no memory; `bm_hal_free(NULL)` does nothing. Any task, not an ISR. Interface version 1.5 |
| Runtime | `bm_hal_request_scoped()`: true when every HTTP request is its own PHP request (`web-server`), for the whole run; event sources are refused then. Interface version 1.1 |

## The event core

`event_bus.h` is the C core under the PHP layer: the slot pools (sized by `events-small`, `events-medium`,
`events-large`), the per-class counters and coalesce flags, the frames and the cascade limits (`evt_*`
functions). The pool functions are safe from any task and from an ISR; frames and cascades belong to the PHP
task. Extensions use the `bm_events_*` functions above rather than the core directly.
