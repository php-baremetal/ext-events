---
eyebrow: 'ext-events · Reference'
lede:    'Every exception and warning ext-events raises, and the sizes behind them.'
see_also:
  - { href: '../usage/sources.md', meta: 'PHP API' }
  - { href: './runtimes-and-delivery.md', meta: 'Reference' }
prev: { label: 'Runtimes and delivery', href: './runtimes-and-delivery.md' }
next: { label: 'C API for extensions', href: './c-api.md' }
---

# Errors and limits

ext-events throws plain `\Exception` for runtime failures, `\RuntimeException` for a `now()` nested too deep,
and the standard `\ValueError` / `\TypeError` for invalid arguments; it defines no exception classes of its
own.

## Exceptions

| Message | Thrown by | Cause |
| --- | --- | --- |
| `Events::listen() must be called in setup(); the listener table is frozen afterwards` | `Events::listen()` | `event-driven` runtime, after the top-level script returned. |
| `every(): unknown class '...' or too many sources` | `every()` | The class does not exist or cannot be instantiated (abstract class, interface, trait, enum), or the event source table (32 entries) is full. |
| `every(): cannot start the timer` | `every()`, `Timer::setPeriod()` | The platform could not create or start the timer (after `setPeriod()` the timer is left stopped). |
| `Cannot directly construct Baremetal\Timer` (`\Error`; the same for `Baremetal\GpioWatch` and `Baremetal\Button`) | `new` | A handle can only be returned by a function; the same for a PHP subclass of `Baremetal\Source`, with its own name. |
| `Trying to clone an uncloneable object of class Baremetal\Timer` (`\Error`; the same for `Baremetal\GpioWatch` and `Baremetal\Button`) | `clone` | A handle cannot be cloned. |
| `Serialization of 'Baremetal\Timer' is not allowed` (`\Exception`; the same for `Baremetal\GpioWatch` and `Baremetal\Button`) | `serialize()` | A handle cannot be serialized. |
| `watch_gpio(): unknown class '...' or too many sources` | `watch_gpio()` | As for `every()`. |
| `watch_gpios(): <Class> must extend Baremetal\PinEvent` (`\TypeError`) | `watch_gpios()` | `<Class>` is the name passed, and it does not extend `PinEvent`. |
| `watch_gpio(): $pin must be a GPIO number between 0 and 63` (`\ValueError`) | `watch_gpio()` | `$pin` is negative or above 63. |
| `watch_gpios(): $pins must be a non-empty list of GPIO numbers` (`\ValueError`) | `watch_gpios()` | `$pins` is empty, not a list, or holds something other than an integer from 0 to 63. |
| `watch_gpios(): unknown class '...' or too many sources` | `watch_gpios()` | As for `every()`. |
| `watch_button(): too many buttons` | `watch_button()` | Four buttons are already watched. |
| `watch_button(): unknown class '...' or too many sources` | `watch_button()` | A class in `$on` does not exist or cannot be instantiated, or the source table is full. |
| `events: the event pools could not be allocated` | `every()`, `watch_gpio()`, `watch_gpios()`, `watch_button()`, ext-i2c `poll()` with an event | The event slots could not be allocated at boot (see below). |
| `Events::now(): nested too deep (limit 8)` (`\RuntimeException`) | `Events::now()` | A listener called `now()` with 8 `now()` deliveries already nested. It reaches the caller of that `now()`. |

`\ValueError` is thrown by `watch_gpio()` for a `$pin` outside 0 to 63 (`watch_gpio(): $pin must be a GPIO number between 0 and 63`), by `watch_gpios()` for an invalid `$pins` list, and by `every()` for `$ms` below 0.1 or NaN (`every(): Argument #1 ($ms) must be at least
0.1`) and above 4294967295 or INF (`every(): Argument #1 ($ms) must be at most 4294967295`); `Timer::setPeriod()` throws the same errors, with its own name (`Baremetal\Timer::setPeriod(): Argument #1 ($ms) must be at least 0.1`). `\TypeError` is thrown by
`Events::listen()` for a listener that is not callable, and by `Events::now()` / `Events::dispatch()` for an
argument that is not a `Baremetal\Event`.

## Dropped events and warnings

Nothing is thrown for a dropped event. A source event that finds no free slot is dropped and counted for its
class in `Events::stats()`; `Events::dispatch()` returns `false` (and counts it) when no slot is free or the
cascade is deeper than 16. The first drop of each class prints one PHP warning, and so do the interrupt pool
and the cascade, once each:

| Warning | When |
| --- | --- |
| `events: dropped a <Class> event (no free small slot); further drops of <Class> are counted in Events::stats()` | The first drop of an event class (a source, `dispatch()`, an HTTP request or a WebSocket frame). |
| `events: dropped an interrupt event (the interrupt pool is full); further drops are counted in Events::stats()` | The first `watch_gpio()` edge that found the 8 interrupt slots taken. |
| `events: Events::dispatch() refused an event nested deeper than 16; further refusals are counted in Events::stats()` | The first `dispatch()` refused for the cascade depth. |

Coalesced events are counted, never warned about. At boot, when the slots cannot be allocated, the firmware
prints `events: cannot allocate <n> <small|medium|large> event slots (<KB> KB): lower events-<class>` (or the
interrupt-pool variant) and the event sources throw.

## Limits

| Limit | Value |
| --- | --- |
| Small event slots | 64 (`events-small`, 8 to 65535), shared by every source, `dispatch()`, `ext-i2c` and `ext-web` |
| Medium / large event slots | 16 / 8 (`events-medium`, `events-large`, 0 to 65535), for events with a payload |
| Interrupt slots (`watch_gpio()`, `watch_gpios()`) | 8, shared by every pin; each pin of `watch_gpios()` counts as one `watch_gpio()` |
| Release reserve | 4 small slots |
| `dispatch()` cascade | 16 levels |
| `now()` nesting | 8 levels |
| `every()` period | 0.1 ms to 4294967295 ms, rounded to the microsecond. 0.1 ms is a floor, not a rate to aim for: each event costs a PHP call of tens of microseconds, so a timer faster than its listener coalesces (one tick waits at a time) or, with `coalesce: false`, runs out of slots and drops ticks |
| Event sources | 32 entries, shared with `ext-i2c` sample events; one class used with both `coalesce` values takes two; `watch_gpios()` takes one entry per class, however many pins |
| Buttons (`watch_button()`) | 4 |
| Button sample period | 10 ms |
| `watch_gpio()` debounce | 200 ms per event class |
| `watch_gpios()` debounce | 200 ms per pin |
| Garbage collection in `event-driven` | every 256 events |

The slot counts are package options of ext-events, set in `extra.baremetal.options` of the project
`composer.json` (see [Runtimes and delivery](./runtimes-and-delivery.md#event-slots-and-the-queue)); the other
limits are fixed in the extension. A timer, a pin watch or a button is paused with the `stop()` of its handle, and the
button sampler of all buttons with `button_sampling()`; neither frees its entry in the tables above.
