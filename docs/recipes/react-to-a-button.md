---
eyebrow: 'ext-events · Recipe'
lede:    'Count clicks on the BOOT button, reset on a long press, and announce every tenth click as an event of its own.'
see_also:
  - { href: '../usage/sources.md', meta: 'PHP API' }
  - { href: '../usage/events-and-listeners.md', meta: 'PHP API' }
  - { href: 'package:php-baremetal/ext-gpio', meta: 'Extension', label: 'ext-gpio' }
prev: { label: 'Run code periodically', href: './periodic-tick.md' }
next: { label: 'Sleep between events', href: './power-save.md' }
---

# React to a button

The BOOT button of a Waveshare ESP32-S3-Zero (N8R8) is on GPIO0 and pulls it to GND when pressed, which is
what `watch_button()` expects by default. Use the project `composer.json` of
[Run code periodically](./periodic-tick.md).

## The script

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Click extends Event {}
final class LongPress extends Event {}

final class Milestone extends Event            // created in PHP: a constructor is fine
{
    public function __construct(public readonly int $clicks) {}
}

$clicks = 0;

Events::listen(Click::class, function () use (&$clicks): void {
    $clicks++;
    echo "click $clicks\n";
    if ($clicks % 10 === 0) {
        Events::dispatch(new Milestone($clicks));
    }
});

Events::listen(LongPress::class, function () use (&$clicks): void {
    $clicks = 0;
    echo "reset\n";
});

Events::listen(Milestone::class, function (Milestone $e): void {
    printf("%d clicks!\n", $e->clicks);
});

watch_button(0, ['click' => Click::class, 'held' => LongPress::class], ['holdMs' => 1500]);

echo "press BOOT; hold it 1.5 s to reset\n";
```
<!-- @endcode-block -->

## What happens

| Action | Result |
| --- | --- |
| Short press and release | `Click` on release. |
| Press held 1.5 s | `LongPress` while still pressed; no `Click` for that press. |
| Tenth click | The `Click` listener dispatches a `Milestone`, delivered right away by `dispatch()`. |

Add `'double' => DoubleClick::class` to `$on` and `'doubleMs' => 300` to the options for a double-click;
every single `click` is then delayed by up to 300 ms while the sampler waits for a second one.

## The one-event variant

When one event per press is enough, `watch_gpio()` raises it from the pin interrupt with a fixed 200 ms
debounce, and costs nothing while the button is idle:

<!-- @code-block language="php" label="watch_gpio" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Pressed extends Event {}

Events::listen(Pressed::class, fn () => print("pressed\n"));
watch_gpio(0, Pressed::class);
```
<!-- @endcode-block -->

## Gotchas

- A button wired to 3.3 V instead of GND needs `'activeLow' => false` (the pin then gets a pull-down).
- At most four buttons; a listener on a shared parent class receives nothing, so register each concrete
  class.
- To drive an LED from a listener, add `php-baremetal/ext-gpio` and call `gpio_write()` there.
