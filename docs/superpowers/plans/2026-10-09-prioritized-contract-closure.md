# Prioritized contract closure implementation plan

> **For agentic workers:** Execute independently owned tasks in dependency order; the primary owns integration, build scheduling and final acceptance.

**Goal:** Repair demonstrated command, shader, lifetime and tooling defects while preserving strict rendering and measuring affected runtime performance.

**Architecture:** Keep guest contracts in their current subsystem. Use explicit lifetime ownership and enabled device capabilities at the Vulkan boundary. Validate each behavioral repair independently before the combined runtime comparison.

**Tech stack:** Existing C++ and Vulkan toolchain, SPIR-V Tools, focused contract fixtures and bounded runtime observation.

**Spec:** Repository `AGENTS.md`, `docs/BRINGUP.md` and the architecture audit's confirmed defects.

## Global constraints

- Preserve existing local and staged work in the requested feature worktree.
- Reimplement independently; reference implementations are evidence, never source to copy.
- No fabricated descriptors, success results, skipped work or workload-specific branches.
- Choose rendering behavior from enabled features, limits, formats and queue capabilities.
- Keep protected workloads and raw evidence outside version control.
- One guest at a time; no compilation during guest execution.
- A build, synthetic fixture or presentation counter is not gameplay acceptance.

## Review focus

- A truncated packet must never read the following packet as its own body.
- A recorded pipeline remains alive until every referencing command buffer releases it.
- Stale handles and overlapping destruction must not dereference retired host objects.
- Inactive lanes cannot perform image writes or lose future execution through an early discard.
- Performance comparisons need the same scene, input route, driver, resolution and cache conditions.

## Current contract-closure checkpoint (2026-10-10)

The fresh focused pass at translator128 passed 12 AJM, seven decoder-admission,
19 arithmetic, 24 compute-analysis, 11 guest-device-address, one immutable
shader-dump and one optional alias-query cases. One no-FFmpeg case explicitly
skipped on the FFmpeg-enabled host. Native scalar validation passed 81 cases,
including 12 signed 64-bit bitfield cases. Native address and scalar validation
confirmed the validation layer at instance and device creation without matching
VUIDs. These results do not establish strict guest acceptance or other vendors.

The two native reader-lifetime failures now pass with reader-aware preparation,
exact completion acknowledgment and unlocked recording retries. Independent
source review found no supported correctness defect. Production command-buffer
rotation/retry and a dirty fault between preflight and refresh still lack direct
integration fixtures. The actual refresh point has a second reader check;
clean preparation performs two tracked-chunk scans, an unmeasured cost. Page-
granular refresh remains held.

The direct ATRAC9 route has stateful nonzero decoding, bounded pending PCM,
mapping-identity publication and staged-decoder budget fixtures. This closes the
qualified direct subset, not every compressed or split job. The observed direct
mono/stereo Opus split profile now retains partial-packet PCM, snapshots buffer
descriptors and publishes exact decoded output and consumed bytes. Native
dataflow ties the result object to the input cursor and cumulative output
accounting; hardware-equivalent counter units and audible output remain open.
A demonstrated 16-range host transaction limit has been replaced by a bounded
1,024-range Core transaction and a separate 546-plan complete AJM batch limit.
The focused maximum-capacity and late read-only publication fixtures pass on
the clean production binary. A retired bounded experiment preserves decoder PCM
across neutral flag two at a source boundary, then reaches a constant non-unit
`SetResampleParametersEx` request (approximately 0.91565, zero increment and
flags). Source-step ratio direction is supported by native producer dataflow;
stateful resampling and source/output accounting remain open. AudioOut2 also
accepts non-main PCM that its submission route omits; object routing still needs
native contract evidence. See `docs/BRINGUP.md` for the evidence.

Temporary input and draw-cost traces have been removed by narrow deletion;
the final production build, full suite, gates and real workload pass are in
progress. AMD, NVIDIA, Windows, pinned Nix and live CI remain untested.

