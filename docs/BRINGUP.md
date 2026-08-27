# Kyty Bring-Up Manual (extended)

This is the extended strict-frontier manual: investigation loop, phase gates,
frontier history, and the auxiliary-agent handoff template. Consult it when
advancing the strict PS5 compatibility frontier. Day-to-day rules and
invariants live in the repository root `AGENTS.md`, which takes precedence for
process weight (do not apply this full loop to ordinary code tasks).

Note: frontier facts below are a snapshot — always re-capture the first strict
fail on the current HEAD before acting on them.

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
   **delete probes before commit**. To surface the diagnostic dumps (shader
   dumps, command-processor traces) that are compiled as `KYTY_LOG_DEBUG`, set
   `PrintfLevel = Debug` in the config; `Error`/`Warn`/`Info`/`Debug` mirror the
   severity gates used by other emulators. The gate is a single relaxed atomic
   read, so a level that does not pass costs nothing.
6. **Compare working vs failing forms.** Same export with non-null address
   earlier in the run is ABI evidence; a post-Play null pair is a different
   contract to explain, not a free pass to invent addresses.
7. **Consult references only for names and patterns.** Public references may
   inform vocabulary and architecture, but every PS5-specific claim must
   reappear in a local capture or test. No incompatible code paste.

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
swapchain presentation, logos, a recognizable menu, Play / mode selection,
loading-card presentation, and controllable gameplay under **strict** flags
(no `KYTY_BRINGUP_*`). The latest Linux Release+Silent validation used bounded
diagnostic controller input to traverse the menus, held a direction for 180
presentations in gameplay, produced a healthy native capture, and reported no
runtime error. A reset 601-frame gameplay window reported 41.688 FPS with
p50/p95/p99 frame times of 27/35/40 ms and one frame above 50 ms. Diagnostic
input proves the runtime frontier and control path, but is not formal
playability acceptance.

Recent strict bring-up (evidence-backed, focused tests where noted) includes:

- A captured Gen5 compute metadata writer now has a fail-closed semantic path
  to the exact D32S8 HTILE incarnation it initializes. The classifier matches
  the decoded linear `v0` invocation-index, bounds guard, uniform source read,
  and one typed dword store per destination record; runtime guards additionally
  require exact descriptors, full dispatch coverage, immutable zero source and
  parameters, writable storage generations, and submission order. It neither
  skips the guest dispatch nor keys behavior by title, shader checksum, address,
  extent, or host GPU. One bounded strict run consumed the event once, selected
  `CLEAR` with depth zero plus the guest's stencil-zero clear on first use, then
  retained `LOAD`/reverse-Z writes. A temporal capture sequence showed coherent
  track and vehicle geometry across multiple cameras. This closes the missing-
  geometry depth-initialization producer for that observed route, but does not
  establish playability or correct color/material rendering. Independent review
  subsequently tightened the local-ID and combined D32S8 guards; the affected
  emulator and graphics integration targets build and the short integration
  contract passes, while that final hardening has not yet been re-run through
  the private strict route.
- The remaining visible defect is now downstream of coherent geometry: some
  surfaces are black, materials are washed out or over-saturated, and UI layers
  can remain visible across scene/camera changes. A same-scene HDR trace already
  places those pixels in the stable full-resolution producer before downsample,
  with defined `LOAD`, no CMASK fast clear, no target remap and no direct
  sample/attachment feedback. A later checksum-scoped strict draw trace joined
  the same format-122 producer class and found `dcc_enable=0`, a zero DCC base,
  disabled CMASK, `LOAD`, blend disabled, and the expected RGB write mask. This
  closes DCC/CMASK and immediate blend/write-mask as causes for that draw. Do not
  reopen vertex/NaN or depth-clear hypotheses for this symptom. The rebuilt
  translator-v34 module already lowers the sole `v_readfirstlane_b32` as a
  direct copy because its selected value is scalar-uniform. Its remaining
  `OpGroupNonUniformAny` guards a comparison whose operands are also uniform in
  the exact ISA, so the two host subgroup-32 halves take the same branch. This
  closes the observed wave64 operations as the color cause for this draw; the
  unsupported host wave64 width remains a general capability gap. The next
  bounded discriminator is the existing output-preserving final-MRT plus
  same-fence attachment probe, not subgroup emulation or a title-specific rule.
  Two bounded attempts could not select the newly observed material draw in a
  no-input route, and the input-gated attempt remained in loading until its
  watchdog; these are reproduction failures, not shader results. A following
  strict playable-regression run delivered and guest-sampled all three pad taps
  and sustained 38,726 presents without a host error, but never observed the
  required post-input loading transition and therefore correctly produced no
  acceptance capture. Playability and the stable car-selection checkpoint
  remain unproven on the current tree.
- One later present-addressed strict route reached `interactive` after two
  delivered guest-read taps, then exited with a write access violation at guest
  RIP `0x900a1c336` and address `0x350c8390`. Do not symbolize that RIP against
  the host PIE: offline loading proves it is eboot code, specifically a
  `vmovups` zero store to `r14+0x10`, while the destination belongs to a
  direct-memory `memfd` mapping. An identical bounded route subsequently mapped
  the destination read/write, produced two native captures, reported no runtime
  error, and ended only at its 150-second watchdog. The fault is therefore
  intermittent and not reproduced; it does not implicate host COW/string code
  or the capture request. Before changing memory semantics, capture the dirty-
  page tracker state, original protection token and native page protection for
  the exact faulting page from a signal-safe bounded record.
- Graphics pipelines now preserve the decoded Gen5 Z-clipping request instead
  of disabling Vulkan depth clipping unconditionally. Native depth-clip,
  optional core depth-clamp fallback, feature negotiation, and both cache-key
  bits are covered by the graphics integration contract. The first bounded
  strict validation remained on a UI-only `PLAY` frame after both scheduled
  inputs, so this is a corrected renderer contract, not evidence that the
  damaged 3D scene or playability has recovered.

- Gen5 hint-less direct-memory mappings use the guest user-address window with
  a monotonic placement cursor, and physical releases accept fully covered
  subranges or contiguous allocation spans. Focused split/coalescing tests pass;
  a strict 60-second bounded run ended only at its timeout with a 4.6 GiB host
  peak instead of the earlier 8 GiB cgroup termination at 36.7 seconds.
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
- Guest `EarlyZThenLateZ` with pixel kill must not lower to Vulkan
  `EarlyFragmentTests` alone: kill-enabled shaders use late depth commit so
  transparent quad pixels cannot write depth before `OpKill`; opaque early-Z
  shaders retain `EarlyFragmentTests` (`9b026e53`).
- SPIR-V structured loops for backward `S_BRANCH` (`OpLoopMerge` + body +
  continue / unreachable as required); do not regress CFG.
- `v_cvt_i32_f32` (VOP1 `0x8` / VOP3 `0x188`); VOP1/VOP2 SDWA (encoding 249).
- Indexed and automatic draws preserve the guest instance count; indexed
  indirect draws also preserve first instance and signed base vertex, including
  the guest-geometry depth/stencil-copy path. The base vertex is added exactly
  once to the existing register/shader-derived vertex offset before sizing
  vertex reach and issuing `vkCmdDrawIndexed`; out-of-range signed sums or
  negative effective vertex ranges skip the invalid draw with a bounded warning.
  Indirect instance count remains persistent for later direct draws. The current
  failing material trace used zero/one/zero for base/instance/first-instance, so
  this general contract correction is not evidence of a 3D recovery.
- A remaining cross-title indexed-draw defect is recorded at
  `GraphicsRenderDraw.cpp`: command processing preserves guest index type `2`
  and the indirect path sizes it as one byte per index, but the renderer only
  materializes types `0` and `1`. Type `2` therefore reaches the render path
  with a zero upload size and the default 16-bit Vulkan type. The general fix
  should widen the guest bytes to a supported 16-bit index buffer (including
  primitive-restart semantics) or use a proven enabled host 8-bit-index
  contract. The exact 3,564-index material draw used type `0`, so this defect
  cannot explain that capture and must not be mixed into its next experiment.
- Gen5 MUBUF/MTBUF address generation now preserves the RDNA2 `IDXEN+OFFEN`
  VADDR contract: lane 0 is the element index and lane 1 is the byte offset.
  The previous lowering exchanged those lanes, so indexed material-table loads
  could read the wrong in-range records without producing an OOB failure.
  `IDXEN`-only and `OFFEN`-only retain their scalar VADDR behavior. The existing
  addressing integration test covers all three forms, and translator version 32
  prevents reuse of stale modules. An exact regenerated material module validates
  and forms `index * stride + offset` from the corrected lanes. A later bounded
  strict route delivered both requested input edges and captured the intended
  PLAY-era checkpoint at present 8,095. The image still contained only small
  warm fragments over an otherwise black world (`healthy=false`, entropy
  `0.2896`, 176 bins), so the address correction is necessary shader semantics
  but is not sufficient to recover the vehicle or world. 3D recovery remains
  unproven.
- `kyty_graphics_diagnostics_integration` is inconsistent on current `HEAD`:
  `ShaderResourceAnalysis.cpp` classifies an unreferenced `DirectResource` as
  `UnusedMetadata`, while the integration requires `NoMatchingInstruction`.
  Restricting that classification to `MetadataSharp` made the integration pass,
  but the strict private workload then failed to reach present 8,000 within the
  existing 45-second gate and received no input. The experiment was reverted.
  Before changing global storage pruning, isolate which direct binding would be
  retained and prove its ownership, size, and real shader consumer.
- SMEM dual offset (SGPR soffset + 21-bit imm) and variable-offset
  `s_buffer_load_dword` / `x2` / `x4` with imm constants registered for SPIR-V.
- `s_buffer_load_dwordx8` still lacks that dual-offset/variable-SOFFSET
  lowering in `ShaderSpirvBuffer.cpp`; its current emitter requires a constant
  SOFFSET and omits `smem_imm_offset`. Port the proven x1/x2/x4 address
  formation before accepting an x8 shader that uses either field. The current
  exact Gen5 material instruction uses null SOFFSET and immediate zero, so this
  general defect does not explain that draw's black lighting.
- `image_sample` dmasks including single-channel `0x2`/`0x4` and `0xb` (R+G+A).
- Captured `ds_read2_b32` decodes its two dword-scaled offsets while preserving
  the byte-addressed `vaddr`, and reads through the same Workgroup storage as
  `ds_write_b32` (`990b9a40`).
- Gen5 extended NGS2 rack `max_voices` at option offset `+0x50` when option size
  ≥ `0xb0` (focused Audio tests).
- PS user SGPR window up to 32; CB blend1–7, BufferLoadFormatXyzw, and related
  register/shader contracts from earlier cycles.

