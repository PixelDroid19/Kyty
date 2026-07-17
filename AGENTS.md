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

1. **Evidence before code.** Reproduce the problem, identify the first strict
   failure, and trace the bad state to its producer before editing.
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
strict failure** while preserving everything behind it. Parallel “also fix”
branches, multi-hypothesis edits, and opportunistic refactors inside an open
failure destroy the signal that the next run is comparable.

Before any code change, answer out loud (or in the session handoff):

1. **What is the current verified frontier?** (last place the title runs without
   diagnostic flags)
2. **What is the first strict failure after that frontier?** (exact file, line,
   values)
3. **What single producer created the bad state?** (encoder, parser, HLE ABI,
   layout, resource update—not only the assertion that fired)
4. **What one falsifiable hypothesis will I test next?**
5. **What evidence would prove the hypothesis wrong?**

If any answer is missing, investigate first. Do not edit.

### How plans are formed

Plans are ordered checklists, not wish lists:

1. **Baseline** — clean tree, known HEAD, green focused tests, working build.
2. **Reproduce** — strict run of the private fixture; capture the *first* fail
   completely under an untracked scratch directory.
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
9. **Handoff** — frontier note: previous fail, new fail or checkpoint, residual
   hypothesis, no private paths.

Do not plan modularization, performance campaigns, or multi-NID sweeps while a
strict post-Play (or earlier) blocker is open. Delivery order below is absolute.

### Focus before advancing

- **Stay on the first failure.** Logs after a crash are often wreckage, not
  new work items.
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

1. **Baseline comparison**
   ```bash
   git status --short
   git log -5 --oneline --decorate
   ninja -C _build_macos fc_script
   ```
2. **Strict reproduce** (no `KYTY_BRINGUP_*`, no legacy stub/permissive flags):
   ```bash
   _build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
   # Optional silent runner for speed; record that logging was Silent.
   ```
3. **Capture the full fail** outside Git: message, file:line, PM4 header/body,
   register values, submit id / command offset when available, and whether
   AUTO_CROSS or other diagnostics were set (diagnostics never count as
   acceptance).
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
7. **Consult references only for names and patterns.** Public Gen5 export
   tables, RPCSX wiki, and Vulkan/AMD docs inform vocabulary; every
   PS5-specific claim must reappear in a local capture or test. No GPL code paste.

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
3. **Strict** — rebuild `fc_script`, rerun the private fixture, confirm the
   old fail is gone and no earlier fail returned.
4. **Regress** — re-run GraphicsPackets/GraphicsState (and other touched
   suites). Unfiltered full suite may include a historical date-dependent test;
   do not use it to hide new failures.
5. **Commit** — message describes emulator behavior only
   (`fix(graphics): …`), no fixture identity.

If strict does not advance, the change is not done—even if unit tests pass.

### What “done” means for one cycle

A cycle is complete when **either**:

- **(A) Frontier advanced:** the previous first fail no longer occurs under
  strict flags; a new first fail or a stable checkpoint is captured; focused
  tests pass; change is committed; or
- **(B) Blocker documented:** evidence is insufficient to implement without
  inventing; the fail remains structured and informative; capture and
  residual hypothesis are recorded; temporary probes are removed.

“Process survived” or “non-black frame once” without a strict, flag-free path
is not done.

### Anti-patterns (do not do these)

- Editing while the fail is unreproduced on the current HEAD.
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
2. Re-capture the first strict fail for **that** title; do not assume the other
   title’s frontier applies.
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
post-Play GPU/HLE chain under **strict** flags (no `KYTY_BRINGUP_*`). Linux
Release+Silent dual-strict has sustained
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
unsupported shader/format, or a host fault; treat the first EXIT or structured
error on the current HEAD as the unit of work.

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

**Always re-capture the first strict fail on the current HEAD.** This is not
gameplay acceptance. Diagnostic input, stubs, permissive GPU skips, and
console logging are not supported runtime modes.

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

The following prompt is the canonical brief for an auxiliary agent. Give the
agent a private guest root through `KYTY_GUEST_ROOT`; never paste that path,
title identifiers, binaries, keys, save data, shaders, textures, screenshots,
or logs into tracked files or commit messages.

