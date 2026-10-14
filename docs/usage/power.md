---
eyebrow: 'ext-events · PHP API'
lede:    'With the power-save option the chip light-sleeps by itself whenever every task waits. power_hold() and power_release() keep it awake for a stretch; without power-save they do nothing.'
see_also:
  - { href: '../recipes/power-save.md', meta: 'Recipe' }
  - { href: './sources.md', meta: 'PHP API' }
  - { href: 'package:php-baremetal/esp32-events-hal', meta: 'HAL', label: 'esp32-events-hal' }
prev: { label: 'Sources', href: './sources.md' }
next: { label: 'Runtimes and delivery', href: '../reference/runtimes-and-delivery.md' }
---

# Power hold and power-save

## The power-save option

`power-save` is an SDK option, off by default. Turn it on in the project `composer.json`:

<!-- @code-block language="json" label="composer.json, extra.baremetal" -->
```json
{
  "runtime": "event-driven",
  "options": { "power-save": "on" }
}
```
<!-- @endcode-block -->

The firmware then enables automatic light sleep at boot: whenever every task is waiting (the `event-driven`
runtime waiting for the next event, or `init-loop` between two `loop()` calls with a `loop-interval`), the
chip drops into light sleep, keeps its RAM, and wakes on the next timer, interrupt or network activity. No
sleep call is needed in PHP.

- `init-loop` with `loop-interval` 0 never sleeps: the PHP task never waits.
- While a USB host is connected to the USB-JTAG console, the chip does not light-sleep. Power the board
  from an external 5 V supply without the data cable to see it sleep.
- `ext-i2c` refuses `Bus::CORE1` while power-save is on.

## power_hold()

<!-- @method name="power_hold(): void" returns="void" -->

Forbids light sleep until the matching `power_release()`. Calls nest: each `power_hold()` needs its own
`power_release()`. Use it around work that must not be slowed or interrupted by sleep, such as a tight timing
loop.

## power_release()

<!-- @method name="power_release(): void" returns="void" -->

Undoes one `power_hold()`. Extra calls are ignored.

## Holds end with the event

In the `event-driven` runtime, any hold still standing when an event's listeners return is released, so a
forgotten `power_release()` cannot keep the chip awake for good, and a hold never spans two events. Under
`init-loop` there is no such release: a hold lasts until `power_release()`.

Without power-save (or on a build where power management could not start) both functions do nothing, so the
same script runs either way.

## What wakes the chip

| Source | Wakes the chip |
| --- | --- |
| `every($ms, ...)` | Every `$ms`. Pick the longest period that works. |
| `watch_gpio()` | Only on an edge. |
| `watch_button()` | Every 10 ms while the button sampler runs; `button_sampling(false)` stops it. |
| `ext-i2c` background samplers | While any sampler is running. |
