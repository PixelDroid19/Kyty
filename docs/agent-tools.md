# Kyty Agent Tools

`kyty_agent` is the supported local interface for inspecting and automating a
running Kyty emulator. It is intended for developers, CI jobs, and coding
agents. Every response is one bounded JSON object, so the same command is
usable interactively or by automation.

The transport never opens a network port:

- Windows uses a local named pipe such as `\\.\pipe\kyty-agent-dev`;
- Linux and macOS use an absolute Unix socket path with permissions `0600`.

Set the same endpoint for the emulator and CLI:

```powershell
$env:KYTY_AGENT_ENDPOINT = '\\.\pipe\kyty-agent-dev'
.\_build_windows_release\fc_script.exe .\run.lua
.\_build_windows_release\agent\kyty_agent.exe wait-ready --timeout-ms 30000
```

```sh
export KYTY_AGENT_ENDPOINT=/tmp/kyty-agent-dev.sock
./_build_linux/fc_script ./run.lua
./_build_linux/agent/kyty_agent wait-ready --timeout-ms 30000
```

`--endpoint ENDPOINT` overrides `KYTY_AGENT_ENDPOINT`.

## Diagnostic workflow

For an early vertex native-wave admission failure, opt in before launch with
`KYTY_NATIVE_WAVE_REPORT=<private prefix>` and optionally
`KYTY_NATIVE_WAVE_SHADER_DUMP=<existing private directory>`. The first
wave-sensitive vertex input writes `<prefix>-native-wave-input.json` (at most
8 KiB), with actual draw arguments, input primitive type and raw index offset,
architectural-width/proof metadata and raw GE register provenance.
Its optional `ge_state.output_state` also records four
output-register words with assignment provenance, the pixel-program identity,
and bounded effective pixel-input/raster state. Effective values are explicitly
emulator state, not proof of raw assignment; an absent output snapshot is null.
Unknown arguments remain null. ES and GS-back copies
use separate metadata/readable-range leases, at most 256 KiB each, and complete
before filesystem writes. The exclusive files are `native-wave-es.bin` and
`native-wave-gs-back.bin`; each has its own copy/write status. This works with
Silent printf. It observes inputs before pipeline admission, does not count a
skipped draw and does not establish execution or a linked-program proof.

Use condition-based commands instead of fixed sleeps:

1. `wait-ready` proves the native transport and protocol are live.
2. `doctor` checks protocol health and the current runtime state.
3. `wait-phase interactive` or `wait-present --delta 60` proves progress.
4. `watch` classifies frame, presentation, and FPS stalls.
5. `events`, `last-error`, `threads`, `sync-waits`, and `diagnostics` narrow the
   failing subsystem.
6. `capture` and `score` preserve bounded visual evidence.

Useful commands:

```text
kyty_agent status
kyty_agent diagnostics
kyty_agent perf-snapshot
kyty_agent sync-waits
kyty_agent threads
kyty_agent events --last 100
kyty_agent last-error
kyty_agent watch --seconds 15
kyty_agent capture --timeout-ms 10000
kyty_agent score
```

For a bounded point-in-time diagnostic followed by an exact event wait, use
the snapshot's `event_seq_end` as the cursor for the next request:

```text
kyty_agent snapshot --events 100
kyty_agent wait-event --kind error --after-seq <event_seq_end> --code device_lost --timeout-ms 10000
```

`stable:false` means the event sequence changed while the snapshot sections
were being assembled. The snapshot is observational and bounded; it is not a
compatibility claim or proof that a workload rendered correctly.

`capture` writes an emulator-native PNG readback, not a desktop screenshot.
Start the emulator with `KYTY_NATIVE_CAPTURE_DIR` set to an absolute or
process-relative output directory. `KYTY_NATIVE_CAPTURE_MAX_EDGE` bounds the
longest written edge and `KYTY_NATIVE_CAPTURE_KEEP` bounds retained PNG/JSON
pairs.

`score` only analyzes the most recent native capture; it does not accept a
caller-supplied path.

Render-target lifetime diagnostics can be armed from a visually confirmed
native capture instead of an unstable presentation number. Set
`KYTY_TRACE_RT_LIFETIME_AFTER_CAPTURE=N` with the lifetime trace, where `N` is
a strict positive decimal ordinal. The gate opens only after the Nth newer
successful agent/manual capture and affects render activity beginning with the
following frame. Automatic, trigger-file, failed, timed-out, and superseded
captures do not advance the ordinal. A pending explicit request takes priority
over automatic first/interval policy and retains exact request ownership until
publication. Combine this gate with `KYTY_TRACE_RT_LIFETIME_COLOR_FORMAT` to
select one Vulkan color format without relying on a process-specific guest
address. Both controls are diagnostic-only and disabled by default.

