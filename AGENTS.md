# Kyty Engineering Guide (agent-facing)

Kyty is an experimental PS4/PS5 emulator (MIT). This fork advances the PS5
path to correct, interactive rendering on Linux/macOS/Windows and AMD/Intel/
NVIDIA/Apple GPUs. Accuracy beats superficial progress.

The full bring-up manual (phases, frontier history, handoff template) lives in
`docs/BRINGUP.md`. Read it **only** when doing strict-frontier compatibility
work, not for ordinary code tasks.

Agent orchestration (contract gates, manifest, matrix harness, install):
`skills/kyty-frontier-swarm/SKILL.md` and `INSTALL-AGENTS.md`. Verify with
`python3 scripts/verify_agent_toolkit.py`.

## Agent session start (use the toolkit)

```bash
python3 scripts/kyty_agent_doctor.py --task start
export KYTY_FRONTIER_MANIFEST=/tmp/kyty-frontier.json
```

Read `skills/kyty-frontier-swarm/references/quick-recipes.md` for copy-paste
loops (`kyty_agent`, matrix, strict replay, manifest import). Cursor loads
`.cursor/rules/kyty-agent-toolkit.mdc` automatically in this repo.

## Invariants (never weaken)

1. **Evidence before code.** Reproduce and trace bad state to its producer
   before editing.
2. **Never invent guest behavior.** No guessed NIDs, ABI signatures, packet
   layouts, register meanings, formats, tiling, alignments, or return codes.
3. **One behavior, one implementation.** Direct/indirect PM4 share a decoder;
   all surface consumers share one descriptor-to-layout calculation.
4. **No behavioral fallbacks.** Unsupported behavior fails structurally and
   informatively (no assumed RGBA8, linear tiling, default success, placeholder
   shaders, fabricated resources).
5. **Capability-driven rendering.** Vulkan strategy from features/limits/
   formats/queues. Vendor IDs are diagnostics, never policy.
6. **Platform code at the boundary.** Guest HLE and GPU semantics stay
   platform-neutral; `__APPLE__`/`_WIN32`/Linux branches only in platform files.
7. **Do not regress the working frontier.** Preserve existing execution,
   rendering, input, and build behavior.
8. **Report reality.** Separate verified facts, captured evidence, hypotheses,
   and untested assumptions. Never report a hypothesis as a finding.

## Match effort to the task (do not over-process)

| Task class | Required process |
|---|---|
| Host tooling, build, scripts, docs | Edit + build/test what you touched. No strict runs, no handoff report. |
| Host-side bug with a known repro (unit/integration test, gdb capture) | Fix + focused test. Re-run only the affected suites. |
| Guest contract change (HLE, PM4, layout, shader) | Reproduce first, one hypothesis, failing test, minimal fix, focused green, then one strict re-run. |
| Strict-frontier advancement | Full loop in `docs/BRINGUP.md` (one frontier, one failure, one hypothesis). |

If a task fits a lighter row, use the lighter row. Long strict replays and
phase-gate reports are for frontier work only.

## Real investigation, not false hypotheses

- **A hypothesis needs an observation behind it.** State the evidence line
  (log, register dump, packet bytes, test failure) that motivated it. If you
  cannot cite one, you are guessing — go collect evidence instead.
- **Falsifiable or worthless.** Before testing a hypothesis, write what result
  would prove it wrong. If nothing could, discard it.
- **One variable per experiment.** Change a single contract; revert dead ends
  immediately; `git diff` shows only the live hypothesis.
- **Confirm at the producer, not the symptom.** A null pointer is fixed by
  finding who should have written it, not by null-checking the reader.
- **Prefer cheap evidence first.** gdb at the fault, `KYTY_FAULT_LOG=1`,
  existing captures under `/tmp`/scratch, and unit tests are faster than a
  fresh 90 s guest replay. Re-run the guest only when the evidence requires it.
- **Time-box.** If two clean experiments do not move the capture, stop,
  re-classify the failure, and write down the negative result (one line:
  hypothesis → observation → next hypothesis).
- **Logs after the first failure are wreckage**, not new work items.

## Code rules

### Style and structure
- C++17, follow `source/.clang-format` (tabs, 140 columns as configured).
- Focused functions, explicit types, minimal headers, clear ownership.
- No broad renames, compatibility aliases, dead code, commented-out paths,
  magic constants without provenance, or unrelated cleanup in a fix.
- Comments explain evidence, invariants, and non-obvious hardware semantics —
  never narrate the code or the change you just made.
- Warnings, `git diff --check` failures, and new validation messages are
  defects to investigate, not noise.

### Errors and contracts
- Guest-expected invalid input → guest error return. Violated emulator
  invariants → `EXIT`/assert. Never swap these.
- Unknown registers/descriptors/NIDs fail with full context (packet type,
  register, value, submit id, offset) — enough to implement later.
- HLE exports need an evidenced name, NID, signature, arg validation, return
  codes, and side effects; registration stays in the owning `Lib*.cpp`.
- Delete superseded implementations in the same change; no permanent
  forwarding aliases or dual paths.

### Tests
- Every semantic change: smallest deterministic failing test first (red),
  minimal implementation (green), then re-run touched suites.
- Sanitized PM4 packets/descriptors/ABI args are valid fixtures; guest code,
  assets, and private paths are not.
- Verify a renamed/new gtest filter actually selects tests
  (`--gtest_list_tests`).

## Build and test (Linux is the primary host here)

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script        # runtime only, when a full build is unnecessary
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='Suite.Test'
ctest --test-dir _build_linux --output-on-failure -R <IntegrationRegex>
```

macOS uses `_build_macos`; do not put macOS build steps in Linux checklists or
vice versa. Windows follows CI's generators — inspect the workflow, don't
invent commands.

Authorized private replay (only when the task requires runtime validation):

```bash
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Strict runs set no `KYTY_BRINGUP_*` variables. Diagnostics (`KYTY_FAULT_LOG`,
`KYTY_TLS_DIAG`, `KYTY_BRINGUP_MODE=unsafe`, `KYTY_AUTO_CROSS`) are discovery
tools and never count as acceptance or compatibility evidence.

## Privacy and hygiene

- Never commit guest paths, title IDs, keys, binaries, saves, screenshots,
  crash dumps, raw multi-megabyte logs, or `_Shaders/` dumps. Reference
  fixtures only as `$KYTY_GUEST_ROOT` (or case IDs).
- Commit messages describe emulator behavior only
  (`fix(graphics): validate Gen5 barrier range`) — never a title.
- Session evidence goes under untracked scratch (`/tmp/...`,
  `_scratch_playable/`).
- Remove temporary probes before commit.
- No GPL code paste. Record borrowed facts with URL/provenance and confirm
  them with a local test before implementing against Kyty types.

## Done checklist (behavior changes)

1. Focused test failed before, passes after.
2. Build succeeds on the host you are on (`ninja -C _build_linux`).
3. `git diff --check` clean; no new warnings.
4. For guest-contract changes: strict scenario advances or renders more
   correctly, with no stub/permissive flag required for the claim.
5. No tracked file contains fixture identity or generated evidence.
6. Existing working behavior rechecked (touched suites green).

## Reporting

Keep reports proportional: for small tasks, outcome + what was verified in a
few sentences. The full handoff template (evidence table, phase gates,
frontier note) applies only to strict-frontier sessions and lives in
`docs/BRINGUP.md`.
