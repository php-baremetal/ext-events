---
eyebrow: 'ext-events · Reference'
lede:    'Every exception ext-events throws, and the fixed sizes behind them.'
see_also:
  - { href: '../usage/sources.md', meta: 'PHP API' }
  - { href: './runtimes-and-delivery.md', meta: 'Reference' }
prev: { label: 'Runtimes and delivery', href: './runtimes-and-delivery.md' }
next: { label: 'C API for extensions', href: './c-api.md' }
---

# Errors and limits

ext-events throws plain `\Exception` for runtime failures and the standard `\ValueError` / `\TypeError` for
invalid arguments; it defines no exception classes of its own.

## Exceptions

| Message | Thrown by | Cause |
| --- | --- | --- |
| `Events::listen() must be called in setup(); the listener table is frozen afterwards` | `Events::listen()` | `event-driven` runtime, after the top-level script returned. |
| `every(): unknown class '...' or too many sources` | `every()` | The class does not exist, or 16 source classes are already in use. |
| `every(): cannot start the timer` | `every()` | The platform could not create or start the timer. |
| `watch_gpio(): unknown class '...' or too many sources` | `watch_gpio()` | As for `every()`. |
| `watch_button(): too many buttons` | `watch_button()` | Four buttons are already watched. |
| `watch_button(): unknown class '...' or too many sources` | `watch_button()` | A class in `$on` does not exist, or the source table is full. |

`\ValueError` is thrown by `every()` for `$ms` not greater than 0. `\TypeError` is thrown by
`Events::listen()` for a listener that is not callable, and by `Events::now()` / `Events::dispatch()` for an
argument that is not a `Baremetal\Event`.

Nothing is thrown for a dropped event: a source event that finds the queue full is lost without notice.

## Limits

| Limit | Value |
| --- | --- |
| Event queue | 32 messages, shared with `ext-i2c` and `ext-web` |
| Event classes used by sources | 16, shared with `ext-i2c` sample events |
| Buttons (`watch_button()`) | 4 |
| Button sample period | 10 ms |
| `watch_gpio()` debounce | 200 ms per event class |
| Garbage collection in `event-driven` | every 256 events |

These are fixed in the extension; a project cannot change them. Timers, pin watches and buttons cannot be
stopped once started (only the button sampler can be paused).