For a scene whose timing is not repeatable, start the process with both
`KYTY_TRACE_RT_LIFETIME=1` and
`KYTY_AGENT_TRACE_RT_LIFETIME_ARM=1`, visually confirm the damaged scene with
an explicit native capture, and then issue:

```text
kyty_agent trace-rt-lifetime-arm
```

The response confirms only that one request is pending. The render thread
consumes it atomically on subsequent eligible activity and opens the existing
bounded lifetime trace; `ARM`, `WRITE`, `PASS_BEGIN`, `SAMPLE`, and
`SAMPLE_ATTACHMENT_ALIAS` records remain the actual evidence. Repeated requests
fail after the gate is pending or open, and the state resets only with the
process. The command does not expose or mutate render targets, guest memory, or
host paths. Optional capture-ordinal and minimum-present gates remain
conjunctive; omit them when the explicit command itself is the scene fence.

Color and depth lifetime selectors are separate diagnostic modes. Do not set a
color address/format selector together with a depth address/extent/format
selector: the render thread rejects that combination and
`trace-rt-lifetime-arm` reports `trace_disabled`. For a visually correlated
depth investigation, use the native capture only to confirm the scene, then arm
a process started with the depth selector alone.

Fatal host faults can write a bounded JSON context by setting
`KYTY_CRASH_REPORT` to an absolute scratch path. When only `KYTY_CAPTURE_DIR`
is set, the runtime uses `crash-context.json` inside that directory. The report
contains registers and up to 128 stack words. Setting `KYTY_CRASH_MEMORY=1`
also captures at most 24 fault-safe 64-byte windows around plausible guest-data
pointers found on the stack on supported POSIX hosts. Memory windows are
disabled by default, may contain guest data, and must never be committed.

After a fatal exit, inspect the bounded report through the local CLI:

```text
kyty_agent crash-context --path /absolute/scratch/crash-context.json
```

Vulkan device-loss evidence is separate from a host crash context. Start the
process with `KYTY_SUBMIT_FAULT_TRACE=1` to retain eight submit attempts and
publish the first `device_lost` fatal event, even with `PrintfDirection=Silent`.
Optionally set `KYTY_SUBMIT_FAULT_REPORT` to a new scratch file for a durable
JSON report (at most 8 KiB). The runtime creates no directories and never
overwrites an existing file. These settings are process-local opt-ins, not
agent-supplied paths or commands.

Use `events`, `last-error` or `wait-event --kind fatal --code device_lost` while
the process is live. The event's `report` field distinguishes disabled, written
and failed file output. The JSON preserves the failing command-buffer context
separately from the bounded attempt trail: `completed` means the submit call
returned, **not** that its fence completed. A missing context or rolled-out
attempt remains unknown. Report failure never retries a Vulkan call, publishes
GPU completion or replaces the original fatal exit. Keep raw reports outside
Git; they are diagnostic evidence, not playability acceptance.

To identify the work inside the failing submission, also set
`KYTY_RECENT_DRAW_REPORT` to another new scratch file (it requires
`KYTY_SUBMIT_FAULT_TRACE=1`). A ring of the newest recorded guest draws and
dispatches — `KYTY_RECENT_DRAW_CAPACITY`, 1–256, default 64; an invalid value
disables it — is written once, at the same first device loss, at most 128 KiB.
Unlike the first-occurrence draw census, repeated draws of one shader are kept.
Each record carries the queue, slot and host sequence of its command buffer,
the guest submit and PM4 packet when known, the guest shader checksums and
only the draw/dispatch arguments needed for correlation; no guest buffer is
copied. `recorded` is the only stage a record proves by itself.
`submit_called`, `submit_result` and `gpu_completed` are joined from that
host submission and stay `null` until observed; `gpu_completed` means the
command buffer's fence was observed signaled. The event's `draws` field
reports `disabled`, `written`, `size_limit`, `open_failed` or `write_failed`.
Internal depth/stencil copy draws are not recorded.

The first-failure latch copies both bounded trails before serializing or writing
either report. Peer recording during slow file output cannot evict entries from
those copies. Their separate lock acquisitions are not an atomic GPU snapshot.
Offline joins require a known nonzero host sequence; two unknown sequences do
not identify the same submission. An immediate submit report can instead identify
its exact nonzero attempt ID. Generated interpolation geometry cache candidates
use the pixel shader identity, not the vertex identity.

