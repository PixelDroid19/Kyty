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
  samples progress/timeline/health. Each accepted publication updates the
  worker-owned monotonic heartbeat at wire offset `0x140`; the supervisor
  consumes that value rather than treating its own sampling clock as worker
  activity.
- On confirmed live stall: writes one sanitized bundle and **continues** sampling.
- On proven exit/crash/opaque termination: finalizes once, writes a terminal
  bundle when coherent evidence exists, returns the child status.
- Status decode errors write one bundle from the last coherent publication and
  return supervisor failure (exit 125) without inventing an exit/crash class.
- Handshake timeout closes diagnostics, retains process ownership, waits once,
  returns `WorkerHandshakeFailed` (exit 125). Production `run` never auto-kills.

Exit codes: child exit code; `128+signal` on POSIX crash; `125` for
supervisor-owned failures.

### Handshake lifecycle

The shared header uses release/acquire state transitions:

`ParentReady` → `WorkerReady` → `WorkerClosing`

If worker initialization fails after publishing its identity, it transitions
to `WorkerRejected`. A parent may accept an already validated handshake while
it is in `WorkerClosing`, which covers workers that finish before a sampling
poll observes `WorkerReady`; `WorkerRejected` is never accepted as a valid
worker.

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

## Host support status

The verified supervisor path in this repository is the POSIX implementation.
The CMake file names Windows supervisor adapters, but those source files and
the Windows orchestration path are not yet present. In addition, `Supervisor.cpp`
currently owns POSIX-only `pipe`, fixed file-descriptor, `close`, and absolute
POSIX-path assumptions; adding only the five missing adapter files would not
make a Windows build or runtime path complete. No Windows toolchain is
available in the current workspace, so Windows runtime support remains
unverified and must not be inferred from the portable protocol types or from a
successful non-Windows build.
