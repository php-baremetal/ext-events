---
eyebrow: 'ext-events · PHP API'
lede:    'An event is an object of a Baremetal\Event subclass. Listeners are registered per exact class; now() delivers inline, dispatch() delivers breadth-first and says whether it took the event; stats() counts it all.'
see_also:
  - { href: './sources.md', meta: 'PHP API' }
  - { href: '../reference/runtimes-and-delivery.md', meta: 'Reference' }
prev: { label: 'Overview', href: '../overview.md' }
next: { label: 'Sources', href: './sources.md' }
---

# Events and listeners

## Baremetal\Event

The base class of every event. Declare one class per kind of event; a source or another extension creates
the objects, or you create them yourself and pass them to `Events::now()` or `Events::dispatch()`.

| Property | Type | Meaning |
| --- | --- | --- |
| `$device` | `?object` | The object that raised the event (for example an `ext-i2c` driver for a sample event), or `null`. |
| `$time` | `int\|float`, readonly | When the event was raised, in nanoseconds on the base of `hrtime(true)`: an `int` where PHP integers are 64-bit, a `float` on the 32-bit board. |

<!-- @code-block language="php" label="event classes" -->
```php
<?php
use Baremetal\Event;

final class Tick extends Event {}                 // raised by a source: keep it empty

final class Milestone extends Event              // created in PHP: a constructor is fine
{
    public function __construct(public readonly int $clicks) {}
}
```
<!-- @endcode-block -->

Events raised by a source (`every()`, `watch_gpio()`, `watch_button()`, an extension) are created without
calling the constructor: their properties keep their declared defaults, and `$device` is set when the source
passes an emitter. `$time` is the moment the source raised the event (not when the listener runs), so
`hrtime(true) - $e->time` is how long the event waited. An event you create with `new` takes its `$time` at
the `new`; `Events::now()` and `Events::dispatch()` keep it. `$time` is readonly: writing it throws.

## Events::listen()

<!-- @method name="Events::listen(string $event, callable $listener): void" returns="void" visibility="public static" -->

<!-- @params -->
<!-- @param name="$event" type="string" required -->
The event class name. Pass `Foo::class`: the name is compared byte for byte with the class name as declared,
so a different case or a leading backslash never matches.
<!-- @endparam -->
<!-- @param name="$listener" type="callable" required -->
Called with the event object as its only argument. Returning `false` stops delivery of that event to the
listeners after it.
<!-- @endparam -->
<!-- @endparams -->

Listeners of a class run in registration order. Throws `\TypeError` when `$listener` is not callable, and
`\Exception` with `Events::listen() must be called in setup(); the listener table is frozen afterwards` once
the table is frozen: in the `event-driven` runtime, as soon as the top-level script has returned (also from
inside a listener).

<!-- @callout variant="warning" title="Exact class only" -->
There is no inheritance in matching. If `ButtonA` and `ButtonB` extend `ButtonEvent`, a listener on
`ButtonEvent::class` never runs: register one for each concrete class. A listener on `Baremetal\Event`
receives nothing.
<!-- @endcallout -->

## Events::now()

<!-- @method name="Events::now(Event $event): void" returns="void" visibility="public static" -->

Runs the listeners of `$event`'s class immediately, on the caller's stack, and returns when they are done. It
takes no event slot. Nested `now()` calls nest like ordinary function calls, up to 8: a `now()` called with 8
`now()` deliveries already running throws `\RuntimeException` with `Events::now(): nested too deep (limit 8)`,
to its caller. An exception thrown by a listener stops the chain and propagates to the caller of `now()`.

## Events::dispatch()

<!-- @method name="Events::dispatch(Event $event): bool" returns="bool" visibility="public static" -->

Delivers breadth-first and returns whether the event was accepted. When no `dispatch()` is in progress, the call delivers `$event` at once, then every
event its listeners dispatched meanwhile, batch by batch, and returns when nothing is left. A `dispatch()`
made by a listener during that cascade only appends to the next batch, so it runs after every event of the
current batch.

Every event waiting in the cascade holds a small event slot until it is delivered. `dispatch()` returns
`false`, throws nothing and counts the refusal in `Events::stats()` when no small slot is free (the first time
for a class it also prints a warning), or when the event would be more than 16 levels deep in the cascade (an
event dispatched by a listener of an event dispatched by a listener ..., counted from the event the reactor
delivered).

An exception in a listener stops the cascade: the events still queued are discarded and the exception leaves
the outermost `dispatch()`.

<!-- @code-block language="php" label="now() and dispatch()" -->
```php
<?php
use Baremetal\Event;
use Baremetal\Events;

final class A extends Event {}
final class B extends Event {}
final class C extends Event {}

Events::listen(A::class, function (): void {
    echo "A\n";
    Events::dispatch(new B());   // queued: runs after A's batch
    Events::now(new C());        // inline: runs before this line returns
    echo "A done\n";
});
Events::listen(B::class, fn () => print("B\n"));
Events::listen(C::class, fn () => print("C\n"));

Events::dispatch(new A());
// A
// C
// A done
// B
```
<!-- @endcode-block -->

Use `now()` when the caller needs the listeners' effects before it continues. Use `dispatch()` to announce
something without nesting the listeners of one event inside those of another.

## Events::stats()

<!-- @method name="Events::stats(): array" returns="array" visibility="public static" -->

The event counters since boot, never reset: the slot pools (`size`, `in_use`, `peak`), per event class the
events `delivered`, `dropped` and `coalesced` (and `queued`, the events of the class waiting now), and the
deepest `dispatch()` cascade and `now()` nesting with their refusals. It works in every runtime, and `phpinfo()` shows the same figures. The full
structure is in [Runtimes and delivery](../reference/runtimes-and-delivery.md#statistics-and-warnings).

<!-- @code-block language="php" label="watch for drops" -->
```php
<?php
use Baremetal\Events;

final class Report extends Baremetal\Event {}

Events::listen(Report::class, function (): void {
    foreach (Events::stats()['classes'] as $class => $c) {
        if ($c['dropped'] > 0) {
            printf("%s: %d dropped\n", $class, $c['dropped']);
        }
    }
});
every(60000, Report::class);
```
<!-- @endcode-block -->

## Listener state

The script stays resident, so a variable captured by reference (`use (&$count)`) or a `static` variable in a
listener keeps its value from one event to the next. A listener runs on the PHP task, one event at a time,
and never races another listener.