**Last accepted strict frontier:** the earlier Linux Release+Silent validation
completed more than 24,000 presents without a structured failure;
`ds_read2_b32` remains implemented and covered by focused parser/SPIR-V tests.
The current 2026-08-23 worktree is not accepted at that frontier: two
separately created bounded artifacts record the same Vulkan device loss in
`vkQueuePresentKHR` after the first diagnostic input edge. Their guest logs are
byte-identical and contain no run configuration or submit join, so durable
evidence does not independently establish two differently configured launches.
A later manifest-backed strict diagnostic run with the submit trace enabled did
not reproduce device loss: one input edge was delivered, present/frame advanced
from roughly 11,000 to 14,500, and the run was terminated deliberately with no
last error. The older failures and this negative run are not configuration- or
binary-equivalent evidence, so neither a persistent failure nor a correction is
established. Do not report the historical 24,000-present run or the later
negative diagnostic as validation of the present worktree.

A later bounded Release+Silent strict run exercised translator version 32 and
regenerated the exact material shader with the corrected MUBUF VADDR order. It
reached present 8,003, delivered exactly one requested input edge, and advanced
to present 9,049 before presentation stopped advancing while frame processing
continued. No second edge or capture was requested. `last-error` stayed null,
no synchronization wait was blocked or suspended, and the sole process was
stopped deliberately. The cumulative diagnostics include command/draw/dispatch
processing calls lasting several seconds, but do not identify their producer or
connect the presentation stall causally to the address correction. This run is
integration-failure evidence only, not visual validation or acceptance.

A subsequent bounded run with the exact descriptor discriminator did not
repeat that stall. It reached present 10,513, delivered exactly two input edges
with 40-present deltas, and captured at present 10,617 before deliberate exit.
The material V# was linear (`stride=16`, `records=31`) with `ADD_TID=0`,
swizzle disabled, index stride zero, and `OOB_SELECT=0`, excluding those
descriptor modes for this shader. The capture reached the transmission-choice
screen and showed a coherent but mostly gray scene; its automated score was
low entropy (`healthy=false`, entropy 2.3809, 134 color bins, no directional
stripes). It is not the gameplay checkpoint, a visual A/B, or 3D acceptance.
`last-error` remained null and the only process was stopped immediately after
the bounded capture.

The next ordinary translator-32 route started the native agent before guest
initialization, waited for present 8,000, delivered exactly two `cross` edges
with 40-present deltas, and captured at present 8,095. It reached the PLAY-era
checkpoint but still showed only small warm fragments over a black world; the
score was `healthy=false`, entropy 0.2896, 176 color bins, with no stripe
classification. This is the first aligned visual evidence for the corrected
MUBUF module and proves that the lane-order correction alone does not restore
the vehicle or world. It does not invalidate the corrected RDNA2 address
contract.

The exact 41,910-index live-resolver discriminator now reuses the bounded
vertex probe rather than logging MUBUF activity. Its `VCPROB7` layout records
at most one first-executed embedded-MUBUF decision after the normal resolver
has chosen validity, slot, and byte offset; ordinary shader modules contain no
diagnostic SSBO or resolver-probe symbols. The focused graphics integration,
`fc_script` build, and independent isolation review pass. An initial strict
run was inconclusive, but a later bounded run completed the exact fence and
reported `c=0`: no embedded-MUBUF address setup executed. That is not an
invalid descriptor. The persisted module has distinct position, normal, and
UV attribute loads, so the exact draw uses semantic `Fetch` and does not
consume the appended stream SSBO through the live resolver. Close this seam;
do not change MUBUF or `DetectFetch` on this evidence.

The repeated gate delay exposed a separate bounded performance issue. Slow
records showed 525-to-849-KiB immutable vertex buffers just above the 512-KiB
snapshot ceiling. Snapshots now accept at most 1 MiB while retaining the
existing read-only/alias validation and authoritative fallback. D16 scratch
uses a distinct command-buffer-owned 16-MiB pool, so larger snapshots cannot
consume critical detile capacity; the shared pool remains 16 MiB. Focused
tests, graphics integration, build, and final independent review pass. The
first 45-second checkpoint improved from present 4,231 at about 8 FPS to 5,769
at 68.3 FPS, with roughly 2.15 GiB host memory and zero cgroup swap. Treat this
as workload evidence, not a general benchmark.

The completed run reached present 8,225 and delivered both scheduled taps, but
its native capture was the credits screen. The scorer's `stripey` result came
from horizontal white text, not the reported 3D tearing. It is not gameplay
evidence and does not validate the Z-clipping correction. The current frontier
is reproducing PLAY under the faster renderer without a third input before
another graphics semantic is changed.

The exact draw fence cannot replace the first input milestone: a single strict
no-input attempt reached present 32,329 at roughly 309 FPS without emitting
the event, then stopped cleanly with zero cgroup swap. The draw is downstream
of input. Do not repeat that wait or infer a shader failure from its absence.

Absolute presentation timing is also not a stable visual milestone after the
snapshot performance change. Two strict runs scheduled exactly two `cross`
taps at presents 8,000 and 8,080. Both delivered both taps without cancellation
and emitted the same finite 41,910-index clip/parameter aggregate between the
two inputs. One later frame at present 9,161 showed the coherent `PLAY` prompt
without the vehicle; the other advanced continuously through a 20-second watch
(273 presents, no blocked sync wait or structured error) but captured the logo
at present 8,982. The latter run slowed from about 23 to 11 FPS rather than
hanging, stayed below a 2-GiB observed cgroup current value with zero cgroup
swap, and was stopped deliberately. Do not classify a timed-out absolute
`wait-present` as a graphics deadlock while `watch` still observes progress,
and do not use either UI-only frame as 3D acceptance.

Capturing immediately after the exact resolver event with only the first
scheduled tap did not turn that event into a scene fence. The event arrived
443 ms after the tap began, with `c=0` and the same finite clip/`param0`
aggregate, but the native frame at present 8,169 was the credits screen with a
small central color fragment. Its score was `scene_ok=false`,
`gameplay_like=false` (entropy `0.5394`, 167 quantized colors). Therefore the
41,910-index draw executes in more than one UI phase or is otherwise not unique
enough to identify the vehicle scene. Keep the resolver exclusion, but stop
using this draw alone to trigger PLAY captures.

The same selected probe now appends six draw-bounded clip-population counters
without changing guest-visible state. Raw NaN/Inf positions are counted once;
finite `w <= 0` terminates before division; positive-W positions are classified
against raw `+/-w`, `0..w`, and `-w..w` bounds. The 37-word layout has a new
diagnostic identity, ordinary modules remain free of probe storage, maximum
counter serialization is bounded in a separate event, blocking and
nonblocking fence integration modes pass, `fc_script` builds at `-j1`, and an
independent correction review passes.

One strict one-edge run reported all 27,937 selected invocations in disjoint
classes: `wnp=25677`, `oxy=2260`, and zero Z-outside or inside vertices under
both clip conventions (`oz01=0 in01=0 ozn=0 inn=0`). `nf=0`, `last-error` was
null, the event ring did not overflow, cgroup memory peaked at 1,889,259,520
bytes, cgroup swap stayed zero, and the process was stopped deliberately. For
this occurrence, every position is rejected by W/XY clip before depth testing;
an incorrect depth attachment, stale clear, or HTILE/read-write identity cannot
be the reason this exact mesh occurrence is absent. This does not generalize to
the 3,564-index occurrence (previously inside and fragment-tested) or establish
PLAY/gameplay, because the 41,910-index selector is not a unique scene fence.
The retained same-material trace named the 39,120-index draw as the next
bounded correlation target after an off-screen 41,910 occurrence. A strict
one-edge run has now closed that target too: all 26,079 selected invocations
were rejected before depth (`wnp=24679`, `oxy=1400`, all four Z counters zero,
`nf=0`). The result arrived 560 ms after the input edge, the event ring had no
drops, and cgroup memory was about 1.74 GiB with zero swap. An immediate native
capture request timed out after 20 seconds and produced no file; the 90-second
service watchdog then stopped the run. Do not repeat that capture or claim a
visual scene correlation from it. Both large-draw results are exact-occurrence
depth exclusions, not evidence that the scene-wide transform is wrong or that
PLAY has been reached.

The probe can now preserve its one-shot reservation until an optional strict
decimal present threshold (`KYTY_VS_CLIP_PROBE_MIN_PRESENT`, with the analogous
pixel-input setting). With the setting absent it still arms immediately; a
malformed value fails closed. Before the threshold, matching draws remain
ordinary and do not acquire diagnostic shader or pipeline identities. Because
VS and PS share one lifecycle, selecting both waits for both thresholds (the
effective maximum) rather than letting the earlier stage consume the one-shot.
The focused graphics integration, unequal-threshold regression, and serial
`fc_script` build pass under the bounded zero-swap build envelope.

That gate produced the first phase-correlated result. One strict Silent run
scheduled exactly two `cross` edges at presents 8,000 and 8,080, selected the
41,910-index draw only from present 8,500, and completed after both inputs. Of
27,937 invocations, 19,217 had non-positive W, 6,722 positive-W invocations
were outside XY, and 1,998 were inside XY and Z under both conventions; no
position was nonfinite and no Z-only rejection occurred. The resolver still
reported no executed embedded-MUBUF address setup and `param0` stayed finite.
Both taps were delivered without cancellation, the event ring had no drops,
the last live cgroup snapshot peaked at 1,889,488,896 bytes with zero swap, and
the 90-second watchdog ended the process shortly after the event. `last-error`
was therefore unavailable after shutdown; do not infer it was null. Unlike the
earlier credits-phase occurrences, this later occurrence sends 1,998 vertices
past clip, so depth/clear/HTILE remains a live explanation for its missing
fragments.

That pixel-input discriminator is now complete on the same delayed draw and
two-input route. The late-test diagnostic observed 207,953 fragment-shader
invocations, all finite, with `input0.xy` ranges
`[-0.0910642,0.86913] x [0.0101471,0.958984]`. Thus the later occurrence has
real raster coverage and finite interpolation before depth; total clipping,
culling/no rasterization, and a nonfinite first varying are excluded for it.
The event arrived before the 95-second watchdog; the process then ended before
status/`last-error` could be queried, and the journal reported a rounded 2-GiB
peak. Do not claim `last-error=null` or a visual fix. The next bounded question
is whether any samples pass the unchanged depth/stencil test, versus failure
later in the pixel shader/color path.

The selected renderer probe now wraps an applicable exact draw in one
host-only Vulkan occlusion query. It resets before the render pass, begins/ends
inside the pass around every chunk of the selected draw, and reads availability
only after the same command-buffer fence. The final event is
`depth_stencil_probe` and reports explicit depth, stencil, and depth-bounds
applicability flags plus `precise=0 any_passed=<0|1>`; color-only matches report
`applicable=0` without issuing the query. Three separately registered focused
integration modes cover a real empty render-pass query/readback, depth-bounds
alone, and color-only handling; the original lifecycle gate remains separate.

