---
eyebrow: 'ext-events · Reference'
lede:    'The C functions another extension calls to raise events through the reactor queue, plus the primitives an events HAL implements.'
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
so code on a timer, an interrupt or the executor only posts to the queue, and PHP objects are built when the
reactor delivers the message.

## Raising events of your own class

| Function | Use |
| --- | --- |
| `zend_class_entry *bm_events_base_ce(void)` | `Baremetal\Event`, to register event subclasses at module startup (ext-events starts first, priority 10). |
| `bool bm_events_source_tag(zend_string *class_name, uint16_t *out_tag)` | PHP task. Maps an event class to a tag, allocating one of the 16 source slots on first use, and creates the queue. False if the class is unknown or the table is full. |
| `void bm_events_emit(uint16_t tag, void *device)` | Any task. Posts an event of the tag's class; `device` (a `zend_object *` or NULL) becomes `$e->device`. Never blocks; dropped when the queue is full. |
| `void bm_events_release_later(zend_object *obj)` | PHP task. Releases an emitter object only after the messages already queued (which may point at it) are delivered. |
| `bool bm_events_power_save(void)` | Whether the build runs with power-save, valid once ext-events has started. `ext-i2c` uses it to refuse `Bus::CORE1`. |

`ext-i2c` is the reference user: `Device::poll(..., event: X::class)` resolves the tag on the PHP task, and the
executor's sample callback calls `bm_events_emit(tag, device)`.

## Request/response sources

Used by `ext-web` and its HAL; another server-style source can use them the same way.

| Function | Use |
| --- | --- |
| `void bm_events_ensure_queue(void)` | Create the queue now. |
| `bool bm_events_post_http(void *handle)` | Post a request handle without waiting; false when full. |
| `void bm_events_set_http_handler(void (*fn)(void *handle))` | Called on the PHP task for each posted request. |
| `void bm_events_set_http_release(void (*fn)(void *handle, int status))` | Called instead with status 503 when no request handler is set. |
| `bool bm_events_post_ws(void *handle, uint32_t wait_ms)` | Post a frame, waiting up to `wait_ms` for room; on false the caller frees the handle. |
| `void bm_events_set_ws_handler(void (*fn)(void *handle))` | Called on the PHP task for each posted frame. |
| `void bm_events_deliver_object(zval *event)` | PHP task. Runs the listeners of a ready event object. |
| `int bm_events_deliver_answering(zval *event, zval *ret_out)` | PHP task. Runs listeners until one returns an object, stored in `ret_out`. Returns 1 answered, 0 none answered, -1 a listener threw, -2 no listener. |

The remaining declarations (`bm_events_freeze()`, `bm_events_receive()`, `bm_events_deliver_msg()`,
`bm_events_power_autorelease()`) belong to the `event-driven` runtime.

## The events HAL

`bm_events_hal.h` is what a platform implements to provide `php-baremetal/events-hal`:

| Group | Functions |
| --- | --- |
| Clock | `bm_hal_now_us()` |
| Queue | `bm_hal_queue_create()`, `bm_hal_queue_send()`, `bm_hal_queue_send_isr()`, `bm_hal_queue_receive()` (wait in ms; `UINT32_MAX` waits forever) |
| Timer | `bm_hal_timer_create()`, `bm_hal_timer_start()` (periodic, ms), `bm_hal_timer_stop()` |
| GPIO | `bm_hal_gpio_input()` (`BM_HAL_PULL_UP` / `_DOWN` / `_NONE`), `bm_hal_gpio_level()`, `bm_hal_gpio_on_falling()` |
| Power | `bm_hal_power_save()`, `bm_hal_pmlock_create()` (NULL when power management is off), `bm_hal_pmlock_acquire()`, `bm_hal_pmlock_release()` |

ext-events also ships `event_bus.h`, a C event core with fixed slot pools and a dispatcher (`evt_*`
functions). The PHP API above does not use it, and no package calls it today.
