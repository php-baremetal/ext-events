---
eyebrow: 'ext-events · Reference'
lede:    'Every event travels in a preallocated slot; the event-driven runtime reads the queue and runs your listeners on the PHP task. Slot sizes, what each runtime does with events, the statistics and warnings, and what happens when a listener fails.'
see_also:
  - { href: '../usage/events-and-listeners.md', meta: 'PHP API' }
  - { href: './errors-and-limits.md', meta: 'Reference' }
  - { href: 'package:php-baremetal/ext-web', meta: 'Extension', label: 'ext-web' }
prev: { label: 'Power hold and power-save', href: '../usage/power.md' }
next: { label: 'Errors and limits', href: './errors-and-limits.md' }
---

# Runtimes and delivery

## Event slots and the queue

Everything that reaches PHP from outside the PHP task travels in an **event slot**:

- events from `every()`, `watch_gpio()` and `watch_button()`;
- sample events from `ext-i2c` background samplers;
- HTTP requests and WebSocket frames from `ext-web`.

`Events::dispatch()` uses the same slots for the events waiting in its cascade. `Events::now()` takes none.

The slots are allocated once at boot (in PSRAM when the chip has it), in three sizes set by package options
of ext-events in the project `composer.json`:

| Option | Default | Range | Slot | Memory per slot |
| --- | --- | --- | --- | --- |
| `events-small` | 64 | 8 to 65535 | 32-byte payload | about 57 bytes |
| `events-medium` | 16 | 0 to 65535 | 256-byte payload | about 281 bytes |
| `events-large` | 8 | 0 to 65535 | 2 KB payload | about 2 KB |

The memory per slot includes its bookkeeping and its entry in the queue. Every event kind uses small slots
today; the medium and large ones are reserved for events with a payload. On top of these there are 8 small
slots for interrupts (`watch_gpio()`) and 4 small slots kept for object releases (an `ext-i2c` emitter that
must outlive the events that point at it). The defaults take about 25 KB.

<!-- @code-block language="json" label="composer.json, extra.baremetal" -->
```json
{
  "runtime": "event-driven",
  "options": { "events-small": 128, "events-large": 0 }
}
```
<!-- @endcode-block -->

If the slots cannot be allocated at boot, the firmware prints
`events: cannot allocate <n> <small|medium|large> event slots (<KB> KB): lower events-<class>`, the event
sources throw `events: the event pools could not be allocated` and `Events::dispatch()` returns false.

Timers, interrupts and samplers never call PHP: they take a slot, fill it and post it to the queue, which is
as long as all the slots together, so a source that got a slot never finds the queue full. When no slot is
free:

| Source | Effect |
| --- | --- |
| `every()`, `watch_gpio()`, `watch_button()`, `ext-i2c` samples | The event is dropped and counted for its class; the first drop of a class prints a warning. |
| `ext-web` HTTP request | Answered 503, and counted as a drop of `Baremetal\Http\Request` (the first one prints the warning of its class). |
| `ext-web` WebSocket frame | Waits up to 2 s for a free slot, then is dropped and counted as a drop of `Baremetal\Http\Message` (with its class warning the first time). |
| Object release | Uses the 4 reserved slots; when they are taken too, the object is released once the reactor has read the messages queued before it. |

### Coalescing

A coalescing source does not queue an event while another event of the same class from a coalescing source
is still waiting to be delivered: the new one is merged into it and counted as `coalesced`, never as dropped.
`every()` coalesces by default, so a listener slower than its timer sees one tick per delivery instead of a
backlog; `watch_gpio()` does not by default, and `watch_button()` never does (gestures are distinct). Both
take a `coalesce` argument, see [Sources](../usage/sources.md).

## By runtime

| Runtime | Source events | `Events::now()` / `dispatch()` | Listener table |
| --- | --- | --- | --- |
| `event-driven` | Delivered by the reactor | Work | Frozen when the top-level script returns |
| `init-loop` | Never delivered: after each `loop()` the runtime gives their slots back, runs the object releases and prints the pending warnings; HTTP requests are answered 503 | Work | Never frozen |
| `web-server` | Refused: `every()`, `watch_gpio()`, `watch_button()`, `button_sampling()` and ext-i2c `poll()` with an event throw, in a request and in the `init` script | Work, inside the request | One per request, dropped when it ends |

