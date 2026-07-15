# Kyty Contributor and Agent Guide

This file contains durable repository instructions for humans and coding agents.
It is not a project-status report, compatibility database, debugging log, task
brief, or handoff record.

Do not add session-specific defects, suspected causes, title observations,
commit hashes, screenshots, performance samples, or temporary implementation
plans here. Those facts become stale and can bias later investigations. Discover
the current state from the worktree, Git history, tests, and a fresh reproduction.
Store temporary evidence outside tracked files; put stable technical contracts in
the closest relevant documentation or tests.

More specific `AGENTS.md` files may add instructions for a subtree. They must
remain consistent with this root guide.

## Project overview

Kyty is an experimental PlayStation 4 and PlayStation 5 emulator written in
C++17. Its main runtime combines guest loading and HLE services, CPU and memory
emulation, graphics command decoding, shader translation, Vulkan rendering,
audio, input, presentation, and host-platform integration.

Correct guest behavior is more important than making execution continue.
Unsupported behavior must remain explicit and diagnosable. A successful build,
non-black frame, or surviving process does not by itself prove compatibility.

The project must remain portable across supported Linux, macOS, and Windows
hosts and across GPU vendors. Guest-visible behavior must not depend on a
specific host vendor or operating system.

## Repository layout

- `source/emulator/src/Loader/`: guest image loading, relocation, NID
  resolution, native-call trampolines, and exception integration.
- `source/emulator/src/Kernel/`: guest memory, files, threads, synchronization,
  and time.
- `source/emulator/src/Libs/`: HLE libraries and guest API contracts.
- `source/emulator/src/Graphics/Graphics.cpp`: PS4/PS5 command-buffer builders
  and AGC-facing exports.
- `source/emulator/src/Graphics/GraphicsRun.cpp`: PM4 parsing and normalized
  graphics-state updates.
- `source/emulator/include/Emulator/Graphics/HardwareContext.h`: normalized
  guest GPU state.
- `source/emulator/src/Graphics/GraphicsRender.cpp`: Vulkan resource binding,
  pipelines, draw/dispatch recording, and synchronization.
- `source/emulator/src/Graphics/ShaderParse.cpp` and `ShaderSpirv.cpp`: guest
  shader decoding and SPIR-V generation.
- `source/emulator/src/Graphics/Tile.cpp`: guest surface layout and addressing.
- `source/emulator/src/Graphics/Objects/`: Vulkan-backed resources and guest
  memory tracking.
- `source/emulator/src/Graphics/VideoOut.cpp` and `Window.cpp`: display,
  surfaces, swapchains, and presentation.
- `source/emulator/src/Agent/` and `source/agent/`: opt-in realtime diagnostic
  server and `kyty_agent` client.
- `source/devtools/`: passive native diagnostics and supervisor tooling.
- `source/lib/`: reusable host runtime, platform, threading, filesystem, math,
  and script infrastructure.
- `source/unit_test/`: GoogleTest suites and deterministic fixtures.
- `scripts/`: runtime scripts, including the guest runner.
- `docs/`: stable subsystem documentation and tool contracts.

Read the owning module, its consumers, adjacent tests, and relevant documentation
before editing. Do not infer architecture from filenames alone.

## Build and development commands

The source root for CMake is `source/`. Use a build directory dedicated to the
current checkout and host; never reuse another worktree's artifacts.

Configure and build on Linux:

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux
```

Configure and build on macOS:

```bash
cmake -S source -B _build_macos -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_macos
```

Windows supports the generators and toolchains defined by the CMake files and CI.
Do not invent a Windows command from a Linux or macOS layout; inspect the active
workflow and generator first.

Build only the main script runtime when a full build is unnecessary:

```bash
ninja -C <build-dir> fc_script
```

Run focused tests through `fc_script`:

```bash
<build-dir>/fc_script '{kyty_run_tests()}' \
  --gtest_filter='SuiteName.TestName'
```

Confirm that a new or renamed filter actually selects the intended tests:

```bash
<build-dir>/fc_script --gtest_list_tests '{kyty_run_tests()}'
```

Run an authorized private fixture only when the task requires runtime validation
and `KYTY_GUEST_ROOT` is already available:

```bash
<build-dir>/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Do not put private fixture paths or identifiers in commands committed to Git.
Do not enable permissive, stub, trap-skipping, fabricated-input, or
behavior-changing diagnostic modes as acceptance evidence.

Use `source/.clang-format` for touched C/C++ files. Avoid formatting unrelated
code or entire legacy files as part of a focused change.

## Architecture and design rules