```text
You are a senior emulator/runtime engineer working inside the Kyty repository.
Your mission is to advance the strict PS5 runtime from the current menu frontier
to a genuinely playable frame, then freeze that frontier and only afterward
perform carefully bounded modularization. Correctness, evidence, portability,
and preservation of working behavior outrank speed or line-count reduction.

CURRENT FRONTIER

- Build works on Linux (`_build_linux`) and macOS (`_build_macos`); use the host
  you are on. Prefer Release + `PrintfDirection=Silent` for wall-clock.
- Vulkan device/swapchain, Gen5 shaders, indexed draws, VideoOut flips, logos,
  recognizable menu, Play/mode transitions (AUTO_CROSS is discovery only),
  loading-card pixels, and deep post-Play GPU/HLE into gameplay-era frames are
  exercised under strict flags. Silent gameplay-era FPS ~11–13 on the reference
  Linux host is not acceptance; never compare to Console logging.
- In tree (do not regress): GpuMemory multi-parent (VB reclaim + surface link;
  Texture mixed parents; IndexBuffer-in-Texture link; WriteBack parent
  classify); GPU-owned RT layout preserve on Update; tile-27 size+4bpp detile;
  Gen5 EUD type-5; formats 14/56/71; multi-RT CB_SHADER_MASK; EXP Param5/6 +
  multi-MRT; structured SPIR-V loops; `v_cvt_i32_f32`; SDWA; SMEM dual-offset +
  variable SBuffer; image_sample dmasks 0x2/0x4/0xb; NGS2 extended max_voices.
- **First strict fail (re-capture on HEAD):** none fixed in the last 90–180 s
  dual-strict after IndexBuffer-in-Texture. Always re-capture; next EXIT or
  structured unsupported is the unit of work.
- **Visual residual:** HUD/UI may work while world is white. Prefer layout /
  texture / RT alias evidence over inventing clears or ThreadFlag. RT UNDEFINED
  discard after SB WriteBack parent invalidation is already fixed — re-verify.
- **Later symptom only:** `ThreadFlag` bit `0x1` (mode `0x21`, 40 ms) with no
  observed Set. Never fabricate the signal. EventFlag live-handle registry
  (garbage → ESRCH) is not Set. Trace the producer after earlier GPU/shader
  aborts are gone.

IMMEDIATE OBJECTIVE AND SUCCESS CONDITION

Advance the strict post-Play path to a **controllable, correctly rendered**
gameplay scene without diagnostics or fabricated success. Process survival and
HUD-only correctness are not playability.

If dual-strict shows a process EXIT, that is first priority (GpuMemory, shader,
format, HLE). If the process survives but the world is wrong (e.g. white world
with working HUD), treat that as the rendering frontier: identify the first
bad producer (RT layout, texture update after WriteBack, sample format, clear,
composite) with capture evidence — do not paper over with permissive flags.

`ThreadFlag` remains deferred while earlier GPU/render issues dominate:

- one event named `ThreadFlag` is created with initial bits `0x0` in older
  captures;
- loading/wait mode `0x21` for bit `0x1` with 40 ms timeout was observed;
- do not Set the bit from Wait, timers, or the render loop.

PRIMARY ORDER OF WORK (DO NOT REORDER)

1. Reproduce the strict frontier with the current checkout and private fixture
   (`$KYTY_GUEST_ROOT` only; never name the title in commits).
2. Fix the first strict failure (re-capture on HEAD) with a documented
   hypothesis and a focused deterministic test or sanitized fixture.
3. Re-run strict execution and advance one failure at a time until the title
   reaches the first controllable gameplay scene under the playability table.
4. Prove real keyboard/controller press+release, movement both ways, one
   action, stable flips, correct geometry/colors, no device-loss, validation
   clean where available. Record Silent FPS + resolution + shader cache.
5. Freeze this working frontier with regression/characterization tests and a
   short evidence report (untracked scratch; no private paths in Git).
6. **Only after steps 1–5 pass and gameplay is reproducible from a baseline
   commit**, modularize oversized files one seam at a time. Every extraction
   must be behavior-neutral and must preserve the frozen gameplay evidence.
   Do not start modularization while a post-Play strict blocker is open.

PHASE GATES AND REQUIRED DELIVERABLES

Phase 0 — Baseline and reproducibility:

- Record HEAD, branch, `git status --short`, build result, focused test result,
  host GPU/capability summary, logging mode, resolution, and shader-cache state.
- Confirm no permissive/stub/trap-skip environment variable is active.
- Reproduce the loading frontier twice so a one-off race is not mistaken for a
  stable contract.
- Save all raw output beneath ignored scratch. The tracked report contains only
  sanitized facts, durations, counts, and source locations.

Phase 1 — Resolve the synchronization frontier:

- Map `ThreadFlag` creation to the guest call site and owning subsystem.
- Map every possible producer path to its HLE export, worker entry point,
  queue/command input, and expected `SetEventFlag` or equivalent completion.
- Capture thread start/exit and the last successful contract on the producer
  thread. The first earlier failure on that thread supersedes the wait timeout.
- Add a deterministic test for the evidenced missing contract before changing
  implementation. A generic EventFlag test alone is insufficient if EventFlag
  itself is behaving correctly and the producer never runs.
- Implement one semantic change and prove the signal now originates from the
  real producer. Record the next strict frontier.

Phase 2 — Reach and prove gameplay:

- Advance one strict blocker at a time through loading and scene creation.
- Use real keyboard/controller press and release edges for acceptance.
- Demonstrate a controllable character, movement in both directions, and at
  least one jump/attack/interact action while frames continue presenting.
- Inspect the scene for correct geometry, colors, texture interpretation,
  viewport/scissor behavior, and stable frame progression.
- Run with Vulkan validation where supported and record zero relevant errors,
  no device loss, no render-thread timeout, and no stuck GPU label.
- Measure performance only with silent function logging, fixed resolution, and
  recorded cache state. Do not make a target FPS claim from console logging.

Phase 3 — Freeze the working frontier:

- Add characterization tests for every compatibility seam required to reach
  gameplay, using sanitized packets/descriptors/ABI arguments only.
- Create a sanitized frontier report containing commit, commands, test counts,
  input sequence, frame/flip evidence, validation result, and performance
  conditions. Do not include the private fixture identity.
- Establish a baseline commit before any architectural extraction. If the
  strict scenario cannot be reproduced from that commit, the freeze is invalid.

Phase 4 — Architecture inventory:

- Measure files and functions, but classify them by responsibility, mutable
  state, ownership, threading, callers, dependencies, and existing tests.
- Current size signals include `ShaderSpirv.cpp` (~8,290 lines),
  `GraphicsRender.cpp` (~5,725), `GraphicsRun.cpp` (~4,521),
  `ShaderParse.cpp` (~3,473), `Shader.cpp` (~3,186), `Pthread.cpp` (~2,807),
  `Graphics.cpp` (~2,740), `Audio.cpp` (~2,716), `Window.cpp` (~2,539), and
  `GpuMemory.cpp` (~2,515). Recount before planning; these numbers are a
  snapshot, not acceptance thresholds.
- Produce an extraction table for each candidate: responsibility to move,
  proposed typed interface, inputs/outputs, owner, thread contract, error
  contract, mutable globals removed or retained, dependency direction,
  characterization tests, and strict-runtime verification command.
- Reject any boundary that cannot be described without generic `Utils`,
  `Common`, `Manager`, forwarding aliases, or bidirectional dependencies.

Phase 5 — Incremental modularization:

- Extract one cohesive responsibility per commit. Do not combine behavior
  changes with file movement or rename campaigns.
- Add characterization coverage first, move the implementation second, delete
  the old implementation in the same change, then rebuild and re-run gameplay.
- Preserve public behavior and one source of truth. Direct and indirect PM4
  paths must still share decoders; all surface consumers must still share one
  layout model; renderer policy must still depend on explicit capabilities.
- Revert an extraction if build, focused tests, menu, loading time, gameplay,
  input, frame output, validation state, or performance materially regresses.
- Update module documentation after each accepted extraction: purpose, public
  interface, invariants, ownership, thread safety, dependency direction, error
  behavior, and tests. Comments must explain contracts, not restate code.

NON-NEGOTIABLE RULES

- Read this entire AGENTS.md before editing. Do not weaken its invariants.
- Reproduce before editing. Capture the first strict error, packet/register
  values, submit ID, command offset, guest call path, and relevant state.
- Never invent a NID, ABI, structure layout, register meaning, tile mode,
  pitch, alignment, return code, or synchronization result. Triangulate from
  guest evidence, local call sites, upstream references, and a test.
- Never use `KYTY_BRINGUP_MODE=unsafe`, trap skipping, default success, assumed
  RGBA8/linear layout, fabricated resources, or placeholder shaders in
  acceptance runs. Unsafe bring-up is discovery-only diagnostics.
- One behavior has one implementation. Direct and indirect PM4 paths share a
  decoder; all resource consumers share one descriptor-to-layout calculation.
- An unsupported behavior must fail structurally and informatively. Do not
  hide it behind a generic fallback, compatibility alias, vendor check, or
  duplicated legacy path.
- Guest semantics remain platform-neutral. macOS, Linux, Windows, Vulkan
  extension, and GPU-vendor details belong at explicit host seams.
- AMD, Intel, NVIDIA, and Apple are capability inputs, never correctness
  policy switches. Select strategies from features, limits, formats, queues,
  and tested semantic alternatives.
- Do not add private fixture paths, title IDs, keys, binaries, saves, assets,
  screenshots, crash dumps, or raw logs to Git. Keep them under an ignored
  local directory and refer to them only as `$KYTY_GUEST_ROOT`.
- Do not use a commit message that identifies the private workload. Describe
  emulator behavior, for example `fix(graphics): validate Gen5 barrier range`.
- Preserve existing working behavior. If an experiment regresses menu reach,
  pixels, input, flips, or build, remove/revert the experiment before trying
  the next hypothesis.

REPRODUCTION AND VERIFICATION COMMANDS

```bash
git status --short
git log -5 --oneline --decorate
cmake -S source -B _build_macos -G Ninja
ninja -C _build_macos fc_script