A strict VS-only run retained the guest pixel shader's normal early tests. Its
prototype event returned a raw zero, which the corrected final contract
expresses only as `depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0`, for the
delayed 41,910-index occurrence. Its companion aggregate had 7,283 vertices inside
the clip volume, `nf=0`, and finite positive `z/w`; the retained material trace
also has stencil disabled, GEQUAL depth, depth writes, LOAD, clear value zero,
and no explicit depth clear. Both inputs were delivered, `last-error` was null,
the ring had no drops, cgroup peak was 2,037,301,248 bytes, swap stayed zero,
and the process was stopped cleanly. Thus all covered samples for this exact
occurrence are rejected by depth, not stencil or pre-depth geometry loss.
This does not yet prove the attachment is wrong: earlier GEQUAL world draws may
legitimately occlude it. One attempt to query the earlier 15,366-index draw with
the same present-8,500 gate emitted no selected event before the 95-second
watchdog; the retained sequence places that draw earlier, so the absence is a
timing exclusion, not shader/query evidence. Do not repeat that threshold or
change clear, HTILE, compare, or identity from the 41,910 result alone.

A single corrected control lowered only that gate to present 8,090 while
keeping the same two inputs. The 15,366-index draw completed 1,452 ms after the
second input with `depth=1 stencil=0 bounds=0 ready=1 any_passed=1`. Its 9,151
invocations were finite: none had non-positive W, 1,476 were outside XY, and
7,675 were inside XY and Z under both conventions. `param0` was finite and the
resolver remained `c=0`. Both taps were delivered without cancellation,
`last-error` was null, the ring had no drops, cgroup memory peaked at
2,149,572,608 bytes with zero swap, and the process was stopped cleanly. Thus
the shared depth image and GEQUAL sequence can pass substantial geometry; the
41,910 zero is local to its ordering/coverage and may be legitimate occlusion.
Do not change attachment identity, clear, HTILE, or compare from that draw.
Return to the material/color path on the passing 15,366 draw.

The material path now has a separate output-preserving pixel sample probe.
`KYTY_PS_SAMPLE_PROBE=<checksum>:@<instruction-ordinal>` selects one exact
`ImageSampleB` together with the existing exact draw and minimum-present
selectors. The selected PS retains its normal early fragment tests and MRT
dataflow; a host-only one-shot SSBO records RGBA count, nonfinite observations,
and ordered extrema after the sample and before its destination stores. The
absolute instruction ordinal, probe kind, descriptor set, and raw-layout
revision are part of shader/module identity. The focused graphics integration
validates the 47-word layout, fail-closed parser, absolute-ordinal contract,
SPIR-V ordering, early tests, and binary validity.

One strict Silent attempt selected ordinal 24 of the 15,366-index shader and
reported 737,427 finite post-depth observations with nonzero ranges
`R=0.611765..0.623529`, `G=0.184314`, `B=0.0941176..0.105882`, and `A=1`;
the paired fixed-test query reported `any_passed=1`. However, agent timing
reached present 8,689 before the first input, and the probe event occurred
1.1 seconds after that first edge but before the second edge. This excludes a
globally null/nonfinite base sample for that first-edge occurrence only. It is
not evidence for the intended post-two-input PLAY occurrence and is not a 3D
advance. Both taps eventually delivered, `last-error` was null, the ring had
no drops, memory peaked at 2,239,434,752 bytes with zero cgroup swap, and the
service was stopped. Do not change sample, LOD, tiling, or color arithmetic
from this timing-misaligned result; a future discriminator must gate the same
probe strictly after both input edges.

One timing-corrected retry moved the reservation gate to present 9,200 and
placed the entire `wait 8000 -> CROSS -> +80 -> CROSS -> event` route in one
local command. Its cold diagnostic cache reached only present 4,902 in the
unchanged 70-second gate, so the command timed out before sending either input
and no sample event was possible. The live process was still interactive at
8.925 FPS with `last-error=null`, no ring drops, no pending/delivered taps, a
2,149,572,608-byte cgroup peak, and zero swap; it was stopped immediately.
This is pre-gate performance evidence only. Do not lengthen or repeat the same
cold route. A later material discriminator must reuse a proven warm diagnostic
cache or otherwise reach the two-edge checkpoint without changing the render
contract.

The native controller scheduler now provides that reproducible route: one
request commits exactly two `cross` taps at presents 8,000 and 8,080 before the
first frame. A warm strict run using that route showed why minimum present alone
was insufficient. The first exact `indexed:15366` match after present 8,090
completed after both scheduled starts but reported `sn=0` together with
`any_passed=0`; it was a depth-rejected occurrence, not a null texture sample.
The sample probe therefore accepts the optional zero-based
`KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL=N`. Skipped exact matches stay ordinary and
do not allocate diagnostic Vulkan resources or consume the process one-shot;
invalid paired PS selection clears its VS peer. Parser/lifecycle integration,
a fresh-process Vulkan skip/select path, emitted `m=N` provenance, serial
`fc_script`, and independent review are green.

One strict warm run changed only that selector to `N=1`. Both scheduled inputs
were delivered, none was cancelled, and the second exact occurrence emitted:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 ord=24
sn=1618 snf=0 sfin=1
r=0.623529:0.623529 g=0.184314:0.184314
b=0.105882:0.105882 a=1:1 m=1
```

Its paired fixed-test query reported `depth=1`, `stencil=0`,
`any_passed=1 m=1`. `last-error` was null, the event ring had no drops, the
service used a rounded 1.6 GiB peak with cgroup swap disabled, and it was
stopped immediately without a capture. This excludes a null, nonfinite, or
black base `ImageSampleB` result for the contributing occurrence. Do not change
base image decode, sampler, LOD, tiling, attachment depth, clear, or HTILE from
this evidence. It is not visual/gameplay acceptance: final shader export,
blend/color attachment, and target lifetime remained unproven at that point.

The next bounded discriminator now observes one active MRT0 export by absolute
instruction ordinal immediately after final RGBA assembly and before the
unchanged `OpStore`. `KYTY_PS_MRT_PROBE` is mutually exclusive with coordinate,
sample-result, and fragment-tap diagnostics; it rejects inactive color-output
modes, zero-channel exports, wrong targets, and wrong opcodes before reserving
the process one-shot. It retains guest early fragment tests and reuses the
bounded 51-word aggregate under a distinct module identity and paired
`ps_mrt_probe` / `ps_mrt_coverage`
event. The existing focused integration validates source and binary SPIR-V,
output-store preservation, fail-closed selection, and the `m=1` skip/select
lifecycle.

One strict warm run kept the same exact draw, 8,090 present threshold,
zero-based occurrence `m=1`, and scheduled inputs at presents 8,000 and 8,080.
Both inputs delivered without cancellation. At present 8,090 the selected
final MRT0 export emitted:

```text
mrt=0 ord=229 on=408 onf=0 ofin=1
r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1
```

`last-error` was null, the event ring had no drops, cgroup swap was disabled,
and the service was stopped immediately without a capture. This proves that
the contributing occurrence reaches its unchanged MRT0 store with a finite,
nonblack assembled color. Combined with the preceding finite base sample and
passing fixed-test query, the live branch is now downstream of pixel-shader
material arithmetic. Do not change sample decode, sampler, LOD, tiling, depth,
clear, HTILE, or MRT export arithmetic from this evidence. It remains
diagnostic rather than visual/gameplay acceptance: blend state, color write
mask/attachment identity and layout, render-target lifetime, and later
composition remain unproven and are the next causal seam.

The exact second MRT occurrence is now disproven as a PLAY/gameplay scene
fence. A later output-preserving run kept the same selector and two scheduled
inputs while adding only host `FragCoord` extrema to the selected MRT export.
After both taps started, the paired events reported:

```text
mrt=0 ord=229 on=129111 onf=0 ofin=1
r=0.452881:1.33496 g=0.133789:0.381104
b=0.0769043:0.191284 a=1:1 m=1
x=427.5:1479.5 y=618.5:1079.5 cfin=1 m=1
```

The fixed-test query also reported `any_passed=1 m=1`. The immediate native
capture at present 8,212 showed the logo/PLAY prompt without the vehicle and
scored `low_entropy` (`0.2047`, 149 bins). Both taps delivered, none was
cancelled, the event ring had no drops, `last-error` was null, memory peaked at
a rounded 1.7 GiB with cgroup swap disabled, and the runtime was stopped
deliberately. Thus this exact draw executes broadly across the lower/right
host framebuffer and reaches a finite, nonblack unchanged MRT export; its
visible vehicle contribution is lost after that observation point. This rules
out zero/tiny raster coverage and total depth rejection for the selected draw,
but does not prove correct primitive placement or identify the later loss.
Investigate blend/write mask, bound attachment identity/view/layout, obsolete
clear or depth/HTILE state, and read/write lifetime through composition. Do not
use this occurrence as a semantic scene fence and do not claim a 3D advance.

The optional host-only post-blend readback now copies one eligible color
attachment after its render pass and maps it only after the owning command
buffer fence. It accepts a single-sampled transfer-source image in one of the
bounded known formats, caps the copy at 64 MiB, restores the tracked image
layout, and reports packed RGB occupancy plus a raw-byte hash. Its private
staging allocation is fail-closed and nonfatal; it does not use the renderer's
fatal general allocator.

The first strict use of that discriminator did **not** reproduce the preceding
positive occurrence. With the same checksum/draw/export selector, `m=1`, and
scheduled inputs, the selected draw instead reported `on=0`, `cfin=0`, and
`any_passed=0`. The completed 1920x1080 B10G11R11 attachment copy was uniformly
RGB-zero (`nz=0`, `in=0`), which is expected for a draw with no surviving
fragment invocation and therefore says nothing about blend retention. Both
inputs delivered without cancellation, `last-error` was null, the ring had no
drops, the cgroup peaked at a rounded 1.7 GiB with swap disabled, and the
immediate capture was the main menu rather than the failing 3D checkpoint.
This independently reconfirms that minimum-present plus ordinal is not a
stable scene/occurrence identity. Do not repeat that route or interpret the
zero attachment as a renderer fix or failure.

Attachment observation now boundedly re-arms after an empty fenced attempt.
It discards at most four empty exact matches, carries the already-consumed
match ordinal across each retry and context teardown, releases each staging
buffer only after its owning fence, suppresses intermediate events, and emits
the discarded count as `r=N`. The fifth attempt is terminal even when empty,
so the opt-in diagnostic cannot loop indefinitely.

One subsequent strict run reached the terminal `r=4` attempt with finite MRT
coverage (`b=1`) and read back the 1920x1080 B10G11R11 attachment as 82,791
RGB-nonzero pixels overall and 55,937 inside that same attempt's host
coverage box. This excludes a uniformly black immediate attachment for the
selected contributing attempt. It does **not** yet prove that those pixels
were written by that draw: the readback has no pre-draw snapshot and can count
content retained from earlier writers. The native client recovered the event,
but the 120-second service cap stopped the process before a capture or final
input/status query; the service used a rounded 1.8 GiB peak with swap disabled
and wrote no crash report. The next discriminator is a bounded before/after
attachment delta or an exact downstream overwrite/clear identity, not another
shader/sample probe or guessed ordinal.

That before/after boundary is now implemented without changing guest output.
For `LOAD` color attachments with defined contents, the probe copies the same
selected image immediately before its render pass and again after it, using two
32 MiB-capped coherent buffers (64 MiB total). Both copies stay on the owning
command buffer and are mapped only after its fence. `CLEAR`/`DONT_CARE` and
undefined initial state fail closed for the delta while preserving the existing
post-only event. The copy helper now restores
`COLOR_ATTACHMENT_OPTIMAL` with color-attachment read **and** write access;
the read scope is required for the implicit attachment load. The focused
contract and ordinal integrations, serial emulator build, and an independent
barrier/lifetime review passed with zero swap.

One strict Silent run then delivered exactly the two scheduled input edges at
presents 8,000 and 8,080 and selected the contributing 15,366-index occurrence
without a retry. Its exact-fence events were:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
on=120494 onf=0 ofin=1 r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
cfin=1 x=537.5:929.5 y=720.5:1079.5 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
f=b10g11r11 e=1920x1080 nz=122244 b=1 in=96923
h=e728d902f16790b5 ok=1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
l=0 d=94978 b=1 in=94978 up=94804 dn=0 m=1 r=0
cs=0a0005c0ef41d630 k=i n=15366 s=2 applicable=1
depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=1 m=1
```

