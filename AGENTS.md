# Kyty Engineering Guide

This file is the canonical operating manual for every contributor and agent
working in this repository. Read it completely before changing code. More
specific `AGENTS.md` files may add local rules, but they may not weaken the
invariants defined here.

## Mission

Kyty is an experimental PlayStation 4 and PlayStation 5 emulator. This fork is
bringing the PS5 path from early execution to correct, interactive rendering
while preserving a design that can run on macOS, Linux, and Windows and on AMD,
Intel, NVIDIA, and Apple GPUs.

Accuracy comes before superficial progress. A frame that is merely non-black,
a process that survives because unsupported behavior was ignored, or a build
that succeeds without exercising the runtime is not compatibility.

## Non-negotiable invariants

1. **Evidence before code.** Reproduce the problem, identify the first evidenced
   strict failure or bad rendered state, and trace it to its producer before
   changing guest semantics.
2. **Never invent guest behavior.** Do not guess NIDs, ABI signatures, packet
   layouts, register meanings, formats, tiling, alignments, or return codes.
3. **One behavior, one implementation.** Direct and indirect encodings of the
   same GPU state share a decoder. Resource sizes, offsets, and pitches come
   from one layout model consumed by every caller.
4. **No behavioral fallbacks.** Never continue with assumed RGBA8, linear
   tiling, default success, skipped state, placeholder shaders, or fabricated
   resources. Unsupported behavior fails with enough evidence to implement it.
5. **Capability-driven rendering.** Select Vulkan strategies from features,
   limits, formats, queues, and extensions. Vendor IDs are diagnostic data, not
   policy switches.
6. **Keep platform code at the boundary.** OS-specific memory, exceptions,
   threads, windows, surfaces, controllers, and dynamic loading stay in focused
   platform modules. Guest HLE and GPU semantics are platform-neutral.
7. **Do not regress the working frontier.** Preserve existing execution,
   rendering, input, and build behavior unless a test proves that behavior is
   itself incorrect.
8. **Report reality.** Distinguish verified behavior, captured evidence,
   hypotheses, and untested assumptions in code reviews and handoffs.

## Investigation and advancement methodology

This section is the practical operating system for agents and humans advancing
the PS5 path. It does not replace the invariants above; it explains *how* to
obey them day to day. Speed without this loop is noise.

### Core principle: one frontier, one failure, one hypothesis

Compatibility is a chain. The only unit of progress is **advancing the first
evidenced strict failure or bad rendered state** while preserving everything
behind it. Parallel “also fix” branches, multi-hypothesis edits, and
opportunistic refactors inside an open frontier destroy the signal that the
next run is comparable.

Before any guest-semantic behavior change, answer out loud (or in the session
handoff):

1. **What is the current verified frontier?** (last place the title runs without
   behavior-changing diagnostic flags)
2. **What is the first strict failure or bad rendered state after that
   frontier?** (exact file/line/values or first incorrect producer/consumer)
3. **What single producer created the bad state?** (encoder, parser, HLE ABI,
   layout, resource update—not only the assertion that fired)
4. **What one falsifiable hypothesis will I test next?**
5. **What evidence would prove the hypothesis wrong?**

If any answer is missing, investigate first. Do not change guest semantics.
Behavior-neutral observability may be added when it is required to obtain those
answers, but first record a baseline and define how enabled/disabled runs will
prove that the diagnostic path did not move the frontier.

### How plans are formed

Plans are ordered checklists, not wish lists:

1. **Baseline** — clean tree, known HEAD, green focused tests, working build.
2. **Reproduce** — strict run of the private fixture; capture the *first* fail
   or bad rendered state completely under an untracked scratch directory.
3. **Classify** — HLE/ABI, PM4 encode, PM4 parse, surface layout, GPU memory
   relation, shader, VideoOut, sync/label, or host Vulkan.
4. **Hypothesize** — one cause, expected packet/state delta, success criterion.
5. **Test-first** — smallest deterministic fixture or unit test that fails for
   that cause.
6. **Minimal implement** — only the behavior the test (and capture) require.
7. **Verify** — focused green, then strict re-run; expect the same or a *later*
   frontier.
8. **Commit or revert** — commit evidenced behavior; revert failed experiments
   before trying the next hypothesis.
9. **Handoff** — frontier note: previous failure/bad state, new frontier or
   checkpoint, residual hypothesis, no private paths.

Do not plan broad modularization, performance campaigns, or multi-NID sweeps
while a strict or visual post-Play (or earlier) blocker is open. Delivery order
below is absolute.

### Focus before advancing

- **Stay on the first frontier.** Later logs or pixels after a crash/corruption
  are often wreckage, not new work items.
- **Name the seam.** Touch only the module that owns the bad contract
  (e.g. packet encoder vs CP parser vs GpuMemory update). Cross-cutting drive-bys
  are out of scope for that cycle.
