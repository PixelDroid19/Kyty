# Kyty agent tools (realtime)

Opt-in local Unix socket that lets an agent (or human) control and observe a
running emulator **in realtime** without Python, xdotool, or window screenshots.

## Enable

Start the guest with an absolute socket path and a capture directory:

```bash
export KYTY_AGENT_SOCK=/tmp/kyty-agent.sock
export KYTY_NATIVE_CAPTURE_DIR=/abs/path/to/_scratch_playable/native_frames
# then launch fc_script / run_guest.lua as usual
```

If `KYTY_AGENT_SOCK` is unset, the emulator behaves exactly as before (no thread,
no socket).

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

Stdout is one JSON object per call. Exit `0` on success, `1` on tool failure or
`healthy:false` (stall **or** frame corruption), `125` on transport/usage errors
(guest dead, stale socket, bad args). Do not “fix” a `125` by sleeping longer.

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

Protocol: JSON lines, schema version 1. Pad tools are **diagnostic_input** only —
not gameplay acceptance (same rule as `KYTY_AUTO_CROSS`).

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
- Responses never include private guest roots.
- Capture fails with `capture_dir_unset` unless `KYTY_NATIVE_CAPTURE_DIR` is set.
- Optional `KYTY_NATIVE_CAPTURE_MAX_EDGE=1280` downscales the saved BMP (readback
  still uses the full VideoOut extent). Prefer this on 8GB hosts during agent
  loops; delete BMPs after making PNG previews.
- Does not wake guest EventFlags, fabricate ThreadFlag, or skip traps.
- Separate from `kyty_devtools` (passive supervisor stays passive).

## Typical agent loop

1. Launch emulator with `KYTY_AGENT_SOCK` + `KYTY_NATIVE_CAPTURE_DIR` (no
   `KYTY_AUTO_CROSS` during gameplay verification).
2. `kyty_agent wait-ready` (then `doctor` if needed) until ping succeeds.
3. `status` → read `phase`. Use `wait-present --delta` and `wait-phase
   interactive` instead of absolute present floors with multi-minute timeouts.
4. Sparse `pad tap` / `pad down`+`pad up` only when a capture shows a menu or
   HOLD Skip; then `pad clear`. After a confirm that may load, prefer
   `wait-phase loading` (optional, short) then `wait-phase interactive`.
5. `capture` (auto-scores) once `phase=interactive`; treat `healthy:false` as a
   frontier signal. Delete large BMPs after making a PNG preview on 8GB hosts.
6. Exit `125` means the guest/socket is gone — relaunch; do not extend sleeps.
7. `events` / `last-error` when something looks wrong.