### Evidence and guest semantics

- Reproduce the current behavior before changing guest-visible semantics.
- Trace invalid state to its producer, not only to the assertion or crash that
  exposed it.
- Do not guess NIDs, ABI signatures, packet layouts, register meanings, surface
  formats, tiling, alignments, return codes, or synchronization behavior.
- Support a contract only when it is backed by local captures, deterministic
  tests, authoritative public documentation, or an already established project
  invariant.
- Keep unsupported cases explicit. Do not silently return success, invent
  resources, assume a default format, skip unknown state, or swallow failures.
- Treat diagnostic observations as hypotheses until a test or reproduction
  verifies the causal contract.

### Separation of concerns

- Guest ABI decoding belongs in the HLE or command-builder boundary.
- PM4 decoding produces normalized guest GPU state.
- Rendering consumes normalized state and selects a semantically equivalent
  Vulkan strategy from host capabilities.
- Surface sizes, offsets, pitches, tiling, and address relations must come from
  shared layout logic rather than duplicated calculations.
- OS-specific memory, exceptions, processes, threads, windows, controllers,
  surfaces, and dynamic loading stay in focused platform modules.
- Host capabilities may select implementation strategy; vendor IDs are
  diagnostic information, not guest-semantic policy.
- Keep diagnostic observers passive unless an explicitly designed tool contract
  says otherwise. Observation must not wake guest synchronization, fabricate
  completion, or change scheduling.

### Modularity

- Give each function and module one coherent responsibility.
- Prefer narrow typed interfaces and explicit ownership.
- Reuse an established decoder, layout model, resource abstraction, error
  pattern, or test helper before creating another implementation.
- Do not create generic `Utils`, `Common`, `Manager`, compatibility aliases,
  parallel old/new paths, or feature flags without a demonstrated migration
  requirement.
- Do not perform broad extraction or renaming while solving an unrelated
  compatibility issue.
- When extracting behavior, add characterization coverage first and remove the
  superseded implementation in the same cohesive change.

### Dependencies and external references

- Do not add a dependency when the platform or an existing project component
  already provides the required capability.
- Keep third-party code behind the existing wrappers under `source/3rdparty/`.
- Verify license compatibility before adapting external code.
- Public emulator projects and hardware documentation may provide vocabulary and
  comparison points, but PS5-specific behavior still requires evidence in this
  project.
- Do not copy code from an incompatible license or proprietary source.

## Code conventions

- Follow the surrounding C++ style and `source/.clang-format`.
- Use descriptive domain names; preserve established GPU, ABI, and Vulkan
  terminology.
- Avoid ambiguous boolean arguments, hidden side effects, unexplained magic
  values, broad catch blocks, and silent defaults.
- Validate at the boundary that owns the contract.
- Use guest error returns for expected invalid guest input. Assertions and
  process exits are for violated emulator invariants.
- Document why a non-obvious hardware or ordering constraint exists. Do not
  narrate obvious code.
- Keep public contracts, thread ownership, lifetime rules, and failure behavior
  explicit.
- Do not leave temporary probes, commented experiments, dead paths, or
  title-specific remarks in production code.

## Required workflow

### 1. Establish current state

- Read the user request and derive concrete acceptance criteria.
- Inspect `git status --short` before editing and preserve unrelated work.
- Inspect relevant recent history when behavior or architecture may have changed.
- Locate the implementation, all known consumers, existing tests, and stable
  documentation.
- Determine the correct build directory for the current host and checkout.

Never use this file as evidence that a particular failure, rendering defect, or
compatibility frontier currently exists.

### 2. Reproduce and classify

For defects, reproduce the issue on the current HEAD when the required fixture or
environment is available. Record the first relevant failure or bad state in an
untracked scratch location. Classify the owning seam before changing behavior.

If the issue cannot be reproduced, continue with read-only investigation or add
the smallest behavior-neutral observability needed. Do not manufacture a failure
or preserve a stale hypothesis merely because an older document mentioned it.

### 3. Form a falsifiable hypothesis

State one suspected cause, the expected observable change, and what result would
disprove it. Change one contract at a time. Remove failed experiments before
trying another explanation.

### 4. Implement test-first

For a semantic bug fix or feature:

1. Add the smallest deterministic test that fails for the missing contract.
2. Build the test runtime and confirm the test fails for the expected reason.
3. Implement the behavior in the module that owns it.
4. Run the focused test and related regression suites.
5. Re-run the real workflow when the required environment is available.

Sanitized PM4 packets, descriptors, ABI arguments, and layout cases are suitable
fixtures. Proprietary guest code and assets are not.

