---
eyebrow: 'ext-events · Recipe'
lede:    'Run code every N milliseconds without a loop: one timer source per period, state kept in ordinary variables between events.'
see_also:
  - { href: '../usage/sources.md', meta: 'PHP API' }
  - { href: '../reference/runtimes-and-delivery.md', meta: 'Reference' }
prev: { label: 'C API for extensions', href: '../reference/c-api.md' }
next: { label: 'React to a button', href: './react-to-a-button.md' }
---

# Run code periodically

Two timers at different rates: a fast one that counts, a slow one that reports. Any board works; this one is
for a Waveshare ESP32-S3-Zero (N8R8).

## The project

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/periodic-tick",
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

## The script

<!-- @code-block language="php" label="project-src/index.php" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class Fast extends Event {}
final class Report extends Event {}

$count = 0;
$started = microtime(true);

Events::listen(Fast::class, function () use (&$count): void {
    $count++;
});

Events::listen(Report::class, function () use (&$count, $started): void {
    printf("%.1f s: %d fast ticks, %d KB in use\n",
        microtime(true) - $started, $count, intdiv(memory_get_usage(), 1024));
});

every(100, Fast::class);     // 10 per second
every(5000, Report::class);  // every 5 s

echo "periodic-tick: running\n";
```
<!-- @endcode-block -->

After about five seconds the console shows a report with roughly 50 fast ticks, then one every five seconds.

## How it works

- Each `every()` starts its own hardware timer. The timer callback only posts a message; the listener runs
  later, on the PHP task, when the runtime takes the message from the queue.
- The period is kept by the timer, not by your code: a slow listener does not shift the next tick, but
  ticks that pile up beyond the 32-message queue are dropped.
- `$count` lives as long as the program: the script's variables are never reset between events.
- To stop reacting to a timer, have the listener return early on a flag: a started timer cannot be stopped.
