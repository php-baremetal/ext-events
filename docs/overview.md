---
eyebrow: 'Package · Extension'
lede:    'Typed events, listeners and the sources that raise them (timer, pin edge, button), plus a lock that keeps the chip out of light sleep. The event-driven runtime requires it; conductor adds the events HAL for your platform.'
see_also:
  - { href: './usage/events-and-listeners.md', meta: 'PHP API' }
  - { href: './usage/sources.md', meta: 'PHP API' }
  - { href: './reference/runtimes-and-delivery.md', meta: 'Reference' }
  - { href: 'package:php-baremetal/esp32-events-hal', meta: 'HAL', label: 'esp32-events-hal' }
  - { href: 'package:php-baremetal/ext-web', meta: 'Extension', label: 'ext-web (HTTP as events)' }
  - { href: 'package:php-baremetal/ext-i2c', meta: 'Extension', label: 'ext-i2c (sensor events)' }
prev: { label: 'No previous page', href: '#' }
next: { label: 'Events and listeners', href: './usage/events-and-listeners.md' }
---

# Events and the reactor

ext-events lets a script react instead of running top to bottom. An **event** is an object of a class you
declare as a subclass of `Baremetal\Event`. **Listeners** are callables registered per event class with
`Baremetal\Events::listen()`. **Sources** raise events from below PHP: a periodic timer, a falling edge on a
pin, gestures on a push button. In the `event-driven` runtime the script runs once to register all of this,
then the runtime blocks on the event queue and delivers each event to its listeners on the PHP task, sleeping
in between.

The same queue carries events from other extensions: `ext-i2c` raises sample events from background
samplers, `ext-web` delivers HTTP requests and WebSocket frames. `power_hold()` and `power_release()` keep the
chip awake for a stretch when the `power-save` option is on.

## Requirements

| Requires | Why |
| --- | --- |
| `php-baremetal/events-hal` (virtual) | The platform side: the event queue, periodic timers, GPIO interrupts, the light-sleep lock. On ESP32 it is `php-baremetal/esp32-events-hal` (platform `esp32`, over `esp_timer`, `esp_driver_gpio` and `esp_pm`). |

You never require the HAL yourself. Conductor reads the platform from `extra.baremetal.sdk` (`esp32@^1.0`
gives `esp32`) and adds `php-baremetal/esp32-events-hal` while Composer resolves. The PHP module is `events`;
it registers with priority 10, before other extensions, so `Baremetal\Event` exists when `ext-i2c` and
`ext-web` define their event classes.

The `event-driven` runtime declares ext-events as a required package: `phpbm` refuses to build an
`event-driven` project without it and tells you to run `composer require php-baremetal/ext-events`.
`ext-i2c` and `ext-web` require it too, so it also comes in with them.

## Install

<!-- @steps -->
- **Install conductor once per machine**

  `composer global require php-baremetal/conductor` keeps the plugin active during the first solve of a new
  project.

- **Create the project**

  `phpbm init my-events` writes `composer.json` with conductor (`^1.0`, allowed as a plugin), the package
  registry, and the SDK, board, runtime and storage you pick. Pick the `event-driven` runtime.

- **Require the extension**

  `composer require php-baremetal/ext-events`

- **Build and flash**

  `phpbm flash` builds the firmware and flashes it; `phpbm monitor` opens the serial console.
<!-- @endsteps -->

The resulting `composer.json`, for a Waveshare ESP32-S3-Zero (N8R8), whose BOOT button is on GPIO0:

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/my-events",
  "type": "project",
  "repositories": [
    { "type": "composer", "url": "https://repository.php-baremetal.com", "canonical": true }
  ],
  "require": {
    "php-baremetal/conductor": "^1.0",
    "php-baremetal/ext-events": "^1.0"
  },
  "config": {
    "allow-plugins": { "php-baremetal/conductor": true },
    "platform": { "php": "8.5.11" }
  },
  "extra": {
    "baremetal": {
      "sdk": "esp32@^1.0",
      "board": "waveshare-esp32-s3-zero-n8r8",
      "runtime": "event-driven",
      "storage": "embedded"
    }
  }
}
```
<!-- @endcode-block -->

## A first script

`project-src/index.php` for the `event-driven` runtime: the top level registers listeners and starts
sources, then returns. From then on the runtime delivers events.

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Tick extends Event {}
final class BootPressed extends Event {}

$ticks = 0;

Events::listen(Tick::class, function (Tick $e) use (&$ticks): void {
    printf("tick %d\n", ++$ticks);
});

Events::listen(BootPressed::class, function (BootPressed $e): void {
    echo "BOOT pressed\n";
});

every(1000, Tick::class);          // timer source
watch_gpio(0, BootPressed::class); // GPIO0 falling edge, 200 ms debounce

echo "listeners and sources registered\n";
```
<!-- @endcode-block -->

`$ticks` keeps its value between events: the script's variables stay alive for the whole run.

## PHP API at a glance

| Name | Kind | Meaning |
| --- | --- | --- |
| `Baremetal\Event` | class | Base class of every event. Property `$device`: the object that raised it, or `null`. |
| `Baremetal\Events::listen(string $event, callable $listener): void` | static method | Register a listener for one exact class. |
| `Baremetal\Events::now(Event $event): void` | static method | Deliver at once, on the caller's stack. |
| `Baremetal\Events::dispatch(Event $event): void` | static method | Deliver breadth-first: events dispatched by listeners run after the current batch. |
| `every(int $ms, string $event): void` | function | Raise `$event` every `$ms` milliseconds. |
| `watch_gpio(int $pin, string $event): void` | function | Raise `$event` on each falling edge of `$pin`, 200 ms debounce. |
| `watch_button(int $pin, array $on, array $opts = []): void` | function | Raise events for button gestures: `pressed`, `released`, `held`, `click`, `repeat`, `double`. |
| `button_sampling(bool $on): void` | function | Pause or resume the button sampler. |
| `power_hold(): void`, `power_release(): void` | functions | Forbid or allow automatic light sleep. |

Failures throw `\Exception` with a readable message, invalid arguments `\ValueError` or `\TypeError`. See
[Errors and limits](./reference/errors-and-limits.md).

## Dependencies

- Requires the virtual `php-baremetal/events-hal`, provided on ESP32 by
  [esp32-events-hal](package:php-baremetal/esp32-events-hal).
- Required by the `event-driven` runtime, by [ext-i2c](package:php-baremetal/ext-i2c) (sample events) and by
  [ext-web](package:php-baremetal/ext-web) (requests and frames as events).

## Notes

- Sources only post to a queue. The `event-driven` runtime is what reads it: under `init-loop` the timers
  and interrupts run but their events are never delivered. `Events::now()` and `Events::dispatch()` work in
  every runtime. See [Runtimes and delivery](./reference/runtimes-and-delivery.md).
- Listeners match the exact class: a listener on a parent class receives nothing from its subclasses.
- In `event-driven` the listener table is frozen when the top-level script returns; `Events::listen()`
  throws from then on.
- Events raised by a source are created without calling their constructor.
- The queue holds 32 messages, shared by every source and extension; a source event that finds it full is
  dropped. At most 16 distinct event classes can be used by sources.