- **Prefer producers over symptoms.** A null WaitRegMem address is fixed by
  finding who should write the address (guest patch HLE, adjacent ReleaseMem,
  encoder contract)—not by making the waiter skip null.
- **Freeze working behavior.** If menu reach, flips, or focused tests regress,
  stop and undo before inventing a second fix.
- **Reject “make it continue” patches.** Silent success, assumed RGBA8, linear
  tile, or fabricated labels without a documented encode/execute contract are
  not progress.

### Investigation loop (read-only until evidence is enough)

Set `build_dir` for the current host (`_build_linux` on Linux or
`_build_macos` on macOS); do not reuse artifacts from another worktree.

1. **Baseline comparison**
   ```bash
   git status --short
   git log -5 --oneline --decorate
   build_dir=_build_linux # Use _build_macos on macOS.
   ninja -C "$build_dir" fc_script
   ```
2. **Strict reproduce** (no `KYTY_STUB_MISSING`, `KYTY_GFX_PERMISSIVE`, or
   `KYTY_SKIP_UD2`):
   ```bash
   "$build_dir"/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
   # Optional silent runner for speed; record that logging was Silent.
   ```
3. **Capture the full frontier** outside Git: failure message and file:line, or
   the first bad rendered state and producer/consumer evidence; PM4 header/body,
   register values, submit id / command offset when available; and whether
   AUTO_CROSS or other diagnostics were set (behavior-changing diagnostics never
   count as acceptance).
4. **Map encode → execute.** For GPU issues, locate:
   - HLE builder in `Graphics.cpp` (guest call, arguments, returned pointer)
   - Packet dwords at encode time
   - Whether the guest patches later (`GetDataPacketPayloadAddress`, EopPatch,
     direct stores through the returned header)
   - CP parser in `GraphicsRun.cpp` and the values at execute time
5. **Use Silent vs Console deliberately.** `PrintfDirection = Silent` is for
   wall-clock and long runs; it hides HLE prints. For encode/patch sequences,
   use Console or temporary `stderr` probes that do not depend on Printf, then
   **delete probes before commit**.
6. **Compare working vs failing forms.** Same export with non-null address
   earlier in the run is ABI evidence; a post-Play null pair is a different
   contract to explain, not a free pass to invent addresses.
7. **Consult references only for names and patterns.** SharpEmu, RPCSX wiki,
   Vulkan/AMD docs inform vocabulary; every PS5-specific claim must reappear
   in a local capture or test. No GPL code paste.

### Hypothesis and trial-and-error discipline

Trial and error is allowed; **uncontrolled** trial and error is not.

| Rule | Practice |
| --- | --- |
| One variable | Change a single contract (offset, accepted bit, opcode case). |
| Falsifiable | “If PayloadAddress returns cmd+1 for WaitMem, guest patches non-null.” |
| Time-box | If the capture does not move after a clean experiment, stop and re-classify. |
| Revert failed experiments | Do not stack dead ends; `git diff` should only show the live hypothesis. |
| Record negatives | “Return payload not header breaks SizeDw” is permanent evidence. |
| Prefer structure over silence | Unknown stays `EXIT` / guest error with body dump, not success. |

When a hypothesis fails, write one line: *hypothesis, observation, next
hypothesis*. Then remove the code from that experiment before coding the next.

### Debug and execution tactics

- **First failure only.** Fix order is boot → logos → menu → Play/load →
  gameplay. Never jump ahead because a later log line looks interesting.
- **Encoder dump vs CP dump.** Print or test packet dwords at HLE return *and*
  at parser entry; many bugs are “guest never patched” vs “parser wrong layout.”
- **Adjacent packets.** ReleaseMem/WaitRegMem, WaitFlipDone neighbors, and
  SizeDw residual registers (pointer arithmetic at ±packet size) often explain
  deferred address fills.
- **Return-value contracts.** Gen5 builders often return the **packet header**
  for SizeDw / EopPatch. Returning a mid-packet payload pointer can “fix” a
  store test and break SizeDw—prove return use from capture.
- **Thread races.** Render-thread IndexBuffer update vs CP-thread WaitRegMem
  can both be real; fix the first process exit, then re-run for the next.
- **Input.** `KYTY_AUTO_CROSS` is discovery only. Acceptance needs real edges
  or an explicitly recorded non-claim.
- **Performance.** Never compare FPS under Console logging to Silent; record
  logging mode, resolution, and shader-cache state.
- **Scratch evidence.** Save logs under untracked `_scratch_playable/` or a
  session scratch dir. Never commit guest paths, title IDs, screenshots, or raw
  multi-megabyte logs.

### Red → green → strict

For every semantic change:

1. **Red** — add or extend a unit test with sanitized PM4/HLE args that fails
   for the missing contract.
2. **Green** — implement the minimum; focused filter passes.
3. **Strict** — rebuild `fc_script`, rerun the private fixture, confirm the old
   failure or bad rendered state is gone or later and no earlier frontier
   returned.
