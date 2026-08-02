# Native Agent Debugger Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded one-call runtime debug snapshot, exact event waits, and versioned capability discovery to the existing native `kyty_agent` interface.

**Architecture:** Keep the current local transport and single public agent protocol. Add a small pure snapshot-composition module under `Emulator/Agent`, then connect it to the existing server and CLI; event matching remains an `EventRing` responsibility. Reuse current JSON component builders and expose no arbitrary memory, path, network, or shell capability.

**Tech Stack:** C++17, Kyty native JSON-lines protocol, Unix sockets/Windows named pipes, project unit and integration test harnesses, CMake.

## Global Constraints

- Preserve every pre-existing dirty file and unrelated local artifact.
- Keep `kyty_agent` as the only public runtime debugging surface.
- Keep the transport local-only and opt-in through `KYTY_AGENT_ENDPOINT`.
- Keep every response at or below `Kyty::Agent::kResponseLineMax`.
- Add no per-frame logging, arbitrary host paths, shell execution, guest-memory access, or execution mutation.
- Controller overlay remains the only enabled runtime mutation.
- Use protocol version `6` for the new wire contract.
- Do not name reference projects or private workloads in production code or commit messages.

---

### Task 1: Pure snapshot and event-match contracts

**Files:**
- Create: `source/emulator/include/Emulator/Agent/DebugSnapshot.h`
- Create: `source/emulator/src/Agent/DebugSnapshot.cpp`
- Modify: `source/emulator/include/Emulator/Agent/EventRing.h`
- Modify: `source/emulator/src/Agent/EventRing.cpp`
- Modify: `source/emulator/include/Emulator/Agent/Protocol.h`
- Modify: `source/emulator/src/Agent/Protocol.cpp`
- Modify: `source/include/Kyty/Agent/WireContract.h`
- Test: `source/unit_test/src/emulator/UnitTestAgentTools.cpp`

**Interfaces:**
- Produces: `DebugSnapshotParts`, containing sequence bounds and six JSON component strings.
- Produces: `std::string BuildDebugSnapshotResult(const DebugSnapshotParts& parts)`.
- Produces: `bool EventMatches(const EventRecord& event, EventKind kind, const char* code)`; `nullptr` or empty `code` matches every code of the requested kind.
- Produces: `Tool::DebugSnapshot` parsed from `"debug_snapshot"`.

- [ ] **Step 1: Write failing unit tests**

Add tests that require the new interfaces before implementing them:

```cpp
TEST(AgentTools, DebugSnapshotComposesStableBoundedSections)
{
    DebugSnapshotParts parts {};
    parts.event_seq_start = 41;
    parts.event_seq_end = 41;
    parts.status_json = R"({"schema":"runtime_status"})";
    parts.diagnostics_json = R"({"schema":"runtime_diagnostics"})";
    parts.threads_json = R"({"available":true})";
    parts.sync_waits_json = R"({"enabled":true})";
    parts.events_json = R"({"schema":"event_history","events":[]})";
    parts.last_error_json = R"({"event":null})";
    const std::string json = BuildDebugSnapshotResult(parts);
    EXPECT_NE(json.find(R"("schema":"debug_snapshot")"), std::string::npos);
    EXPECT_NE(json.find(R"("stable":true)"), std::string::npos);
    EXPECT_NE(json.find(R"("event_seq_start":41)"), std::string::npos);
    EXPECT_LE(json.size() + 1, Kyty::Agent::kResponseLineMax);
}

TEST(AgentTools, EventMatchUsesExactOptionalCode)
{
    EventRecord event {};
    event.kind = EventKind::Error;
    std::snprintf(event.code, sizeof(event.code), "%s", "device_lost");
    EXPECT_TRUE(EventMatches(event, EventKind::Error, nullptr));
    EXPECT_TRUE(EventMatches(event, EventKind::Error, "device_lost"));
    EXPECT_FALSE(EventMatches(event, EventKind::Warn, "device_lost"));
    EXPECT_FALSE(EventMatches(event, EventKind::Error, "device"));
}
```

- [ ] **Step 2: Run the focused tests and verify failure**

Run:

```bash
cmake -S source -B _build_linux
cmake --build _build_linux --target unit_test -j2
_build_linux/unit_test "{kyty_run_tests()}" --gtest_filter='AgentTools.DebugSnapshotComposesStableBoundedSections:AgentTools.EventMatchUsesExactOptionalCode'
```

Expected: compile failure because `DebugSnapshotParts`, `BuildDebugSnapshotResult`, and `EventMatches` do not exist.

