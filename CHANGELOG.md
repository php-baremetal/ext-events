# Changelog

## 1.2.0

- Added: `bm_events_on_idle()` and `bm_events_off_idle()` in the C API: C work that runs each time the PHP task returns to the event loop (at the start of every `bm_events_receive()` and at the end of `bm_events_service()`), for an extension that hands work over to its own task once a listener is done (Slate delivers the scene changes there).
- Added: `bm_events_watch()` and `bm_events_unwatch()` in the C API: a C listener of one event class, run before the PHP listeners and never frozen, for an extension that reacts to events without a PHP callable.
- Changed: a delivery no longer runs the PHP listeners of an event when an exception is already pending (a C watch that threw, for example): the exception propagates as one from a listener would.
- Added: bm_hal_task_start(), bm_hal_alloc() and bm_hal_free() in the events HAL contract (events-hal 1.5), for packages that need their own task, or internal and external RAM (BM_HAL_MEM_INTERNAL, BM_HAL_MEM_EXTERNAL, BM_HAL_MEM_EXTERNAL_ONLY), without ESP-IDF. Requires php-baremetal/events-hal ^1.5.

## 1.1.0

- Added: ide/ext-events.stub, the PHP declarations of the package for IDEs (conductor copies it into vendor/php-baremetal/ide-stubs/).
- Changed: up to 32 distinct event classes can be used by sources (was 16).
- Added: `bm_events_emit_from_isr()`, `bm_events_emit_data()` and `bm_events_slot_data()` in the C API: emit from an interrupt, and events that carry up to 16 bytes.
- Added: every() returns a Baremetal\Timer (a Baremetal\Source): stop(), start(), running(), setPeriod() and period(); a dropped handle leaves the timer running. C API bm_source_ops_t, bm_events_source_ce(), bm_events_source_new() and bm_events_source_rec() for packages with their own sources.
- Added: watch_gpio() and watch_gpios() return a Baremetal\GpioWatch: stop() detaches its pins, start() takes them back (a later watch of the same pin takes it over, as before); pins(). Needs php-baremetal/events-hal 1.4 (bm_hal_gpio_off()).
- Added: watch_button() returns a Baremetal\Button: stop() and start() pause one button (a press held at start() is ignored until released); pin(). button_sampling() still pauses all of them.
- Added: every event has $time (nanoseconds, the base of hrtime(true)), the moment its source raised it; Baremetal\TimerEvent with $ticks (1 plus the ticks coalesced into it), filled by every() when the class extends it; C API bm_events_set_filler() for packages that fill their own event classes.
- Added: Baremetal\PinEvent with $pin and $level, filled by watch_gpio() and watch_button() when the class extends it; watch_gpios(array $pins, string $event, bool $coalesce = false) watches several pins with one PinEvent class, debounced per pin.
- Added: every() takes a float period in milliseconds (down to 0.1 ms, rounded to the microsecond), e.g. every(1000 / 60, …) for 60 events per second.
- Added: C API bm_events_set_long(), bm_events_set_bool(), bm_events_timer_ce() and bm_events_pin_ce() for fillers and packages that build on the payload classes.
- Added: C API bm_events_set_zval(), for fillers that write a string, array, enum or object property (ext-wifi's events).
- Changed: the C core header event_bus.h: evt_t has two new fields, merged (ticks coalesced into the slot) and time_us (when the slot was acquired); event_bus_php.h now includes event_bus.h.
- Changed: Baremetal\Event declares public readonly int|float $time, so a user class that redeclares $time no longer compiles.
- Changed: requires php-baremetal/events-hal ^1.4 (bm_hal_lock_create(), bm_hal_lock_enter(), bm_hal_lock_exit(): a lock usable from tasks and ISRs, 1.3; bm_hal_gpio_off(), 1.4; esp32-events-hal 1.1.0).
- Changed: the C core header event_bus.h: evt_bailout() is removed (the caller unwinds with evt_frames_unwind_to() inside its own catch), evt_now() returns bool, EVT_FLAG_COALESCE is replaced by per-tag coalesce (evt_tag_set_coalesce()), the default small pool has 64 slots (options events-small, events-medium, events-large), and there are new statistics (evt_pool_stats(), evt_isr_stats(), evt_tag_stats(), evt_cascade_stats()), the release reserve (evt_pool_acquire_reserve()) and the frame API (evt_frame_open(), evt_frame_close(), evt_frames_on_close(), evt_dispatch_prepare()).
- Changed: events travel in preallocated slots (sizes: options events-small, events-medium, events-large) instead of a 32-message queue; a dropped event is counted per class and the first drop of a class prints a warning.
- Added: Events::dispatch() returns whether the event was accepted; Events::now() throws beyond 8 nested levels; Events::stats() and a phpinfo() section; coalesce argument of every() and watch_gpio().
- Fixed: a fatal error in a listener released neither the event's slot nor its power_hold(); an emitter object released (bm_events_release_later(), ext-i2c poll()) while the event queue was full was never freed.
- Fixed: watch_gpio() accepted any pin number; a pin outside 0..63 now throws \ValueError (`watch_gpio(): $pin must be a GPIO number between 0 and 63`) instead of undefined behaviour in the platform layer.
- Fixed: in the `web-server` runtime the refusal of `bm_events_source_tag()` named `poll()` whatever the caller;
  it now names the PHP function that asked (`wifi_events()`, ext-display `onFlushed()` and `onVsync()`; ext-i2c
  `poll()` as before).
- Fixed: a source started with an abstract class, an interface, a trait or an enum crashed the firmware at its first event; every(), watch_gpio(), watch_gpios(), watch_button() and ext-i2c poll() now refuse it with their "unknown class" message.

## 1.0.2

- Fixed: in the `web-server` runtime the listener table and the dispatch queue outlived the request that
  created them and were used after being freed. They now belong to the request and are dropped when it ends,
  together with any `power_hold()` left behind.
- Changed: `every()`, `watch_gpio()`, `watch_button()` and `button_sampling()` throw in the `web-server`
  runtime, in a request and in the `init` script, where nothing delivers their events: `... is not available
  in the web-server runtime`. `bm_events_source_tag()` (used by ext-i2c `poll()` with an event) refuses in the
  same way.
- Fixed: `watch_button()` with a `double` gesture and no `doubleMs` option never raised `double` (the window
  defaulted to 0, which turns detection off). The window is now 300 ms when `double` is wired; an explicit
  `'doubleMs' => 0` still turns it off.
- Changed: requires `php-baremetal/events-hal` ^1.1 (`bm_hal_request_scoped()`, esp32-events-hal 1.0.2).

## 1.0.1

- Changed: the author's contact email.
- Docs: projects use `"sdk": "esp32@^2.0"`.

## 1.0.0

Initial release.