4. **Regress** — re-run GraphicsPackets/GraphicsState (and other touched
   suites). Unfiltered full suite may include a historical date-dependent test;
   do not use it to hide new failures.
5. **Commit** — message describes emulator behavior only
   (`fix(graphics): …`), no fixture identity.

If the strict or visual frontier does not advance, the behavior change is not
done—even if unit tests pass.

### What “done” means for one cycle

A cycle is complete when **either**:

- **(A) Frontier advanced:** the previous first fail or bad rendered state no
  longer occurs under strict flags, or its first occurrence moves later; a new
  first fail/state or stable checkpoint is captured; focused tests pass; the
  change is committed; or
- **(B) Blocker documented:** evidence is insufficient to implement without
  inventing; the fail or bad state remains structured and informative; capture
  and residual hypothesis are recorded; temporary probes are removed.

“Process survived” or “non-black frame once” without a strict, flag-free path
is not done.

### Anti-patterns (do not do these)

- Changing guest semantics while the fail or bad state is unreproduced on the
  current HEAD.
- Multiple hypotheses in one commit.
- Keeping a failed experiment “just in case.”
- Broad renames/refactors on an open compatibility blocker.
- Vendor or OS branches inside guest decode/layout.
- Claiming playability with AUTO_CROSS, stubs, or permissive GPU skips.
- Leaving permanent dual implementations or feature-flagged legacy paths.

### Multi-title bring-up

When switching private fixtures (or adding a second root):

1. Keep each title’s root in an **env var only** (`$KYTY_GUEST_ROOT` or a second
   untracked env). Never write absolute private paths into tracked files.
2. Re-capture the first strict failure or bad rendered state for **that** title;
   do not assume the other title’s frontier applies.
3. Prefer HLE exports that are **named and sized** (measure APIs, standard libc)
   before open-ended stubs. Unknown Share/Ampr NIDs may log arguments and return
   success only when that is the smallest way to reach the next evidenced fail—
   document residual name/ABI debt in scratch, not as playability claims.
4. After any dependency bump (SDL, etc.), re-run **both** focused unit filters
   and the primary title’s strict path before claiming no regression.

### Vendored dependency bumps

- Prefer official upstream releases into `source/3rdparty/` behind the existing
  CMake wrappers (static SDL, etc.).
- **One high-impact dep at a time** (SDL first). Rebuild `fc_script`, focused
  tests, then the primary strict fixture.
- Hold Vulkan headers and other ABI-sensitive pins unless MoltenVK/runtime is
  revalidated. Prefer SDL2 over SDL3 for this tree.
- Commit messages: host/build behavior only (e.g. `build: upgrade vendored SDL2
  to 2.32.10`), never private title names.

## Current verified frontier

The local reference workload reaches Vulkan device creation, guest engine
startup, Gen5 shader creation, indexed draws, VideoOut submission, repeated
swapchain presentation, logos, a recognizable menu, Play / mode selection
(discovery auto-input is not acceptance), loading-card presentation, and a deep
post-Play GPU/HLE chain under **strict** flags (no `KYTY_STUB_MISSING`,
`KYTY_GFX_PERMISSIVE`, or `KYTY_SKIP_UD2`). Linux Release+Silent dual-strict has
sustained
**gameplay-era frames** for 90–180 s (~11–13 FPS Silent, AUTO_CROSS discovery
only) without a process-killing structural EXIT on recent captures.

Recent strict bring-up (evidence-backed, focused tests where noted) includes:

- GpuMemory multi-parent alias policies (Texture/Storage/Vertex/RenderTexture
  relations as captured; inverse or unobserved relations stay strict).
- GpuMemory: multi-parent VertexBuffer with surface link + peer VB reclaim;
  Texture mixed parents (VB reclaim/link, SB/RT/Texture
  Contains/IsContainedWithin/Crosses); **IndexBuffer Contained in Texture**
  (and other surfaces) **links**, does not reclaim the Texture
  (`GpuMemoryAllowsIndexContainedInSurface`; captured IB size `0xe4`).
- WriteBack multi-parent classification: Equals → propagate hash; Crosses /
  Contains / IsContainedWithin → invalidate only (partial overlap).
- GPU-owned tiled RenderTexture (no write-back): `update_func` must **not**
  force `VK_IMAGE_LAYOUT_UNDEFINED` on Update re-entry. StorageBuffer WriteBack
  invalidates alias parents; UNDEFINED→COLOR transitions **discard** prior
  render-pass contents (user-visible white intermediate world with HUD still
  drawing). Create still starts UNDEFINED once.
- Gen5 tile mode 27 (`SW_64KB_R_X`) **size** and **CPU detile for 4 bpp** sample
  textures (16-pipe non-RbPlus pattern table reimplemented from public MIT
  ADDRLIB vocabulary; visual sample quality still needs post-playability QA).