The fault report is version 2: fields the failing command buffer never had
(no host submission, no command-processor context) are `null`, and
`presented_frame` replaces the former `frame` counter. Independently of these
opt-ins, every guest draw the renderer declines to emit is counted by reason;
the first of each reason is published as a `warn` event with code
`draw_skipped`, and the counts appear as `skipped_draws` in the recent-draw
report. A skipped draw is a strict-mode omission to fix, not a valid result.

`KYTY_SKIPPED_DRAW_REPORT=<scratch-prefix>` optionally retains the first event
per reason as an exclusively created `<scratch-prefix>-<reason>.json`. Version 2
adds bounded `ge_state` metadata for unsupported GE draws: ES, merged GS back and
legacy GS bases, checksums, resource fields, user SGPR words and GE controls. It
does not dereference program or resource addresses. A zero legacy GS base does
not imply an absent merged back program. No directory is created or existing
evidence overwritten; these process-local reports belong outside Git.

With that report enabled, `KYTY_SKIPPED_SHADER_DUMP=<existing-scratch-directory>`
also copies the first unsupported GE draw's registered ES and GS-back program
spans to `skipped-ge-es.bin` and `skipped-ge-gs-back.bin`. Each copy is capped at
256 KiB, uses a guest readable-range lease, and goes through the shared diagnostic
dump budget/exclusive writer. The report distinguishes unmapped, unreadable,
complete and truncated snapshots and records file-write status. This additional
opt-in captures private workload code; keep the files outside Git. It is never
an agent-protocol memory reader or a substitute shader.

`wait_event` returns `event_cursor_lost` when `--after-seq` predates the
bounded retained event history; reacquire a fresh snapshot before waiting.

Controller automation is explicitly diagnostic input:

```text
kyty_agent pad tap cross
kyty_agent pad tap cross --at-present 8000 --repeat 2 --present-delta 40
kyty_agent pad hold right --delta 120 --timeout-ms 10000
kyty_agent pad axis left_x 255
kyty_agent pad clear
```

`pad tap --at-present` commits one bounded local schedule (at most eight
targets) before its response is returned; closing or losing that request socket
does not cancel it. `at-present` is an absolute future presentation count,
`repeat` defaults to one, and a repeated tap requires a positive
`--present-delta`. The presentation path starts the existing
release → press → release FSM immediately after it records the exact target
present. The press is held for two advancing guest samples, while
`delivered_taps` advances on the first sampled press.
If a target is missed, its button is held, or the prior FSM is still pending,
that target is cancelled rather than delayed; `status.pad` exposes
`scheduled_taps`, `next_target_present`, and `cancelled_scheduled_taps`.
`pad clear` and emulator shutdown empty the pending schedule.

It is evidence for reaching and exercising a runtime frontier, not by itself a
gameplay compatibility claim.

Address-coherency timing in `diagnostics.performance` includes
`dispatch_writeback`, `guest_address_prepare`, `guest_address_residency` and
`guest_address_refresh`, each with `_calls`, `_ns` and `_max_ns` fields.
Dispatch write-back measures the processor drain before guest-address work;
prepare includes registry locking and table preparation, while residency and
refresh measure its nested page-discovery and snapshot stages. Counts and
times use the same snapshot window as other performance metrics. Nested times
and concurrent processors overlap; do not add them as exclusive frame costs.

## Stable behavior

- Protocol and payload limits are versioned in
  `source/include/Kyty/Agent/WireContract.h` (protocol version 8).
- Exit `0` means the requested tool completed successfully.
- Exit `1` means a tool or health check reported failure.
- Exit `125` means invalid usage or unavailable transport.
- Requests and responses are line-delimited JSON.
- Host paths and workload identities in lifecycle diagnostics are sanitized.
- Only one request client is serviced at a time; retries must be bounded.
- Runtime mutation is limited to the documented controller overlay. Arbitrary
  host-memory access and arbitrary command execution are not agent features.

## Tool ownership

The realtime server owns live state, progress waits, input, and captures.
The process-isolated DevTools core owns durable crash/stall evidence and remains
an internal engine. New developer-facing diagnostics should be exposed through
`kyty_agent` instead of creating another public CLI or a script-only contract.

Native C++ integration coverage validates the Windows named-pipe and POSIX
socket transports, request/response bounds, clean interruption, and
`wait-ready` behavior. Python is not required to build or test the agent.
