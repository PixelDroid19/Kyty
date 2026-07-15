# Runtime stall snapshot (v1)

Passive native supervisor for Kyty. Identifies which observable execution domain
stopped, preserves the last coherent state across a worker hang or crash, and
writes a deterministic privacy-safe diagnostic bundle.

## Binary

`kyty_devtools` (target in `source/devtools/`) links only `kyty_devtools_supervisor`
and `kyty_devtools_core`. It never links the emulator.

## Commands

### `run --output-dir ABS_DIR [--recording=full|metrics-only] -- WORKER [ARG...]`

- Requires an **absolute** `--output-dir`. There is no cwd default.
- Creates an exclusive shared mapping and parent-liveness pipe, launches the
  worker with fixed fds 3/4 and `KYTY_DEVTOOLS_BOOTSTRAP_V1`, handshakes, then
  samples progress/timeline/health.
- On confirmed live stall: writes one sanitized bundle and **continues** sampling.
- On proven exit/crash/opaque termination: finalizes once, writes a terminal
  bundle when coherent evidence exists, returns the child status.
- Status decode errors write one bundle from the last coherent publication and
  return supervisor failure (exit 125) without inventing an exit/crash class.
- Handshake timeout closes diagnostics, retains process ownership, waits once,
  returns `WorkerHandshakeFailed` (exit 125). Production `run` never auto-kills.

Exit codes: child exit code; `128+signal` on POSIX crash; `125` for
supervisor-owned failures.

### `self-test --output-dir ABS_DIR --mode=MODE ...`

Internal harness. Re-execs this binary as `synthetic MODE` under supervision.

Modes: `blocked-lane`, `publication-stop`, `parent-disconnect`, `privacy-canary`,
`normal-exit`, `crash`.

### Synthetic worker (`synthetic MODE`)

Not a user-facing production command. Used by `self-test` and focused tests.

## Bundle contents

Atomic directory `stall-bundle-<generation>/` with:

- `progress.json`, `threads.json`, `wait_graph.json`, `gpu.json`
- `timeline.bin` (64-byte header + 72-byte records)
- `manifest.json` (artifact CRCs, loss owners kept separate)
- `complete.marker` (64-byte LE trailer written last)

Automatic artifacts are allowlist-only. No argv, environment, guest paths,
workload IDs, shader hashes, logs, textures, or screenshots.

## Not in v1

- `capture-now`, attach mode, control queue
- Automatic worker termination on stall
- Arbitrary C++/shader hot reload
- Network or remote debugging

## Privacy

Canary values placed in synthetic argv/env/local stacks must not appear in any
automatic artifact byte. Bundle writer serializes only compiled keys and
numeric/enumerated fields.