Thus the selected draw changes 94,978 RGB pixels inside its own coverage after
a defined `LOAD`; 94,804 change from RGB-zero to nonzero and none change in the
opposite direction. Together with `any_passed=1`, this excludes total depth
rejection, a discarded render-pass load, a uniformly black immediate color
attachment, and a completely suppressed color write for this occurrence. The
native frame still showed only the logo/PLAY prompt, no vehicle or world, and
scored `low_entropy` (entropy 0.2083, 171 bins), so there is no visual advance.
The process was stopped deliberately at present 8,484 with `last-error` null;
the cgroup peaked at 1,937.6 MiB with zero swap. The next discriminator is now
the first later clear, overwrite, alias, resolve, or consumer of this exact
image before present. Do not change depth compare, MRT export arithmetic, or
the immediate blend/write path from this result.

The existing render-target lifetime trace now accepts an optional strict color
guest-address selector and includes `guest_count` in `WRITE` records. When the
color selector is active it no longer arms or emits depth lifetime events; an
unfiltered attempt had produced 128 unrelated depth records and reduced the
runtime to roughly 8 FPS. The filtered color path restores bounded diagnostic
cost and preserves later guest-or-host identity matching for rematerialization.
A subsequent audit also found that descriptor-sample auto-promotion could arm
an unrelated RT despite the selector; that promotion now requires the same
address match. The serial build and focused contract integration stayed green.

Two bounded attempts exposed a process-lifetime identity problem rather than a
renderer result. The first recovered the selected target as guest address
`0x4f250000`. It showed a first `CLEAR` writer, then `LOAD` writers, and a later
sample by the 1920x1080-to-960x540 downsample. That occurrence itself was the
fifth empty probe match (`r=4`, no fixed-test pass), so its sequence cannot be
used to explain the positive draw. In the next process the active full-size HDR
target was instead at `0x51b50000`; the stale address selector could not arm the
selected target, and the 15,366-index draw never appeared in that route. Both
scheduled inputs delivered in each attempt, both services were stopped
deliberately, cgroup peaks stayed below 2.0 GiB with zero swap, and there is no
visual evidence from either attempt.

This closes a cross-process guest address as a stable selector. Do not repeat a
filtered run using an address learned from another process. The next minimal
diagnostic must arm the lifetime target dynamically from the exact MRT probe
reservation inside the same process, then correlate its `guest_count=15366`
`WRITE` with the first later `WRITE`, `SAMPLE`, or `RESOLVE`.

That same-process arming is now available through the lifetime trace's explicit
MRT-probe mode. A successful exact FinalMrtResult reservation publishes its
current guest address and host allocation before descriptor binding and before
`TraceRenderTargetLifetimeDraw`; generic color arming, depth tracing, and sample
auto-promotion are disabled in this mode. A retry that selects a new ping-pong
image emits `PROBE_REMAP`. Mixed depth/color selectors fail closed.

One bounded run demonstrated the ordering, although all five selected probe
attempts were depth-rejected and the terminal result was again empty. The first
attempt emitted `PROBE_ARM` for a 1920x1080 format-122 image followed immediately
by the exact indexed `guest_count=15366`, pixel-shader `210005b0766a27a5`
`WRITE`. All later writers used `LOAD`; no later clear was observed. The first
consumer was the `210001e03375e575` downsample into 960x540, followed by the
existing derived chain and the `210006800bf364a9` full-resolution compositor.
The next frame selected the other 1920x1080 ping-pong image and correctly
emitted `PROBE_REMAP` before the same exact draw/consumer order.

This proves that the dynamic trace follows the exact target and that the normal
path is mesh writes -> downsample -> compositor, not an intervening clear. It
does **not** close a later overwrite or consumer defect for the prior positive
94,978-pixel occurrence because this run's selected draws had `any_passed=0`.
Do not change render-pass load ops or remove later guest draws from this empty
sequence. The next useful run is the same bounded dynamic trace only when the
probe also reports `d>0`, `in>0`, and `any_passed=1`; then inspect the first
later significant write and downsample result from that exact attempt.

A draw-scoped fragment-tap retry for the 3,564-index material was allowed one
unchanged 60-second gate after the diagnostic shader and pipelines had already
been cached. It completed both requested inputs and captured at present 8,093
with no structured error, but input timing placed the workload in the main menu
rather than the PLAY-era vehicle draw. The menu capture scored
`hot_corruption` (entropy 1.7525, 78 bins) and cannot establish whether the
vehicle's `param4/location4` reached the pixel shader. Do not treat the absence
of the tap color in that frame as a producer failure, and do not repeat the
same route merely to force the desired checkpoint.

Offline extraction of the persisted translator-32 modules then narrowed the
post-HDR path. The downsample uses one `ImageSampleImplicitLod` with Bias; the
active compositor uses two equivalent HDR samples plus four explicit LOD-zero
LUT samples. Their `{bias,x,y}` and LOD-zero lowering agrees with the current
Gen5 image contract, and all extracted modules pass SPIR-V validation. The
compositor also has a distinct no-input variant whose implicit sample
coordinates are `(0,0)`; the active variant instead consumes a smooth
`Location 0` varying. `ShaderGetIdPS` already separates those interfaces, so a
cache collision is not established. The bounded render-target lifetime sample
trace now records `input_num`, the first four interpolator settings, source and
destination host extents/formats, view, and layouts. It remains opt-in and does
no image readback. One strict two-edge cycle captured the first downsample and
the compositor with a real destination: both used `input_num=1`, interpolator
zero `0x00000000`, and the exact 1920x1080 HDR image. The downsample wrote
960x540 and the compositor wrote 1920x1080, all in host format 122 with view
zero. This excludes the no-input constant-coordinate variant, a cache-interface
collision, and an attachment identity/view/format/extent mismatch for that
cycle. It does not inspect interpolated coordinates, scale arithmetic, the
sample result, blend, or the compositor output. Its native capture remained
black apart from the title/HUD and a thin horizontal fragment
(`healthy=false`, entropy `0.2337`, 164 bins).

The lifetime trace can now start at a minimum presentation without depending
on the broader material trace, preserves exact guest-plus-host identities for
derived render targets, reserves a consumer event, and promotes only an exact
render target sampled into a distinct smaller destination. A later strict
Silent no-input run reached present 8,017 with no structured error but executed
neither known downsample nor compositor shader; it produced no sample or
derived-chain event. It is not evidence for or against compositor identity or
content. Do not repeat that no-input route. The next bounded integration must
use the established two-edge route and stop after the first complete derived
cycle.

A subsequent cgroup-bounded strict Silent two-edge cycle did exercise one
complete derived edge. The half-resolution producer sampled the exact
full-resolution HDR guest-plus-host image and wrote a 960x540 format-122 target;
the immediate consumer sampled that exact 960x540 identity in the same cycle.
Offline inspection identifies this consumer as a positive-weight nine-tap
horizontal blur rather than the full-resolution compositor. This closes a
missing bind, guest-upload substitution, and host-image identity mismatch at
that edge, but not the sampled texel contents, blur destination, or a later
overwrite. The native capture at present 9,840 still showed coherent PLAY UI
over a black world (`healthy=false`, entropy 0.2089, 173 bins), with no
structured error or blocked synchronization wait. Do not repeat the lifetime
trace for the same identity question. The next bounded integration should tap
the 3,564-index material's raw base sample at ordinal 24 and compare it with the
retained draw-scoped `param4/location4` coverage before testing final material
channels or changing renderer semantics.

Two attempts to collect that raw-base discriminator produced no material
evidence. The first split the agent route across host calls and let the hot
pre-input loop overshoot into the credits screen before the input sequence
completed. The one allowed warm retry reached present 8,000 and delivered the
first edge at 8,006, but its local route driver aborted after misreading the
valid JSON value `tap_pending=false` as a failed predicate; it sent no second
edge and requested no capture. Both runtimes were stopped cleanly. A future
attempt must use one atomic local route and non-predicate boolean parsing; do
not lengthen the gate or use either failed attempt as shader evidence.

The corrected atomic route subsequently completed both delivered edges and
both 40-present deltas, then captured the draw-scoped raw four-channel base
sample at present 8,098. It exposed additional nonblack material fragments but
still no coherent vehicle or world (`healthy=false`, entropy 0.3657, 183 bins),
with no structured error, blocked synchronization wait, or excessive resident
memory. A retained exact trace contains one 3,564-index instance in its bounded
window, but the older `attr3` image is eleven presents earlier and visualizes
only one scalar channel. Their low mask overlap cannot establish incorrect UV
or geometry coverage.

The remaining parent-differential candidate was the BC mip tail: this material
declares eleven 1024x1024 BC1 levels while the current host policy stops at 4x4.
Vulkan permits complete 2x2/1x1 compressed mips at a subresource edge. A
test-first experiment preserved all guest levels and passed the existing
focused mip contracts, but its strict runtime did not reach present 8,000
inside the unchanged 70-second gate. It remained healthy near present 5,843
yet fell to roughly 4.5 FPS as per-frame texture upload/recreation work grew to
thousands of calls and tens of MiB; no input or capture occurred. The experiment
and test were restored and both binaries rebuilt. Record the truncated tail as
a general correctness gap, not the demonstrated 3D producer. Before retaining
all levels, capture the effective material LOD and eliminate the newly exposed
texture lifetime/upload churn; do not repeat the same toggle.

The next bounded attempt selected only compositor checksum
`0x210006800bf364a9` after present 8,080 to inspect its dynamic storage values.
The workload stalled before input at present 6,264 for about 154 seconds;
`phase=stalled`, `last-error=null`, and the trace filter had not armed. The
process was stopped deliberately without retrying or extending the timeout.
This is liveness-failure evidence only and provides no compositor constants or
visual A/B. A deterministic bounded route still has to distinguish consumer
coordinates/sample arithmetic, dynamic storage values, compositor output, and
a downstream overwrite; do not change Bias, LOD, SLOAD, interpolation, or depth
speculatively.

