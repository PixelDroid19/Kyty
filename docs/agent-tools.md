# Kyty agent tools (realtime)

Opt-in local Unix socket that lets an agent (or human) control and observe a
running emulator **in realtime** without Python, xdotool, or window screenshots.

This is the **only** supported realtime debugging interface for developers and
automated agents. Do not introduce a second protocol or debug framework.

## Protocol version

Live wire version: **`protocol_version`: 3** (`kAgentProtocolVersion`).

Every response envelope (success or error) includes the same field:

```json
{"id":1,"ok":true,"protocol_version":3,"result":{...}}
{"id":1,"ok":false,"protocol_version":3,"error":{"code":"malformed","message":"..."}}
```

`help`, `ping`, `status`, `diagnostics`, and `events` result bodies also embed
`protocol_version` where they are self-describing snapshots. CLI transport
failures print the same envelope shape with stable `error.code` values.
Requests are limited to 4096 bytes and responses to 262144 bytes by the shared
wire contract. Diagnostics keeps full aggregate counts but bounds detailed load
plan arrays; `load_plan.truncated:true` means agents should use the counts as
authoritative rather than assuming every detail was serialized.

## Enable

Start the guest with an absolute socket path and a capture directory:

```bash
export KYTY_AGENT_SOCK=/tmp/kyty-agent.sock
export KYTY_NATIVE_CAPTURE_DIR=/abs/path/to/_scratch_playable/native_frames
# then launch fc_script / run_guest.lua as usual
```

If `KYTY_AGENT_SOCK` is unset, the emulator behaves exactly as before (no thread,
no socket). Lifecycle events still accumulate in-process when AgentTools init
runs so later diagnostics remain useful.

## Client

```bash
kyty_agent --sock /tmp/kyty-agent.sock doctor
kyty_agent --sock /tmp/kyty-agent.sock wait-ready --timeout-ms 30000
kyty_agent --sock /tmp/kyty-agent.sock status
kyty_agent --sock /tmp/kyty-agent.sock wait-present --delta 20 --timeout-ms 15000
kyty_agent --sock /tmp/kyty-agent.sock wait-phase interactive --timeout-ms 45000
kyty_agent --sock /tmp/kyty-agent.sock capture
kyty_agent --sock /tmp/kyty-agent.sock pad tap cross
kyty_agent --sock /tmp/kyty-agent.sock watch --seconds 10 --min-fps 2
```

`KYTY_AGENT_SOCK` may be omitted on the CLI when the same env var is already set.

Stdout is one JSON object per call. Exit codes (stable):

| Exit | Meaning | Typical `error.code` |
|------|---------|----------------------|
| `0` | Success | — |
| `1` | Tool failure or `healthy:false` | server `error.code`, or `timeout` / `unhealthy` |
| `125` | Transport / usage | `transport`, `usage` |

Do not “fix” a `125` by sleeping longer. Machine-readable example:

```json
{"ok":false,"protocol_version":3,"error":{"code":"transport","message":"socket_not_live"}}
```

## Tools

| Tool | Purpose |
|------|---------|
| `help` / `ping` / `doctor` | Discoverability and liveness |
| `wait-ready` | Poll until the socket accepts `ping` (boot/relaunch; default 30s) |
| `status` / `diagnostics` | Frame/present/FPS, `phase`, `ms_since_*`, pad overlay |
| `events` / `last_error` / `wait_event` | Bounded structured event ring |
| `sync-waits` | Live snapshot of opt-in blocked pthread condition waits (`KYTY_SLOT_TRACE=1`) |
| `threads` | Live snapshot of guest pthread lifecycle state |
| `capture` | Native VideoOut readback on next present; scores the BMP by default (`score:false` / `--no-score` to skip) |
| `score` | Classify last capture (or `--path ABS.bmp`) with coarse frame heuristics |
| `pad_down` / `pad_up` / `pad_tap` / `pad_axis` / `pad_clear` | Diagnostic pad overlay. `pad_tap` schedules release→press→release across **three** `PadReadState` samples (not host sleep). |
| `pad hold BUTTON --delta N` | CLI helper: `pad_down` → `wait_present --delta N` → `pad_up` (for `(HOLD) Skip`) |
| `wait_present` / `wait_frame` | Wait until absolute `--min` **or** relative `--delta` from now |
| `wait_phase` | Wait until `status.phase` matches for `--stable-ms` |
| `watch` | Sample progress for N ms; classify stalls; optional capture + score |

