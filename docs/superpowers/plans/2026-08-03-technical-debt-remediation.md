# Kyty Technical Debt Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the reported Kyty technical debts into explicit, independently verified architecture and build increments without changing guest ABI or claiming compatibility from static checks.

**Architecture:** Start with a pure `kyty_audio_pcm` library and build-ownership cleanup, then enforce source-graph rules before extracting SDL/window/clock seams. Larger domain files are split only at tested protocol boundaries; the Kernel/Graphics cycle is deferred until its allocation and submission contracts have dedicated integration coverage.

**Tech Stack:** C++17, CMake/Ninja, SDL2, existing Kyty integration harness, optional GoogleTest, Python 3 source-graph checker.

## Global Constraints

- Preserve unrelated user changes and generated local artifacts.
- Keep `HostAudio.h`, guest HLE names, NIDs, and runtime behavior stable during extraction.
- Keep `kyty_agent`, Lua scripts, and `kyty_devtools_core` until a build-matrix decision proves a replacement or unreachable path.
- Reconfigure after CMake source-list edits.
- Validate both `KYTY_BUILD_UNIT_TESTS=OFF` and `ON` configurations.
- Do not claim boot, rendering, gameplay, or compatibility from these refactors.
- Use Conventional Commits and do not include private workload names or paths.

---

### Task 1: Extract the pure PCM boundary

**Files:**
- Create: `source/audio_pcm/CMakeLists.txt`
- Modify: `source/CMakeLists.txt`
- Modify: `source/emulator/CMakeLists.txt`
- Modify: `source/emulator/src/AudioPcm.cpp`
- Modify: `source/emulator/include/Emulator/AudioPcm.h`
- Create: `source/integration_test/src/AudioPcmIntegration.cpp`
- Modify: `source/integration_test/CMakeLists.txt`

**Interfaces:**
- Produces target `kyty_audio_pcm` containing `AudioPcm.cpp`.
- Keeps `AudioPcmBytesPerSample`, `AudioPcmQueueBytes`, and
  `AudioPcmApplyChannelVolumes` signatures unchanged.
- `emulator` links `kyty_audio_pcm` privately; no AudioOut symbol moves.

- [ ] Add the integration executable and tests for valid S16/F32 channel
  scaling, zero/overflow inputs returning false or zero, and queue-byte
  calculation at 48 kHz stereo.
- [ ] Run the new executable before moving the source and record the expected
  pass/fail output.
- [ ] Add `audio_pcm` as a subdirectory and remove `AudioPcm.cpp` from the
  emulator glob with an explicit target source.
- [ ] Link `emulator` to `kyty_audio_pcm`, set include directories through the
  target, and keep all namespace/API declarations unchanged.
- [ ] Reconfigure and build `kyty_audio_pcm`, `emulator`, and the integration
  executable; run `ctest -R 'KytyAudioPcmIntegration'`.
- [ ] Run the existing `KytyAudioHostIntegration.PacingAndLifecycle` test to
  prove the consumer still uses the extracted implementation.
- [ ] Commit as `refactor(audio): isolate host-independent pcm processing`.

### Task 2: Fix build ownership and forced compilation

**Files:**
- Modify: `source/src_script.cmake`
- Modify: `source/3rdparty/CMakeLists.txt`
- Modify: `source/CMakeLists.txt`
- Modify: `source/integration_test/CMakeLists.txt`

**Interfaces:**
- `KYTY_BUILD_UNIT_TESTS=OFF` no longer compiles `gtest-all.cc` into
  `fc_script`.
- ASTC remains available as a target but is excluded from the default `all`
  build when no consumer requests it.

- [ ] Make `KYTY_SCRIPT_SRC` contain only `KytyScripts.cpp` by default and
  append `gtest-all.cc` only when unit tests are enabled.
- [ ] Mark ASTC `EXCLUDE_FROM_ALL` and verify its target still configures.
- [ ] Keep integration targets explicit and ensure the fixture build still
  requests every required executable.
- [ ] Configure/build with unit tests OFF and inspect Ninja query output for
  absence of `gtest-all.cc` and `kyty_astcenc` from `fc_script` dependencies.
- [ ] Configure/build a separate unit-tests-ON directory and run the focused
  GTest suites plus the PCM integration.
- [ ] Commit as `build: make optional test and astc targets explicit`.

### Task 3: Add source-graph dependency guards

**Files:**
- Create: `scripts/check_emulator_boundaries.py`
- Modify: `source/CMakeLists.txt`
- Create: `source/integration_test/src/ArchitectureBoundaryIntegration.cpp`
- Modify: `source/integration_test/CMakeLists.txt`

**Interfaces:**
- Checker accepts the source root and emits one deterministic diagnostic per
  forbidden edge; exit 0 only when all rules pass.
- Initial rules reject `Audio.cpp`/`AudioHost.cpp`/`AudioPcm.cpp` including
  `Emulator/Graphics/*`, and reject `Loader/RuntimeLinker.cpp` including
  devtools headers. Rules are path-based and do not parse comments.

- [ ] Write checker tests with temporary fixture files for allowed includes,
  forbidden includes, missing roots, and stable sorted diagnostics.
- [ ] Run them red against a synthetic forbidden fixture.
- [ ] Implement bounded UTF-8 text scanning with explicit file allowlists and
  no filesystem traversal outside the supplied root.
- [ ] Add a CTest executable that invokes the checker against the repository
  and reports a failing process on a violation.
