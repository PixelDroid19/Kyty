# Native Agent Debugger Improvements

## Goal

Make the existing native `kyty_agent` interface materially more useful for
developers and coding agents during live runtime investigation. The first
increment must reduce command orchestration and stale-event mistakes without
adding another public debugger, exposing arbitrary host capabilities, or
slowing the emulator with unbounded logging.

## Considered approaches

### Full execution-control debugger first

Add pause, continue, stepping, breakpoints, registers, and guest-memory access
in one increment. This eventually provides the richest interactive workflow,
but Kyty does not yet have one safe global stop-the-world boundary for every
guest execution path. Adding the protocol before that boundary would risk torn
register state, deadlocks, and unsafe memory access.

### Offline graphics replay first

Build semantic GPU capture and replay before changing the live agent. This is
valuable for graphics diagnosis, but it is a separate subsystem and would not
immediately improve loader, synchronization, crash, or general runtime work.

### Staged native debugger improvements

Extend `kyty_agent` first with an atomic-looking, bounded diagnostic snapshot,
precise event waits, and versioned capability discovery. Then add live
execution control only after a safe suspension contract exists. This approach
is selected because it improves daily work immediately and preserves a sound
foundation for later breakpoints and register inspection.

## Selected design

### One-call diagnostic snapshot

Protocol v6 adds a `debug_snapshot` tool and a `snapshot` CLI command. One
request returns:

- runtime status and frontier classification;
- bring-up, loader, event-ring, and bounded performance diagnostics;
- guest thread inventory;
- synchronization waits;
- recent events selected by `events_last` and `events_after_seq`;
- the last error;
- `event_seq_start`, `event_seq_end`, and a `stable` flag showing whether the
  event stream changed while the snapshot was assembled.

The snapshot is observational and does not reset counters, inject input,
capture a frame, pause execution, or write files. Existing component builders
remain the source of truth so schemas cannot silently diverge. Event history is
limited to 1-128 records, and the complete response must stay within the
existing wire response bound.

### Precise event waits

`wait_event` and `kyty_agent wait-event` accept optional `code` and
`after_seq` filters. The current default remains "events produced after the
wait starts." A caller can instead provide the sequence from a prior snapshot
and wait for one exact event kind/code pair. This prevents an agent from
mistaking an old warning or crash for a new failure.

Every poll advances its cursor even when nonmatching events arrive. Matching is
exact and bounded; empty or oversized event codes are rejected.

### Versioned capability discovery

The existing `help` result remains the single discovery surface. Protocol v6
adds a stable `capabilities` object that declares:

- local-only transport;
- diagnostic snapshot support;
- event sequence and code filtering;
- bounded responses;
- controller input as the only enabled runtime mutation;
- absence of arbitrary host commands, host paths, guest-memory access, and live
  execution control.

This lets agents adapt safely instead of guessing which commands exist. Future
pause/register/breakpoint work can change these flags only when its underlying
suspension contract is implemented and tested.

## Data flow

1. The CLI validates numeric ranges and serializes one JSON-lines request.
2. The local-only transport sends it to the existing single-client server.
3. The server records the event sequence, assembles existing bounded component
   results, records the ending sequence, and returns one versioned envelope.
4. An agent can pass `event_seq_end` to a later `wait-event --after-seq` call,
   optionally with `--code`, to observe only new evidence.

## Error handling and safety

- Unknown arguments and invalid ranges fail with exit 125 in the CLI or a typed
  `invalid_args` protocol error.
- Snapshot construction performs no guest or host mutation.
- No network listener, arbitrary shell execution, arbitrary host path, or
  arbitrary memory operation is introduced.
- No per-frame logging or unbounded capture is added.
- A response that would exceed the wire limit fails explicitly instead of
  being truncated into invalid JSON.

## Verification

- Unit tests cover tool parsing, capability discovery, snapshot composition,
  sequence stability, event range normalization, and exact code filtering.
- Protocol integration covers the v6 contract, bounded maximum response, and
  sanitized output.
- Transport integration proves the new command over the native local endpoint.
- Build the affected `kyty_agent`, `fc_script`, unit-test, and agent integration
  targets.
- Review the complete diff for unrelated dirty changes, private paths,
  protected workload data, external-project references in production code, and
  generated artifacts.

## Deferred work

Guest pause/continue, instruction stepping, breakpoints, registers, guest memory
inspection, symbolized guest backtraces, and GPU replay remain separate
increments. They require safe execution-suspension and capture contracts; this
design deliberately does not advertise unimplemented control.