- [ ] **Step 3: Implement the pure contracts**

Define `DebugSnapshotParts` with `uint64_t event_seq_start`, `uint64_t event_seq_end`, and `std::string` members named `status_json`, `diagnostics_json`, `threads_json`, `sync_waits_json`, `events_json`, and `last_error_json`. Compose this exact top-level shape without reparsing component JSON:

```json
{"protocol_version":6,"schema":"debug_snapshot","event_seq_start":41,"event_seq_end":41,"stable":true,"status":{},"diagnostics":{},"threads":{},"sync_waits":{},"events":{},"last_error":{}}
```

Treat an empty component as `null`. If the final line plus newline exceeds `kResponseLineMax`, return an empty string so the handler can emit a typed `response_too_large` error. Implement exact `EventMatches` with `std::strcmp`. Add `Tool::DebugSnapshot`, map `"debug_snapshot"`, and bump `kProtocolVersion` from 5 to 6.

- [ ] **Step 4: Re-run focused tests**

Run the command from Step 2. Expected: both tests pass.

- [ ] **Step 5: Commit the task**

```bash
git add source/emulator/include/Emulator/Agent/DebugSnapshot.h source/emulator/src/Agent/DebugSnapshot.cpp source/emulator/include/Emulator/Agent/EventRing.h source/emulator/src/Agent/EventRing.cpp source/emulator/include/Emulator/Agent/Protocol.h source/emulator/src/Agent/Protocol.cpp source/include/Kyty/Agent/WireContract.h source/unit_test/src/emulator/UnitTestAgentTools.cpp
git commit -m "feat(agent): add bounded debug snapshot contract"
```

### Task 2: Server and CLI behavior

**Files:**
- Modify: `source/emulator/src/Agent/AgentServer.cpp`
- Modify: `source/agent/src/Cli.cpp`
- Test: `source/unit_test/src/emulator/UnitTestAgentTools.cpp`

**Interfaces:**
- Consumes: `BuildDebugSnapshotResult(const DebugSnapshotParts&)` and `EventMatches(...)` from Task 1.
- Produces: protocol tool `debug_snapshot` with `events_last` and `events_after_seq` arguments.
- Produces: CLI command `snapshot [--events N] [--after-seq N]`.
- Extends: `wait_event` with optional exact `code` and explicit `after_seq`.
- Extends: CLI `wait-event --kind KIND [--code CODE] [--after-seq N] [--timeout-ms N]`.

- [ ] **Step 1: Add the failing unstable-snapshot expectation**

Add a unit test for an unstable snapshot (`event_seq_start=7`,
`event_seq_end=8`) that expects `"stable":false`. Protocol discovery and the
CLI request surface are covered process-isolated in Task 3 because
`HelpResult` is server-private.

- [ ] **Step 2: Verify the new expectations fail**

Run:

```bash
cmake --build _build_linux --target unit_test kyty_agent -j2
_build_linux/unit_test "{kyty_run_tests()}" --gtest_filter='AgentTools.*Snapshot*:AgentTools.ProtocolParsesRequestAndFormatsResponse'
```

Expected: failure until the pure snapshot builder reports changing sequence
bounds as unstable.

- [ ] **Step 3: Implement server behavior**

Add `HandleDebugSnapshot` to `kHandlers`. Normalize `events_last` to 50 when it is outside 1-128. Record `EventRing::Instance().NextSeq()` before and after collecting these existing builders: `StatusResult`, `DiagnosticsResult`, `ThreadsResult`, `SyncWaitsResult`, `EventsResult`, and `LastErrorResult`. Pass the parts to `BuildDebugSnapshotResult`; return `FormatErr(req.id, "response_too_large", "debug snapshot exceeds response limit")` on an empty result.

Extend `HandleWaitEvent` to read optional `code`. Reject codes with length 0 or
greater than 31 bytes when the argument is present. Replace the kind-only
comparison with `EventMatches(records[i], kind, code_filter)`. Preserve cursor
advancement and the current default cursor of `NextSeq()`.

Extend `HelpResult` with `debug_snapshot` and this exact capability policy:

```json
"capabilities":{"transport":"local_only","bounded_responses":true,"debug_snapshot":true,"event_sequence_filter":true,"event_code_filter":true,"controller_input":true,"live_execution_control":false,"guest_memory_access":false,"host_path_access":false,"host_command_execution":false}
```

- [ ] **Step 4: Implement CLI validation and requests**

Add usage text for `snapshot` and the new event options. Accept only decimal unsigned integers and reject malformed or trailing characters; enforce snapshot events 1-128 and event codes 1-31 bytes. Serialize:

```json
{"id":1,"tool":"debug_snapshot","args":{"events_last":50,"events_after_seq":0}}
```

and include only supplied `code`/`after_seq` fields in `wait_event`. Keep invalid usage at exit 125.

- [ ] **Step 5: Run focused tests and CLI smoke checks**

Run:

```bash
cmake --build _build_linux --target unit_test kyty_agent -j2
_build_linux/unit_test "{kyty_run_tests()}" --gtest_filter='AgentTools.*Snapshot*:AgentTools.ProtocolParsesRequestAndFormatsResponse:AgentTools.EventMatchUsesExactOptionalCode'
_build_linux/agent/kyty_agent --endpoint /tmp/kyty-agent-unreachable.sock snapshot --events 0
_build_linux/agent/kyty_agent --endpoint /tmp/kyty-agent-unreachable.sock wait-event --kind error --code ''
```

Expected: tests pass; both invalid CLI calls exit 125 without attempting transport.

- [ ] **Step 6: Commit the task**

```bash
git add source/emulator/src/Agent/AgentServer.cpp source/agent/src/Cli.cpp source/unit_test/src/emulator/UnitTestAgentTools.cpp
git commit -m "feat(agent): expose coherent runtime debug snapshots"
```

### Task 3: Native integration, documentation, and complete validation

**Files:**
- Modify: `source/integration_test/src/AgentProtocolIntegration.cpp`
- Modify: `source/integration_test/src/AgentTransportIntegration.cpp`
- Modify: `source/integration_test/CMakeLists.txt`
- Modify: `docs/agent-tools.md`

**Interfaces:**
- Consumes: protocol v6, `debug_snapshot`, capability discovery, and exact event filters from Tasks 1-2.
- Produces: user-facing documented workflow `snapshot -> wait-event --after-seq` and process-isolated regression coverage.

- [ ] **Step 1: Write failing integration scenarios**

Add `debug_snapshot_bounded` to `AgentProtocolIntegration.cpp`. Build maximum bounded component strings, call `BuildDebugSnapshotResult`, and assert protocol version 6, all section names, `stable`, and `size + 1 <= kResponseLineMax`. Extend `protocol_version_consistent` to assert `ParseTool("debug_snapshot") == Tool::DebugSnapshot`.

Extend transport integration to send a `help` request and require `"debug_snapshot":true`, then send `debug_snapshot` and require the `debug_snapshot` schema plus `event_seq_start` and `event_seq_end`.

- [ ] **Step 2: Register and run failing integration tests**

Add the new process-isolated scenario to the existing scenario list in `source/integration_test/CMakeLists.txt`, then run:

```bash
cmake -S source -B _build_linux
cmake --build _build_linux --target kyty_agent_protocol_integration kyty_agent_transport_integration -j2
ctest --test-dir _build_linux --output-on-failure -R 'kyty_agent_(protocol|transport)_integration'
```

Expected before final integration: the new scenario or transport assertions fail.

- [ ] **Step 3: Complete integration support and documentation**

Wire any missing link dependencies for `DebugSnapshot.cpp`. Update `docs/agent-tools.md` with:

```text
kyty_agent snapshot --events 100
kyty_agent wait-event --kind error --after-seq <event_seq_end> --code device_lost --timeout-ms 10000
```

Explain that `stable:false` means events changed during assembly and that the snapshot is observational, bounded, and not a compatibility claim.

- [ ] **Step 4: Run strongest relevant validation**

Run:

```bash
cmake -S source -B _build_linux
cmake --build _build_linux --target fc_script kyty_agent unit_test kyty_agent_protocol_integration kyty_agent_transport_integration -j2
_build_linux/unit_test "{kyty_run_tests()}" --gtest_filter='AgentTools.*'
ctest --test-dir _build_linux --output-on-failure -R 'kyty_agent_(protocol|transport)_integration'
git diff --check
```

Expected: every command exits 0. Existing linker warnings may be reported but must not be treated as new failures.

- [ ] **Step 5: Perform security and scope review**

Inspect the complete task diff and confirm: response sizes are bounded; event codes are exact and sanitized; no arbitrary host/memory/shell capability exists; no external project or private workload identifier appears in production code; no pre-existing dirty file was staged accidentally.

- [ ] **Step 6: Commit the task**

```bash
git add source/integration_test/src/AgentProtocolIntegration.cpp source/integration_test/src/AgentTransportIntegration.cpp source/integration_test/CMakeLists.txt docs/agent-tools.md
git commit -m "test(agent): validate native debugger workflow"
```
