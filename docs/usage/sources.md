---
eyebrow: 'ext-events · PHP API'
lede:    'Sources raise events below PHP: a periodic timer, a debounced falling edge, a push button with gestures. Each event takes a small slot and goes to the event queue, which the event-driven runtime reads.'
see_also:
  - { href: './events-and-listeners.md', meta: 'PHP API' }
  - { href: '../recipes/react-to-a-button.md', meta: 'Recipe' }
  - { href: '../reference/errors-and-limits.md', meta: 'Reference' }
prev: { label: 'Events and listeners', href: './events-and-listeners.md' }
next: { label: 'Power hold and power-save', href: './power.md' }
---

# Sources

A source is started once, usually from the top-level script, and runs until the board restarts, unless the
handle it returns stops it (see [Controlling a source](#controlling-a-source)). Each time it fires, the source takes a small event slot and posts
it to the event queue without waiting; the `event-driven` runtime turns it into a new object of the event
class, delivers it to the listeners and gives the slot back. When no slot is free the event is dropped and
counted for its class in `Events::stats()`, and the first drop of a class prints a warning (see
[Runtimes and delivery](../reference/runtimes-and-delivery.md#event-slots-and-the-queue)).

A **coalescing** source does not queue an event while one of the same class from a coalescing source is still
waiting: the new one is merged into it and counted as `coalesced`. `every()` coalesces by default,
`watch_gpio()` does not, `watch_button()` never does.

The event class is looked up when the source starts (autoload included) and must exist; it should extend
`Baremetal\Event`. Up to 32 distinct event classes can be used by sources in the whole firmware, shared with
the sample events of `ext-i2c`. Using the same class for several sources costs one entry (a class used by both
coalescing and non-coalescing sources costs two).

## every()

<!-- @method name="every(int|float $ms, string $event, bool $coalesce = true): Baremetal\Timer" returns="Baremetal\Timer" -->

<!-- @params -->
<!-- @param name="$ms" type="int|float" required -->
Period in milliseconds, from 0.1 to 4294967295. A float is rounded to the nearest microsecond, so
`every(1000 / 60, Frame::class)` raises 60 events per second (a period of 16667 µs). `\ValueError` below 0.1
(NaN included) and above 4294967295 (INF included). A numeric string is converted as for any `int|float`
parameter.
<!-- @endparam -->
<!-- @param name="$event" type="string" required -->
The event class to raise each period, e.g. `Tick::class`.
<!-- @endparam -->
<!-- @param name="$coalesce" type="bool" default="true" -->
`true`: a tick is not queued while a tick of this class still waits for delivery, so a slow listener gets
one tick per delivery instead of a backlog. `false`: every tick takes its own slot; ticks that find no free
slot are dropped.
<!-- @endparam -->
<!-- @endparams -->

Starts a periodic hardware timer (on ESP32 an `esp_timer`); each call starts a new timer and returns its
`Baremetal\Timer` handle (see [Controlling a source](#controlling-a-source)). Throws
`every(): unknown class '...' or too many sources` and `every(): cannot start the timer`.

Every event has a `$time` (see [Baremetal\Event](./events-and-listeners.md#baremetalevent)): the moment the
tick fired. If the class extends `Baremetal\TimerEvent`, `every()` also fills `$ticks`: 1 plus the ticks
coalesced into the event while it waited, so a slow listener still knows how many periods went by.

<!-- @code-block language="php" label="counting coalesced ticks" -->
```php
<?php
use Baremetal\Events;
use Baremetal\TimerEvent;

final class Frame extends TimerEvent {}

Events::listen(Frame::class, function (Frame $e): void {
    if ($e->ticks > 1) {
        echo "late: {$e->ticks} periods since the last event\n";
    }
});
every(1000 / 60, Frame::class);
```
<!-- @endcode-block -->

With `coalesce: false` every event has `$ticks === 1`. A class that extends `Baremetal\Event` directly works
as before and has no `$ticks`. `$ticks` is filled once per event class and `coalesce` value, not per source: if another source (`watch_gpio()`,
`poll()`) raises the same `TimerEvent` class with the same `coalesce` value, its events get `$ticks` too (1 plus
what was coalesced into them), so `$ticks` is only meaningful for `every()`'s events. A `TimerEvent` created with
`new` has it uninitialised and reading it throws. It saturates at 4294967295 on 64-bit PHP and at 2147483647 on the board's 32-bit PHP.

A short period is only as useful as the listener is fast: each event costs a PHP call of tens of
microseconds. A timer faster than its listener coalesces its ticks (`Events::stats()` counts them as
`coalesced`), or with `coalesce: false` runs out of slots and drops them. See
[Errors and limits](../reference/errors-and-limits.md#limits).

## watch_gpio()

<!-- @method name="watch_gpio(int $pin, string $event, bool $coalesce = false): Baremetal\GpioWatch" returns="Baremetal\GpioWatch" -->

<!-- @params -->
<!-- @param name="$pin" type="int" required -->
GPIO number, 0 to 63 (`\ValueError` otherwise; the chip decides which numbers exist). The pin becomes an input with the internal pull-up enabled.
<!-- @endparam -->
<!-- @param name="$event" type="string" required -->
The event class to raise on each falling edge.
<!-- @endparam -->
<!-- @param name="$coalesce" type="bool" default="false" -->
`true`: an edge is not queued while an edge event of this class still waits for delivery. `false`: every
edge takes its own slot. A coalesced `PinEvent` keeps `$pin` and `$level` of the first edge.
<!-- @endparam -->
<!-- @endparams -->

The edge is caught by an interrupt, so nothing runs while the pin is quiet. Edges closer than 200 ms to the
previous one of the same event class are ignored (the debounce is per class, so two pins sharing a class
share it too). Edges take their slots from a pool of 8 kept for interrupts; when all 8 wait for delivery the
edge is dropped, and the first such drop prints a warning. Wire the button or signal between the pin and GND.
Returns a `Baremetal\GpioWatch` handle (see [Controlling a source](#controlling-a-source)). Throws `watch_gpio(): unknown class '...' or too many sources`, and `\ValueError` `watch_gpio(): $pin must be a GPIO number between 0 and 63`.

If the class extends `Baremetal\PinEvent`, the event carries `$pin` and `$level`, the level read from the pin
in the interrupt (`false` is low).

<!-- @code-block language="php" label="PinEvent" -->
```php
<?php
use Baremetal\Events;
use Baremetal\PinEvent;

final class DoorEdge extends PinEvent {}

Events::listen(DoorEdge::class, function (DoorEdge $e) {
    echo "pin {$e->pin} is now ", $e->level ? 'high' : 'low', "\n";
});
watch_gpio(4, DoorEdge::class);
```
<!-- @endcode-block -->

`Baremetal\PinEvent` extends `Event` and declares `public readonly int $pin` and `public readonly bool $level`.
A class that extends only `Event` still works and has neither property. For `watch_button()`, `$level` is the
electrical level after debounce: with the default `activeLow`, `pressed` and `held` events have `$level ===
false` and `released` and `click` events have `true`.

## watch_gpios()

<!-- @method name="watch_gpios(array $pins, string $event, bool $coalesce = false): Baremetal\GpioWatch" returns="Baremetal\GpioWatch" -->

<!-- @params -->
<!-- @param name="$pins" type="array" required -->
A non-empty list of GPIO numbers, each 0 to 63.
<!-- @endparam -->
<!-- @param name="$event" type="string" required -->
A class that extends `Baremetal\PinEvent`, raised on each falling edge of any of the pins.
<!-- @endparam -->
<!-- @param name="$coalesce" type="bool" default="false" -->
`true`: an edge is not queued while an event of this class still waits. `false` (the default): every edge takes
its own slot.
<!-- @endparam -->
<!-- @endparams -->

One class for a group of pins, such as the rows of a keypad: `$pin` says which one fired. The default is
`coalesce: false` because with `true` an edge on one pin would be swallowed while an event for another pin
waits, and the listener could not tell which pin it lost. Each pin is debounced on its own (200 ms), so an edge
on pin 5 right after one on pin 4 is delivered, while two edges on pin 4 within 200 ms give one event. Every
pin counts as one `watch_gpio()` for the interrupt slots, and all of them share one entry in the event source
table and one line of `Events::stats()`. A pin already watched by `watch_gpio()` moves to the new class. Returns one `Baremetal\GpioWatch` for all the pins.

Throws `\TypeError` `watch_gpios(): <Class> must extend Baremetal\PinEvent`, `\ValueError` `watch_gpios(): $pins
must be a non-empty list of GPIO numbers`, and `watch_gpios(): unknown class '...' or too many sources`.

## watch_button()

<!-- @method name="watch_button(int $pin, array $on, array $opts = []): Baremetal\Button" returns="Baremetal\Button" -->

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
| `doubleMs` | `300` with `double` in `$on`, otherwise `0` | A second click within this window raises `double` instead of two `click`s. `0` turns double-click detection off. Before ext-events 1.0.2 the default was always `0`, so `double` never fired without this option. |
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

## Controlling a source

`every()` returns a `Baremetal\Timer`, a `Baremetal\Source`. The handle controls the source; it is not the
source.

`Baremetal\Source` is abstract, and a handle can only come from a function that returns one: `new Baremetal\Timer()`
and `new` of a PHP subclass of `Source` throw `\Error` (`Cannot directly construct Baremetal\Timer`), and a handle
can be neither cloned nor serialized. Its methods are `final`:

| Method | Meaning |
| --- | --- |
| `stop(): void` | Stops the source. Events already queued are still delivered, and a tick already firing when `stop()` is called may still arrive. Does nothing on a stopped source. |
| `start(): void` | Starts it again. Does nothing on a running source. If the platform cannot restart the timer, `running()` stays `false`. |
| `running(): bool` | Whether the source is running. |

`Baremetal\GpioWatch` (from `watch_gpio()` and `watch_gpios()`) adds:

| Method | Meaning |
| --- | --- |
| `pins(): array` | The pins of the watch, as a list in the order of the call. |

For a `GpioWatch`, `stop()` detaches the interrupt of every pin the watch still owns (the pins stay inputs),
and `start()` attaches them again. A pin has one watch at a time: the last call wins, as `watch_gpio()` has
always worked. A watch that lost every pin to a later one reports `running()` `false`; its `stop()` leaves those
pins to the later watch, and its `start()` takes them back (and the other watch then stops running if that was its
only pin). A watch that kept some of its pins still runs, and `stop()` detaches only those it owns.

<!-- @code-block language="php" label="the last watch of a pin wins" -->
```php
<?php
use Baremetal\PinEvent;

final class Door extends PinEvent {}
final class Alarm extends PinEvent {}

$door  = watch_gpio(4, Door::class);
$alarm = watch_gpio(4, Alarm::class);   // takes pin 4: edges raise Alarm

var_dump($door->running());   // false
$door->stop();                // nothing to detach: pin 4 still raises Alarm
$door->start();               // pin 4 raises Door again; $alarm->running() is now false
```
<!-- @endcode-block -->

`Baremetal\Button` (from `watch_button()`) adds:

| Method | Meaning |
| --- | --- |
| `pin(): int` | The GPIO number of the button. |

For a `Button`, `stop()` pauses that button only: the others keep sampling. `start()` resumes it from a clean
state (no click, double-click or repeat pending), and a press that is still held at `start()` is ignored until the
button is released: it raises no `pressed`, `held`, `repeat`, `released` or `click`, and the next press is seen as
usual. `button_sampling(false)` is the global pause of the sampler: it stops every button whatever its
`running()`, and `button_sampling(true)` resumes them (a button stopped through its handle stays stopped).

`Baremetal\Timer` adds:

| Method | Meaning |
| --- | --- |
| `setPeriod(int\|float $ms): void` | Sets the period, with the limits and the `\ValueError` of `every()`. A running timer restarts with the new period; a stopped one keeps it for `start()`. Throws `every(): cannot start the timer` (and is left stopped) when the platform cannot restart it. |
| `period(): float` | The period in milliseconds, as given (the timer runs it rounded to the microsecond). |

The source does not depend on its handle: a handle that goes out of scope leaves the source running, so
`every(1000, Tick::class);` without keeping the result works as before. `stop()` keeps the event class, so
the source table (32 entries) does not shrink. Stopping one of two timers of the same class leaves the other
running.

<!-- @code-block language="php" label="slowing a timer down, then pausing it" -->
```php
<?php

final class Tick extends Baremetal\Event {}

$timer = every(100, Tick::class);

Baremetal\Events::listen(Tick::class, function () use ($timer) {
    $timer->setPeriod($timer->period() * 2);   // 200 ms, 400 ms, ...
    if ($timer->period() > 1000) {
        $timer->stop();
    }
});
```
<!-- @endcode-block -->
