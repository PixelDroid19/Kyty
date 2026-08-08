# Reproducible guest input routes

Kyty can drive a connected virtual pad from a bounded, text-based route. Set
`KYTY_PAD_SCRIPT` to a route file (or to an inline semicolon-separated route)
before starting the guest:

```sh
KYTY_PAD_SCRIPT=/absolute/path/to/reach_first_gameplay.pad \
  ./_build_linux/fc_script scripts/run_guest.lua /path/to/guest-root
```

The clock starts at the first `scePadReadState` or `scePadRead` call, not when
the process launches. Each line uses `start-end:button+button` in seconds; a
single timestamp uses a 300 ms press. Comments begin with `#`. The parser
accepts face, shoulder, directional, and analog actions such as
`left-stick-right` and `right-stick-up`. Analog actions drive the corresponding
axis to its full travel for the interval and return to the neutral value between
intervals. Routes are limited to 4096 entries and 24 hours, and one immutable
route is published per process. Set `KYTY_PAD_SCRIPT_LOG=1` to log only state
transitions.

The route is applied after the host/controller state and is OR-ed with the
diagnostic agent overlay. It reports a connected pad while active, so titles
that gate their startup screens on controller presence follow the same path as
an attached device. It does not alter graphics, timing, or game memory.

## Continuous polling contract

`scePadRead` keeps transition history bounded, but exhausting that history does
not make a connected, stationary pad disappear. A valid poll returns one fresh
copy of the current report when no transition remains, including an updated
timestamp. Scripted and agent input is merged into that report, so a held axis
continues to reach the guest without requiring a new SDL event. The focused
`EmulatorPad.PadReadReturnsCurrentStateAfterHistoryIsDrained` test protects this
contract.

`scripts/input/reach_first_gameplay.pad` is an integration route for the long
opening/tutorial flow used to reproduce the first playable room. It is a
diagnostic route, not a compatibility claim: visual or gameplay acceptance
still requires a bounded guest run and a captured frame after the route.
