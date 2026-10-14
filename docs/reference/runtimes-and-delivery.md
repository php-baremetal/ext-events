---
eyebrow: 'ext-events · Reference'
lede:    'Sources post to one queue; the event-driven runtime reads it and runs your listeners on the PHP task. What each runtime does with events, and what happens when a listener fails.'
see_also:
  - { href: '../usage/events-and-listeners.md', meta: 'PHP API' }
  - { href: './errors-and-limits.md', meta: 'Reference' }
  - { href: 'package:php-baremetal/ext-web', meta: 'Extension', label: 'ext-web' }
prev: { label: 'Power hold and power-save', href: '../usage/power.md' }
next: { label: 'Errors and limits', href: './errors-and-limits.md' }
---

# Runtimes and delivery

## The event queue

One queue of 32 messages carries everything that reaches PHP from outside the PHP task:

- events from `every()`, `watch_gpio()` and `watch_button()`;
- sample events from `ext-i2c` background samplers;
- HTTP requests and WebSocket frames from `ext-web`.

Timers, interrupts and samplers never call PHP: they post a small message and return. A source event that
finds the queue full is dropped silently; an `ext-web` request that finds it full is answered 503, and a
WebSocket frame waits up to 2 s for room.

## By runtime

| Runtime | Source events | `Events::now()` / `dispatch()` | Listener table |
| --- | --- | --- | --- |
| `event-driven` | Delivered by the reactor | Work | Frozen when the top-level script returns |
| `init-loop` | Never delivered (sources run, the queue is never read) | Work | Never frozen |
| `web-server` | Not supported | Not supported | Not supported |

In `web-server` every request starts with a fresh PHP state, while ext-events keeps its listener table for
the whole run: do not use ext-events there. For HTTP with resident state, use `ext-web` in the
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

- **Holds are released**

  Any `power_hold()` left by the listeners is released. Every 256 events the garbage collector runs.
<!-- @endsteps -->

If the queue cannot be created, the runtime prints `phpbm: cannot create the event queue` and the program
ends.

## When a listener fails

An uncaught exception in a listener stops delivery of that event and follows the SDK option `on-error`:

| `on-error` | Effect |
| --- | --- |
| `stop` (default) | The program stops (`--- stopped: fatal error ---`) and the board restarts after `restart-delay` seconds. |
| `continue` | The exception is printed as a warning and the reactor goes on with the next event. |

A fatal error (including `max_execution_time`) always stops the program. `set_exception_handler()` is
honoured.

## Ordering

- Events from the queue are delivered one at a time, in arrival order. A listener never runs concurrently
  with another listener.
- A long listener delays everything behind it, HTTP requests included, and lets the queue fill. Keep
  listeners short; split long work into steps driven by `every()`.