A later pre-input checksum-scoped trace did capture all 28 floats read by that
compositor variant. The 112-byte range was fully materialized and in bounds;
its active values were coherent with neutral sampling and a 1024x32 packed LUT
(`bias=0`, texel steps `1/1024` and `1/32`, dimension-minus-one `31`, unit
gain, and the optional radial mask disabled). None of those coefficients can
turn every finite nonnegative HDR sample black by itself. This draw targeted a
different pre-input host format and address than the failing gameplay-era
consumer, so it excludes only an obvious coefficient/range defect in that
variant. It does not prove the gameplay compositor input, sample results, or
output.

The pixel sample probe now has an opt-in sparse observation mode. It elects
one host invocation per fragment subgroup before the existing aggregate, so a
full-resolution diagnostic no longer executes the aggregate atomics for every
fragment. Sparse modules have a distinct diagnostic identity, require fragment
stage basic subgroup support, and fall back to the unchanged ordinary draw
before lifecycle reservation when that host capability is absent. Malformed
configuration fails closed. The existing graphics integration validates the
generated structured SPIR-V, unsupported-host fallback, exact fence lifecycle,
and explicit `sparse=1` event provenance; serial builds and independent review
passed.

One strict two-edge run applied the sparse probe to the active full-resolution
compositor's first HDR sample. It recorded 66,561 elected subgroup lanes, zero
nonfinite values, `R/G=0..0.215088`, `B=0..0.0562439`, and `A=1`, with both
scheduled inputs delivered, no cancellations, no ring drops, and no structured
error. The bounded service peaked at 2,021,982,208 bytes with zero swap and was
stopped immediately. The native capture at present 8,183 showed only the
logo/PLAY screen and scored `low_entropy` (`0.2046`, 150 bins). Therefore this
excludes a globally black or nonfinite compositor input for that exact title
screen occurrence only. It is not gameplay, visual acceptance, or evidence
that the later compositor output/attachment is correct. Do not cite it as a 3D
advance or repeat the same route merely to obtain a later screenshot.

The Gen5 HLE `EVENT_WRITE` encoder previously discarded every non-null address
and always emitted the two-dword form. Addressed event type `0x39` now
emit the evidenced four-dword packet with event index one and an aligned 64-bit
destination; the PM4 parser consumes either form. Until native host occlusion
queries exist, event `0x39` publishes ready, monotonically increasing begin/end
counter values to the sixteen interleaved DB pairs after validating the guest
range. The existing graphics diagnostics integration executes the addressed
packet through the real PM4 parser, checks all sixteen DB pairs, monotonic
increment, following-packet alignment, and rejection of a misaligned target;
it also protects the ordinary short encoder boundary. One bounded strict
two-edge run reached present
11,752 with no structured error or blocked wait, but its capture remained
nearly black (`healthy=false`, entropy `0.2024`, 158 bins). No retained counter
proves that this workload executed event `0x39`, so this is a general contract
correction, not evidence that occlusion caused or fixed the missing 3D. Event
`0x38` remains on the short form until its addressed semantics have independent
evidence and a matching consumer.

Two general shader-contract defects remain recorded but are not established as
the current visual producer. First, pixel analysis calculates an exact
`required_subgroup_size` for shaders using guest-wave operations, while graphics
pipeline creation does not attach a
`VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT`; device creation also
does not enable the subgroup-size-control feature. The current Intel host
reports a default subgroup size of 32 and a controllable range of 8 through 32,
so this omission does not demonstrate a mismatch for the observed material.
A general correction must query and enable the feature, include the required
width in pipeline creation/identity, and reject a guest width the host cannot
represent rather than silently falling back. Second, `ShaderParseEXP.cpp`
retains the guest `EN` nibble for parameter exports, but the `ParamN` SPIR-V
lowering writes all four source channels. The shader probe now includes that
already-decoded nibble in its opt-in instruction dump. A bounded strict probe
resolved the exact VS masks as `param0=0x3`, `param1=0x7`, `param2=0x7`,
`param3=0x1`, and `param4=0x7`; `Pos0` remained `0xf`. Thus all three
`param4.xyz` channels that feed the observed material normal/color path are
enabled and only its unused `w` source is outside the guest mask. Partial
parameter lowering remains a general contract gap, but it does not explain the
failing draw's dark XYZ or missing position geometry. Do not change export
behavior without a red partial-mask consumer contract; neither defect is
strict 3D acceptance.

A narrower translator correction now avoids native lane exchange for
`V_READFIRSTLANE_B32` only when a same-basic-block reaching-definition proof
shows a wave-uniform source and unchanged initial EXEC. The proof rejects
vector VCC writers, every supported implicit `VCMPX`/`S*SAVEEXEC` EXEC writer,
multi-register VGPR overwrites, control-flow boundaries, and incoming edges
that can bypass a producer; all other cases retain the original subgroup
ballot/broadcast path. On the exercised material module this removed the
ballot/count/find-first/broadcast sequence, and the rebuilt SPIR-V validated.
It did not remove the module's subgroup requirement: the selected module still
contains a VCC-wide branch reduction and a lane-ID-addressed buffer operation,
so the host trace correctly remained `required_subgroup=64`. A strict bounded
route delivered exactly the two scheduled input edges and captured a coherent
main menu, not gameplay. This is a verified general translation improvement,
not evidence that wave width caused or fixed the missing 3D.

The mask probe was produced before the diagnostic input gate. Its bounded
`wait-present --min 8000` attempt timed out at present 6,094 after 70 seconds;
`last-error` was null, presentation was still advancing near 9.7 FPS, and
`sync-waits` reported no blocked wait. No input or capture was requested, and
the sole process was stopped immediately after reading the probe. This run is
parser evidence only, not a visual result or a liveness regression.

**Host submission failure contract:** the current worktree publishes and
reuses a command buffer only after successful submit and fence completion.
Timeout, not-ready, device-loss, and success fixtures cover the pure policy;
blocking waits are bounded and no incomplete fence drains callbacks, resets the
buffer, or clears its in-flight state. `KYTY_SUBMIT_FAULT_TRACE=1` adds a
CPU-only eight-attempt trail across every existing submit and emits it once on
the first observed device loss. A submit failure identifies that exact host
attempt; a loss first observed by fence, acquire, or present leaves only a
bounded predecessor set. It does not identify a raw Vulkan command and is not a
visual fix. One guest run has now exercised the enabled no-loss path; because no
loss occurred, the one-shot fault emission and submit join remain
runtime-unexercised.

**Linked-buffer lifetime frontier:** the same post-input snapshot grew from 15
live GPU objects to 5,038, including 4,659 live `StorageBuffer` objects and
3,916 `NewLinked` creations, while instantaneous FPS fell below 4. Static
tracing confirms that distinct overlapping storage ranges create distinct
linked backings and that periodic retirement skips every object with alias
links. This is evidence of an unbounded-lifetime mechanism, not yet proof that
all observed objects are stale, that it caused the earlier device loss, or that
it produces the missing geometry. Any correction must preserve submission
dependencies, GPU-owned surface state, and writable-buffer publication.
The current worktree adds anonymous aggregate `NewLinked` topology categories
for buffer-only read-only, surface-connected, mutable/other, and truncated
components. Classification is limited to 64 unique nodes and 128 examined
parent/link edges, and every `NewLinked` event contributes to exactly one
category. One later strict, 150-second cgroup-bounded run reset this window
before submitting one diagnostic `cross`. Over the following 107.7 seconds it
recorded 7,445 linked storage creations: 1,767 buffer-only read-only, one
mutable/other, no proven surface connection, and 5,677 traversal-truncated.
Live storage objects reached 8,145, live objects overall reached 8,524, and
instantaneous FPS was 6.754. Host memory peaked at 2,156,904,448 bytes with no
swap before the timeout terminated the process. The dominant truncated result
means the run does not prove those components surface-free; it rules out an
unconditional linked-object retirement and does not yet establish a safe or
effective lifetime correction.
`FrameDone` now has one conservative correction for the proven subset: it may
retire a component containing storage only after a complete current preflight
shows no more than 64 SB/VB/IB nodes and 128 links, all read-only, `Common`, at
least 120 frames old, free of bound depth metadata, and with completed
submission dependencies. It frees the complete component or nothing, and a
2,048-unit global scan budget bounds each retirement pass. Host tests prove
successful whole-component retirement with a real storage write-back callback,
and prove that truncated and surface-connected components remain intact. No
visual acceptance has exercised this correction. A later single strict run did
exercise it: before input, one linked storage creation had one logical free and
five live storage objects. After resetting the window and submitting one
diagnostic `cross`, 57.752 seconds produced 8,135 linked storage creations but
only 102 logical frees; storage live count reached 8,902, total live objects
9,249, and instantaneous FPS fell to 2.723. Of those links, 6,320 were
traversal-truncated and 1,815 buffer-only read-only. The bounded watch still
observed frame/present progress and no last error, while cgroup memory peaked at
2,185,240,576 bytes with zero swap before timeout exit 124. This falsifies
fixed-size whole-component retirement as a sufficient containment mechanism;
it does not prove that lifetime growth causes the 3D failure.

The current host-only follow-up routes safe read-only buffer snapshots up to
512 KiB through the command-buffer-owned transient pool instead of creating a
persistent alias. The pool remains capped at 16 MiB, reserves 1 MiB for
mandatory UBO/scratch uploads, clears the unused tail of reused slabs, and keys
storage/UBO descriptors by both backing identity and logical range. Writable,
surface-connected, unallocated, oversized, and truncated overlap states retain
the authoritative persistent path. One subsequent 105-second strict diagnostic
run reset the counters before its first input edge. After 58.497 seconds and two
bounded input edges it had created only 18 linked storage objects, retained 30
live storage objects and 116 live GPU objects overall; the previous comparable
post-input window created 8,135 linked storage objects and retained 8,902
storage objects and 9,249 objects overall. The sampled cgroup peak after the
first transition was 2,149,568,512 bytes with zero swap, and timeout ended the
sole process with exit 124. Native captures showed a coherent credits screen
and main menu with no directional stripes, but the capture gate correctly
classified both as non-gameplay scenes. This establishes containment on that
observed route, not correct PLAY geometry, long-session memory stability, or
playability.

**Visual frontier (not yet playability acceptance):** horizontal stripes and
opaque black sprite/prop rectangles are absent after the RenderTexture layout,
null MRT discard-tail, and pixel-kill late-depth fixes. The rectangle producer
was Vulkan `EarlyFragmentTests` committing depth before an existing `OpKill`
in a guest `EarlyZThenLateZ` shader. A gameplay-era native discovery capture
shows coherent background, props, character, lighting, and transparency.
The latest strict diagnostic route additionally exercised sustained directional
movement and stable presentation with healthy output. Formal acceptance still
requires a repeatable non-diagnostic controller run, an action beyond movement,
and validation-clean output.