In `web-server` every HTTP request is its own PHP request and nothing reads the event queue. Listeners
registered in a request live until that request ends, so `Events::now()` and `dispatch()` work within it;
a `power_hold()` left behind is released at the end too. Event sources would outlive the request that made
them and never deliver, so they throw, also in the `init` script (the runtime tells ext-events through the
events HAL, `bm_hal_request_scoped()`). For HTTP with resident state and sources, use `ext-web` in the
`event-driven` runtime.

## The event-driven reactor

<!-- @steps -->
- **The script runs once**

  The top-level script registers listeners and starts sources, then returns. A `setup()` or `loop()`
  function is not called in this runtime.

- **The table freezes**

  `Events::listen()` throws from now on. The queue is created if no source created it yet.

- **The reactor waits**

  It blocks on the queue; the PHP task sleeps, and with `power-save` the chip light-sleeps.

- **Each message is delivered**

  A source event becomes a new object of its class, built without its constructor, with `$device` set when
  the emitter passed one; its listeners run in registration order. HTTP requests and WebSocket frames go to
  `ext-web`, which builds a `Request` or `Message` and runs those listeners.

- **The slot and the holds go back**

  When the listeners return, the event's slot goes back to its pool and any `power_hold()` taken during the
  event (nested `now()` and dispatched events included) is released. Pending warnings are printed. Every 256 events the garbage collector runs.
<!-- @endsteps -->

If the queue cannot be created, the runtime prints `phpbm: cannot create the event queue` and the program
ends.

## When a listener fails

An uncaught exception in a listener stops delivery of that event and follows the SDK option `on-error`:

| `on-error` | Effect |
| --- | --- |
| `stop` (default) | The program stops (`--- stopped: fatal error ---`) and the board restarts after `restart-delay` seconds. |
| `continue` | The exception is printed as a warning and the reactor goes on with the next event. |

A fatal error (including `max_execution_time`) always stops the program. On the way out the slots of the
event being delivered and of the events waiting in a `dispatch()` cascade go back to their pools, and every
`power_hold()` is released. `set_exception_handler()` is honoured.

## Statistics and warnings

`Events::stats()` returns the counters since boot (never reset), in every runtime:

<!-- @code-block language="php" label="Events::stats()" -->
```php
[
    'pools' => [
        'small'   => ['size' => 64, 'in_use' => 0, 'peak' => 3],
        'medium'  => ['size' => 16, 'in_use' => 0, 'peak' => 0],
        'large'   => ['size' => 8,  'in_use' => 0, 'peak' => 0],
        'isr'     => ['size' => 8,  'in_use' => 0, 'peak' => 1, 'dropped' => 0],
        'reserve' => ['size' => 4,  'in_use' => 0],
    ],
    'classes' => [
        'Tick' => ['queued' => 0, 'delivered' => 120, 'dropped' => 0, 'coalesced' => 4],
    ],
    'cascade' => ['dispatch_max_depth' => 1, 'dispatch_refused' => 0, 'now_max_depth' => 0, 'now_refused' => 0],
]
```
<!-- @endcode-block -->

- `pools`: per slot class, its `size`, the slots `in_use` now and the `peak`; for the interrupt pool also the
  events `dropped` because it was full. The release reserve is listed apart.
- `classes`: per event class (its full name; `Baremetal\Http\Request` and `Baremetal\Http\Message` for the
  `ext-web` requests and frames), the events waiting (`queued`), `delivered` (by the reactor, `dispatch()`
  and `now()`), `dropped` for lack of a slot, and `coalesced`.
- `cascade`: the deepest `dispatch()` cascade and `now()` nesting reached, and how many were refused.

`phpinfo()` shows the same figures in its `ext-events` section.

The first drop of a class prints one warning; later drops of that class are only counted:

```text
events: dropped a Tick event (no free small slot); further drops of Tick are counted in Events::stats()
```

The interrupt pool and the `dispatch()` cascade warn once each:
`events: dropped an interrupt event (the interrupt pool is full); further drops are counted in Events::stats()`
and `events: Events::dispatch() refused an event nested deeper than 16; further refusals are counted in
Events::stats()`. Coalesced events never warn. A drop on a timer, an interrupt or another task is printed
by the PHP task at the next delivery (under `init-loop`, after the next `loop()`). The warnings are PHP
`E_WARNING`s, so `set_error_handler()` sees them.

## Ordering

- Events from the queue are delivered one at a time, in arrival order. A listener never runs concurrently
  with another listener.
- A long listener delays everything behind it, HTTP requests included, and holds slots meanwhile: a
  non-coalescing source behind it can run out of slots and drop events. Keep listeners short; split long work
  into steps driven by `every()`.
