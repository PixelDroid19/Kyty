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
intervals. A scripted `l2` or `r2` press also reports its analog trigger fully
pressed, as a physical trigger does. Routes are limited to 4096 entries and 24 hours, and one immutable
route is published per process. Set `KYTY_PAD_SCRIPT_LOG=1` to log only state
transitions.

Set `KYTY_PAD_RECORD` to a file to record a route from a manual session. The
host keyboard and controller input the guest reads is written in the same
format and on the same clock (seconds from the first pad read), one line per
completed press. A stick is written as its full-travel direction while it is
past half deflection, and a press shorter than a poll is kept for 50 ms so a
replay delivers it to a read. Script and agent input are not recorded. The
file is written only when the user ends the session by closing the window or
with the host quit key; a session that ends in a fault or a signal, or that
exceeds the 4096-entry route limit, writes nothing (an earlier file at that
path is left as it was). A press still held at the end is not written. A
replay follows wall-clock time, so loading-time differences
between the recorded and the replayed run shift the guest state the inputs
meet; record with the shader caches the replay will use.

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

The playable runner's external scene/action checkpoint can use `pad_down` with
an explicit `hold_s` (0.001–30 seconds), followed by `pad_up`. Hold-only milestones
set `min_pad_taps` to zero and specify the expected `min_guest_read_samples` or
`min_guest_read_state_samples`; each hold must be observed by a guest pad read.
Every sequence attempts `pad_clear`, including error exits. Effective input
acceptance additionally requires the checkpoint's expected native-image response;
see [graphics captures](graphics-captures.md#playable-scene-and-action-checkpoints).

Keyboard and active physical-controller button holds have separate ownership.
Releasing or disconnecting one source preserves a button held by the other,
including its transition history. Stick axes retain active-controller priority;
script and agent overlays continue to merge after the host state.
