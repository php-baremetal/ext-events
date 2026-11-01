# Changelog

## 1.0.2

- Fixed: in the `web-server` runtime the listener table and the dispatch queue outlived the request that
  created them and were used after being freed. They now belong to the request and are dropped when it ends,
  together with any `power_hold()` left behind.
- Changed: `every()`, `watch_gpio()`, `watch_button()` and `button_sampling()` throw in the `web-server`
  runtime, in a request and in the `init` script, where nothing delivers their events: `... is not available
  in the web-server runtime`. `bm_events_source_tag()` (used by ext-i2c `poll()` with an event) refuses in the
  same way.
- Fixed: `watch_button()` with a `double` gesture and no `doubleMs` option never raised `double` (the window
  defaulted to 0, which turns detection off). The window is now 300 ms when `double` is wired; an explicit
  `'doubleMs' => 0` still turns it off.
- Changed: requires `php-baremetal/events-hal` ^1.1 (`bm_hal_request_scoped()`, esp32-events-hal 1.0.2).

## 1.0.1

- Changed: the author's contact email.
- Docs: projects use `"sdk": "esp32@^2.0"`.

## 1.0.0

Initial release.