- Gen5 sample formats in PrepareTextures: Ufmt 56 (RGBA8), 14 (RG8 linear
  pitch), 71 (RGBA16F RT alias). Tile 27 pure CPU upload remains format-56 only.
- Gen5 EUD: direct resource type 5 as EUD pointer when `eud_size_dw != 0` and
  `srt_size_dw == 0`; overflow sharp offsets map through EUD base
  `round_up(user_sgpr_num, 4)`.
- Multi-RT `CB_SHADER_MASK` full-channel nibbles (`0` or `0xf` per RT).
- EXP Param5 (`0x25`) / Param6 (`0x26`) and multi-MRT compressed / null EXP for
  MRT0–3 (including `done=0` / `vm=0` variants observed on load).
- SPIR-V structured loops for backward `S_BRANCH` (`OpLoopMerge` + body +
  continue / unreachable as required); do not regress CFG.
- `v_cvt_i32_f32` (VOP1 `0x8` / VOP3 `0x188`); VOP1/VOP2 SDWA (encoding 249).
- SMEM dual offset (SGPR soffset + 21-bit imm) and variable-offset
  `s_buffer_load_dword` / `x2` / `x4` with imm constants registered for SPIR-V.
- `image_sample` dmasks including single-channel `0x2`/`0x4` and `0xb` (R+G+A).
- Gen5 extended NGS2 rack `max_voices` at option offset `+0x50` when option size
  ≥ `0xb0` (focused Audio tests).
- PS user SGPR window up to 32; CB blend1–7, BufferLoadFormatXyzw, and related
  register/shader contracts from earlier cycles.

**First strict fail on recent Linux Release+Silent+AUTO_CROSS discovery runs
(always re-capture on HEAD):** none observed within 90–180 s dual-strict after
`dd64b41` (IndexBuffer-in-Texture). Prior first fail was

`unknown relation: Texture - Contains - IndexBuffer`

at ~frame 1540 (single-parent GpuMemory CreateObject). That policy is in tree.
Re-capture may still surface a **later** GpuMemory multi-parent topology, an
unsupported shader/format, a host fault, or an earlier bad rendered state; treat
the first evidenced strict failure or bad rendered state on the current HEAD as
the unit of work.

**Visual residual (not a process EXIT):** gameplay **HUD/UI can be correct while
the world is white**. First hypothesis addressed by preserving GPU-owned RT
layout across Update (`b86c730`). If white remains after that commit, next
evidence targets are texture CPU reload after WriteBack invalidation, intermediate
format-71 sampling, and clear/composite — not ThreadFlag fabrication.

`ThreadFlag` bit `0x1` (mode `0x21`, 40 ms waits, no observed Set in earlier
captures) remains a **later** suspected synchronization symptom: do not fake
the bit from `WaitEventFlag`, timers, or the render loop. Identify the producer
only after the GPU/shader chain no longer aborts earlier. EventFlag **handle
registry** (reject unregistered/garbage pointers with `ESRCH`) is host safety,
not a substitute for Set.

Linux host path builds with `_build_linux` / Ninja Release; macOS continues to
use `_build_macos`. Prefer default `CMAKE_BUILD_TYPE=Release` on single-config
generators. Default `scripts/run_guest.lua` uses `PrintfDirection = 'Silent'`
for usable FPS; Console logging is evidence-only and destroys frame-time
comparability. Session evidence may live under a local untracked directory
(e.g. Documents `Kyty-implementer/` copy of implementer scratch); never commit
guest paths, title IDs, raw multi-megabyte logs, or `_Shaders/` dumps.

**Always re-capture the first strict failure or bad rendered state on the current
HEAD.** This is not gameplay acceptance. Diagnostic input, stubs, permissive
GPU skips, trap skipping, and console logging are not supported acceptance
modes.

## Architecture map

- `source/emulator/src/Loader/`: guest image loading, relocation, NID resolution,
  native-call trampolines, and exception integration.
- `source/emulator/src/Kernel/`: guest memory, direct/flexible allocation,
  pthreads, synchronization, files, and time.
- `source/emulator/src/Libs/`: HLE export registration and guest API contracts.
- `source/emulator/src/Graphics/Graphics.cpp`: PS4/PS5 command-buffer builders
  and Gen5 AGC-facing exports.
- `source/emulator/src/Graphics/GraphicsRun.cpp`: PM4 packet parsing and updates
  to normalized `HW::Context` state.
- `source/emulator/include/Emulator/Graphics/HardwareContext.h`: normalized guest
  GPU state used by rendering.
- `source/emulator/src/Graphics/GraphicsRender.cpp`: resource binding, pipeline
  construction, draw/dispatch recording, and synchronization.
- `source/emulator/src/Graphics/ShaderParse.cpp` and `ShaderSpirv.cpp`: guest
  shader decoding and SPIR-V generation.
- `source/emulator/src/Graphics/Tile.cpp`: guest surface layout and addressing.
- `source/emulator/src/Graphics/Objects/`: Vulkan-backed guest resources and
  memory tracking.