### `status.phase`

| Phase | Meaning |
| --- | --- |
| `not_ready` | Graphics/window not ready |
| `booting` | Graphic ready, very few presents yet |
| `loading` | Presents advance but FPS below interactive (loading card / heavy work) |
| `interactive` | FPS recovered; safe to capture / light pad |
| `stalled` | Presents stopped |

Prefer `wait-present --delta` and `wait-phase interactive` over absolute
`wait-present --min` with multi-minute timeouts.

```bash
kyty_agent pad tap cross
kyty_agent wait-phase loading --timeout-ms 10000      # optional
kyty_agent wait-phase interactive --timeout-ms 45000
kyty_agent capture --no-score
```

Protocol: JSON lines, **`protocol_version` 3**. Pad tools are **diagnostic_input**
only — not gameplay acceptance (same rule as `KYTY_AUTO_CROSS`).

### `status.frontier` (current state)

Coarse label so an automated agent can pick the first real runtime frontier
without parsing free-form logs:

| Frontier | Meaning |
| --- | --- |
| `none` | No strong signal yet |
| `launch` | Graphics not ready / host launch path |
| `unsupported` | BringUp halt / breaker / unsupported contract |
| `stall` | Presents stopped or classified stall |
| `graphics` | Graphic ready and presents advancing |
| `interactive` | `phase=interactive` |
| `error` | Other structured last_error |

`status.phase` remains the host-visible present/FPS phase. `frontier` folds
`last_error` + phase into one decision field. **Events are history**; status is
**current state**. A recovered interactive/graphics phase supersedes historical
nonfatal errors. Unsupported halts and actual host crashes remain terminal.

### Event ring (history)

Bounded ring (capacity **512**). Each event has monotonic `seq`, stable
`t_ms` (ms since ring start), `kind`, `code`, `message`. When capacity is
exceeded, oldest events are overwritten; `dropped` / `overflowed` stay visible
in `events` and `status.event_ring` / `diagnostics.event_ring` so diagnostics
remain useful when history is truncated.

Lifecycle `code` values (sanitized messages — no private host paths):

| Code | Seam |
| --- | --- |
| `startup_config` | BringUp mode applied |
| `executable_discovered` | Primary guest executable identity |
| `module_discovery` | Load plan counts |
| `module_loaded` | Adjacent module relative key |
| `missing_import` | Missing Func import stub assign |
| `relocation_failure` | Unresolved relocation |
| `bringup_continue` / `bringup_halt` / `bringup_breaker` | BringUp decision |
| `graphics_init` | Graphic ready (once) |
| `first_frame` / `first_present` | First host frame/present (once) |
| `input_ready` | Pad overlay path ready (once) |
| `guest_exit` | Guest main returned |
| `host_crash` | Actual host invariant/fault path |

An intentional strict `not_implemented_halt` is reported as
`bringup_halt`, not `host_crash`.

Example:

```bash
kyty_agent events --last 50
# {"id":…,"ok":true,"protocol_version":3,"result":{"protocol_version":3,"schema":"event_history",
#  "ring":{"capacity":512,"size":…,"next_seq":…,"dropped":0,"overflowed":false},
#  "events":[{"seq":1,"t_ms":12,"kind":"info","code":"startup_config","message":"mode=strict explicit=false"},…]}}
```

Malformed requests—including trailing bytes after the top-level JSON
object—return typed errors (`malformed` / `unsupported` / `policy_denied`) and
**do not** mutate guest state or crash the process.

Button names are PlayStation face buttons (`cross`, `circle`, `square`, `triangle`).
UI glyphs that look like an Xbox “X” on PS titles are usually **Cross** (confirm),
not Square — confirm with a capture crop before sweeping inputs.

### Detecting freezes (e.g. Loading… at ~1 FPS)

When the UI sticks on a loading card and FPS collapses:

```bash
kyty_agent --sock /tmp/kyty-agent.sock watch --seconds 10 --min-fps 2
```

