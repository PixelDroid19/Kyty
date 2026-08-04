# Kyty Technical Debt Remediation

## Goal

Reduce the structural, build, platform, state-management, monolith, and
testing debt identified in the Kyty baseline while preserving guest ABI,
runtime behavior, and the native diagnostic surface. The work is an ordered
series of independently buildable increments; the complete debt inventory
remains the long-term scope.

## Current-state boundary

The current checkout is not the unmodified original. `agent_transport`, Lua,
and `kyty_devtools_core` are active dependencies; 82 CTest entries are
registered, including Kernel, Graphics, Loader, Audio, and agent integration.
The critical `Kernel::Memory`/`Graphics::GpuMemory`/`VideoOut` cycle carries
allocation, protection, and submission ordering, so it must be changed only
through explicit contracts and regression tests. The first safe extraction is
the pure PCM transformation layer, which has no guest, graphics, loader, or
platform state.

## Approaches considered

### Rewrite the domain graph in one migration

Move Kernel, Graphics, Libs, Loader, and Audio into separate libraries and
rename namespaces in one pass. This gives the cleanest end state but creates a
large untestable interval and obscures ABI or lifetime regressions.

### Delete diagnostic and scripting dependencies first

Remove agent, devtools, Lua, and vendored dependencies from the emulator. The
current build and integration harness still use these paths, so deletion would
destroy the canonical runtime diagnostics and script runner before replacements
exist.

### Incremental contracts and seams (selected)

Extract pure host-independent units first, make the build express their
ownership, add automated dependency guards, and then split larger domains one
seam at a time. Each increment has a focused integration or unit test and can
be reverted without changing guest ABI. Dead-code removal happens only after a
configured build matrix and reference scan prove a target unreachable.

## Architecture increments

1. **Build ownership:** compile the pure `AudioPcm` implementation as
   `kyty_audio_pcm`; keep `HostAudio` and guest AudioOut in `emulator` until a
   clock/SDL boundary exists. Make the embedded GTest source conditional on
   `KYTY_BUILD_UNIT_TESTS` and keep ASTC available without forcing it into the
   default build.
2. **Dependency contract:** add a bounded source-graph checker and a CTest
   entry that rejects new guest-domain includes of host-only modules. The
   initial forbidden edge is Audio → Graphics; later phases extend the rule to
   Kernel → render-loop and Loader → devtools.
3. **Host boundaries:** introduce explicit clock, window/input, audio, and
   capture interfaces under `Emulator/Host`; move SDL/X11/Win32 code behind
   those interfaces without changing the public guest HLE headers.
4. **Domain modularization:** split GraphicsRun, Shader/ShaderParse, Audio,
   LibC, and Pthread by pure parsing/decoding/state seams. Keep one canonical
   decoder per protocol and preserve existing symbols through thin internal
   ownership changes.
5. **State and diagnostics:** replace mutable subsystem registration and
   unbounded direct diagnostics incrementally with explicit lifecycle objects
   and the existing logging/agent sinks; add migration counters and bounded
   output before removing macros.
6. **Coverage and generated data:** add integration coverage for each new
   boundary, restore the enabled GTest configuration as a tested matrix, and
   version a reproducible generator plus golden checks for `Graphics/Tables`.

## First deliverable

The first implementation batch creates `kyty_audio_pcm`, adds an integration
test for its overflow, format, and per-channel-volume contracts, conditions
GTest compilation on the unit-test option, and isolates ASTC from the default
`all` build. It must not rename namespaces, remove Lua/agent/devtools, or touch
the Kernel/Graphics lifetime cycle.

## Verification and completion rules

- Reconfigure after every CMake source-list change.
- Build both `KYTY_BUILD_UNIT_TESTS=OFF` (integration harness) and `ON` (GTest)
  configurations before claiming build hygiene.
- Run the focused PCM integration, the full existing integration filter, and
  the enabled GTest suites affected by each seam.
- Use source-graph checks as regression gates, not as proof that a runtime
  contract is correct.
- Review the complete diff for private paths, secrets, generated artifacts,
  unsupported compatibility claims, and unrelated changes.
- Do not remove a target or table until a configured build and test demonstrate
  that no supported path consumes it.

## Deferred but required follow-on increments

The remaining inventory is not marked complete by the first deliverable:
Kernel/Graphics cycle removal, platform window and GPU backend interfaces,
monolith decomposition, global-state lifecycle conversion, centralized logging,
dead-code decisions for agent/devtools/Lua/vendored libraries, broader domain
coverage, naming cleanup, and table generation provenance each require their
own evidence-backed change set.