# Focused tests; avoid the historical date-dependent unfiltered suite.
_build_macos/fc_script '{kyty_run_tests()}' \
  --gtest_filter=<Suite>.<Test>

# Strict integration run. The fixture root is private and untracked.
_build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

For performance characterization, make a temporary untracked copy of the Lua
runner with `PrintfDirection = 'Silent'`, record that fact, and compare the
same scene with the same resolution and shader-cache state. Never treat a
diagnostic auto-input pulse as real playability. For visual evidence, keep
captures outside the repository and record only dimensions, frame count,
average/minimum FPS, input sequence, and whether the image is recognizable.

STRICT DEBUG LOOP

For each failure:

1. Save the complete failure text outside Git.
2. Locate the producer of the bad value, not just the assertion that noticed it.
3. State one falsifiable hypothesis and the expected packet/state change.
4. Add the smallest failing unit test or sanitized PM4/surface fixture.
5. Implement the smallest evidenced semantic change.
6. Run the focused test, rebuild, then rerun the strict workload.
7. Update the frontier note with verified facts, remaining hypothesis, and
   exact next experiment.

GEN5 GRAPHICS CHECKLIST

- Validate PM4 envelope, packet type, opcode/register, declared length, and
  payload bounds before decoding fields.
- Normalize direct/indirect state through one decoder and report unknown
  register, packet, submit ID, and command offset.