- `source/emulator/src/Graphics/VideoOut.cpp` and `Window.cpp`: display buffers,
  Vulkan device/swapchain setup, and presentation.
- `source/lib/`: reusable host runtime, platform, threading, memory, filesystem,
  math, and script infrastructure.
- `source/unit_test/`: GoogleTest registration and deterministic fixtures.

Keep guest API decoding, guest GPU semantics, normalized state, Vulkan objects,
and host platform adapters conceptually separate even where legacy files still
contain more than one responsibility. Improve the seam being touched; do not
perform unrelated mass refactors.

## Auxiliary-agent handoff prompt

Do not maintain a second copy of this manual inside an agent prompt. Auxiliary
agents must read this entire `AGENTS.md`; the current frontier, delivery order,
required workflow, diagnostic rules, and completion checklist below remain the
single source of truth.

Pass this bounded brief after assigning one concrete responsibility:

```text
You are a senior emulator/runtime engineer working inside the Kyty repository.

Read AGENTS.md completely before acting. Follow its current verified frontier,
delivery order, required workflow, diagnostic rules, and completion checklist;
this handoff does not weaken or duplicate them.

Objective:
- Advance exactly the assigned first evidenced strict failure or bad rendered
  state, or implement the assigned behavior-neutral observability seam.
- Preserve every earlier execution, rendering, input, build, and portability
  checkpoint.

Constraints:
- Reproduce on current HEAD before changing guest semantics.
- If existing evidence is insufficient, add only bounded passive instrumentation,
  then prove observer-disabled/enabled equivalence before forming a semantic fix.
- Use the private fixture only through $KYTY_GUEST_ROOT. Never place fixture
  paths, identifiers, binaries, assets, screenshots, or raw logs in Git.
- Never fabricate guest results, pump the scheduler, signal waits from the
  observer, skip unsupported GPU state, or use permissive/stub/trap-skip modes
  for acceptance.
- Work test-first for semantic changes; add only focused tests that protect a
  meaningful contract.
- Do not begin broad extraction until reproducible correctly rendered gameplay
  has been frozen. A new diagnostic module is not permission to refactor an
  unrelated compatibility seam.
- Keep each commit to one behavior or cohesive extraction, use a short subject,
  and add no Co-authored-by trailer.

Handoff:
- Base revision and dirty-tree facts.
- Reproduction command and first strict failure/bad rendered state.
- Verified producer evidence, one falsifiable hypothesis, and disproof condition.
- Files changed, focused tests, build, strict/visual replay, and observer
  equivalence where relevant.
- Risks, assumptions, remaining work, and exact next frontier.
```

## Delivery order

Compatibility work and architecture work have a strict order:

1. Establish a reproducible baseline and the passive observability required to
   identify the first strict or visual failure. Observability must not change
   guest behavior and is not permission for broad modularization.
2. Advance the strict runtime one evidenced failure or bad rendered state at a
   time.
3. Reach a recognizable menu without diagnostic behavior changes.
4. Prove real controller input, stable frame progression, and representative
   gameplay without Vulkan errors or fabricated guest results.
5. Freeze the working frontier with focused regression and characterization
   tests.
6. Only then decompose oversized modules incrementally, re-running the strict
   scenario after every extraction.

Do not postpone a necessary correctness seam solely because it lives in a large
file. Conversely, do not mix broad file decomposition into an unresolved
compatibility change. A refactor is not evidence that the runtime improved.

### History gate before `main`

- A final tree that reverts fabricated behavior is insufficient when the
  fabricated commits remain ancestors. Build a new curated integration branch
  from the trusted base and replay only evidenced net behaviors.
- Do not rewrite or force-push archival branches merely to make history look
  clean. Preserve them until the curated branch proves the same or later
  strict/visual frontier.
- Protect dirty worktrees and existing stashes. Reconcile them file by file;
  never apply an opaque whole-tree patch over overlapping changes.
- Before `main`, inspect the full commit bodies, verify zero `Co-authored-by`
  trailers in the incoming range, use `range-diff` against the source branches,
  and prove that intentionally omitted commits are not ancestors.
- Push only the curated branch until focused tests, supported-host builds, two
  strict/visual reproductions, and history review all pass.

### Gameplay acceptance gate

Before declaring the primary workload playable or starting broad extraction:

- Reproduce the same correctly rendered gameplay checkpoint twice from the same
  baseline commit under strict flags.
- Use real keyboard/controller press and release edges. Demonstrate movement in
  both directions and at least one jump, attack, or interaction while frames
  continue presenting. Discovery auto-input is not acceptance.
- Verify geometry, colors, texture interpretation, viewport/scissor behavior,
  stable flips, and scene progression. HUD-only rendering, a white world,
  channel swaps, saturation/fringing, or a non-black frame is not success.
- Record no device loss, stuck GPU label, render-thread timeout, or relevant
  Vulkan validation error on a host where validation is available.
