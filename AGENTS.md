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
   tables and Vulkan/AMD documentation inform vocabulary; every
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
- Compute LDS is represented as a correctly sized SPIR-V `Workgroup` array.
  `COMPUTE_PGM_RSRC2.LDS_SIZE` is a 9-bit value in 128-dword units and is part
  of compute shader identity. Captured `ds_write_b32 v5, v4 offset:0` uses a
  byte address and preserves the written 32-bit payload (`516934ca`).
- Captured compute `s_barrier` lowers to `OpControlBarrier` with Workgroup
  execution/memory scope and `AcquireRelease | WorkgroupMemory`; its SPIR-V
  semantics constant must be registered explicitly, and the lowering remains
  compute-only (`b4480131`).
- Captured `ds_read2_b32` decodes its two 8-bit dword-scaled offsets while
  keeping `vaddr` byte-addressed, and loads both values through the same
  Workgroup storage used by `ds_write_b32` (`990b9a40`).
- `v_cvt_i32_f32` (VOP1 `0x8` / VOP3 `0x188`); VOP1/VOP2 SDWA (encoding 249).
- SMEM dual offset (SGPR soffset + 21-bit imm) and variable-offset
  `s_buffer_load_dword` / `x2` / `x4` with imm constants registered for SPIR-V.
- `image_sample` dmasks including single-channel `0x2`/`0x4` and `0xb` (R+G+A).
- Gen5 extended NGS2 rack `max_voices` at option offset `+0x50` when option size
  ≥ `0xb0` (focused Audio tests).
- PS user SGPR window up to 32; CB blend1–7, BufferLoadFormatXyzw, and related
  register/shader contracts from earlier cycles.

**First strict fail on current HEAD (always re-capture):** none observed in the
latest Linux Release+Silent strict run through more than 2,300 presents.
`ds_read2_b32` is implemented and covered by focused parser/SPIR-V tests.
The next structured EXIT, host fault, or earlier regression on a fresh capture
becomes the process frontier.

**Visual residual (current frontier, not a process EXIT):** the horizontal
stripe corruption is absent after the GPU-owned RenderTexture layout and null
MRT discard-tail fixes, but opaque black rectangles remain around sprite and
prop bounds while HUD/UI and scene geometry continue drawing. The rectangles
move with scene content, so investigate the sprite/G-buffer writer and its
lighting consumer before VideoOut. The next discriminating evidence is a
same-draw comparison of sampled atlas alpha, MRT3 alpha immediately after the
writer, and the consumer threshold/output. End-of-frame RT dumps alone do not
identify the producer. Do not invent clears, alpha tests, or ThreadFlag signals.

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
  variable SBuffer; image load dmask `0x1`; image_sample dmasks 0x2/0x4/0xb;
  correctly sized compute LDS, `ds_write_b32`, `ds_read2_b32`, Workgroup
  `s_barrier`; null MRT discard tails; NGS2 extended max_voices.
- **First strict fail (re-capture on HEAD):** none observed through more than
  2,300 presents in the latest Linux Release+Silent strict run. The next
  structured EXIT or host fault is the process unit of work.
- **Visual residual:** horizontal stripes are absent, but opaque black
  rectangles remain around sprite/prop bounds. Capture sampled atlas alpha,
  MRT3 alpha immediately after its writer, and consumer threshold/output for
  the same draw before changing shader, blend, clear, or composite behavior.
- **Later symptom only:** `ThreadFlag` bit `0x1` (mode `0x21`, 40 ms) with no
  observed Set. Never fabricate the signal. EventFlag live-handle registry
  (garbage → ESRCH) is not Set. Trace the producer after earlier GPU/shader
  aborts are gone.

IMMEDIATE OBJECTIVE AND SUCCESS CONDITION

Advance the strict post-Play path to a **controllable, correctly rendered**
gameplay scene without diagnostics or fabricated success. Process survival and
HUD-only correctness are not playability.

If dual-strict shows a process EXIT, that is first priority (GpuMemory, shader,
format, HLE). If the process survives but the world is wrong, treat that as the
rendering frontier: identify the first bad producer at the bound sample,
writer MRT, or consumer/composite boundary with capture evidence — do not
paper over it with permissive flags.

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
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script        # runtime only, when a full build is unnecessary
_build_linux/fc_script '{kyty_run_tests()}' --gtest_filter='Suite.Test'
ctest --test-dir _build_linux --output-on-failure -R <IntegrationRegex>
```

macOS uses `_build_macos`; do not put macOS build steps in Linux checklists or
vice versa. Windows follows CI's generators — inspect the workflow, don't
invent commands.

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

Windows supports the generators and toolchains defined by the CMake files and CI.
Do not invent a Windows command from a Linux or macOS layout; inspect the active
workflow and generator first.

Build only the main script runtime when a full build is unnecessary:

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