- [ ] Run the checker, the architecture integration, and the full integration
  test list.
- [ ] Commit as `test(architecture): guard emulator dependency direction`.

### Task 4: Introduce host clock and audio lifecycle seam

**Files:**
- Create: `source/emulator/include/Emulator/Host/Clock.h`
- Create: `source/emulator/src/Host/Clock.cpp`
- Modify: `source/emulator/src/AudioHost.cpp`
- Modify: `source/emulator/CMakeLists.txt`
- Modify: `source/integration_test/src/AudioHostIntegration.cpp`

**Interfaces:**
- `HostClock::NowMicroseconds()` and `HostClock::SleepUntil()` own host timing;
  `AudioHost` consumes them without including `Kernel/Time.h`.
- Existing `HostAudio` methods and pacing behavior remain unchanged.

- [ ] Add deterministic clock contract tests for monotonicity and bounded
  sleep; run them red before replacing `LibKernel::KernelGetProcessTime`.
- [ ] Implement the clock using `steady_clock` and `Core::Thread::SleepMicro`.
- [ ] Replace only AudioHost timing calls and remove its Kernel include.
- [ ] Run audio host integration under SDL disk driver and compare pacing
  bounds with the baseline.
- [ ] Commit as `refactor(audio): isolate host timing from kernel hle`.

### Task 5: Split platform window/capture ownership

**Files:**
- Create: `source/emulator/include/Emulator/Host/Window.h`
- Create: `source/emulator/src/Host/WindowSdl.cpp`
- Modify: `source/emulator/src/Graphics/Window.cpp`
- Modify: `source/emulator/src/Graphics/DebugOverlay.cpp`
- Modify: `source/emulator/src/Graphics/NativeCapture.cpp`
- Modify: `source/emulator/src/Graphics/Image.cpp`
- Add platform-specific integration cases under `source/integration_test/src/`

**Interfaces:**
- `HostWindow`, `HostSurface`, and `HostCapture` own SDL/X11/Win32 handles;
  Graphics consumes opaque operations and never includes platform headers.

- [ ] Characterize current Linux and Windows compile paths before moving code.
- [ ] Add opaque lifecycle tests for create/destroy, resize, input focus, and
  capture failure; do not require a visible window in headless CI.
- [ ] Move one operation group at a time, preserving error codes and event
  ordering; remove direct platform includes from Graphics after each group.
- [ ] Build Linux and Windows-cross configuration where available and run all
  graphics diagnostics integrations.
- [ ] Commit each operation group separately.

### Task 6: Modularize GraphicsRun and shader translation

**Files:**
- Create focused headers/sources under `source/emulator/include/Emulator/Graphics/`
  and `source/emulator/src/Graphics/`
- Modify: `GraphicsRun.cpp`, `Shader.cpp`, `ShaderParse.cpp`, and CMake source
  lists
- Add focused parser/decoder integration fixtures

**Interfaces:**
- PM4 packet decoding, state application, submission scheduling, shader parse,
  and SPIR-V emission have separate internal interfaces.
- Existing `GraphicsRun` entry points and canonical decoders remain the only
  public behavior.

- [ ] Inventory functions and global state by responsibility; freeze a source
  map in the plan before moving code.
- [ ] Extract one pure decoder with a red regression test, then move its call
  sites and delete the duplicate body.
- [ ] Repeat for shader parse/emission and command submission, rebuilding after
  each seam.
- [ ] Run GraphicsPackets, GraphicsState, shader, and graphics diagnostics
  integrations after every extraction.
- [ ] Do not interpret a non-black frame as compatibility proof.

### Task 7: Convert lifecycle/global diagnostics incrementally

**Files:**
- Modify subsystem registration headers/implementations and affected globals
- Create lifecycle and logging contract tests
- Update `docs/ARCHITECTURE.md` with ownership rules

**Interfaces:**
- Explicit lifecycle objects own state previously hidden in `KYTY_SUBSYSTEM_*`
  registrations; compatibility adapters remain until all consumers migrate.
- Error/log sinks are bounded and injectable in tested seams.

- [ ] Generate an authoritative inventory of subsystem macros, `static g_*`,
  direct diagnostics, and Common.h includes.
- [ ] Migrate one subsystem with a lifecycle test and an integration fixture.
- [ ] Add bounded logger adapters before deleting direct calls; preserve fatal
  semantics for unsupported guest contracts.
- [ ] Repeat per subsystem and remove adapters only after graph scans are clean.

### Task 8: Complete coverage, naming, and table provenance

**Files:**
- Add Kernel/Libs/Loader/Graphics integration cases
- Rename `Graphics/Image.cpp`, `Library/KeyboardInput.cpp`, and the audio/video
  namespace only with public include aliases or coordinated call-site changes
- Create a versioned generator under `tools/graphics_tables/` and golden tests
- Update architecture and build documentation

**Interfaces:**
- Every moved name has one canonical header and a documented migration path.
- Table generation is deterministic from versioned input and checks generated
  output in CI; no unchecked `.inc` drift is accepted.

- [ ] Add tests for each previously uncovered domain contract before renaming.
- [ ] Recover table input provenance from repository history or encode the
  current data as explicit versioned input; verify byte-for-byte output.
- [ ] Rename only after include-graph and build checks pass.
- [ ] Run both build matrices, all CTest integration suites, enabled GTests,
  and source-graph checks; review the complete diff.