- Record resolution, Silent logging mode, shader-cache state, frame/flip counts,
  median and minimum FPS, host GPU/capabilities, and the exact input sequence.
  Never compare performance against Console logging.

### Frozen-frontier gate

The baseline before extraction must contain focused characterization coverage
for each compatibility seam needed to reach gameplay and a sanitized untracked
frontier report with commit, commands, test counts, input sequence, frame/flip
evidence, validation result, and performance conditions. It contains no private
fixture identity, path, asset, screenshot, or raw log. If that checkpoint cannot
be reproduced from the recorded commit, the frontier is not frozen.

## Module and function boundaries

- A function has one observable purpose and one reason to change. Separate
  packet decoding, validation, state mutation, resource layout, Vulkan object
  creation, synchronization, and diagnostic formatting.
- A source file owns one cohesive capability. Large legacy translation units
  are migration sources, not patterns for new centralized implementations.
- Line count is a review signal, not a mechanical rule. Do not split an
  eight-line function that expresses one atomic operation, and do not keep an
  eight-thousand-line module merely because individual functions are short.
- Before extracting code, identify the proposed module's inputs, outputs,
  ownership, error contract, threading contract, and tests. If those cannot be
  stated concisely, the boundary is not ready.
- Extract behavior behind narrow typed interfaces. Do not create generic
  `Utils`, `Common`, `Manager`, compatibility aliases, forwarding layers, or
  duplicate old/new implementations.
- Keep dependency direction explicit: guest descriptor decoding feeds
  normalized state; normalized state feeds resource and render planning; host
  capabilities select a semantically equivalent Vulkan strategy. Host details
  never flow backward into guest semantics.
- Add characterization tests before moving existing behavior. The extraction
  commit must be behavior-neutral, and the focused tests plus strict scenario
  must show the same or later frontier.
- Delete the superseded implementation in the same extraction. There is no
  permanent legacy path, hidden fallback, or feature flag selecting duplicate
  semantics.
- Document public module contracts and non-obvious hardware invariants. Avoid
  comments that merely restate code or contain private workload information.

## Required workflow

### 1. Establish a clean comparison

Before editing:

```bash
git status --short
git log -5 --oneline --decorate
build_dir=_build_linux # Use _build_macos on macOS.
ninja -C "$build_dir"
```

Run focused tests with a filter. The historical unfiltered suite contains a
date-dependent assertion and must not be used to conceal new failures:

```bash
test_filter='SuiteName.*' # Replace SuiteName with the focused suite.
"$build_dir"/fc_script "{kyty_run_tests()}" --gtest_filter="$test_filter"
```

### 2. Reproduce strictly

Use `scripts/run_guest.lua` with a local, untracked fixture root. Do not place
fixture paths, title identifiers, keys, binaries, shaders, textures, screenshots,
or logs in tracked files.