- For `AcquireMem`, classify cache action, target mask, extended action, GCR
  control, base, size, and stall mode independently. Prove whether non-zero
  range fields are legal for the observed full-target barrier; do not simply
  mask them away.
- For ReleaseMem/WaitRegMem/WriteData patch helpers, validate the packet's own
  sub-op and patch only the evidenced payload dwords. Return the guest error
  for invalid packets.
- For every surface, derive format, bytes per element/block, dimensions, pitch,
  mip count, tile mode, metadata, sample count, size, and alignment in one
  layout function. Use it for allocation, overlap tracking, CPU upload,
  detiling, and write-back.
- Treat mode 27 (`SW_64KB_R_X`) as a real tiled layout. Do not replace it with
  linear memory or a four-byte assumption. Add round-trip/layout tests for
  representative non-power-of-two dimensions.
- For GPU-memory aliases, model the exact relation and object types observed:
  Equals, Crosses, Contains, and IsContainedWithin are not interchangeable.
  Link or reclaim only when the producer/consumer semantics are evidenced.
- A GPU-owned render target with no write-back must not be spuriously uploaded
  from CPU memory. Its first use must be a validated render-pass transition.

PLAYABILITY ACCEPTANCE GATE

Do not move to refactoring until all rows below have evidence from a strict run:

| Area | Required evidence |
| --- | --- |
| Boot | No missing-import stub, trap skip, or permissive register skip |
| Menu | Recognizable, correctly proportioned image and completed flips |
| Input | Real keyboard/controller press and release edges, not auto-pulse |
| Play path | Play -> mode selection -> loading -> controllable scene |
| Simulation | Character movement plus one attack/jump/interact action |
| Rendering | Correct colors, geometry, target interpretation, and no black/corrupt frame |
| Sync | No render-thread timeout, stuck label, or unbounded wait |
| Vulkan | No validation errors where validation is available; no device-loss |
| Performance | FPS/frametime measured with logging mode and resolution recorded |
| Portability | No guest-state or tile-layout branch keyed to one OS/GPU vendor |
| Regression | Existing focused unit tests and prior menu evidence still pass |

ARCHITECTURE FREEZE AND MODULARIZATION PLAN

After the acceptance gate, record a baseline commit, test output, strict
frontier, and visual/performance measurements. **Do not start file
decomposition while a post-Play (or earlier) strict blocker is open** — first
reach reproducible controllable gameplay, freeze it, then modularize.

Approximate line counts (recount with `wc -l` before planning; snapshot ~2026-07):

| File | ~Lines |
| `ShaderSpirv.cpp` | 8490 |
| `GraphicsRender.cpp` | 5740 |
| `GraphicsRun.cpp` | 4530 |
| `ShaderParse.cpp` | 3520 |
| `Shader.cpp` | 3350 |
| `Pthread.cpp` | 2810 |
| `Graphics.cpp` | 2740 |
| `Audio.cpp` | 2720 |
| `Window.cpp` | 2700 |

Then inventory large modules by responsibility, mutable state, callers, and
tests. Likely seams are:

- `ShaderSpirv.cpp`: Op emitters; EXP/MRT; SMEM/SBuffer; **structured CFG
  (loops/if)**; constant pool; entry/annotations. Highest risk/priority after
  playability because it is the largest and owns the open loop CFG debt.
- `GraphicsRun.cpp`: packet envelope/parser, opcode decoders, cache/barrier
  semantics, and normalized-state mutation. Extract only after identifying the
  shared state contract; direct and indirect paths must call the same decoder.
- `Graphics.cpp`: Gen5 packet builders, ABI-facing patch helpers, and pure
  packet encoders. Keep ABI registration out of packet math.
- `GraphicsRender.cpp`: render planning, resource binding, pipeline creation,
  synchronization, and presentation preparation. A planner should consume
  normalized state and explicit host capabilities, not read global Vulkan state.
- `ShaderParse.cpp`: per-encoding parsers and opcode tables only.
- `Tile.cpp`: format/block geometry, surface-size calculation, address/swizzle
  math, and conversion. Keep one descriptor/layout model and test it without
  Vulkan.
- `GpuMemory.cpp`: relation classification, lifetime/linking, allocation, and
  update/write-back policy. Deepen the module behind typed relation/layout
  contracts; do not create a generic `Utils` or `Manager` bucket.
- `source/emulator/src/Libs/`: one cohesive HLE library per subsystem, with
  centralized registration and contract tests for each new export.

Recommended extraction order after the gameplay freeze is based on dependency
direction, not file size:

1. Extract pure packet/descriptor decoding and layout calculations that can be
   tested without Vulkan or global runtime state.
2. Separate normalized-state mutation from PM4 envelope walking so direct and
   indirect packets call the same typed decoders.
3. Separate shader instruction decoding and intermediate representation from
   SPIR-V emission. Split `ShaderSpirv.cpp` by cohesive emission domains only
   after tests characterize shared type, control-flow, resource, and capability
   contracts.
4. Separate GPU-memory relation classification and write-back policy from
   Vulkan object lifetime, keeping exact alias relations typed and tested.
5. Separate render planning from Vulkan execution/resource creation, passing an
   immutable normalized draw/dispatch plan plus explicit host capabilities.
6. Split host window/surface/input/audio/platform adapters only at existing OS
   boundaries; guest semantics must not move into platform files.
7. Revisit large kernel/HLE files last, one subsystem contract at a time, once
   gameplay no longer depends on unresolved ABI behavior in that seam.

For every proposed new module, write this contract before creating the file:

```text
Purpose:
Public API:
Inputs and outputs:
State and ownership:
Threading and synchronization:
Error/unsupported behavior:
Allowed dependencies:
Forbidden dependencies:
Characterization tests:
Strict runtime checkpoint:
```

Line count is a signal, not the goal. Do not split an atomic eight-line
function. Do split a long function that mixes parsing, state mutation,
allocation, Vulkan calls, and logging. Each extracted function/module must have
one purpose, explicit inputs/outputs, ownership, thread contract, error
contract, and a focused test. Delete the superseded implementation in the same
change; do not leave permanent forwarding aliases or duplicate semantics.

REFERENCES AND HOW TO USE THEM

Use references for behavioral facts, architecture patterns, and test ideas;
never copy incompatible or GPL implementation code into Kyty:

- Public Gen5 export / NID tables — PS4/PS5 export naming and ABI vocabulary
  only; verify every claim with a local capture or focused test.
- RPCSX: https://github.com/RPCSX/rpcsx and its wiki — Linux-first portability,
  runtime boundaries, logging, and compatibility transparency.
- EmuC0re: https://github.com/egycnq/EmuC0re — small emulator seams and
  bring-up discipline; do not infer PS5 GPU behavior from its unrelated target.
- Ryubing/Ryujinx: https://git.ryujinx.app/projects/Ryubing/ryujinx — mature
  separation of guest services, translator/cache boundaries, capability-aware
  host rendering, and regression-oriented compatibility work.
- Vulkan specification and refpages: https://registry.khronos.org/vulkan/ —
  authoritative synchronization, image layout, format, feature, and limit
  semantics.
- AMD GPUOpen/Adrenalin GPU documentation: https://gpuopen.com/ — public GPU
  terminology and cache/tiling concepts; verify every PS5-specific claim
  against Kyty evidence rather than assuming desktop AMD equivalence.

For each borrowed fact, record the URL, license/provenance, exact behavior
learned, and the local test that confirms it. Do not copy GPL code; reimplement
the verified behavior using Kyty's types and MIT-compatible source.

HANDOFF REPORT TEMPLATE

End every auxiliary-agent session with:

- Commit/base revision (no private fixture names).
- Build and focused-test commands plus pass/fail output.
- Strict command and first failure or verified gameplay checkpoint.
- Evidence table: verified fact, source, local reproduction, confidence.
- Files changed and why each belongs to the seam.
- Regression checks and performance measurement conditions.
- Exact next blocker and one proposed falsifiable hypothesis.
- Explicit statement that no diagnostic flag was required for acceptance.
- Phase and gate status: baseline, synchronization, gameplay, freeze,
  inventory, or extraction.
- If gameplay is not reached, the exact producer-side first failure and one
  falsifiable next hypothesis; never report the loading screen as success.
- If modularization began, the frozen gameplay commit and before/after evidence
  proving the extraction was behavior-neutral.

If a required fact cannot be evidenced, stop at a structured unsupported error,
report the blocker, and do not paper over it with a fallback or broad refactor.
```

## Delivery order

Compatibility work and architecture work have a strict order:

1. Advance the strict runtime one evidenced failure at a time.
2. Reach a recognizable menu without diagnostic behavior changes.
3. Prove real controller input, stable frame progression, and representative
   gameplay without Vulkan errors or fabricated guest results.
4. Freeze the working frontier with focused regression and characterization
   tests.
5. Only then decompose oversized modules incrementally, re-running the strict
   scenario after every extraction.

Do not postpone a necessary correctness seam solely because it lives in a large
file. Conversely, do not mix broad file decomposition into an unresolved
compatibility change. A refactor is not evidence that the runtime improved.

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
ninja -C _build_macos
```

Run focused tests with a filter. The historical unfiltered suite contains a
date-dependent assertion and must not be used to conceal new failures:

```bash
_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=<Suite>.*
```

### 2. Reproduce strictly

Use `scripts/run_guest.lua` with a local, untracked fixture root. Do not place
fixture paths, title identifiers, keys, binaries, shaders, textures, screenshots,
or logs in tracked files.

