---
eyebrow: 'ext-events · Recipe'
lede:    'A board that wakes on a timer and a pin, does its work, and light-sleeps the rest of the time, with one stretch held awake.'
see_also:
  - { href: '../usage/power.md', meta: 'PHP API' }
  - { href: '../usage/sources.md', meta: 'PHP API' }
prev: { label: 'React to a button', href: './react-to-a-button.md' }
next: { label: 'No next page', href: '#' }
---

# Sleep between events

A heartbeat every 10 s and the BOOT button (GPIO0) on a Waveshare ESP32-S3-Zero. Between events the chip is
in automatic light sleep; every third heartbeat holds it awake for about 2 s.

## The project

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/power-save",
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
      "sdk": "esp32@^2.0",
      "board": "waveshare-esp32-s3-zero",
      "runtime": "event-driven",
      "storage": "embedded",
      "options": { "power-save": "on" }
    }
  }
}
```
<!-- @endcode-block -->

## The script

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Heartbeat extends Event {}
final class Pressed extends Event {}

Events::listen(Heartbeat::class, function (): void {
    static $n = 0;
    $n++;
    echo "beat $n\n";

    if ($n % 3 === 0) {
        power_hold();                         // no light sleep from here
        $end = microtime(true) + 2.0;
        while (microtime(true) < $end) {
            // time-critical work
        }
        power_release();                      // sleep allowed again
    }
});

Events::listen(Pressed::class, fn () => print("button\n"));

every(10000, Heartbeat::class);   // the only scheduled wake-up
watch_gpio(0, Pressed::class);    // interrupt: no periodic wake-up

echo "power-save: heartbeat every 10 s, light sleep in between\n";
```
<!-- @endcode-block -->

## How it works

- With `power-save` on, the firmware turns on automatic light sleep at boot. When the reactor waits for the
  next event and no other task has work, the chip sleeps; the timer or the pin interrupt wakes it.
- `power_hold()` forbids sleep until `power_release()`. If the listener forgot the release, the runtime would
  release the hold when the listener returns.
- `watch_gpio()` wakes the chip only on an edge. A `watch_button()` here would wake it every 10 ms.

<!-- @callout variant="warning" title="Measure without the USB data cable" -->
While a USB host is connected to the USB-JTAG console the chip does not light-sleep, so the current does not
drop. Power the board from an external 5 V supply with the data cable disconnected to see the effect.
<!-- @endcallout -->