The current Gen5 missing-geometry investigation has also narrowed one material
path. A selected VS exported finite clip position and finite `PARAM0.xy`; a
diagnostic-only late-depth PS variant then observed 167 finite input-zero
fragments whose aggregate extrema matched that producer. Ordinary shaders and
selected shaders with any retained guest side effect keep their guest
`EarlyFragmentTests`; only the cache-separated, statically side-effect-free
host probe executes before the ordinary depth test. Thus the earlier zero PS
count was early-depth rejection, while the new aggregate excludes only zero,
grossly out-of-range, and non-finite input for this occurrence. It does not
prove per-fragment correspondence, derivatives, or exact interpolation. The
selected draw occupies only a thin NDC band and is not evidence for the full
missing vehicle silhouette. Strict 3D recovery remains unproven; first
correlate the actual large-geometry producer or occluder with a same-scene
native capture before changing depth, interpolation, texture coordinates, or
renderer lifetime.

The exact FinalMrtResult diagnostic now also retains the last depth clear for
up to 64 exact `{guest depth address, host image identity}` pairs from process
start, independently of the later trace threshold. Normal indexed/auto passes
and both direct depth-copy render-pass routes feed the same bounded tracker;
the newest 256 possible depth writers remain a separate recent-history window.
The focused graphics integration and serial `fc_script` build pass under the
zero-swap cgroup. Independent review found the render-pass coverage and tracker
mechanics sound, but noted that eviction and direct-copy behavior do not yet
have their own automated contract; the strict runtime below is the retained
integration evidence for the exact pre-threshold identity.

One corrected strict Silent run scheduled only `cross` at presents 8,000 and
8,080. Five selected 15,366-index attempts at presents 8,109 through 8,113 had
the same exact depth/stencil read-write bases, host image, HTILE range,
`LOAD`, defined attachment layout, enabled depth test/write, and reverse-Z
`GEQUAL`. For every attempt the tracker recovered the same last clear at
present 8,009: `load=CLEAR`, initial and tracked layout `UNDEFINED`,
`clear=1`, `suppress=0`, and the same depth/host/HTILE identity. No later
normal or direct-copy clear replaced it. The run peaked at 1,911,508,992 bytes
with zero cgroup swap and was stopped deliberately. A native capture at
present 8,497 still showed only the logo/PLAY screen and scored `low_entropy`
(`0.2084`, 171 bins), so this is diagnostic evidence, not a 3D advance.
It proves that the recent empty occurrences did not use an unknown split
attachment or an unobserved later clear; it does not prove the old clear is
wrong, because preceding `GEQUAL` writers may legitimately occlude them and a
prior passing 15,366-index occurrence already proves this attachment sequence
can pass substantial geometry. Do not synthesize a per-frame clear, disable
depth, or reopen HTILE identity from this result. Return to the first later
color mutation/consumer of a contributing occurrence or to a separately
captured renderer invariant.

The FinalMrtResult attachment probe can now require a significant contribution
with the host-only positive decimal
`KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS=N`. It is valid only when attachment
readback is enabled, defaults to one, leaves shader identity unchanged, and
re-arms sub-threshold fenced results within the existing eight-retry bound. The
existing graphics integration was extended rather than adding a new suite; its
RED compile, GREEN serial build, and focused contract run are retained.

One strict two-edge run with a 10,000-invocation minimum selected 17,443 finite
MRT invocations, passed fixed depth tests, and changed 17,008 pixels inside its
coverage from zero to nonzero. Same-process lifetime tracking then recorded 118
later `LOAD` writers with the same exact HDR identity before the known
half-resolution downsample sampled that image as `rt-exact`. No later clear,
identity split, guest-upload substitution, or resolve preceded the consumer.
The 128-event cap ended before the full-resolution compositor, and the native
capture was a later credits frame, so neither compositor output nor same-scene
visual recovery is proven. Do not reopen immediate MRT/depth/attachment
identity or remove later guest draws. The next bounded seam is one intervening
writer's actual overlap/delta or the downsample's input-to-output content.

The first post-material writer is now identified as PS
`2100099068cc5c23` with VS `0a0005c092436153`. A strict bounded trace captured
four 252-index occurrences: all sampled indices fit 250 declared records, the
stride-48 position/normal/half-UV layout is valid, sampled inputs and known
object coefficients are finite, all textures are bound, depth provenance is
exact, and DCC/CMASK are off. A NaN in the generic fixed `VS_SLOT_OFF272` peek
is not evidence because no read of that skybox-oriented offset is established.
Two exact post-transform probe attempts (70 and 100 seconds) ended at their
watchdogs before present 8,090, below 2.21 GiB and with zero cgroup swap; they
yielded no vertex result and must not be repeated or lengthened. Continue at a
bounded writer attachment delta or downsample input/output boundary.

That attachment-delta boundary now excludes the first writer. A targeted
ShaderProbe established its only final MRT0 export at ordinal 402. The exact
`indexed:252` MRT probe then exhausted the first occurrence plus eight bounded
retries with zero MRT invocations, zero coverage, `any_passed=0`, and zero
changed attachment pixels, while the attachment remained valid. Both inputs
delivered, no event was dropped, `last-error` was null, peak memory was about
1.81 GiB, and cgroup swap was zero. These draws cannot alter the earlier
material contribution; do not reopen their VS or depth identity. The next
unclassified lifetime writer is PS `210001009057ad42`, `indexed:2112`.

The 2,112-index writer is now excluded in the correct later phase. Its
alpha-tested PS has one enabled MRT0 export at ordinal 18. At present 8,093 it
produced 858 finite candidate colors over finite coverage while the exact HDR
attachment already contained 40,995 nonzero pixels, but fixed depth reported
`any_passed=0` and the before/after attachment delta was exactly zero. Inputs,
event ring, error state, and the 1.76-GiB zero-swap resource envelope remained
clean. Candidate shader output is not a color write: this writer cannot cause
the observed smear. Continue with PS `21000220c3cdade2`, `indexed:1536`.

The procedural 1,536-index PS is also excluded. Its only enabled MRT0 export
is ordinal 43, and an exact present-8,092 run exhausted nine bounded
occurrences with zero MRT invocations, zero coverage, `any_passed=0`, and zero
attachment delta while 95,457 nonzero pixels remained present. Runtime health
and the 1.76-GiB zero-swap envelope stayed clean. The repeated PS `21000870488b957d`, `indexed:2418` candidate is now excluded.
Targeted ShaderProbe identified its enabled MRT0 export at ordinal 363. An exact
probe across match 0 (load_op=1 / CLEAR) and match 1 (LOAD) exhausted all 9 bounded
occurrences with zero MRT invocations (`on=0`), zero finite coverage (`cfin=0`),
`any_passed=0`, and zero attachment delta (`d=0`). This definitively closes the
census of posterior writers: no posterior draw alters or occludes the material
contribution. The investigation focus shifts exclusively to the vertex shader
`0a0005c0ef41d630` dataflow, numerical lowerings, and vertex-to-pixel interface.

The existing VS aggregate was next paired with the significant MRT selector
for VS `0a0005c0ef41d630`, PS `210005b0766a27a5`, `indexed:15366`, MRT0
ordinal 229, match 1, and a 10,000-invocation minimum. The exact fence produced
9,151 finite clip and `PARAM0.xy` observations with zero NaN/Inf. Clip
population was `wnp=6177`, `oxy=1731`, `in01=1243`; `PARAM0.xy` stayed in
`[-0.088562,0.973145] x [0.788574,0.845215]`. Fixed depth passed and the draw
changed 86,376 pixels from zero to nonzero. The run used the exact taps at
presents 8,000 and 8,080, had no dropped events or structured error, peaked at
1.9 GiB, and used zero cgroup swap.

The native frame at present 8,223 was the main menu. Its `hot_corruption`
classification is a menu-color false positive, so the wide finite clip range
and nonpositive-W population are not evidence about the reported damaged 3D
scene. Do not sanitize position, change matrix offsets, or alter clipping from
this occurrence. Reuse the now-working paired probe only after establishing a
same-scene selector for a frame that visually contains the progressive 3D
stretching.

A subsequent strict create-order failure identified one real missing lifetime
contract: an existing IndexBuffer may be wholly contained by a newly registered
RenderTexture. The inverse direction and Texture form already preserve both
views. The render-target policy now links only the observed
`IndexBuffer IsContainedWithin RenderTexture` relation. A focused graphics
integration derives this relation through the production create path, failed
with the same `DATAERR` before the change, and now proves that both views remain
live while partial, reverse-containment, and exact forms stay strict. The rebuilt strict
runtime passed the former `DATAERR`, reached present 8,150, delivered exactly
the taps at 8,000 and 8,080, and left no process after its watchdog. Build and
runtime cgroups both used zero swap; runtime peak stayed near 2 GiB.

This advance is not the 3D correction. The new render target occupies
`0x478d0000..0x478e0000`; the retained 41,910-index draw uses index range
`0x47a00030 + 83820` and vertex range beginning at `0x47900020`. The ranges do
not overlap. Do not add image-to-index readback for that draw or generalize a
foreign buffer-cache policy from this alias.

A cold-cache strict replay with the correct VS selector and floor 8,000 reached
damaged gameplay at present 8,932. The selected 41,910 occurrence completed
before the second tap: all 27,937 outputs were finite, but 25,677 had
non-positive W, the remaining 2,260 were outside XY, no invocation entered
either Z population, and fixed depth saw no passing sample. Moving only the
floor to present 8,500 reached gameplay but produced no later matching result
before the 115-second watchdog. Both runs peaked around 2 GiB with zero swap.
Checksum plus index count is therefore not a unique gameplay fence; do not
repeat or lengthen that selector. Correlate a bounded gameplay-phase draw trace
with the damaged frame before changing transforms, exports, depth, or resource
materialization.

A bounded post-8,800 PS census then found one 234-index blended draw whose valid
80-record RGB32F position stream contains NaN triples at indexed records 39,
58, and 77--79. Its index range, stride, format, semantic split, and Vulkan
layout are internally coherent, and the persisted VS directly propagates the
loaded position through multiply/add/FMA to the clip export. This proves a
non-finite bound stream, not its provenance or visual ownership. The census was
too intrusive for scene correlation: it slowed to 0.672 FPS at present 8,813
while still `loading`, produced no capture, peaked at 2.16 GiB, and used zero
cgroup swap. Do not repeat the 32-entry census or add an unconditional NaN
clamp.

The later exact draw trace closed the float-mode question without authorizing a
global NaN rewrite. The fused vertex stage reported `dx10_clamp=1` and
`ieee_mode=0`, but the RDNA2 contract applies that mode to the per-instruction
floating `CLAMP` output modifier: ordinary unclamped `V_ADD`/`V_MUL`/`V_FMA`
still propagate NaN. Kyty now carries the effective VS/GS/PS mode into shader
identity and emits the mode-correct post-operation clamp (`NaN -> +0` only for
DX10 clamp, preserve NaN otherwise, and ignore output modifiers in IEEE mode).
Translator version 34 prevents cache aliasing with earlier modules. Focused
SPIR-V integration validates the DX10, non-DX10, unclamped, and IEEE variants;
this is a general ISA correction, not a compatibility claim. The exact shader
contains no instruction clamp in its position chain, so this change cannot
repair its non-finite output and must not be broadened into input sanitization.

