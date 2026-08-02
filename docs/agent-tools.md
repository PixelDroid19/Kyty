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

`wait_event` returns `event_cursor_lost` when `--after-seq` predates the
bounded retained event history; reacquire a fresh snapshot before waiting.

Controller automation is explicitly diagnostic input:

```text
kyty_agent pad tap cross
kyty_agent pad hold right --delta 120 --timeout-ms 10000
kyty_agent pad axis left_x 255
kyty_agent pad clear
```

It is evidence for reaching and exercising a runtime frontier, not by itself a
gameplay compatibility claim.

## Stable behavior

- Protocol and payload limits are versioned in
  `source/include/Kyty/Agent/WireContract.h`.
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
