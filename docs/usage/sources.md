---
eyebrow: 'ext-events · PHP API'
lede:    'Sources raise events below PHP: a periodic timer, a debounced falling edge, a push button with gestures. They post to the event queue, which the event-driven runtime reads.'
see_also:
  - { href: './events-and-listeners.md', meta: 'PHP API' }
  - { href: '../recipes/react-to-a-button.md', meta: 'Recipe' }
  - { href: '../reference/errors-and-limits.md', meta: 'Reference' }
prev: { label: 'Events and listeners', href: './events-and-listeners.md' }
next: { label: 'Power hold and power-save', href: './power.md' }
---

# Sources

A source is started once, usually from the top-level script, and runs until the board restarts: there is
no call that stops a timer or a pin watch. Each time it fires, the source posts a message to the event queue
without waiting; the `event-driven` runtime turns the message into a new object of the event class and
delivers it to the listeners. When the queue is full the event is dropped.

The event class is looked up when the source starts (autoload included) and must exist; it should extend
`Baremetal\Event`. Up to 16 distinct event classes can be used by sources in the whole firmware, shared with
the sample events of `ext-i2c`. Using the same class for several sources costs one slot.

## every()

<!-- @method name="every(int $ms, string $event): void" returns="void" -->

<!-- @params -->
<!-- @param name="$ms" type="int" required -->
Period in milliseconds. `\ValueError` when not greater than 0.
<!-- @endparam -->
<!-- @param name="$event" type="string" required -->
The event class to raise each period, e.g. `Tick::class`.
<!-- @endparam -->
<!-- @endparams -->

Starts a periodic hardware timer (on ESP32 an `esp_timer`); each call starts a new timer. Throws
`every(): unknown class '...' or too many sources` and `every(): cannot start the timer`.

## watch_gpio()

<!-- @method name="watch_gpio(int $pin, string $event): void" returns="void" -->

<!-- @params -->
<!-- @param name="$pin" type="int" required -->
GPIO number. The pin becomes an input with the internal pull-up enabled. Not range-checked.
<!-- @endparam -->
<!-- @param name="$event" type="string" required -->
The event class to raise on each falling edge.
<!-- @endparam -->
<!-- @endparams -->

The edge is caught by an interrupt, so nothing runs while the pin is quiet. Edges closer than 200 ms to the
previous one of the same event class are ignored (the debounce is per class, so two pins sharing a class
share it too). Wire the button or signal between the pin and GND. Throws
`watch_gpio(): unknown class '...' or too many sources`.

## watch_button()

<!-- @method name="watch_button(int $pin, array $on, array $opts = []): void" returns="void" -->

<!-- @params -->
<!-- @param name="$pin" type="int" required -->
GPIO number of the button. The pin becomes an input with a pull-up (pull-down when `activeLow` is false).
<!-- @endparam -->
<!-- @param name="$on" type="array" required -->
Gesture name to event class. Keys: `pressed`, `released`, `held`, `click`, `repeat`, `double`. Leave a
gesture out to ignore it; values that are not strings are ignored.
<!-- @endparam -->
<!-- @param name="$opts" type="array" default="[]" -->
Timing options, all in milliseconds except `activeLow`; see the table below.
<!-- @endparam -->
<!-- @endparams -->

| Option | Default | Meaning |
| --- | --- | --- |
| `debounceMs` | `30` | The level must stay changed this long before it counts. |
| `holdMs` | `800` | Pressed this long raises `held`; a press that reaches it never raises `click`. |
| `repeatMs` | `0` | After `held`, raise `repeat` this often while still pressed. `0` turns repeat off. |
| `doubleMs` | `0` | A second click within this window raises `double` instead of two `click`s. `0` turns double-click detection off. |
| `activeLow` | `true` | `true`: pressed reads low (button to GND, pull-up). `false`: pressed reads high (button to 3.3 V, pull-down). |

| Gesture | When |
| --- | --- |
| `pressed` | The debounced level becomes pressed. |
| `released` | The debounced level becomes released. |
| `held` | Once per press, when the press reaches `holdMs`. |
| `repeat` | Every `repeatMs` after `held`, while pressed. |
| `click` | On release of a press shorter than `holdMs`. With `doubleMs` set, delayed until the window closes without a second click. |
| `double` | On release of the second short press within `doubleMs` of the first. |

All buttons are read by one sampler timer every 10 ms, so timings have a 10 ms resolution. At most four
buttons can be watched; the fifth call throws `watch_button(): too many buttons`. An unknown class throws
`watch_button(): unknown class '...' or too many sources`.

<!-- @code-block language="php" label="gestures" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Click extends Event {}
final class DoubleClick extends Event {}
final class LongPress extends Event {}

Events::listen(Click::class, fn () => print("click\n"));
Events::listen(DoubleClick::class, fn () => print("double\n"));
Events::listen(LongPress::class, fn () => print("long press\n"));

watch_button(0, [
    'click'  => Click::class,
    'double' => DoubleClick::class,
    'held'   => LongPress::class,
], ['holdMs' => 1500, 'doubleMs' => 300]);
```
<!-- @endcode-block -->

## button_sampling()

<!-- @method name="button_sampling(bool $on): void" returns="void" -->

`false` stops the 10 ms button sampler, which pauses every watched button: no gestures are detected and the
sampler no longer wakes the chip. `true` starts it again. `watch_button()` starts it by itself.

<!-- @callout variant="tip" title="watch_gpio() or watch_button()" -->
`watch_gpio()` is interrupt-driven and costs nothing while the pin is idle, which suits light sleep.
`watch_button()` gives gestures but samples every 10 ms; pause it with `button_sampling(false)` when the
button does not matter.
<!-- @endcallout -->