The same strict trace found no currently live `GpuMemory` owner or alias for the
960-byte position span. That excludes a current tracked writable storage/RT
object at the selected bind, but does not exclude a historical writer that was
freed before the draw or guest CPU-authored sentinel data. A subsequent exact
vertex-output probe after both established inputs observed 80 invocations, 74
non-finite position exports, and only six finite outputs; the finite subset lay
inside both supported Z clip conventions. The paired fixed-function query saw
depth enabled and no passing sample. For this draw the failure therefore exists
before depth, clear, HTILE, rasterization, or fragment shading. Its raw format-74
RGB32F bytes and translator output agree, so the next investigation boundary is
the producer/lifetime of that guest range or a proven cross-API non-finite clip
rule, not a depth override, format substitution, or global clamp.

The empty live-owner result was a topology snapshot, not temporal provenance:
the vertex path first captures eligible read-only guest bytes into a transient
buffer, so no persistent `GpuMemory` identity is expected. An opt-in writer
history now records effective DMA, immediate `WriteData`, constant-RAM dumps,
addressed occlusion `EVENT_WRITE`, and GPU writeback operations as distinct
classes. Exact `addr:size` mode retains only overlaps in a fixed 128-entry
ring. Relocatable `auto` mode lazily allocates a bounded 65,536-entry ring and
retains covered events until the draw can query its actual guest VA; the large
ring is not reserved in disabled or exact mode. Both modes expose at most the
latest 16 matches and report per-recorder totals, retained/dropped counts, the
total matching count, and output truncation. The recorder is disabled by
default and uses an atomic fast path when unarmed.

An exact-range attempt using the prior process's VA was inconclusive because the
position allocation moved in the next process. The subsequent strict `auto`
run removed that process-relocation bias: at the selected draw it covered the
actual 960-byte range, retained 62,746 eligible events with zero drops, and
found zero overlaps. Recorder totals were 43,493 normal DMA operations and
19,253 immediate `WriteData` operations; custom DMA and all GPU writeback
classes were zero. That historical run preceded the constant-RAM and addressed
`EVENT_WRITE` hooks, so it does not exclude those classes retroactively. Both
scheduled input edges were delivered, `last-error` was
empty, and the process was stopped deliberately after the trace with a 1.9 GiB
memory peak and cgroup swap disabled. This falsifies only the classes covered in
that run. Direct guest CPU stores and deferred EOP `WriteData` remain explicit
blind spots, so the trace does not prove comprehensive provenance or that the
NaN records are intentional. The real command-processor execution integration
now proves that constant-RAM dumps and addressed `EVENT_WRITE` publish history
after their effective host write and flush.

AMD's public RDNA performance guidance explicitly recommends culling a primitive
from the vertex shader by setting any vertex position to NaN. Vulkan defines
clip-volume inequalities but does not state that cross-vendor hosts must preserve
that AMD primitive-assembly behavior. This makes a post-shader, per-primitive
NaN cull the leading general portability hypothesis on the Intel host. It does
not justify treating infinity the same way, changing guest buffers, using a
per-vertex `CullDistance`, killing fragments, or skipping the whole draw. The
bounded history has excluded its covered writer classes without proving whether
the remaining data came from direct guest CPU stores, deferred EOP publication,
or an intentional sentinel. The next rendering experiment must be
primitive-aware NaN culling with a deterministic post-shader contract, not a
numeric sanitizer.

That host-behavior experiment rejects the need for an inserted geometry stage
on the current Intel Vulkan path. A color-only 8x8 integration with depth,
stencil, and face culling disabled measured 18 occlusion samples for a finite
control triangle and zero samples for the same triangle with only one final
`Position.z` changed to quiet NaN. The exact runtime draw also produced zero
passing samples. Therefore the host already suppresses the NaN primitive in the
observed configuration; adding a pass-through geometry shader would not change
this draw and is not justified as the next 3D fix. Retain the AMD rule as a
portability requirement for hosts that classify the diagnostic differently,
but return the active investigation to a visually correlated damaged-gameplay
capture and its actual producing draw. Infinity and finite-but-explosive clip
outputs remain separate cases; do not infer their behavior from the NaN result.

The current strict UI route is now reproducible without relying on wall-clock
menu guesses. Scheduled `cross` taps at 80-present intervals establish these
checkpoints: three taps reach track selection, four reach difficulty, five
reach the black vehicle-preview `PLAY` screen, and six enter the next loading
phase. A six-tap run delivered every edge with no cancellation and no runtime
error, but did not reach gameplay within a 180-second bound. During the second
half it remained in `phase=loading` at roughly 3.4--3.8 FPS, advancing only from
about present 10,285 to 10,721 while the native capture stayed a black logo
card. Host memory peaked at 1.9 GiB with cgroup swap disabled.

The bounded native performance snapshot for that same route is complete; do
not repeat it. Shader generation and pipeline compilation are not the sustained
cost: only seven SPIR-V source/compile operations occurred, and slow frames did
not correlate with pipeline misses. The 119-second window instead attributed
about 95.7 seconds to command processing, 57.2 seconds to draw processing, and
31.0 seconds to draw-state setup. Slow frames repeatedly performed thousands
of `GpuMemory::CreateObject` calls and up to roughly 20,000 uploads (about
72 MiB) per frame. Transient read-only probes accepted about 99% of candidates,
but acceptance means that a command-buffer-owned snapshot was uploaded, not
that content was reused. Fence waits and `WAIT_REG_MEM` were frequent but
individually bounded; the evidence does not show a deadlock. The first strict
blocker is therefore identifying the exact resource type/outcome or draw-state
producer behind that repeated materialization, then changing one ownership or
reuse contract. Do not add a broad cache, overwrite in-flight transient data,
or select another gameplay shader until that producer is causal.

That producer is now narrowed to transient snapshot capacity and large
read-only vertex views. Command-buffer-owned snapshots can reuse the most
recent exact `address/size/usage` entry only when a byte-for-byte comparison,
performed inside a temporary dirty-page transaction, proves that current guest
contents are unchanged. `BeginRead` samples generation before and after arming
protection, capture/compare validates the same observation after the read, and
first-use ranges acquire and release a temporary tracker reference. A host
notification or write fault during either window therefore fails closed before
the transient entry is committed or reused. Changed contents and ranges with a
mutable GPU overlap fall through to a fresh capture or authoritative
`GpuMemory` path. The earlier 1.72-million-reuse run predated this correction;
its 0.15 seconds measured only `memcmp`, not tracker arm/restore cost, and its
visual output must not be treated as accepted evidence.

The remaining fallback was explicit: vertex views between roughly 1.3 and
3.1 MiB exceeded the old 1-MiB per-snapshot ceiling, repeatedly met 24--33
overlap candidates, and were reclaimed/recreated. One run recorded 2,847
`VertexBuffer` `reclaim_new` outcomes and hashed about 6.2 GiB of vertex input.
Raising only the per-entry ceiling to 4 MiB, while retaining the 16-MiB pool and
its critical reserve, removed all vertex reclaims in one warm-cache run and
reduced vertex hashing to about 1 MiB. That run's car-selection capture showed
an enlarged, gray, torn vehicle, but exact reuse still had a CPU-writer race at
that point. The frame is therefore a useful historical symptom, not accepted
evidence for the corrected renderer.

The 4-MiB ceiling alone is not sufficient. A second bounded route exhausted
the unchanged snapshot pool and again recorded 1,850 vertex reclaims plus about
4.2 GiB of vertex hashing, even though sampled slow-create sizes were only
1.67--2.46 MiB. Do not grow the pool globally: the next ownership experiment
must let a previously captured larger vertex range serve an unchanged contained
view with an explicit Vulkan vertex-buffer offset. Storage descriptors and
changed/mutable ranges must retain exact captures. Manual present-scheduled taps
also landed on different screens after the speed change; correlate future draw
probes with a one-input-at-a-time captured route rather than tap counters alone.

The corrected temporary transaction has now been exercised in three bounded
strict runs with hard memory and zero-swap limits. One no-input route measured
1,309,061 transient probes, 1,302,623 hits, and 716,848 exact reuses; tracker
validation consumed 5.75 seconds of an 86.6-second snapshot, versus 0.096
seconds in byte comparison. This overhead is material but not the dominant
70.8 seconds of command processing. A later one-input-at-a-time route reached
the vehicle-preview `PLAY` card after three delivered edges. UI and logo were
coherent, but the vehicle was completely absent; the process was stopped there
at about 2.06 GiB with cgroup swap disabled. The race correction did not recover
3D, and no corrected-run car-selection or gameplay capture exists yet.

The fallback path exposes a separate general ownership defect: existing
`GpuMemory::CreateObject` and `GpuMemory::Update` hash and upload directly from
guest pointers. Host/HLE writers notify before storing and do not take the
GpuMemory mutexes, so a concurrent write can still tear the CPU-to-GPU bytes for
that submission even though the next update retains dirty evidence. A postcheck
after publishing cannot undo the upload. The current bounded experiment stages
read-only vertex/index updates and initial creates up to 4 MiB into a
postvalidated immutable CPU copy. Initial create registers the range before
copying and transfers the stable dirty observation to the published object;
update keeps the previous backing when its observation races. Textures,
storage, larger buffers, and hash-fallback ranges remain on the established
non-immutable path; do not claim the broader race fixed until their
source/publish contracts are refactored and validated.

One strict no-input run exercised that full bounded VB/IB seam in a 3D
`ROLLING START` scene. It recorded 268 stable initial captures totaling about
538 MiB and 81 stable updates, with zero create fallbacks or update deferrals.
The native frame retained recognizable 3D perspective, road, building,
vegetation, and HUD, but most of the world remained black and bright colors
appeared dragged or disconnected. Peak cgroup memory was about 2.15 GiB, swap
was zero, and the process was stopped deliberately. This closes torn bounded
VB/IB source bytes as a sufficient explanation for the remaining visual defect.
The active symptom is downstream color-target corruption, not a demonstrated
vertex-position failure.

The render-target lifetime trace now covers two previously invisible seams. A
bounded `PASS_BEGIN` event observes exact versus guest/host-only attachment
identity, old and initial layouts, load operation, fast-clear state and clear
words before barriers mutate host state, including clear-only passes. A second
bounded event detects when a resolved sampled image aliases an active color
attachment before either layout transition. Both remain disabled by default
and do not alter rendering. Initial attempts did not reproduce the damaged
scene; their zero alias count is therefore inconclusive and must not be used to
change clear or feedback semantics.

Two further scheduled-input attempts confirmed that delivered taps alone do
not identify a visual checkpoint: one reached the difficulty screen and the
other returned to the main menu while all queued taps were reported delivered.
The latter bounded trace observed the full-resolution HDR target begin with a
shader-read-to-color clear followed by defined-layout `LOAD` writers and an
exact downsample chain, with no sampled-image/color-attachment alias in that
menu window. This is route and menu-pipeline evidence only; it does not close
feedback, stale-clear, or lifetime hypotheses for the damaged 3D scene. Stop
using absolute present schedules as a same-scene selector. Drive one edge only
after a stable interactive phase and confirm each screen before interpreting a
graphics trace.

