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
- On confirmed live stall: writes one sanitized bundle per distinct stall (a
  new stuck lane, or a new epoch of the same lane), at most 8 per run, and
  **continues** sampling.
- While the worker's handshake is `WorkerClosing` (orderly shutdown) no live
  stall is classified; the terminal observation decides the outcome.
- On proven exit/crash/opaque termination: finalizes once, writes a terminal
  bundle when coherent evidence exists, returns the child status.
- Status decode errors write one bundle from the last coherent publication and
  return supervisor failure (exit 125) without inventing an exit/crash class.
- Handshake timeout closes diagnostics, retains process ownership, waits once,
  returns `WorkerHandshakeFailed` (exit 125). Production `run` never auto-kills.

Exit codes: child exit code; `128+signal` on POSIX crash; `125` for
supervisor-owned failures.

### Worker publication and progress lanes

A supervised emulator publishes from its own thread every 250 ms, so the
heartbeat stays fresh while the worker process runs, whatever its emulation
threads are doing. Runtime work reports through `Emulator/Ports/ProgressPort.h`
(a no-op in standalone runs); each lane is one operation kind on one host
thread, opened when the operation starts and completed when it returns.
The hot path costs only relaxed stores into a slot the calling thread owns (no
lock, no clock read, no shared write); the publisher thread samples the slots
and turns changes into progress records. Lanes the pool of 512 slots cannot
hold are reported as progress loss, never silently dropped.

| Lane | Domain / operation | Covers |
| --- | --- | --- |
| Graphics commands | `CommandProcessor` / `CommandSubmit` | one graphics ring batch, including its suspended waits |
| Compute commands | `CommandProcessor` / `CommandSubmit` | one compute ring run |
| Fence wait | `GpuQueue` / `FenceWait` | `vkWaitForFences` on a submitted command buffer |
| Present | `Presentation` / `PresentationPresent` | swapchain acquire and its fence, then `vkQueuePresentKHR` |
| Guest thread | `GuestThread` / `ExecutionBoundary` | one guest thread's entry point; marked waiting while it is parked in a blocking mutex, condition, semaphore, event flag, event queue, join, rwlock or address wait |

### Classification

Each sample is classified from the age of each busy lane (time since its last
change), not from how long the whole snapshot looked the same:

1. A heartbeat older than the suspicion threshold is `worker_unresponsive`
   (the process stopped publishing; its lane data is stale and is not used).
2. Otherwise the busy lane past its suspicion threshold that is closest to the
   root cause wins: GPU queue, command processor, renderer, presentation,
   video out, HLE. A hung GPU queue keeps the lanes that wait on it busy, so it
   is reported instead of them. Every lane past its threshold is evidence.
3. Busy lanes younger than their threshold are work in flight (`none`); no
   busy lane at all is `healthy_idle`.

After a stale heartbeat recovers, lane ages are measured from the resumed
heartbeat: a process paused as a whole (debugger, `SIGSTOP`, host suspend) is
one `worker_unresponsive` stall, not also a stall of every lane that was busy
when it paused.

Defaults: suspected after 5 s, confirmed after 15 s. Fence and present waits
use 2 s / 8 s, because the worker gives up on those waits after 10 s and exits.

A command batch that is compiling a pipeline (or waiting for another thread's
compile of the same cache) switches its lane to `PipelineCompile`, judged at
60 s / 180 s, so a long synchronous compile is not a `command_processor_stall`.

`guest_deadlock` (always medium confidence): every guest thread is parked in a
blocking wait, no command lane is busy, and nothing has changed for 10 s
(suspected) / 30 s (confirmed). Any guest thread that could not be tracked
rules the verdict out. Guest threads are never stall candidates on their own:
a thread busy in guest code for a long time is normal.

Limits: waits outside the instrumented primitives (guest spin loops, HLE calls
that block without a wait scope, timed waits) leave the thread `active`, so a
hang through them reads as `healthy_idle`, which is conservative. The rule
sees that every thread is parked, not which thread holds what: it does not
build a wait-for graph.

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

## Open defects

- `RuntimeDiagnosticsSubsystem`
  (`source/emulator/src/DevTools/RuntimeSubsystem.cpp:6`) is compiled but never
  registered. `source/KytyScripts.cpp:190` prepares the worker session and
  starts the publisher, and `source/KytyScripts.cpp:267` shuts it down, so
  `Emulator::DevTools::Initialize()` only reports the prepared flag and no
  subsystem calls it. Suggested direction: delete the subsystem and the
  `Initialize()` entry point, or register it and move the lifecycle into it.