## Contract-closure validation checkpoint at translator127 (2026-10-09)

On retry 2, the focused CPU suite passed 278/278 and selected native integrations
passed 37/37, with no matching Vulkan validation identifiers. Native PARAM
validation covers 30 nonzero-mask pair cases and two EXEC controls, with
enabled-component results proven. Repeated partial-vertex retention, compressed
PARAM exports and VSKIP remain unqualified.

The full CPU suite completed on retry 4 in 55.84 seconds: 2,317 cases across
147 suites, with 2,313 passed, four skipped and zero failed. The skips were
three OS-owned virtual-memory cases and
`EmulatorKernelLifetimes.FailedPthreadDestroyPreservesNativeObject`, unavailable
on this host. The fresh `LastTest` evidence was 526,062 bytes (SHA-256
`a317373c...e6b364`); no VUIDs were found and the foreign-monitor count was 0.
All eight source, table, shader and archive gates exited 0 on retry 4. The
pinned Nix environment was unavailable; validation used the host build.

Earlier attempts are historical: full CPU retry 2 stopped at
`VertexExportsDeclareParametersUsedByTheShader` because `exp_control=0xff` was
unknown; the same default was present in PARAM7/8, which that run did not reach.
Setting it to zero made the exact three-test filter pass 3/3 on retry 3, but the
full retry 3 was interrupted when an external guest run began.

No guest execution result is included in this checkpoint. It establishes no
menu, gameplay, 20 FPS performance or cross-vendor result.

Escalation found no visible reader-completion guarantee for tracked writable
snapshot refresh; host coherency does not establish device execution completion.
Write-back dependencies cover writes, not readers; invalidation and release are
quiesced. An overlap could create a host/device data gap, but the guest trigger
is unknown, no runtime overlap or corruption has been observed, and no runtime
cause is proven. A reader-lease/dependency fixture and a completion wait outside
the command-processor, render and guest-address registry locks are possible
directions only; no implementation is claimed. Keep page-granular refresh on
hold. Source references are recorded in `docs/BRINGUP.md`.

## Historical status at the translator126 / geometry86 checkpoint (2026-10-09)

The selected host validation passed: default and integration builds succeeded; the focused CPU filter passed 277/277;
the full CPU suite passed 2,312/2,316 with four skips and no failures; selected native integration passed 36/36; and
all eight source, table, shader and archive gates exited successfully. The Vulkan validation scan found no matching
identifiers. The skips were three host virtual-memory cases unavailable on this host and one
native pthread-destroy preservation case. The pinned Nix environment was unavailable.

No guest was launched with translator126. The strict affected routes, menu state, runtime performance, cross-vendor
behavior, other operating systems, and live CI remain unverified. The checkpoint does not establish compatibility
for unsupported or untested contracts. At that checkpoint, partial
full-precision PARAM writes and disabled-component preservation were
unqualified; compressed PARAM exports and VSKIP were refused or unknown. P3 IR
and new-backend work is advisory and deferred until an accepted baseline;
neither is implemented here.

## Independently owned tasks

### 1. Command processor bounds

- [x] Add sanitized packet and dispatch fixtures for malformed bodies, null handlers and invalid consumption.
- [x] Validate packet-body bounds and the indexed indirect draw's complete 20-byte argument span before copying.
- [x] Run packet and command integration contracts while preserving valid chained command behavior.
- [ ] Implement or qualify non-indexed indirect draw offsets and semantics; its earlier local candidate is absent from this target.

### 2. Vulkan resource lifetime and capability admission

- [x] Exercise recorded and submitted pipeline ownership during cache eviction.
- [x] Retire pipelines only after their final command-buffer reference is released.
- [x] Validate queue roles against submitted operations and existing exclusive resource ownership.
- [x] Run lifetime fixtures and native Vulkan integration with validation enabled.
- [x] Implement and independently validate reader-aware tracked snapshot
  preparation and exact completion acknowledgment. The recorded and pending
  reader fixtures pass with native validation enabled.