The normal indexed and auto draw paths already emit a post-render-pass Vulkan
memory dependency from color attachment writes to later shader and color
attachment reads. Its destination access scope omitted the following color
attachment write, leaving the consecutive W-to-R/W contract incomplete even
though the color-output stage was ordered. Both paths now include
`VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT` in the destination mask. This is a
general synchronization correction; the affected targets build and the
existing graphics integration passes, but no same-scene 3D capture exists yet.
Do not claim that it fixes the black/color-spread corruption until a strict
damaged-scene A/B demonstrates that result.

The first phase-driven strict integration after that synchronization change
delivered exactly three taps, observed `loading -> interactive`, captured
through the native agent, and shut the guest down cleanly. It reached the car
selection screen, but the vehicle remained absent. Offline scoring reported
`scene_ok=false`, `gameplay_like=false`, and entropy `2.2764`; the service
peaked at about 1.5 GiB with cgroup swap disabled. This is a reproducible
post-change visual failure and proves that the completed color write access
scope is not sufficient to restore the missing 3D. It is not the later damaged
`ROLLING START` scene, so it does not falsify a contribution to the progressive
black/color-spread symptom. The historical visual baseline is no longer present
in scratch and cannot be used for an automated comparison.

A later one-edge-at-a-time strict run did reproduce the active gameplay defect
after the synchronization correction. The native frame retained road
perspective, vehicle silhouette and coherent HUD, while most world and vehicle
color was black and small yellow, red, green and white regions were detached or
dragged across otherwise recognizable geometry. `last-error` remained null;
the bounded service peaked at about 2.16 GiB with cgroup swap at zero and was
stopped deliberately. This is accepted symptom evidence for downstream
color-target retention/composition. It is not evidence of a new exploding-
vertex or non-finite-position failure.

The lifetime trace now accepts a strict Vulkan color-format selector and a
successful-manual-capture ordinal gate. The format-only trial correctly armed
two alternating full-resolution HDR images, but repeated writes, pass begins
and samples exhausted its 64-event window within 17 presents, long before the
damaged scene. The capture gate replaces guessed present thresholds: only the
selected newer successful explicit capture opens the trace on the following
frame; automatic, failed, timed-out or superseded requests do not count. Its
build and existing graphics integration pass. The first bounded navigation
attempt preserved the closed gate through five manual captures but hit its
180-second watchdog before the selected ordinal, so exact damaged-scene HDR
lifetime evidence remains open. Do not infer clear, feedback or identity
behavior from that run.

That same-scene color boundary is now captured. A bounded score-driven selector
opened the one-shot trace only after a native frame showed the damaged 3D world:
road and vehicles remained recognizable, while black regions expanded and
yellow/white scene fragments became detached. Over the following frame the
selected 1920x1080 format-122 HDR image kept one guest address and one host
identity. All 59 observed pass begins used `LOAD` from a defined color-
attachment layout with no CMASK fast clear, no identity mismatch and no active
sample/attachment alias. The first downsample sampled that same image as
`rt-exact`. This closes stale color clear, target remap and direct feedback as
causes for that exact frame; it also places the corruption before the
downsample rather than in the final compositor.

Every recorded HDR writer in the damaged frame had depth test and depth write
enabled. The visible frame lost most opaque surfaces over only a few presents
while bright lights survived, so retained or incorrectly published depth is the
next falsifiable producer. It is not yet confirmed: color and depth lifetime
selectors are intentionally mutually exclusive, and the subsequent depth-only
run did not re-enter the demo before its watchdog. The next run must visually
select the same damaged frame, arm only the 1920x1080 D32S8 depth selector, and
classify its first `DEPTH_USE`, clear source and identity before changing any
load operation or reverse-Z rule.

The earlier HTILE publication experiment was withdrawn after runtime and
independent review. Publishing an exact storage view in a separate operation
before depth materialization left a TOCTOU gap and ignored pending or ambiguous
owners; it also did not improve the frame. Storage destruction no longer
creates a metadata-clear event merely because the mapped bytes resemble a
clear. Depth-image creation also no longer fabricates an HTILE clear; Vulkan
first use remains governed by the existing `UNDEFINED` load-op resolver. Keep
the provenance diagnostics, but do not restore address-only teardown marks or
the separate pre-materialization writeback call. The remaining pending-event
map in `DepthMeta.cpp` is still keyed only by address and is not bounded; a
future semantic change must add size plus allocation/depth generation and a
fail-closed bound before relying on it across retirement or address reuse.

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
Your mission is to advance the strict PS5 runtime from the current controllable
gameplay frontier to stable, validated playability, then freeze that frontier
and only afterward perform carefully bounded modularization. Correctness,
evidence, portability, and preservation of working behavior outrank speed or
line-count reduction.

CURRENT FRONTIER

- Build works on Linux (`_build_linux`) and macOS (`_build_macos`); use the host
  you are on. Prefer Release + `PrintfDirection=Silent` for wall-clock.
- Vulkan device/swapchain, Gen5 shaders, indexed draws, VideoOut flips, logos,
  recognizable menu, Play/mode transitions, loading-card pixels, and
  controllable gameplay are exercised under strict flags. A reset 601-frame
  Release+Silent gameplay window reported 41.688 FPS with p50/p95/p99 frame
  times of 27/35/40 ms. Diagnostic controller input reached and moved through
  that scene; this proves the frontier, not formal acceptance or stable 60 FPS.
- In tree (do not regress): GpuMemory multi-parent (VB reclaim + surface link;
  Texture mixed parents; IndexBuffer-in-Texture link; WriteBack parent
  classify); GPU-owned RT layout preserve on Update; tile-27 size+4bpp detile;
  Gen5 EUD type-5; formats 14/29/56/71; multi-RT CB_SHADER_MASK; EXP Param5/6 +
  multi-MRT; structured SPIR-V loops; `v_cvt_i32_f32`; SDWA; SMEM dual-offset +
  variable SBuffer; image_sample dmasks 0x2/0x4/0xb; `ds_read2_b32`; null MRT
  discard tails; kill-enabled `EarlyZThenLateZ` late depth commit; NGS2
  extended max_voices.
- **First strict fail (re-capture on HEAD):** none observed through more than
  24,000 presents in the latest Linux Release+Silent strict run. The next
  structured EXIT or host fault is the process unit of work.
- **Visual frontier:** horizontal stripes and opaque sprite/prop rectangles are
  absent in a gameplay-era native capture. A bounded diagnostic route consumed
  sustained directional input for 180 presentations. Formal acceptance still
  requires a repeatable non-diagnostic controller route, one action beyond
  movement, stable presentation, and validation-clean output.
- **Later symptom only:** `ThreadFlag` bit `0x1` (mode `0x21`, 40 ms) with no
  observed Set. Never fabricate the signal. EventFlag live-handle registry
  (garbage → ESRCH) is not Set. Trace the producer after earlier GPU/shader
  aborts are gone.

IMMEDIATE OBJECTIVE AND SUCCESS CONDITION

Advance the strict post-Play path to **stable, correctly rendered 60 FPS
gameplay** without diagnostics or fabricated success. Formal acceptance must
use a real controller route, exercise movement plus another action, and remain
validation-clean. Process survival, diagnostic input, and HUD-only correctness
are not playability.

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
ninja -C _build_linux
```

Configure and build on macOS:

Line count is a signal, not the goal. Do not split an atomic eight-line
function. Do split a long function that mixes parsing, state mutation,
allocation, Vulkan calls, and logging. Each extracted function/module must have
one purpose, explicit inputs/outputs, ownership, thread contract, error
contract, and a focused test. Delete the superseded implementation in the same
change; do not leave permanent forwarding aliases or duplicate semantics.

REFERENCE MATERIAL

Use reference material only for behavioral facts, architecture patterns, and
test ideas. Every imported claim must be verified with a local capture or a
focused test before it affects Kyty behavior. Do not copy incompatible code,
private assets, proprietary SDK material, firmware, keys, decrypted content, or
implementation details that cannot be relicensed into Kyty.

For each confirmed fact, record the provenance, license, behavior learned, and
the local evidence that proves it. Reimplement confirmed behavior using Kyty's
own types, boundaries, and diagnostics.

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

Diagnostic continuation is centralized. Do **not** invent per-game exceptions
or cite unsafe survival as compatibility. Neighbor PRX soft-preload is
**unsafe-only** (`prx_preload` feature); strict acceptance never auto-preloads.

| Variable | Meaning |
| --- | --- |
| `KYTY_BRINGUP_MODE=unsafe` | Enable diagnostic policy (absent ⇒ strict). |
| `KYTY_BRINGUP_FEATURES` | CSV: `not_implemented`, `missing_function_import`, `gfx_permissive`, `prx_preload`. Absent under unsafe enables the first three only; **`prx_preload` is always explicit**. |
| `KYTY_BRINGUP_SUBSYSTEMS` | CSV scopes: `core,loader,kernel,graphics,audio,network,hle,other`. Absent ⇒ all. |
| `KYTY_BRINGUP_BURST_LIMIT` | Max hits per site inside the window (default 10000). |
| `KYTY_BRINGUP_BURST_WINDOW_MS` | Window length in ms (default 1000). |

Unknown, empty, zero, or contradictory values abort at process start
(`BringUp::InitFromEnvironment` from Core subsystem init; no silent strict
fallback after a parse error). Circuit-break on a site re-enters the normal
strict abort after printing a summary. Repeated `EXIT_NOT_IMPLEMENTED` continues
log once per site (no full stack spam on every hit). The policy never
fabricates EventFlags, fences, memory, or sync results. Only **Func** imports
may receive missing stubs (with a minimal return-class taxonomy); Object / TLS /
NoType stay strict `EXIT`. Neighbor PRX scan is **not** part of default unsafe
features — set `prx_preload` explicitly.

**Removed (intentional break):** `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE`.
Using them is a configuration error.

Other diagnostics (unchanged, still not acceptance modes):

- `KYTY_FAULT_LOG=1`: signal-safe fault diagnostics.
- `KYTY_CRASH_REPORT=/absolute/path.json`: writes the bounded fatal-fault JSON
  report. `KYTY_CAPTURE_DIR` supplies `crash-context.json` by default when an
  explicit report path is absent.
- `KYTY_CRASH_MEMORY=1`: on supported POSIX hosts, adds at most 24 fault-safe
  64-byte windows around plausible guest-data pointers found on the captured
  stack. It is disabled by default and its output may contain guest data; keep
  the report in untracked scratch.
- `KYTY_TRACE_LIBC=1`: targeted single-step tracing.
- `KYTY_SKIP_UD2=1`: skips a guest trap for diagnostics; invalidates normal
  execution. **Not** part of the bring-up policy.

Agent diagnostics JSON protocol version is **5** and includes `bringup.mode`,
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