Exit `1` with `"healthy":false` means a stall was classified. Important fields:
- `stall_code`: `present_stalled` | `frame_stalled` | `low_fps` | `not_ready`
- `frame_delta` / `present_delta` / end `fps` / `ms_since_*`
- `capture`: VideoOut BMP of the stuck screen (needs `KYTY_NATIVE_CAPTURE_DIR`)
- `score`: frame metrics when a capture succeeded (heuristic labels only; see below)
- `last_error`: last structured agent/runtime error, if any

`watch` answers “is it stuck and what does the screen show?”. For “which HLE/GPU
progress lane died?”, also launch under `kyty_devtools` (passive stall bundles).

If `status.phase` is `loading`, use `wait-phase interactive` (not a 3-minute
absolute present floor). If it flips to `stalled`, capture and inspect
`sync-waits` / `last-error`.

### Frame capture scores (diagnostic only)

UI can still look correct while textures or alpha are wrong, and the reverse.
Use native capture scoring as a coarse gate, not as a list of known title bugs:

```bash
kyty_agent capture                 # BMP + score JSON; exit 1 if unhealthy
kyty_agent score                   # re-score last capture
kyty_agent score --path /abs/x.bmp # score an existing native BMP
```

Score verdicts (`hot_corruption`, `white_world`, `stripey`, `low_entropy`,
`healthy`) are **heuristic labels**. They must not be treated as standing orders
to “fix yellow/red slabs” or “fix white world.”

- Localized warm window light, god-rays, and bloom are often intentional art;
  do not rewrite lighting because a metric flagged saturated pixels.
- Prefer a known-good reference of the same scene. Investigate missing or
  crushed textures, opaque black quads where alpha should reveal the scene,
  clears, sampling, blend, and layout before chasing color wash narratives.
- A `healthy` score does not prove correct rendering; an unhealthy score is a
  signal to form one falsifiable hypothesis from the BMP, not a license to
  invent guest state.

Metrics exposed: `white_ratio`, `saturated_ratio`, `blowout_ratio`,
`hot_block_ratio`, `sparkle_ratio`, `entropy`, `color_bins`, `stripey`.

Example agent loop when presents advance but the frame looks wrong:

1. `status` — confirm presents advance and read `phase`.
2. `capture` — record path + metrics under an untracked scratch dir.
3. Compare to a known-good reference when available; form one hypothesis.
4. Do **not** treat automated input or a score label alone as acceptance.

## Privacy and safety

- Absolute socket path only; no TCP/remote.
- One client at a time (`busy` if a second connects).
- Responses never include private guest roots. Lifecycle publishing replaces
  POSIX and Windows absolute paths and title identifiers with safe markers.
- Capture fails with `capture_dir_unset` unless `KYTY_NATIVE_CAPTURE_DIR` is set.
- Optional `KYTY_NATIVE_CAPTURE_MAX_EDGE=1280` downscales the saved BMP (readback
  still uses the full VideoOut extent). Prefer this on 8GB hosts during agent
  loops; delete BMPs after making PNG previews.
- Observation **never** wakes guest EventFlags, fabricates ThreadFlag, or skips
  traps. Pad tools only touch the diagnostic overlay.
- Separate from `kyty_devtools` (passive supervisor stays passive).

## Typical agent loop (first real frontier)

1. Launch emulator with `KYTY_AGENT_SOCK` + `KYTY_NATIVE_CAPTURE_DIR` (no
   `KYTY_AUTO_CROSS` during gameplay verification).
2. `kyty_agent wait-ready` (then `doctor` if needed) until ping succeeds —
   confirm `"protocol_version":3` on every response.
3. `status` → read `phase` **and** `frontier`. Map:
   - `frontier=launch` → boot/window path
   - `frontier=unsupported` → `events` / `last-error` / `diagnostics` BringUp
   - `frontier=stall` → `watch` + `sync-waits`
   - `frontier=graphics` → wait for interactive FPS
   - `frontier=interactive` → safe sparse pad + capture
4. Use `wait-present --delta` and `wait-phase interactive` instead of absolute
   present floors with multi-minute timeouts.
5. Sparse `pad tap` only when a capture shows a menu; then `pad clear`.
6. `capture` once `phase=interactive`; treat `healthy:false` as a signal.
7. Exit `125` / `error.code=transport` means the guest/socket is gone — relaunch.
8. `events` / `last-error` / `diagnostics` (includes `event_ring.dropped`) when
   something looks wrong — even if the ring overflowed.