- [ ] Exercise production command-buffer rotation/retry and the late dirty-fault
  interleaving directly; retain the page-granular refresh hold.

### 3. Shader numerical and lane contracts

- [x] Reproduce and repair the covered unsigned-immediate, exceptional-conversion and inactive image/GDS side-effect cases.
- [x] Implement the supported instruction semantics without undefined host conversion behavior.
- [x] Qualify tested nonterminal pixel exports and complete-pair mask proofs before changing their control flow.
- [x] Invalidate persisted translation identity after semantic changes and validate generated modules.
- [x] Validate exact enabled-component results across 30 native PARAM mask cases and two EXEC control cases.
- [ ] Qualify enabled-component reads separately through PARAM0 and PARAM1,
  and establish disabled-component write behavior; do not infer it from pixel
  color exports.
- [ ] Qualify compressed PARAM exports and VSKIP before admitting them.

### 4. Kernel, media and loader ownership

- [x] Exercise event/semaphore pins, cancellation epochs, condition/RW-lock destruction failures and thread-creation cleanup.
- [x] Preserve ownership during in-flight media operations; validate generations, guest-copy publication and bounded audio arguments.
- [x] Bound loader symbol/string references and deregister unwind metadata before image retirement.
- [x] Run public lifecycle, loader and synthetic executable contracts.
- [ ] Re-run `EmulatorKernelLifetimes.FailedPthreadDestroyPreservesNativeObject`; it was skipped in the full suite at this checkpoint.
- [x] Replace AJM zero-fill fake success with strict unsupported-run refusal and no output, result or batch publication.
- [x] Implement and validate the evidenced direct ATRAC9 subset, including
  stateful decoding, reset, bounded pending PCM and atomic publication.
- [ ] Qualify remaining compressed, split and callback/cursor contracts before
  admitting them; raw Opus decodability alone does not close these contracts.

### 5. Diagnostics and input boundaries

- [x] Reproduce shutdown with an idle accepted client and oversized raw request input.
- [x] Bound request input and interrupt accepted reads without close-handle races.
- [x] Validate parser depth, file-length arithmetic, argument escaping and CLI failure exit status.
- [x] Run transport and tooling integration fixtures.
- [x] Bound raw shader-dump extent and share the parser's immutable owner;
  the exact extent and adjacent-byte fixture passes.

### 6. Reproducible validation and remaining contract qualification

- [x] Add actual build and contract validation to the pull-request workflow configuration.
- [x] Record that the pinned development environment is unavailable in the validation host.
- [ ] Run the pinned Nix environment and the pull-request workflow on CI.
- [ ] Qualify held indexed sampled-resource bounds/lifetime, DCC plane profile/ownership/completion and streaming-audio append/AudioOut2 contracts separately.
- [ ] Establish an independent oracle for one-byte `SW_64KB_R_X` offsets, including block transitions and slice behavior.
- [ ] Qualify NGG/fragment transport and Windows physical-memory behavior with native evidence.
- [x] Implement only contracts whose inputs, bounds, lifetime and completion are demonstrated; retain strict refusals elsewhere.
- [x] Defer P3 IR and new-backend work until an accepted runtime baseline; no backend implementation is claimed.

### 7. Primary integration and acceptance

- [ ] Inspect every final diff and resolve independent tester/reviewer findings.
- [x] Build in a separately owned directory with bounded parallelism.
- [x] Run translator127 focused CPU and selected native suites, full CPU retry 4,
  all eight gates and selected production-path integrations.
- [ ] Reproduce strict affected routes and shared-path regressions using isolated evidence and save state.
- [ ] Compare repeatable frame-time distributions before claiming a performance improvement.
- [x] Record confirmed repairs, unavailable hardware coverage, skips and unresolved contracts separately.
