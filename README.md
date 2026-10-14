# ext-events

The event bus for the PHP that runs on your board. You declare events as subclasses of `Baremetal\Event`,
register listeners with `Baremetal\Events::listen()`, and start sources that raise them: a periodic timer
(`every()`), a pin edge (`watch_gpio()`) or a push button with click, double-click, hold and repeat
(`watch_button()`). `power_hold()` and `power_release()` keep the chip out of automatic light sleep for a
stretch of work. Other extensions deliver through the same queue: `ext-i2c` sensor samples, `ext-web` HTTP
requests and WebSocket frames.

The `event-driven` runtime requires it: the script registers listeners and sources, then the runtime waits on
the event queue and calls your listeners. ext-events requires the virtual `php-baremetal/events-hal`;
conductor adds the HAL for your SDK platform (`php-baremetal/esp32-events-hal` on ESP32).

## Install

In a project created with `phpbm init`:

```bash
composer require php-baremetal/ext-events
```

Then set `"runtime": "event-driven"` in `extra.baremetal` of the project `composer.json`.

## Example

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
Events::listen(BootPressed::class, fn () => print("BOOT pressed\n"));

every(1000, Tick::class);           // a Tick every second
watch_gpio(0, BootPressed::class);  // GPIO0 falling edge, debounced
```

The script returns after registering; the runtime then sleeps on the queue between events.

## Documentation

- [Overview](docs/overview.md): requirements, install, a first project, the API at a glance.
- [Events and listeners](docs/usage/events-and-listeners.md): `Event`, `Events::listen()`, `now()`, `dispatch()`.
- [Sources](docs/usage/sources.md): `every()`, `watch_gpio()`, `watch_button()`, `button_sampling()`.
- [Power hold and power-save](docs/usage/power.md): `power_hold()`, `power_release()`, the `power-save` option.
- [Runtimes and delivery](docs/reference/runtimes-and-delivery.md), [Errors and limits](docs/reference/errors-and-limits.md)
- [C API for extensions](docs/reference/c-api.md): raising events from another extension.

## AI usage

The policy for AI-assisted work on this package is in `AI_USAGE.md`, next to this README.

## License

MIT.