```bash
_build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

The strict run sets no `KYTY_BRINGUP_*` variables (and no removed legacy flags).
Capture the first error completely, including packet/register values and the
guest/host call path when available.

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

1. Record the pre-refactor strict frontier and focused test results.
2. Add missing characterization tests without changing behavior.
3. Move one responsibility behind a narrow interface.
4. Remove the original implementation rather than leaving an alias.
5. Build, run focused tests, and reproduce the strict frontier.
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

Default runtime mode is **strict**: `EXIT_NOT_IMPLEMENTED` aborts with stack and
subsystem shutdown; missing imports do not receive stubs; unknown indirect GPU
registers are fatal. Strict acceptance runs must not set any `KYTY_BRINGUP_*`
variable (`scripts/run_guest.lua` rejects them unless
`KYTY_BRINGUP_ALLOW_DIAGNOSTIC=1` is set for an authorized smoke only).

### Centralized unsafe bring-up (`Kyty::Core::BringUp`)

Diagnostic continuation is centralized. Do **not** invent per-game exceptions,
preload PRX automatically, or cite unsafe survival as compatibility.

| Variable | Meaning |
| --- | --- |
| `KYTY_BRINGUP_MODE=unsafe` | Enable diagnostic policy (absent ⇒ strict). |
| `KYTY_BRINGUP_FEATURES` | CSV: `not_implemented`, `missing_function_import`, `gfx_permissive`. Absent under unsafe enables all three. |
| `KYTY_BRINGUP_SUBSYSTEMS` | CSV scopes: `core,loader,kernel,graphics,audio,network,hle,other`. Absent ⇒ all. |
| `KYTY_BRINGUP_BURST_LIMIT` | Max hits per site inside the window (default 10000). |
| `KYTY_BRINGUP_BURST_WINDOW_MS` | Window length in ms (default 1000). |

Unknown, empty, zero, or contradictory values abort at process start. Circuit-
break on a site re-enters the normal strict abort after printing a summary. The
policy never fabricates EventFlags, fences, memory, or sync results.

**Removed (intentional break):** `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE`.
Using them is a configuration error.

Other diagnostics (unchanged, still not acceptance modes):

- `KYTY_FAULT_LOG=1`: signal-safe fault diagnostics.
- `KYTY_TRACE_LIBC=1`: targeted single-step tracing.
- `KYTY_SKIP_UD2=1`: skips a guest trap for diagnostics; invalidates normal
  execution. **Not** part of the bring-up policy.

Agent diagnostics JSON protocol version is **2** and includes `bringup.mode`,
features, subsystems, limits, unique sites, continuations, missing-import
metrics, and last circuit-break (`BringUp::WriteDiagnosticsJson`).

No diagnostic flag is enabled by default or cited as proof of compatibility.

Integration matrix (process-isolated):

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script kyty_bringup_integration
ctest --test-dir _build_linux --output-on-failure -R KytyBringUpIntegration
```

## External references and licensing

Reference implementations may establish names, concepts, architecture patterns,
and test ideas. Kyty is MIT-licensed; do not copy GPL implementation code into
this repository. Record the behavioral fact and provenance, then implement it
against Kyty's own types after verifying it locally.

Use PS5-focused references for guest ABI and AGC evidence, mature emulators for
renderer/capability architecture, and official Vulkan documentation for host API
semantics. Do not assume another console's GPU behavior applies to PS5.

## Code quality

- Follow `source/.clang-format` and the existing C++17 style.
- Prefer focused functions and explicit types over duplicated bit manipulation.
- Keep headers minimal and ownership clear.
- Avoid broad renames, compatibility aliases, dead code, commented-out paths,
  magic constants without provenance, and unrelated cleanup.
- Comments explain evidence, invariants, and non-obvious hardware semantics; they
  do not narrate obvious code or advertise another project.
- Treat warnings, `git diff --check`, and new validation messages as failures to
  investigate.

## Completion checklist

Before committing a behavior change:

1. The focused test failed before implementation and passes afterward.
2. `ninja -C _build_macos` succeeds.
3. `git diff --check` succeeds.
4. The strict local scenario advances or renders more correctly.
5. No missing-symbol stub or permissive register skip is needed for the claimed
   behavior.
6. No tracked file contains fixture information or generated evidence.
7. No OS or GPU vendor was made a hidden correctness requirement.
8. Existing working behavior was rechecked.
9. The commit message describes emulator behavior without identifying a private
   compatibility fixture.
10. Any refactor preserves the frozen strict frontier and leaves one active
    implementation of each behavior.
11. New or extracted modules have a documented responsibility, ownership model,
    dependency direction, and focused tests.