For documentation-only or mechanical changes, use proportional verification:
inspect the rendered structure, validate referenced paths and commands, and
review the complete diff. Do not run an unrelated emulator workload merely to
create the appearance of validation.

### 5. Review the result

Before finishing:

- Inspect the complete diff for unrelated changes.
- Check for duplicated behavior, hidden fallbacks, stale comments, temporary
  probes, ownership mistakes, and platform-specific policy leaks.
- Verify every acceptance criterion with fresh evidence.
- Distinguish what was automated, manually verified, and not verified.
- Do not claim broader compatibility than the executed checks demonstrate.

## Testing guidelines

- Match test scope to change risk: focused unit tests first, then related suites,
  build verification, and runtime validation where applicable.
- A passing test from a pre-edit binary is invalid evidence; rebuild the affected
  target first.
- A filter that selects zero tests is invalid evidence.
- Fix production behavior rather than deleting tests, weakening assertions, or
  mocking away the contract under investigation.
- Add regression coverage for reproducible defects whenever the behavior can be
  represented without proprietary data.
- Exercise both direct and indirect encodings when they implement the same state.
- Cover invalid input and unsupported cases at the owning boundary.
- For concurrency changes, verify ordering, ownership, duplicate completion, and
  teardown behavior.
- For graphics changes, validate geometry, formats, layouts, resource lifetime,
  synchronization, presentation progress, and Vulkan diagnostics as relevant.
- A diagnostic score or automated input can guide investigation but cannot alone
  prove correct rendering or interactive compatibility.
- If a required host, GPU, fixture, controller, or validation layer is
  unavailable, state exactly what was not run and what risk remains.

## Diagnostic tools

The realtime agent protocol is documented in `docs/agent-tools.md`. Native
diagnostic and supervisor details live under `docs/devtools/`. Treat those
documents and the tools' current `help` output as the source of truth for
commands and schemas.

Diagnostic output belongs in an untracked scratch directory. Do not copy a
session's verdict, suspected error, frame metrics, socket path, or compatibility
status into this file.

### Visual evaluation

Automated capture scores are heuristics, not a defect catalog. Do **not** invent
or chase a global “yellow/red world” or “white world” failure from score labels,
prior notes, or warm pixels alone.

- Localized warm window light, god-rays, and bloom highlights are often
  art-directed lighting. They are not evidence of global color corruption when
  confined to light sources.
- Judge frames against a known-good reference of the same scene when available.
  Prefer missing or crushed texture detail, opaque black quads where alpha
  should reveal the background, incorrect clears, sampling, blend, and layout
  over color-heuristic verdicts.
- A `healthy` score does not prove correct textures or alpha. Do not “fix”
  intentional lighting to silence a score.

## Security, privacy, and lawful use

- Use only software and data that the contributor is authorized to use.
- Never commit or publish commercial game files, firmware, keys, certificates,
  decrypted executables, proprietary shaders, textures, save data, screenshots,
  title identifiers, private paths, or raw runtime logs.
- Keep fixture roots in environment variables such as `KYTY_GUEST_ROOT`.
- Keep captures and large diagnostic bundles outside tracked files.
- Sanitize tests, issue reports, documentation, commits, and handoffs.
- Do not add DRM circumvention, credential extraction, unauthorized access, or
  distribution mechanisms.
- Never expose secrets or private data in tool output, code, or documentation.

## Git and change discipline

- Preserve dirty worktrees and existing stashes. Never discard or overwrite
  changes you did not create.
- Do not use destructive Git operations or rewrite shared history without
  explicit authorization.
- Keep changes focused on the requested behavior; avoid unrelated cleanup.
- Keep one evidenced behavior or cohesive behavior-neutral extraction per
  commit.
- Use commit subjects that describe repository behavior, not private workloads.
- Do not commit temporary evidence, generated build artifacts, or private data.
- Do not commit, push, merge, delete branches, or open a pull request unless the
  task authorizes that action.

## Definition of done

A change is complete only when:

- the requested behavior and acceptance criteria are satisfied;
- the implementation is in the correct architectural boundary;
- no unsupported behavior is hidden by a fallback;
- relevant tests were rebuilt and passed;
- the appropriate build completed when source code changed;
- the real workflow was exercised when it is necessary and available;
- related behavior and supported hosts were considered for regressions;
- the diff contains no unrelated edits, temporary diagnostics, private data, or
  stale documentation;
- the final report states the exact commands run, their results, and any genuine
  limitations.

Do not use this definition to invent unnecessary work. Verification must be
proportional to the change and must prove the claim being made.