```bash
"$build_dir"/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

The strict run uses none of `KYTY_STUB_MISSING`, `KYTY_GFX_PERMISSIVE`, or
`KYTY_SKIP_UD2`. Capture the first failure or bad rendered state completely,
including packet/register values, producer/consumer evidence, and the guest/host
call path when available.

If existing tools cannot identify the producer, add only the smallest bounded
passive instrumentation. Establish its no-interference hypothesis and focused
test, then prove observer-disabled/enabled equivalence before forming a semantic
fix. The observer does not wake threads, signal waits, pump the scheduler,
fabricate completion, or turn a timeout into success.

### 3. Form one hypothesis

State the suspected root cause and the evidence supporting it. Change one
variable at a time. If a hypothesis fails, remove the experiment before testing
the next one.

### 4. Work test-first

For every behavior change:

1. Add the smallest deterministic failing test.
2. Run it and confirm the expected failure.
3. Implement only the behavior required by the test.
4. Run the focused test until it passes.
5. Build and re-run the strict integration scenario.

Sanitized PM4 packets and surface descriptors are acceptable fixtures. Guest
code and assets are not.

### 5. Verify the real outcome

For graphics changes, a successful build and non-black pixels are insufficient.
Verify geometry, colors, resource interpretation, completed flips, absence of
Vulkan errors, and a recognizable correctly proportioned frame. Preserve local
visual evidence outside Git.

### 6. Refactor only behind a frozen frontier

After strict menu and gameplay acceptance exists, inventory oversized modules
with line counts, responsibilities, dependency direction, mutable globals, and
test coverage. Select one cohesive extraction at a time. For each extraction:

1. Record the pre-refactor strict/visual frontier and focused test results.
2. Add missing characterization tests without changing behavior.
3. Move one responsibility behind a narrow interface.
4. Remove the original implementation rather than leaving an alias.
5. Build, run focused tests, and reproduce the strict/visual frontier.
6. Revert the extraction if the frontier, frame, input, or validation state
   regresses.

## HLE and ABI rules

- Every export needs an evidenced name, NID, signature, calling convention,
  argument validation, return code, and side effect.
- Prefer guest error returns for expected invalid input. Assertions and process
  exits are for violated emulator invariants, not ordinary guest errors.
- Do not map a new NID to a convenient existing function until their contracts
  have been compared, including failure behavior.
- Keep registration centralized in the owning `Lib*.cpp` module.
- A generic missing-symbol stub may be used to discover which import is called;
  it must never be required by acceptance runs or releases.

## Graphics rules

### PM4 and normalized state

- Packet envelope validation belongs to packet parsing.
- Register bit decoding belongs to one state-decoder function.
- Direct and indirect packet handlers call the same decoder.
- Unknown registers report packet type, register, value, submit ID, and command
  offset, then stop in strict mode.
- Never label an unknown register harmless without proving its semantics and
  showing that the workload does not depend on it.

### Surface layout

- Format, block geometry, pitch, mip levels, depth/array layers, sample count,
  tile mode, metadata, size, and alignment form one descriptor-to-layout
  calculation.
- Compressed formats use block dimensions; bytes-per-pixel arithmetic is not a
  substitute.
- CPU upload/detiling, overlap tracking, Vulkan allocation, and writeback consume
  the same layout.
- An unknown descriptor returns a structured unsupported error. It does not
  assume four-byte texels or linear memory.

### Vulkan and GPU portability

- Collect device capabilities once and pass them explicitly to consumers.
- Classify each capability as required, optional with a semantically equivalent
  tested strategy, or diagnostic-only.
- A correct alternative for an absent extension is not a behavioral fallback:
  it must preserve guest-visible semantics and have tests for both strategies.
- Do not add AMD-, Intel-, NVIDIA-, Apple-, MoltenVK-, or driver-specific paths
  to guest state decoding or surface layout.
- Keep Vulkan validation clean when the platform supports the required layers.

## Platform portability rules

- macOS is a distinct supported host, not a Linux build label.
- Use portable C++ and existing Core/Sys abstractions in shared code.
- Confine `__APPLE__`, `_WIN32`, and Linux-specific branches to platform-facing
  implementation files.
- Do not use Apple frameworks, Win32 APIs, or Linux syscalls in HLE, PM4,
  shaders, surface layout, or renderer policy.
- Treat host CPU architecture separately from host OS. Preserve the current
  native x86-64 path while keeping future execution backends possible.

## Diagnostic flags

- `KYTY_STUB_MISSING=1`: resolves unknown imports to logging stubs. This changes
  guest behavior and is for frontier discovery only.
- `KYTY_GFX_PERMISSIVE=1`: skips unknown indirect GPU state. This invalidates
  rendering and is for evidence collection only.
- `KYTY_FAULT_LOG=1`: enables signal-safe fault diagnostics. Treat it as passive
  only after observer-disabled/enabled equivalence is established.
- `KYTY_TRACE_LIBC=1`: enables targeted single-step tracing and changes timing;
  it is evidence-only, not an acceptance mode.
- `KYTY_SKIP_UD2=1`: skips a guest trap for diagnostics and invalidates normal
  execution.

No diagnostic flag is enabled by default or cited by itself as proof of
compatibility. Behavior-changing flags are absent from acceptance; passive
observability may remain only after equivalence is proven.

## Native diagnostics and live-update rules

- Hang detection uses independent progress lanes for guest execution, HLE,
  command processing, GPU submission/completion, VideoOut flips, presentation,
  and explicit waits. A global frame or import counter is insufficient.
- Instrumented hot paths use bounded preallocated records. They do not allocate,
  format strings, write files, acquire emulator-owned locks, or perform recovery.
- Detection and recovery are separate. The default stall action is a sanitized
  snapshot and notification, never process termination or state fabrication.
- A report distinguishes temporal correlation from a proven cause. Include
  sequence and monotonic time, last progress per lane, active wait, submit/fence
  state, and evidenced producer dependencies. If the producer is unknown, the
  cause remains explicitly unknown.
- Keep the in-process recorder independent from a native supervisor so evidence
  can survive a hard hang or crash. Shared protocols are versioned, bounds
  checked, pointer-free, and optional at runtime.
- Automatic evidence excludes private roots, workload identifiers, guest
  binaries, textures, screenshots, and full guest memory. Potentially sensitive
  OS or vendor dumps are local-only and opt-in.
- Diagnostic filters are startup-only until an explicitly designed safe command
  boundary permits replacement; UI-only state may be replaced atomically.
  Shader overrides may reload only through compile/validate, new pipeline
  generation, safe publication, and fence/timeline retirement of old pipelines.
  A failure keeps the currently published generation active. The final
  compatibility replay disables overrides, and the reload path uses the same
  compile/cache contracts as production.
- Arbitrary C++ hot reload is not supported by the current static architecture.
  Scheduler, memory, renderer, resource ownership, and translator changes use a
  rebuild plus controlled restart/replay. A future reloadable HLE seam requires
  a stable C ABI, quiescence, active-call draining, and explicit state migration.

## External references and licensing

Reference implementations may establish names, concepts, architecture patterns,
and test ideas. Kyty is MIT-licensed; do not copy GPL implementation code into
this repository. Record the behavioral fact and provenance, then implement it
against Kyty's own types after verifying it locally.

Use PS5-focused references for guest ABI and AGC evidence, mature emulators for
renderer/capability architecture, and official Vulkan documentation for host API
semantics. Do not assume another console's GPU behavior applies to PS5.

Use this research map by question, and prefer the repository/specification over
third-party summaries:

- SharpEmu and PR 164: runtime diagnostics, thread-state snapshots, AGC naming,
  and negative examples where a watchdog changes execution instead of observing
  it: <https://github.com/par274/sharpemu> and
  <https://github.com/par274/sharpemu/pull/164>. Review the current pull-request
  index for follow-up evidence: <https://github.com/par274/sharpemu/pulls>.
- shadPS4 and RPCSX: native developer-tool surfaces, renderer boundaries, shader
  replacement concepts, and capability-driven host design:
  <https://github.com/shadps4-emu/shadPS4> and
  <https://github.com/RPCSX/rpcsx>.
- Kyty upstream: original intent and regression comparison:
  <https://github.com/InoriRus/Kyty>.
- PS5-focused ABI/loader vocabulary: etaHEN and PS5 ftpsrv
  [self.h](https://github.com/ps5-payload-dev/ftpsrv/blob/master/self.h),
  [self.c](https://github.com/ps5-payload-dev/ftpsrv/blob/master/self.c), and
  [self-prospero.c](https://github.com/ps5-payload-dev/ftpsrv/blob/master/self-prospero.c).
  Use <https://github.com/etaHEN/etaHEN> as authority and its
  [Developer Guide](https://deepwiki.com/etaHEN/etaHEN/4-developer-guide) only
  as a navigation aid that must be checked against the repository.
- Host ABI and libc behavior: FreeBSD source and libc amd64:
  <https://github.com/freebsd/freebsd-src> and
  <https://cgit.freebsd.org/src/tree/lib/libc/amd64>.
- Platform integration patterns only, not PS5 semantic authority: Graphics203's
  PlayStation platform layer:
  <https://github.com/leBFG/Graphics203/tree/e5516aff2705a81001791363d6ff45c4af4f6062/skateboard_engine/Platform/Playstation/PlaystationPlatform>.
- Host API and architecture authority: the Vulkan specification and AMD64
  Architecture Programmer's Manual, Volume 4:
  <https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html> and
  <https://docs.amd.com/v/u/en-US/26568_3.26_APM_Vol4>.

Before adopting a fact, check the source's current license and record the URL,
license, learned behavior, and local confirming test in research notes. Keep
source-project advertising out of implementation comments; include attribution
only when the applicable license requires it.

## Code quality

- Follow `source/.clang-format` and the existing C++17 style.
- Prefer focused functions and explicit types over duplicated bit manipulation.
- Keep headers minimal and ownership clear.
- Avoid broad renames, compatibility aliases, dead code, commented-out paths,
  magic constants without provenance, and unrelated cleanup.
- Do not introduce deprecated APIs. Treat existing deprecation warnings as
  explicit migration debt rather than suppressing them in new code.
- Comments explain evidence, invariants, and non-obvious hardware semantics; they
  do not narrate obvious code or advertise another project.
- Treat warnings, `git diff --check`, and new validation messages as failures to
  investigate.

## Completion checklist

Before committing a behavior change:

1. The focused test failed before implementation and passes afterward.
2. `ninja -C "$build_dir"` succeeds for the current host after `build_dir` is set
   as above.
3. `git diff --check` succeeds.
4. The strict local scenario advances or renders more correctly.
5. No missing-symbol stub or permissive register skip is needed for the claimed
   behavior.
6. No tracked file contains fixture information or generated evidence.
7. No OS or GPU vendor was made a hidden correctness requirement.
8. Existing working behavior was rechecked.
9. The commit message describes emulator behavior without identifying a private
   compatibility fixture.
10. Any refactor preserves the frozen strict/visual frontier and leaves one
    active implementation of each behavior.
11. New or extracted modules have a documented responsibility, ownership model,
    dependency direction, and focused tests.

Before committing a diagnostic change:

1. Bounded-buffer overflow and concurrent snapshot behavior are covered without
   wall-clock sleeps.
2. Shared protocols validate version, offsets, sizes, and disconnect behavior.
3. Observer-disabled and observer-enabled runs preserve focused tests, the same
   strict/visual frontier, and the documented performance gate.
4. Automatic evidence passes byte-for-byte private-value canary checks.
5. When shader reload is touched, failure preserves the published generation,
   and no override is enabled for compatibility acceptance.

Every new commit carries one behavior or one cohesive extraction, uses a short
subject, and contains no `Co-authored-by` trailer.
