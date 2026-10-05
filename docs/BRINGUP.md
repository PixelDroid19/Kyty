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

### CPU/GPU overlap and per-draw cost (2026-10-05, guest verified)

Scope: same strict configuration on the reference host (Intel Arc A770, Mesa `xe`). Profiled with gperftools on the
command-processor thread and the DRM `fdinfo` engine counters for the device. A pixel-art title whose vertex front is
bindless (V#s loaded from tables at runtime offsets) and whose only compute shader is the SDK's 16-byte buffer fill
went from 41 ms to 22 ms per frame (24 to 45 fps) on its start screen. Regression set (runs d586-d602, 90 s each,
previous run in parentheses): Blasphemous 2 107 fps (98), Dreaming Sarah 248 (251), Let's Build a Zoo 239 (220),
Formula Retro Racing 229 in the race (181), The Messenger 594 (378), Dead Cells 201 (112), ANIMAL WELL 37 in its
new-game route (23), Worms 59 (59), JoJo 220 (203), Hades 59 (49), the .NET beat 'em up 11 (11). DREDGE ran clean once
(66 fps, was 52) and twice stopped at 12-14 s on its known stale extended-user-data V# (the same words as runs d221,
d466 and d571).

- **Instruction mnemonics without allocation.** The shader-usage pass runs for every draw and asked `magic_enum` for
  each instruction's name as a heap `String8` to test a prefix; with the SRT, assembled-descriptor and scalar-load
  collectors that was 13% of the processor. `ShaderInstructionTypeName` and `ShaderInstructionTypeStartsWith` return a
  `string_view` from the one translation unit that declares the enum range. Two translation units used magic_enum
  without that declaration (default range -128..127), so the names of later instructions (`SLoad*`, `TBuffer*`,
  `VCmpx*`) could read empty depending on which instantiation the linker kept.
- **Uniform buffer fills published on the device.** Before recording a draw or dispatch that dereferences guest
  addresses, every storage buffer a compute pass had written was written back: submit, CPU fence wait, then a page
  comparison of the whole buffer. A title clearing buffers with the fill shader paid that about 25 times per frame
  (13 waits, 16 ms per frame). For a dispatch the existing uniform-fill proof shows to store one value to every record
  of its bound range, the filled object is now copied on the device into the imported view of that range, between
  barriers, so later device-address reads in that queue see the result in GPU order, as on unified memory. Only
  chunks that alias the guest's physical memory qualify: a snapshot chunk could be refreshed from stale guest bytes
  before the CPU write-back. Device-address consumers on that queue then skip the write-back; the completed
  submission still writes the object back, and because its content is the known pattern the page cache adopts it
  without reading the GPU copy (constant time when the same pattern repeats). Objects with label fences or depth
  metadata in range keep the byte write-back.
- **WAIT_FLIP_DONE waits for that buffer's flip.** It waited for the device to finish everything recorded so far,
  which serialized recording and execution every frame. A GPU flip enters the flip queue when its submission
  completes, so the processor now waits only for the submission that recorded the latest flip to the requested
  display buffer, then for the flip queue as before.
- **Batch completion is asynchronous.** A batch whose completion payload is a flip, an end-of-pipe interrupt or a
  consolidated label wait no longer blocks the processor on its fence. The guest observes those through memory, event
  queues and flip status, never through its submit call; the fence poll of the next submission or the ring's idle
  pump (1 ms) publishes them. A wait on a plain label store of the current submission no longer splits it: a full
  barrier keeps the order.
- **Scalar flow shared per program.** The memoized scalar analyses (live-at-entry, entry-value sources, holding
  sets) were looked up three to five times per draw by materializing the program signature, and each lookup copied
  per-instruction vectors. The signature is now hashed and compared field by field without allocation, and per-draw
  callers share one `ShaderScalarFlow`.
- **Draw-time snapshots without page protection.** Small read-only buffers are copied per draw; each copy and each
  comparison armed and restored write protection around the read (two `mprotect` calls and a TLB shootdown, 18% of
  the processor). A copy is now validated by a second read: unchanged bytes held their value between the two passes,
  unless a store restored the old value in between. A comparison runs alone; a store racing it is the race the
  console GPU has with a pending draw, which guests order with labels.

Measured after these changes on the pixel-art title: the processor is busy about 9 ms per frame and the render
engine about 11 ms; the title submits its next frame after the previous flip, so the two still add up. Next:
per-draw shader usage parsing (about 10% of the processor) and submitting earlier within a frame.

Second round (same day):

- **HTILE size outside the table.** Gen5 depth sizes not in the table fell back to 8x8 cells of 8 bytes over the
  unpadded width, 0x70000 bytes for a 2500x1400 target. The game clears its HTILE with the SDK metadata-fill compute
  over 0x48000 bytes, so the clear never matched the depth target's HTILE range and was never applied: the target kept
  the previous frames' depth, and a racing title's car-select preview (reverse-Z, GEQUAL) showed only the edges of the
  rotating car. HTILE is one 4-byte word per 8x8 tile over whole 1024x512-pixel metadata blocks (32 KiB); this
  reproduces every table entry (720p, 1080p, 2160p) and the guest's clears (0x48000, and 0x8000 for small targets).
  A 32-bit depth row is padded to the 128-pixel width of its 64 KiB block (the colour target of the same pass already
  used pitch 2560).
- **Multisampled HTILE clears.** The metadata-fill clear was translated only for single-sample targets; the preview
  target is 2x MSAA. A zero HTILE word marks every sample of its tile cleared and the attachment clear covers every
  sample, so multisampled targets now take it too.
- **Guest-address residency scan.** Ranges without physical backing were still asked whether their physical range
  was unpopulated, a linear search over all physical mappings that never matches; per-range statistics timers called
  the clock thousands of times per preparation. A .NET title went from 43 to 37 ms per frame (table preparation
  28 to 21 ms). Its physical ranges still rescan whenever the physical population changes (18 ms per frame).

- **Idle queues publish their completions.** The 3D fighting title froze when its story mode started: it submits one
  ACB to each compute queue and spins on the ACB's end-of-pipe label (`while (*label != 1)`, found by profiling the
  frozen run: two guest threads at 100% in the spin and its caller). A plain label store carries no completion
  callback, so nothing polled that queue's fence once its ring went idle; earlier the label was published only when an
  unrelated wait drained completions, which the asynchronous batch completion above removed. Every ring now stays in
  its completion pump until its submissions retire, compute queue rings included.
- **More guest compute queues than host processors.** The same title probes nine queue handles (0x20, 0x21, 0x28,
  0x29, 0x30, 0x38, 0x40, 0x48, 0x50) and the ninth aborted with "exceeds available independent queues". Once each of
  the eight host compute processors has a handle, a new handle shares the processor with the fewest handles; its
  submissions stay in order there. The title now reaches its story-mode cutscenes (textures on characters are still
  corrupted and the scene runs at about 5 fps, open).

Verification: `run-check.sh <n> <slug> [route] [seconds]` in the scratch harness runs one title and prints a verdict
(FATAL with the error line, FROZEN when the observer confirms no presents, BLACK, STATIC, or ADVANCING) with the lit
fraction and change of each capture, so a fix is checked on the affected title instead of the whole set.

Investigated and left open:

- The .NET beat 'em up stops at about 1.7 s in some runs (also before this work, runs d474 and d530): its runtime
  reads a null table pointer plus 0x120 (`mov r11, [rax + r10*8]` with rax = 0) while summing allocation statistics.
- The pixel-art title's new game starts with a "wake up" prompt that none of the scripted pad buttons pass (all face
  buttons, shoulders, triggers, sticks, d-pad, options and touchpad were tried, tapped and held); the title menu
  before it works.

Third round (same day):

- **Published fills skipped write-backs that still had work (regression from the first round).** Dead Cells' gameplay
  smeared sprites horizontally (trails behind the character) and, some seconds later, turned the whole scene white;
  only the HUD survived. It happened in most runs since the device-side publication of uniform fills, and in some
  runs never, depending on how submissions overlapped. Bisection: publishing the fill's bytes into the guest view
  was correct; the defect was the exemption it granted. A published object is skipped by the in-order write-back
  that precedes every device-address draw, so its write-back runs only when a completion finds all of its uses
  retired. The title rewrites the same fill objects every frame, so with submissions in flight that point may never
  come (run d724: after the first gameplay seconds, none of the 642x362 fill objects was written back again). One of
  them is the depth target's HTILE, cleared to `0xfffffff0` by the same fill shader; the depth clear is recognized
  from that write-back, so the depth target kept old depth, the G-buffer draws failed the depth test against it, and
  the stale colour stayed (trails) until the light pass saturated (white). The publication mark now requires that the
  write-back has nothing left to do: no depth metadata on the object, and every overlapping object an exact
  colour-image alias the device wrote after the object's last write (the fill's propagated clear, or later
  rendering). Depth/HTILE, other buffers and partial aliases keep the in-order write-back. Two runs of the walk route
  render correctly at 47-55 fps (the unsound exemption gave 92-100; the depth clear costs one wait per frame again).
- **Write-backs keep newer device content.** Each GPU object records the logical time of its latest writable use
  and of the latest device write into it. A completed write-back no longer reloads or invalidates an overlapping
  object the device wrote after the written-back object's last write; it takes the written guest bytes as that
  object's baseline (hash and dirty generation) so its next use does not reload them over the device content either.
- **Guest-address residency without quadratic lookups.** The .NET beat 'em up maps 12,516 GPU ranges, almost all of
  them physical. Whenever the physical population changed, the preparation asked for each unimported span whether it
  was unpopulated, and each question scanned every physical mapping (122 ms preparations). Physical mappings keep an
  index ordered by guest address, rebuilt only after the list changes; live views never overlap, so the view holding
  an address is the nearest live view at or below it (a miss falls back to the conservative answer). Preparation also
  walks only flexible ranges and ranges holding tracked snapshots instead of all ranges and chunks. Over 180 s the
  preparation total went from 32.7 s to 7.1 s (maximum 123 to 59 ms).

- **Hades reaches gameplay.** Three independent defects stood between its title and the House of Hades:
  - *Branch-heavy shader compile never finished.* After cross, the command processor sampled 70% of the process in
    `ArmReachesBeforeJoin`: a 2,299-instruction pixel shader with 254 branches. Selection-join queries were repeated
    for every label and branch during emission, located instructions by scanning the program, and ordered a join's
    edges by recomputing nesting depth per comparison. Instructions are now found by bisection, reconvergence and
    nesting results are kept per thread for one program and label state (disabled labels change them), and depths
    are computed once per edge.
  - *SW_64KB_S volumes.* A 32x32x32 RGBA8 lookup volume in the 64 KiB standard swizzle was rejected. The GFX10
    64 KiB standard 3D pattern keeps the 4 KiB pattern in its low 12 bits and adds `X3 Z3 Y4 X4` (4-byte elements;
    the table holds every element size), verified against the addrlib pattern tables.
  - *Invisible menu text.* Tile 9 sampled textures uploaded guest bytes only for four formats; the menu's R8 glyph
    atlases (CPU-written) stayed empty, so each label showed only its plate. The tile-9 host detiler matches the
    GFX10 SW_64KB_S pattern for 1, 2, 4, 8 and 16-byte elements, so any such uncompressed format now uploads when no
    live surface covers it.
  Hades now shows its menus and walks in the House at about 10 fps (open: performance, and a crash at 8-11 s in
  some runs where its GPU allocator returns a null block and a row copy writes to 0x40; its direct-memory allocate
  and map calls do not fail in those runs).

- **Linear mip chains.** JoJo's characters showed shredded textures: almost every material is a linear (tile 0)
  BC1/BC3 texture with a full mip chain, and the linear fallback laid levels out from level 0 with halved pitches.
  GFX10 linear mip chains (addrlib `HwlComputeSurfaceInfoLinear`) store the levels from the smallest one to level 0,
  each with its element pitch aligned to 256 bytes, element sizes halving on their own (4x4 blocks for BC). The
  fallback now computes that layout and each mip upload uses its own pitch; the rule reproduces all 2,135
  multi-level entries of the SDK-generated linear RGBA8 table (offsets, sizes, padded pitch and total).

- **SDK pattern fill.** Hades clears its 1920x1152 targets every frame with the SDK pattern-fill kernel
  (`buffer_store_format_x` of `values[i % period]` for `i` below a count, 32-bit UINT records). It was never
  recognized, so the clear reached the storage buffer but not the render-target images: rooms with strong lighting
  showed saturated green, smeared columns and stale silhouettes. The uniform-fill proof now matches that kernel's
  exact template (instruction types, branch targets, operands); with a live period of 1 it is a uniform fill,
  propagated to the exact image aliases and published like the 16-byte form (publishing: 14% more frames, write-back
  time 11.8 s to 0.8 s over 120 s). A first attempt never matched: the proof rejected programs with indirect labels,
  which every conditional branch records for its fall-through, so its measured cost was not the fill's.
- **Device-address scans over writable storage only.** Before a draw or dispatch that reads guest addresses, GPU
  memory looked for storage buffers with pending GPU writes by walking every live storage buffer, twice; JoJo keeps
  about 4,000 alive and the ordered-set walk was the top host hotspot of its cutscene. The scans now walk the
  in-use, writable subset.
- **Partially released direct mappings keep their live part.** The .NET beat 'em up's runtime shrinks a buffer by
  releasing the tail of its direct memory (0x504000 of a 0x55c000 mapping) and maps new memory over that tail. The
  release marked the whole view released, so the fixed map dropped the head too; the unmap then removed only
  protection blocks that start inside it, so the whole-mapping block survived and a virtual query of the head still
  reported 0x55c000 bytes. Its release loop (query, then release `va - start + offset`) then released memory that
  already backed other allocations, or, for the dropped head (a reservation, offset 0), physical address 0: the
  1 GiB "User Malloc" mspace whose header lives there. The title died within 2 s in about half of the runs, with an
  `OutOfMemoryException` from its garbage collector, a corrupted mspace mutex, or a fault on reused GC pages. A
  release now splits the views it covers in part (the released piece stays a released view), an unmap frees exactly
  its view's pages (`FreeRange`, as cuts already did) and trims the protection blocks, and a queried region never
  extends past its mapping. With released views made inaccessible as a check, five runs never touched one.
- **Written-back writable buffers retire.** The same title binds ring-buffer sub-ranges as writable storage views;
  linked writable views never retired, so one 288 KiB region held 4,000 objects and grew by ~1,000 every 25 s, and
  each overlap query returned hundreds of them. A writable buffer owns no content once its writes are written back
  (`in_use` clears), so it now retires after 120 idle frames like a read-only one.
- **Write-back and overlap scans use indexes.** Completed-submission write-backs walked every object of every heap
  (12,546 heaps, ~16,000 objects, on every completion); they now visit an index of in-use writable objects (18 on
  average). Overlap queries walked every heap; the heap span index now lists every heap covering a span. Loading
  frames went from 18-23 to 30-36 fps on that title and Dead Cells' walk route from 47-63 to 86 fps.

Investigated and left open:

- JoJo's in-engine cutscenes run at about 5 fps. The guest spends most of its time spinning on GPU labels
  (`while (*label != 1)` after an ACB submission, and two similar loops), so the frame rate is the round trip of its
  compute work. Each of its ~20 dispatches per frame completes with a write-back of a 15 MB storage buffer of which
  about 53 KB changed; the page comparison reads all 15 MB (3 ms per dispatch, 27% of the time). Skipping it is not
  correct (the CPU may read the results); the planned fix copies GPU-written pages on demand. The dirty-page tracker
  also accepts only 512 ranges, so most of JoJo's buffers fall back to full hashes (250 GB hashed in 2 minutes).
- The .NET beat 'em up plays its logo video (`logos.ogv`, Theora decoded by the title) as a uniform green screen:
  the three Y/U/V textures (R8, SW_4KB_S, 1920x1080 and 960x540) read as zero in guest memory for the whole clip,
  and BT.709 of zero planes is (0, 77, 0). The decoded frames never reach those textures; the upload path (the
  title's own GPU copies) is the next step.
- The .NET beat 'em up is not frozen: its black screen is a loading screen (its two sprite draws are black, vertex
  colour (0, 0, 0, 0.88), on a black target) while it uploads textures through compute image copies, about 20 per
  frame. Each copy uploads its source and destination images and writes the destination back, each a separate
  util-queue submission with a fence wait (`UtilFillImage`/`UtilFillBuffer`): 92,541 waits in 180 s, about 23 ms of
  every 48 ms frame. Next: record uploads into the consuming command buffer. Its host footprint (4.5 GB of guest
  memory plus 4 GB of emulator heap) also exceeds the harness cgroup's 7 GiB `memory.high`, which throttles it.
- Dead Cells sometimes stops after about a minute of gameplay on a G-buffer pixel shader that needs fragment wave
  transport (a derivative fetch inside a lane-divergent loop at pc 0x114, 642x362, four targets); seen in runs d472,
  d721 and d729, so it predates this round.

### Performance, new-title repairs and add-on content (2026-10-04, guest verified)

Scope: strict runs on the reference host (Intel Xe, Vulkan 1.4, Native, `KYTY_SHADER_OPTIMIZATION=None`, shader
validation on). A 2D action title went from 25 to 62 fps; a 3D fighting title boots through its intro and title
screen at 120-140 fps with correct UI art. The existing suite passes (1,873 tests).

- **Performance.** GPU heap lookups use an index of disjoint spans; device-address storage queries are batched;
  physical population is gated; completions are asynchronous with consolidated waits; scalar liveness is memoized
  by content signature (profiled with gperftools, not guessed).
- **Linear block-compressed rows.** Gen5 linear BC rows are padded in block columns (256 bytes of 4x4 blocks), not
  texel columns; aligning texels sheared every block row of BC1/BC3 UI art. The tile-0 size estimate counted texels
  as blocks (16x too large). See `graphics-texture-layout-troubleshooting.md`.
- **WRITE_DATA ordering.** An immediate write to an address whose end-of-pipe write is still pending is deferred
  behind it; landing first let the older value overwrite it (label order inversion, boot stall).
- **AMPR command buffers.** Measure functions depend only on the file offset; the `_04_00` write-address and
  kernel-event forms take a completion-mode flag; record ids are used as-is (0 is valid); every record executes in
  order, completion actions last, and the first failure is the execution result read by `AndGetResult`.
- **Guest malloc before the application heap.** Host-heap allocations are zeroed (a path buffer read stale bytes)
  and GPU objects in those ranges are hashed directly (UI vertex data rewritten every frame looked unchanged).
- **Loader and HLE.** SELF segments are bounded by the real file size (one title declares 8 bytes more); libc is
  mirrored into LibcInternal and POSIX across libkernel by the registry instead of duplicate tables; event flag
  queue/thread attributes are decoded; offline Remoteplay, NpAuth (requests complete SIGNED_OUT) and UDS modules.
- **Add-on content.** One inventory answers AppContent and NpEntitlementAccess from `dlc_emu.ini` and installed
  package folders; mount and unmount follow the published contract. See `addon-content.md`.

Second round (same day):

- **Auto-index draws start at `GE_INDX_OFFSET`.** It is the first vertex of a `DrawIndexAuto` as it is the base vertex
  of an indexed draw; the auto path passed 0, so a pixel-art title drew every tile batch from the start of its shared
  vertex buffer (scrambled streaks). It now reaches gameplay.
- **NGG fronts with a user-data base vertex.** `v_sad_u32 vN, sK, 0, vN` adds the base vertex/instance before the GS
  allocation under an EXEC of all lanes. The proof models three-source lane-local ALU, scalar words derived only from
  user data, and lane-local writes under an EXEC covering the vertex mask (exports keep the exact mask). An artillery
  title now runs at about 58 fps (output is a flat colour, open below).
- **Missing guest data symbols.** Unresolved data imports pointed at the invalid-memory sentinel; the C++ runtime's
  fundamental `type_info` objects (`T`, `T*`, `const T*`) and two vtables come from one table.
- Format 50 had no element size (zero-size texture, fatal overlap query); `localeconv` (Dinkumware layout,
  `decimal_point` at +0x48), `wcsftime`, `pthread_getname_np`, save data parameters per save directory, trophy info
  arrays, `WaitOnAddress` command sizes and graphics user data in indirect SH register lists.

Third round (same day), mostly runtime-library correctness found by Unreal Engine 4 boots:

- **Wide printf.** `vswprintf` accepted only `%%`, `%i` and `%08x` and returned -1 otherwise; string builders grow the
  buffer and retry on -1, so two UE4 titles spun forever in startup. One wide engine (`FormatWide`) now formats every
  conversion through the narrow engine, with `%ls`/`%lc` wide and `%s`/`%c` narrow; the narrow engine accepts `%ls`.
- **Dinkumware character classes.** `_Iswctype` classes are 1 alnum .. 12 blank (titles scan format specifications
  with 2 = alpha and trim with 9 = space); class 2 had been read as "digit". `_Towctrans` (1 lower, 2 upper) and
  `std::_Xinvalid_argument` were registered as data objects; `_Xout_of_range`/`_Xlength_error` returned instead of
  throwing. All four now behave as functions; the `_X` helpers throw `logic_error`-derived exceptions with what().
- **Direct memory mappings may span adjacent allocations.** A title allocates direct memory in chunks and maps 4 GiB
  windows across them; `Map` required a single allocation (EACCES) while `Release` already accepted spans.
- **Guest entry stack.** `InvokeOnStack` placed the terminating frame record below the guest stack pointer, where the
  callee's pushes overwrote it; frame-pointer stack walks then ran off the end. It now sits above the guest stack.
- **Files larger than 4 GiB.** `sys_file_size` truncated through `uint32_t ftell`; a 17 GB pak was reported as
  390 MB and its footer read at the wrong offset. Sizes come from `stat`/`fstat`.
- **Kernel AIO** (`sceKernelAio*`): requests run when submitted and are complete when polled or waited for.
- **Scalar spans of dynamically loaded descriptors.** A buffer descriptor loaded with `S_LOAD` is bound only over the
  bytes its scalar buffer loads read. The span came from the last consumer alone, so constants read earlier through the
  same descriptor fell outside the binding and read as zero. A Unity title's colour-grading LUT was baked from zeroed
  parameters into solid black, which turned every 3D view black while 2D overlays stayed correct; geometry whose
  matrices came through such a descriptor was also scrambled. The span is now the union of every consumer in the
  mapping's window; a consumer that is not a constant-offset scalar load keeps the whole descriptor bound.
- **APR entry points are raw system calls.** Every title links an SDK wrapper around the fourteen
  `sceKernelApr*` imports (resolve, file size/stat, submit, wait) that turns a negative return into
  `0x80020000 | errno` and reads a zero errno as success. The HLE returned the SCE code without setting errno, so a
  missing loose file "resolved" with an uninitialised size; a UE4 title then allocated that size (the out-of-memory
  report printed the 0x7FC0000000 reserved range). The exports now return -1 with errno set.
- **Host-side contention.** Every guest mutex lock took the static-object registry lock and two state locks; guest
  malloc/free took up to four registry and API locks. Existing objects are now returned lock-free (handles are
  published after initialization), mutex ownership is an atomic only the owner writes, and the allocator API table is
  an immutable snapshot.
- **Middleware the title ships.** An HLE library named after a title's own SDK module kept the loader from loading
  the real module; it is removed and the shipped module loads as a package sidecar.
- `wcstof`/`wcstod`/`wcstol`/`wcstoul`/`wcstoll`/`wcstoull`, guest `exit`/`quick_exit`/`abort` and abnormal-termination
  reports log the guest caller, and `NpManager` has one export list.

- **Smaller imports a UE4 title needs at boot.** VoiceQoS is registered under module version 1.1 (the import
  never resolved under 0.0); `sceNetCtlGetInfo(LINK)` reports the link down, consistent with the disconnected
  state; `sceAgcDcbSetPredication` writes the SET_PREDICATION packet, and the command processor accepts clearing
  predication and stops on the query-dependent operations, which no title has issued yet.

- **Clear-state scissors.** CLEAR_STATE and the context-state operations rebuilt the context with every scissor at
  zero, an empty rectangle; the hardware clear state (and the AGC register defaults) cover 16384x16384 with the
  window offset disabled. A title whose video and fade quads relied on the default state showed only its solid cyan
  clear colour, and another title's 3D attract scenes were black.
- **Adaptive mutexes refuse a relock by their owner** (EDEADLK, as error-checking ones do in the BSD thread library).
  Kyty counted the relock as recursion, so a UE4 title that relocks an adaptive mutex and unlocks it once kept it
  held while its game thread polled, and the worker threads that had to finish the polled work blocked forever.
  Normal mutexes keep the recorded recursion behaviour.
- **AGC jumps and predication.** `sceAgcDcbJump`/`sceAgcAcbJump` (INDIRECT_BUFFER with CHAIN) and their GetSize,
  `sceAgcSetPacketPredication`, and SET_PREDICATION with the wait hint, which never skips a packet here because the
  command processor runs ahead of the GPU work that writes the result.
- **Images a UE4 title creates at boot:** 8_8_8_8_UINT and 16_UINT storage images, a BGRA view of an RGBA8 UNORM
  storage image, one-element volumes in any swizzle mode, SW_256B_S (tile mode 1) single-level textures, and a
  storage image created over a smaller sampled texture (linked; it seeds from guest memory).
- **AvPlayer frames use 256-byte rows** in both NV12 planes, the row alignment GPU linear surfaces require; a UE4
  title copies its movie frames with that pitch, and tight rows produced the image repeated about fifteen times
  across the screen. The extended frame info reports the padding as `crop_right_offset = pitch - width`: a Unity
  title sizes its movie texture by the pitch and crops by that field, and with a zero crop the 128 padding columns
  of a 1920-wide movie showed as a green bar (zeroed NV12). `sceAvPlayerGetVideoData` (no pitch in its frame info)
  keeps rows of the visible width.
- **Linked read-only buffers retire one at a time.** Overlapping read-only storage views are linked to each other
  and to the large buffers they alias. Retirement used to free a linked buffer only together with its whole linked
  component (at most 64 nodes, all idle); a roguelike's per-frame views joined one component that also held its
  always-live buffers, so none was ever freed: live objects grew from 98 to 22,064 in 80 s and the frame rate fell
  from 33 to 11. A read-only buffer owns no content (its bytes are guest memory or a copy of a peer, and a pending
  GPU write keeps an object writable), and freeing it drops both link directions, so each one idle for 120 frames
  now retires alone. The same title then holds about 100 live objects and a steady rate over 220 s.
- **Standard 4 KiB arrays use the tiled pitch.** A SW_4KB_S 2D-array texture took the linear row rule (256-byte rows,
  or the T# word-4 pitch), so a 1115x306 RGBA8 slice was laid out with pitch 1152 and its 32-row blocks reached
  0x168000 bytes past a 0x160000-byte guest mapping; the detile read faulted. Hardware applies a custom pitch to
  linear surfaces only, and the canonical pitch (1120) gives 0x15E000, exactly what the title mapped. 2D textures
  already used it; arrays now do too.
- **Descriptors behind shader resource table pointers.** A pixel shader whose SRT holds three 64-bit pointers loads
  its S# from `ptr0+0` and its T# from `ptr2+0` (`s_load_dwordx4/x8` with a constant offset). Such loads, while the
  pointer still holds its user-data value and every consumer is an image or sampler operand, are materialized like
  the extended-user-data (EUD) loads: the table is snapshotted at draw time (two equal reads) and the S_LOAD is
  rewritten through the same PC-keyed mapping. Buffer descriptors behind those pointers keep their existing paths.
  The emitter now takes a mapped load's table offset from its mapping, so a nonzero SMEM immediate also resolves.
- **GLOBAL loads.** `global_load_dword{,x2,x3,x4}` (FLAT encoding, GLOBAL segment) read a per-lane 64-bit address (a
  VGPR pair, or a 64-bit SGPR base plus a 32-bit VGPR offset) plus a signed 12-bit offset through the guest device
  address table, which imports every GPU-visible guest mapping. Any other FLAT/SCRATCH/GLOBAL opcode still stops
  at decode.
- **`sceSslGetCaCerts`/`sceSslFreeCaCerts`** return an empty CA list for a live SSL context (the emulator exposes no
  system certificate store; verification against an unknown issuer then fails as it would). An Epic online SDK
  module calls it at boot.
- **`__powisf2`** (float to an integer power) is exported next to `__powidf2`; both now square and multiply, as the
  guest's compiler runtime does, instead of calling `pow()`.
- **SGPR copy provenance.** The scalar dataflow that told which SGPRs still hold their user-data value now tracks
  which user-data word every SGPR holds at each instruction through S_MOV_B32/B64 copies (a must-analysis over the
  decoded CFG; the old predicate is its "holds its own word" case). A compute shader that moves a V# out of its
  user-data position, or builds two V#s that share their stride, record and format words (s1..s4 and s0 plus
  s2..s4), gets one storage binding per distinct descriptor and register, and the V# registers are written with
  that binding's metadata right before each buffer instruction.
- **More descriptor loads behind SRT pointers.** One `s_load_dwordx16` holding two T# (or an 8-dword load holding two
  S#) is split into descriptor blocks, each analysed as its own load of the same PC; several mapping records of one
  PC are valid when their dword ranges are disjoint. A descriptor loaded before a conditional branch and consumed
  after it is followed across the branch (the scan stops at the first clobber in program order and at unconditional
  branches). The EUD collector keeps its straight-line scan.
- **Scalar data through guest pointers in pixel shaders.** A pixel shader S_LOAD that neither the EUD path nor a
  descriptor mapping resolves (for example two dwords of an SRT-pointed table into VCC, then a load through VCC) now
  enables guest device addressing for that shader only; descriptor loads stay on their bindings, so the earlier
  slowdown of marking every pointer-loading pixel shader does not return. VCC is accepted as a 64-bit S_LOAD base.
  The constants these lowerings name are declared up front; a missing one made the SPIR-V refer to an undefined id.
- **Typed buffer loads through an unbound V#** (a descriptor loaded at a runtime offset, as a vertex front with
  bindless vertex streams does) read through guest device addressing with the formats the bound lowering decodes:
  32-bit components (single component only for formats 20/22/36/39), R16G16_FLOAT and R16G16B16A16_FLOAT; other
  formats read zero, as the bound path leaves them unread. The raw and typed variants share one address helper.
- **Fixed direct maps replace what they cover.** `sceKernelMapDirectMemory` with MAP_FIXED (0x10) and without
  MAP_NO_OVERWRITE (0x80) replaces the mappings in its range, as the BSD kernel and other PS5 implementations do. A
  .NET runtime commits its GC heap by allocating, mapping at a fixed address inside a 256 GiB reservation and
  releasing; after a release it maps a new 256 KiB view straddling two released 256 KiB views, which Kyty refused
  with EBUSY (it replaced only an exactly equal released view), and the title died with OutOfMemoryException. A
  released view is now dropped whole (its pages may already back another allocation) with its parts outside the
  new range returned to the reservation, a live view is replaced when it lies inside the range, and the range
  becomes a reservation the normal fixed-map transaction consumes.
- **Partial unmaps.** `sceKernelMunmap` accepted only an exact mapping or a range inside one reservation block. A sandbox
  title's allocator reserves a large block, maps an aligned part, decommits pieces by reserving over them and later
  frees a span covering many blocks; it also maps over part of a live mapping. Following BSD munmap, a range now
  unmaps every mapping and reservation part it covers, a direct mapping crossing the range edge is cut (the host
  unmaps the page-aligned part and the mapping, its physical offset and protection blocks split; the GPU side
  releases only the cut range), and a fixed map cuts the part of a live mapping it overlaps. Cuts run before any
  other change, so an unsupported cut leaves the state untouched. The host split is implemented on Linux; on
  Windows the views and placeholder reservations are not split yet and such unmaps fail as before. A flexible
  mapping crossing the range edge is still refused.
- **What every guest thread waits on.** The blocking HLE waits (contended mutex, condition, semaphore, event flag,
  event queue, sleep, join) record their kind, object and guest return address in the thread for their duration;
  the agent `threads` tool reports them for the main thread too (it was missing from the list) together with the
  loaded module bases, so a freeze names the waiting code without a debugger. Only a contended mutex lock is
  recorded, so the uncontended path stays a single trylock.

Regression set after these repairs (run d406-d417, 90 s each, same host): GRIS 119 fps (104 before), Blasphemous 2
85 (70), Dreaming Sarah 195 (89), Let's Build a Zoo 202 (83), The Messenger 320 (269), Dead Cells 94 (82), JoJo 87
(54), ANIMAL WELL 22 (20), Formula Retro Racing back to its 3D views at about 110-120 fps in the race. A longer
Blasphemous 2 run reaches gameplay; its intro frames vary with load timing between runs.

Open blockers (one root cause each, none investigated past the point stated):

- The UE4 fighting title now reaches its startup movie in some runs. Open, not yet root-caused: runs end
  nondeterministically around 25 s, either on a guest `scePthreadMutexLock` of a null object plus 0x58 or in a
  stall; a 400x5x5 R11G11B10 volume in SW_64KB_R_X (tile 27) is not laid out yet. About 100 AGC entry points it
  imports (mostly `*GetSize`) are still missing and are added as they are called.
- The isometric action title now runs its whole observation window without a fatal (about 50 fps), but the frames are
  black: its vertex front loads V#s from descriptor tables at runtime offsets (`s_load_dwordx4 s[16:19], s[14:15],
  vcc_lo`) and fetches typed vertex data through them. The typed guest-address loads above cover the decoded
  formats; whether those reads return the sprite data (and which other path still yields nothing) is next.
- A beat 'em up (.NET with SDL_GPU) now runs its whole observation window: the GC heap commits succeed with the
  fixed-map replacement above. Frames stay black (one solid green) at about 11 fps; a fifth of the time is spent
  preparing the guest device address table.
- A first-person puzzle title stalls after one frame: its main thread sleep-polls inside the FMOD Studio module
  (`Sleep(ms)` wrapper at +0x2440) while the Studio threads wait on semaphores and an FMOD core thread sleep-polls;
  the asynchronous bank load or mixer it waits for never completes. Not root-caused yet.
- The arena fighter that froze when its attract sequence started now plays it (3D scenes at about 200 fps for the
  whole window).
- The remaining Unity title still stops 25-80 s in with a garbage V# in extended user data dwords 40-43 of its
  colour-grading pass (the same slot holds valid LUT parameters in earlier runs and in another Unity title); the
  bound span is now the full 272 bytes, so the remaining defect is the stale EUD contents.
- A sandbox title now passes its allocator's partial unmaps and fixed remaps and loads further (about 21 s); it stops
  on a pixel shader with a derivative fetch inside a lane-divergent loop, which needs the fragment wave transport
  the renderer does not select yet.
- A roguelike exits by itself (exit 0, no frame) without calling libc exit.
- A first-person puzzle title: stalls after one frame.
- AMPR `_04_00` counter and wait commands are still no-ops; most `sceAgc*GetSize` entry points are added only when a
  title calls them, from the size the matching Kyty builder writes.

### Title runtime repairs (2026-10-04, guest verified on seven of eight titles)

Scope: strict runs of the eight-title regression set on the reference host (Intel Xe, Vulkan 1.4, Native,
`KYTY_SHADER_OPTIMIZATION=None`, shader validation on, 300 s limit). Seven titles reach gameplay with no fatal for
the whole observation window (five Unity titles, a Construct title and a Haxe title); a sixth Unity title
still stops on an unmapped constant-buffer V# (open item below). Every repair has a focused
red->green test; 1,859 unit tests pass in one process; boundary, table, playable-regression and capture gates pass.

- **Interrupt context id.** `sceAgcDriverGetEqContextId` (`Zw7uUVPulbw`, name confirmed by NID hash) returns the
  `interrupt_ctx_id` of the ReleaseMem that raised a graphics event. The 8-dword ReleaseMem envelope carries it in
  its last dword; the command processor passes it to the interrupt label and the event delivers it as `data` (no
  title reads an EOP timestamp there). The guest indexes per-frame GPU timing slots with it.
- **Case-insensitive guest paths.** An IL2CPP player loads `Il2CppUserAssemblies.prx` that its package stores as
  `Il2cppUserAssemblies.prx`, once, and works on hardware. Mounted paths resolve each missing component to the
  only host entry equal ignoring ASCII case (`Path::MatchCaseInsensitive`); the exact entry wins, an ambiguous one
  stays unresolved, and missing tails stay verbatim so creation and ENOENT follow the guest.
- **VideoOut blank flips and GPU-queued flips.** Index -1 (`SCE_VIDEO_OUT_BUFFER_INDEX_BLANK`) presents a black
  frame (`WindowDrawBlank`) instead of being rejected or re-flipping the current buffer. A flip the command
  processor decoded counts as pending (`gcQueueNum`, included in `flipPendingNum`) until the flip queue owns it
  (`VideoOutFlipPending`); a guest that waits for `IsFlipPending() == 0`, flips blank and unregisters its buffers no
  longer races a flip still in its command stream.
- **Quiesce before the first submission.** A guest munmap of GPU-mapped memory quiesces the GPU; a command
  processor that never recorded has nothing to complete and no longer creates command buffers before the render
  context exists.
- **Malloc replacement initializer.** A Construct title declares `initialize` as a bare `ret`, so rax still holds
  the callback address; libc publishes the allocator regardless of the initializer's return value. The duplicate
  table scan and second `initialize` call carried by the libc runtime port were removed.
- **`v_nop`.** VOP1 `v_nop` carries no operands (its encoding fields are ignored), and the NGG passthrough proof
  treats it as an inert scheduling hint like `S_NOP`.
- **NGG front scalar spills.** A `v_writelane`/`v_readlane` pair with a constant lane that only moves a scalar
  through one VGPR lane is lowered to a private scalar slot; the fused-front body proof now carries the scalar's
  taint through that slot instead of refusing it as a lane exchange. Run-time lane indices stay refused.
- **Ported from `feature/reviewed-main`:** guest `.eh_frame` registration (`d0cdd035`), the libc runtime and C++
  exception helpers (`65edd72c`: `log10`, `log2`, `difftime`, `__cxa_*`, `_Unwind_*`, ...), and the proven SMEM
  span for dynamically loaded storage descriptors plus the readable-memory copy of tiny static spans
  (`290862c0`, `32a1b60c`). 69 further commits of that branch conflict with the current tree and are not
  integrated (list in the handoff).

Open: the remaining Unity title's colour-grading pass loads its constant-buffer V# from extended user data dwords
40-43, which the guest never writes; they hold two 64-bit guest pointers (deterministic across runs). The bind now
requests only the 208 proven bytes, but the base is not mapped, so the strict run stops (12-71 s). The September
binary binds the same slot because the address was readable in its memory layout. Intermittently the same title
also stalls at boot (main thread after `RequestThreadContext`, another thread in `KernelWaitEventFlag`). A
Haxe title shows a flickering element while walking and breaking objects (user report, no root cause yet).

### Reference workload runs d58-d96: stable 266 s, no fatal, still black (2026-10-04)

Scope: this section records what changed between the vertex-front proof below and run d96, what the strict run
now does, and the two open items. It is not a compatibility claim: every capture of the longest runs
(d92, d96; 10 frames each, frames 23-808) scores `entropy=0`, one quantized colour (RGB 0,0,0), so no menu,
input or gameplay is accepted. Host: Intel Xe (experimental KMD), Vulkan 1.4, Native resolution, Silent printf.

What the run does now (d92/d96): no fatal for the whole 265 s window; the guest reaches `StartLevel title`,
`Continue: worldmap` and `LoadLevelResources worldmap`; about 13.5k draws, 60.8k dispatches and 807 flips
(p50 frame 247 ms, about 4 FPS); runtime events only `gfx_stencil_frontier` and `low_entropy`.

Fixes in this chain, each with a focused red->green test (122 suites, 1,808 tests, 0 failures; boundary and table
gates pass):

- **Fused vertex front, body proof.** Scalar loads, wave-uniform branches, lane-bit selects and vertex fetch after the
  width-neutral prologue (`ShaderNggFrontBody`). A MUBUF address holds one VGPR per enabled part (`idxen` plus
  `offen`); the decoder's operand width over-counts `offen` alone. An SDWA compare or VOP2 with whole-dword
  selects and no sub-dword modifiers is the plain instruction (`ShaderComputeWaveSdwa*` predicates); any other
  select stays refused.
- **Fragment mask flow** (`ShaderFragmentMaskFlow`). Flow-sensitive mask provenance now runs on every pixel program
  (the whole-program pass alone cannot account for the WQM widen/restore write of EXEC). It handles divergent
  blocks, the kill pattern with an inert dead tail (now also the dedicated `exp null` valid-mask export, not only
  the MRT-null spelling) and loops: states are iterated to a fixpoint over back edges; a loop whose exit or repeat is
  decided by EXEC or a non-uniform VCC marks every scalar its body writes as diverged once control leaves it
  (lanes that stopped early ran a different iteration count on the host subgroup than on the guest wave);
  implicit-derivative fetches are refused inside such a loop. The refusal reason now names the flow's own
  construct and pc instead of the monotone pass's register-reuse artefact.
- **Sampled/storage aliasing.** `Gen5PickSampleSurfaceAliases` only accepts a surface that holds every mip level and
  array layer the T# views. Gen5 colour storage images (tilings 0, 5, 9, 27; 2D; several levels) are a true mip backing
  with every level the extent allows (`StorageTextureUsesMipBacking`, `StorageTextureMipBackingLevels`), one
  single-level view per level, and descriptors that differ only in BASE_LEVEL are the same object. Captured
  descriptors address level 6 of a resource whose T# says MAX_MIP = 5 (a downsample loop; the 480x270 linear
  chain's allocation of 0x151300 bytes holds exactly six levels plus padding, so the store lands in the slack after
  the allocation on the hardware). The image therefore carries the whole chain rather than failing or clamping the
  store onto a real level. Tile 9 is a different layout on Gen4, so the colour case is Gen5 only.
- **Diagnostics** (bounded, opt-in): the alias refusal, the storage-view refusal and `!create_all_the_same` now
  print the backing image and every overlapping parent with its parameters; the paired-wave compute refusal and the
  pixel transport refusal share `ShaderDumpGuestProgram` (`KYTY_TRANSPORT_DUMP`).

Why the screen is still black (evidence, not yet a proven sole cause): the skipped-draw report names a geometry
state the renderer does not model: GE stages `0x00002030` (merged ES/GS), ES and GS-back programs bound, legacy GS
zero, `max_vert=0x48`, `out_prim=2`, `ge_ngg=0x46`, `max_out=0xd8`, primitive/vertex group sizes 3 and 0x18
(`ShouldSkipUnsupportedGeShader` models only the `0x02002000` passthrough). The decoded GS-back program (540
instructions) is not a culling pass: it is a procedural geometry program that zero-fills LDS, barriers, fetches
through `idxen` buffer loads, computes transformed positions in repeated unrolled blocks guarded by `v_cmpx`/
`s_cbranch_execz`, writes vertices and packed primitive indices to LDS with a running count, and then exports.
The draw census after present 200 (20 unique pixel programs) contains no draw with depth testing enabled, which
is consistent with all mesh geometry being omitted. Implementing this stage is a separate feature (an NGG
geometry-shader execution model: workgroup-wide LDS, barrier, emit counters, host primitive assembly from the
emitted indices); the skip must not become fatal before that exists.

Open items:

1. **Merged ES/GS NGG geometry** (above). Suggested direction: model the merged stage as one host workgroup per guest
   subgroup that runs ES then GS with real LDS and barriers and writes an index/vertex buffer the host then draws,
   proven against the recorded GS-back listing; record each unmodelled instruction class rather than skipping it.
2. **Timing-dependent compute refusal.** One run (d95) stopped at 249 s with `paired-wave dispatch admission
   unsupported: mode=0x41 pc=0x5a8 reason=instruction DsWriteB32 is outside the paired compute-wave admission set;
   native-equivalence rejected pc=0x570 reason=lane-conditional branch is not a forward region`. Two later runs
   (d92, d96) completed the window without reaching it, so it depends on how far the slow world-map load gets. The
   program was not captured; the refusal now writes `cs_<checksum>.txt/.bin` when `KYTY_TRANSPORT_DUMP` names an
   existing directory, so the next occurrence is classifiable. It is the compute analogue of the loop support added
   to the fragment flow: a lane-conditional loop around an LDS write.

Known approximations and debt recorded by this chain:

- A colour mip-backed storage image seeds only level 0 from guest memory for the tiled layouts (no level layout is
  implemented for tiles 5, 9, 27) and is not re-seeded from a newer render target each frame, unlike a sampled
  `Texture` built from a surface parent (which is reclaimed when the parent is newer). A per-frame refresh is still
  needed for a chain whose level 0 is a render target.
- `ShaderParseMUBUF.cpp:53` sizes the vaddr operand from the raw field and ignores `idxen`; `SBarrier` is still the
  placeholder for roughly 659 unimplemented parser sites; the host has no demote-to-helper discard (only `OpKill`).
- The fragment flow merges taint by OR, so a register that is a mask on one path and a number on another is reported
  as a mask (a numeric use is refused, an EXEC assignment is accepted); it is sound for refusals but imprecise.
- `KYTY_DUMP_VIDEOOUT` writes present-source PNGs only for 8-bit and FP16 formats; the reference workload presents a
  10-bit format, so that dump (and the paired `KYTY_DUMP_RT`) produces nothing there.

### NGG passthrough front width neutrality (2026-10-03, CPU verified; strict run advances)

The first draw's vertex stage (GE stages `0x02002000`, an ES program fused with the
GS prologue, guest Wave64) used to stop at native-wave admission: the prologue writes
EXEC, so `ShaderUsesNativeWaveState` classified it `ExactSubgroup` and the Intel host
(subgroup range 8..32) has no 64-lane subgroup. The refusal was correct, not a GPU
fault. Two routes were open: an exact lane map for graphics (as `Paired64On32` is for
compute) or a proof that the prologue is width-neutral. The second is implemented.

`ShaderProveNggFrontLaneLocal` (`ShaderNggFront.h`) quantifies the existing
count-specific analyzer `ShaderAnalyzeNggPassthrough` over **every** launch a wave of
the guest width can carry: all ES vertex counts and all GS primitive counts in
`1..W` (W = 32 or 64). Per launch the analyzer proves the allocation payload, that
EXEC is exactly the low-lane mask of the relevant count at every retained
instruction, that `exp prim` forwards the hardware primitive unmodified, and that the
retained vertex instructions read no wave-derived scalar/EXEC/VCC and exchange nothing
between lanes. A constant mask correct for one launch (a plausible wrong shape) is
refused. `ShaderApplyNggFrontProof` then upgrades `ExactSubgroup` to `LaneLocal`
only when the decoded GE kind is `NggPassthrough` with a known word and no unknown
bits; the merged ES/GS form (`0x00002030`), legacy VS, partial or missing stage state
keep the generic verdict. The verdict is computed once per immutable cached program
and width (`ShaderNggFrontVerdict` inside `ShaderVertexProgram`), not per draw.

The host executes the scalar prologue for real, so the generator's initial `s3` was
made part of the proof: it is now `kShaderNggFusedFrontWaveInfo` (one ES vertex, one GS
primitive, `0x0101`), a launch inside the proved set (`static_assert`ed), replacing
the former `1` (zero primitives, which the analyzer rightly treats as invalid). The
constant is named `%uint_0x00000101` in the module.

What this does not establish: the renderer contract that the host assembles
primitives itself (the proof needs the decoded `NggPassthrough` shape and does not
replace a topology/launch proof); the Layer output contract the analyzer still reports
(`requires_layer_output_contract`); any program with branches, continuations, LDS,
memory or lane operations (all refused); floating-point exactness of the retained
vertex ALU on the host. Decoding gaps also fail closed: unimplemented SOP2 opcodes are
decoded as `SBarrier` placeholders (`ShaderParseSOP2.cpp` lines 64, 159, 171, 181,
185, and 659 "treated as" sites across the parsers), which no proof accepts but which
are silently wrong elsewhere; they remain unfixed debt.

Evidence: red->green `EmulatorNggFront` (7 tests; with the upgrade disabled the
admission test fails and the six refusal/plumbing tests pass), `EmulatorNggPassthroughProof`
(18), `EmulatorShaderExport` (15, now pins the proved `s3`), 120 separately executed CPU
suites: 1,768 passes, 0 failures. Boundary and table gates pass. Strict run d57 of the
reference workload: the native-wave refusal of d56 (2.5 s) is gone, inferred from the
shader dump order (d56 stops after the vertex and pixel modules; d57 goes on to compile
two more compute shaders); no present was observed. The real program's module stores
`%uint_0x00000101` into `s3` and keeps both 64-bit `s_lshr_b64 exec` writes. The run
ends after 2.4 s at a **new** frontier, a compute shader that writes a 3D storage image
(`ShaderSpirvImage.cpp:743`, `unsupported storage image declaration: ... 3D writable
images require a 3D storage declaration`; the storage bank is 2D-only, see
`ShaderPlanStorageImages`). No frame was presented; this is not gameplay.

Suggested direction for the next frontier: a typed 3D storage bank end to end
(descriptor plan and layout, `OpTypeImage ... 3D ... Storage` declaration and 3D
`ImageStore` coordinates, a 3D storage `VkImageView`, and barriers for volume
layouts). Its RED test is the existing rejection, so write the generator test first.

### Writable 3D images, linear volumes, fragment mask flow and the full vertex front (2026-10-03, CPU verified; strict run stalls)

Four blockers in a row stopped the reference workload after the vertex-width proof above. Each was
fixed at its cause, with a red test first (the admission or shape test fails with the change disabled,
its refusal tests keep passing). Strict runs d59-d65; each one got further than the last (2.6 s, 12 s,
29 s, then 67 s without a fatal).

1. **Writable 3D images** (`ShaderSpirvImage.cpp`, `ShaderSpirvGenerator.cpp`). `ShaderPlanStorageImages`
   rejected every 3D writable descriptor. The writable bank now has one shape per module (2D, 2D array
   or 3D, `ShaderStorageImagePlan::three_dimensional`); mixing shapes is refused with
   "separate shape banks"; packed-mip stores and the R32 atomic alias stay 2D only. The declaration is
   `OpTypeImage ... 3D ... 2 <format>` and `image_store` writes a three-component bounds-checked
   coordinate; a 2D-coordinate instruction on a 3D descriptor is refused. The sampled 2D types keep their
   own dimension. The resource side (3D image and view in `StorageTexture.cpp`, `VIEW_3D`) already existed.
2. **Linear volumes** (`Gen5TextureVolumeLayout`, now `Gen5GetVolumeTextureLayout`). SW mode 0 is admitted
   for any non-block-compressed element size, one mip: `linear_size = pitch * height * depth * bytes`,
   allocation rounded up to 256 bytes (T# base-address granularity), upload is a checked copy. The T# stores
   no slice stride for a linear surface, so this is the tight convention shared with the 2D linear
   estimate (`TileGetTextureSize2`), not a measured padding; only a GPU-side consumer has exercised it.
   Standard 4KB stays 4-byte only; every other mode and any mip chain is still refused.
3. **Fragment mask flow** (`ShaderFragmentMaskFlow`). A Wave64 pixel shader of the form
   `s_mov_b64 S, exec; s_wqm_b64 exec, exec; ... s_cselect_b64 vcc, exec, 0; v_cndmask; s_mov_b64 exec, S`
   (with the descriptor registers reused as `S` after a scalar buffer load wrote `vcc_lo` numerically) was
   refused because the whole-program mask proof is monotone: once a register ever holds EXEC it is a mask
   everywhere. The new proof is flow sensitive for straight-line programs (a numeric overwrite ends the
   taint, SCC is cleared only by a compare, a select under a non-mask SCC is mask algebra, EXEC accepts only
   masks) and validates the `save / s_wqm / restore` bracket (save area intact, no export or memory write
   inside, no memory write after the restore so helper lanes stay unobservable). It only runs when the
   monotone proof refuses, so earlier verdicts cannot change.
4. **Full vertex fronts** (`ShaderNggFrontBody`, `ShaderAnalyzeNggPassthroughPrologue`). The second draw's
   vertex program has the same wave-count prologue followed by scalar descriptor loads, a wave-uniform
   `s_cbranch_vccnz`, `s_cselect_b64 ..., exec, 0` feeding `v_cndmask`, and vertex fetch. The exact
   analyzer now has a prologue mode that stops where EXEC becomes the vertex-count mask and reports which
   scalar words still depend on wave-level data; the body is proved once by a forward flow walk (launch data
   never reaches a vector instruction, scalar-load address, branch condition or EXEC; mask words are lane
   bits; VCC branches need a comparison of wave-uniform values; forward branches only; vector sources
   defined on every path; no stores, atomics, LDS or lane exchange). `s3` is overwritten by a descriptor load
   in that program, which is why the kill sets matter. Both real vertex programs are proved at Wave32 and
   Wave64.
5. **Texture shape authority** (`ShaderGen5InstructionShapeAppliesToType`). A compute kernel read a plain
   2D T# (1920x1080 R11G11B10F, SW mode 27, depth 1) with an `image_load` whose DIM is 3D; the descriptor
   adopted the volume shape and then had no layout. The texture unit addresses by the T# type, so a volume
   DIM no longer turns a non-3D resource into a volume (the array-shaped reinterpretations that tests pin
   are unchanged). This assumes the third coordinate is zero or ignored for the 2D resource.

Diagnostics added: the volume-layout fatal now names the T# type, base array, usage, whether the shape came
from an instruction and the descriptor register; the protected launcher sets `KYTY_TRANSPORT_DUMP` for the
fragment-transport refusal (the decoded pixel program is written under `programs/`).

Evidence: 121 separately executed CPU suites, 1,789 passes, 0 failures; `EmulatorNggFront` (12),
`EmulatorFragmentMaskFlow` (9), `EmulatorGraphicsResources` (34), `EmulatorGraphicsState` shape test; boundary and
table gates pass. Run d65 (binary built from this tree): no fatal for 67 s, 7 draws, 33 dispatches, 70
submits, then the observer stops it because GPU work no longer advances for more than 15 s. Frame counter 1,
no present, phase `booting`; three bounded `gfx_storage_frontier` events (`r=metadata_only`, unknown address
source) and one `frame_stalled` warning. The threads snapshot lists 32 started guest threads and sync-wait
tracking was off, so the wait site is not yet identified. **This is a stall, not an accepted run.**

Debt recorded, not fixed: unimplemented SOP2 opcodes decode as `SBarrier` placeholders
(`ShaderParseSOP2.cpp` lines 64, 159, 171, 181, 185; 659 "treated as" sites across the parsers). The body
analyzer and the pure analyzer refuse them, but other paths treat them as barriers silently.

### First-draw metadata contract (2026-10-03, observation only)

Further bounded observations identify the rejected draw's color-control mode as
fast-clear elimination, with CMASK enabled and DCC disabled. AMD's documented
operation materializes the retained clear color into blocks marked fast-cleared;
the pixel shader's ordinary export is not that operation's attachment result.
The same-context host image is newly created with an undefined layout. An
existing expanded-image no-op therefore does not cover this instance.

The source-bound input integration build passes 119 isolated CPU suites
(1,742 passes and the same 16 environmental skips) and required CPU/static
checks. A subsequent read-only observation records a compute producer whose
production-decoded descriptor starts at the observed CMASK address, spans
4 KiB, and supplies four zero fill words. Its bounded program parse contains
the indexed vector store. Function returns and copied bytes do not establish
GPU completion, a whole-plane CMASK extent/equation, or CPU/GPU coherence.
The first observation stops at the diagnostic output quota before the original
admission failure; preserve it as partial evidence. Compact serialization keeps
the same quota and permits a later complete failure replay. It observes the
same 4 KiB requested and returned storage-buffer range and descriptor range,
then the normal post-dispatch marker for four workgroups of 64 invocations.
The following flush-event parser returns record GPU memory barriers, not host
fence completion. The same-context image is still undefined, and the replay
ends after 10.11 seconds at the unchanged native-wave refusal. Native input and
all six SPIR-V cache containers match the earlier complete-program build.
Subsequent audio edits remain outside this captured binary's qualification.

The inspected public GFX10 CMASK equation takes the **FMASK** swizzle as its
input. Its accepted swizzles do not include the observed zero FMASK swizzle;
the separate color tile mode cannot replace that input. The bound 4 KiB view
therefore still supplies no complete addressed-plane proof. This is a gap in
applying the public PC layout contract to the captured configuration, not a
claim that the console's configuration is invalid.
The mask prerequisite is separate: copied register inputs confirm shader mask
3 and target mask F. The strongest GFX10-applicable public FCE guidance programs
both CB masks to F; no mask-3 exception or mask-independent preservation rule
was established. A two-word export is not a four-channel CB-mask certificate,
and the GFX11 packed-export exception cannot supply the missing GFX10 rule.

The on-chip control observations decode to equal vertex/primitive capacities
of 64, output capacity 64 and amplification 1. These constrain a conditional
single-productive-wave argument but still do not exclude consequential
zero-role waves in the captured launch mode. A count-specific CPU proof and
conditional ordinary rectangle interior coverage are insufficient to admit
the actual metadata operation. Real surface ownership/materialization,
primitive execution and enabled observers remain prerequisites. Gameplay,
action response, the playable gate and sustained runs remain unestablished.
The zero-allocation alternative also remains unproved: a zero-role wave in a
productive subgroup does not own a separate empty allocation. Published
zero-output compiler paths suppress exports or perform explicit completion
operations; they do not establish that an extra zero request followed by
full-mask exports is inert in this mode.

The pinned Intel subgroup tutorial demonstrates subgroup partitioning. The
examined current and release vkd3d-proton implementations reject a required
64-lane width on a device whose maximum is 32; native-width heuristics and
size-query workarounds do not implement the guest's numeric EXEC/NGG contract.
Neither source authorizes replacing strict admission with a forced proof bit.

The portability correction in
`ShaderComputeWaveVulkan.cpp:ShaderSelectNativeSubgroup` and
`ShaderWave32FragmentNativeLaneExchangeSupported` accounts for the fact that absence of
`ALLOW_VARYING_SUBGROUP_SIZE` does not fix SPIR-V 1.6 stages to the device default.
The Vulkan [varying-size contract](https://github.com/KhronosGroup/Vulkan-Docs/blob/e4e53e4b31e13eeaee1ad99fb940aa72b2ec1b14/chapters/shaders.adoc#L1940-L1974)
allows varying sizes for these modules. A width-sensitive stage on a multi-size
device needs an explicitly supported required size, or a proof covering every
legal size. Final admission now reads the actual checked module version;
fragment preflight conservatively includes variation. Core property queries
retain genuine bounds even without the promoted extension name. A covered
multi-size selection explicitly means an unspecified width, not physical size
zero. Exact native64 and the composed neutral32/exact64 case remain valid;
exact64 on max32 still refuses. Paired-compute lowering is unchanged.

All six retained containers have actual SPIR-V 1.6 headers. The unchanged
independent 40-case CPU fixture goes from 22 failures to 40 passes against the
corrected production archive. Two owned modules differing only in the 1.5/1.6
version word validate under Vulkan 1.4. Ten focused regression tests and the
five selected adjacent suites pass. Full-tree qualification passes all 119
isolated suites (1,753 passes, the same 16 exact environmental skips) and the
required CPU/static checks, including the preserved input and audio changes.
Fresh binary-bound helper controls and the Vulkan 1.4 device preflight pass.
The strict replay reaches the unchanged guest64 vertex refusal after 10.33
seconds, with actual module version 1.6 and queried host range 8..32. Native
input, the 4 KiB storage-view observation and all six cache containers retain
their earlier identities. No new narrower NGG proof or gameplay result follows;
the playable runtime gate remains unavailable with exit 2.

Independent source review finds no new blocking issue in the eight-file
correction. It records a pre-existing conditional defect at
`GraphicsRenderPipeline.cpp:412-420`: defining `KYTY_FINAL` without
`ASSERT_ENABLED` makes `EXIT_IF` omit evaluation of the side-effecting subgroup
attachment call. That configuration is not established for the qualified run.
Before supporting it, move required attachment mutations outside disabled
assertions and retain an unconditional failure path.

### Vertex-wave admission frontier (2026-10-03, passthrough program captured)

The complete-program integration build binds Gen5 vertex-wave analysis, module identity and
both renderer compilation paths to one immutable complete program. A copied
front/back program is acquired under a mapping-generation lease; its stable
content fingerprint separates different continuations even when their front
checksum and wave classification agree. Map, continuation and debug changes
invalidate parsed-cache generations; already acquired draws retain their code.
Translator identity is 101; generated geometry remains 86.

The unchanged independent fixture changes from three failures to five passes:
back-only lane and numeric-EXEC reads require guest64, distinct neutral backs
compile separately, and neutral/nonterminal controls retain their behavior.
All 20 owned-program regressions pass, including emission after guest storage
release with Vulkan 1.4 module validation. The four-target build passes 119
separately executed CPU suites: 1,737 passes and the same 16 exact environmental
skips from 1,753 cases. The new suite and four adjacent suites also pass two
same-process iterations; required CPU/static gates pass.

Fresh binary-bound layouts, host-main checks and the exact Vulkan 1.4/Wayland
device preflight pass. The strict run ends after 3.68 seconds at the same vertex
guest64 admission refusal, before any observed present. Its complete shader,
draw, launch and output records match the preceding capture, including
known/written output primitive 3. All six cached SPIR-V payloads are unchanged
and validate offline; their cache keys use the new translator identity. The
captured vertex module's content fingerprint matches the separately observed
288-byte program. This is value agreement between observations, not atomic
guest-memory provenance. Gameplay, action response and the two sustained runs
remain unestablished.

The preceding qualified build corrects primitive-state output ownership and passes
118 separately executed CPU suites: 1,717 passes and 16 exact environmental
skips from 1,733 cases. Its 13 new producer controls and four adjacent suites
pass two same-process iterations. The unchanged independent 21-case fixture
now passes every case, compared with 19 failures before the correction.
Required CPU/static gates and both freshly bound native-device preflights pass.
Two strict observations end after about 4.03 seconds at the same vertex guest64
admission refusal, before any observed present. The first draw now has output
primitive **3**, explicitly known and written. The second trace observes the
actual indirect-register callback write 3 and the later report read 3 from the
same Context. Constructor-return to command-memory copying and complete
last-writer attribution remain unobserved; optimized-out data stays unknown.
The captured program and all six shader-cache containers match the earlier
build. These short observations do not establish gameplay or a completed
five-minute window.

The preceding extended-snapshot build passes 117 separately executed CPU
suites: 1,704 passes and 16 explicit environmental skips from 1,720 cases.
Its 18 conditional NGG-proof controls and 16 native-wave snapshot controls pass
two same-process iterations, as do adjacent GE/packet/state suites. Required
CPU/static gates, fresh binary layouts and the exact-device preflight pass.
The extended strict capture again stops at vertex wave admission after about
3.25 seconds. Its program bytes and all retained shader-cache containers match
the earlier capture. These are input observations, not an elapsed gameplay run.

Before the producer correction, four output words had explicit same-draw
known/written provenance:
VS output configuration `0x80`, position format `0x44`, output control
`0x01240000`, and GS output primitive **zero**. This disproves the hypothesis
that the reported zero was merely an unassigned reset field. The subsequent
producer correction below needs no hardware ignore-zero rule. The pixel-program
identity and
bounded effective interpolation/raster state are also retained; effective
state without assignment tracking is labeled separately from raw provenance.

The previous qualified diagnostic build passes 116 separately executed CPU suites:
1,684 passes and 16 explicit environmental skips from 1,700 enumerated cases.
All 14 native-wave snapshot controls pass two same-process iterations; adjacent
packet/state/admission/export repeat controls and required CPU/static gates
also pass. Its rebound production native32 probes compare 12,288 words with
observed fences and zero validation errors. Fresh binary-bound layouts and the
exact Vulkan 1.4/Wayland device preflight pass before the strict capture.

The first wave-sensitive vertex input is now captured: an NGG passthrough draw,
stage word `0x02002000`, three auto-indexed vertices with input primitive type 7, one draw
instance, zero user-SGPR count and zero LDS. Its 288-byte mapped program is
complete; both GS-back and legacy GS addresses are explicitly zero. The six
tracked raw registers are known and written. This is an earlier draw than the
retained merged `0x2030` failure, whose actual program pair remains uncaptured.
Bounded production parsing yields 26 instructions, all satisfying lowering
preconditions. The three cached Vulkan modules are byte-identical to the
previous qualified run. Strict execution still refuses vertex guest64 on the
host without native64, before any observed present.

The program derives allocation and packed EXEC masks from the system word's
vertex/primitive counts, forwards the primitive-input VGPR, then transforms
vertex/instance inputs without memory or lane-exchange instructions. That is a
candidate for an explicit independent-vertex proof, not permission to classify
all such vertex programs as lane-local. The current `s3=1` seed, physical host
lane numbering and empty primitive-export emitter do not implement its launch
contract. A proof must account for allocation, primitive forwarding, actual
logical lanes, every live scalar/mask dependency and any unprovided system bits.
Execution strategy and gameplay acceptance remain pending.

AMD's public PAL topology contract distinguishes type 7 (`2D_RECTANGLE`, the
bounding box of an arbitrary 2D triangle) from type 17 (`RECTLIST`, three corners
of an axis-aligned rectangle). Both consume three vertices. The existing shared
triangle-strip conversion is not evidence that their raster contracts are
identical; arbitrary type-7 inputs also refute a generic affine fourth-corner
rule. Keep the three guest vertices and any four-vertex host completion
separate. Same-draw output-control and raster
state were missing from the first snapshot; the qualified additive diagnostic
extension supplies the evidence above. An output or reset value without a recorded
assignment must not be promoted to a known raw register word.
The generation-qualified AMD LLPC enum assigns GFX10+ output 3 to `RECT_2D`
and output 4 to `RECTLIST`; the local secondary GFX10 register table instead
names output 3 `RECTLIST`. Preserve that discrepancy rather than relying on a
same-spelled name. The primary references are PAL
[`palCmdBuffer.h`](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L87-L107)
and LLPC
[`AbiMetadata.h`](https://github.com/GPUOpen-Drivers/llpc/blob/40cb8d95ad8d6f7f1652e3fd47d39667594cce08/lgc/include/lgc/state/AbiMetadata.h#L895-L902).
The corrected output-mode selection is recorded below; exact raster
observables still require evidence before this class can be admitted.
The producer audit identified an unconditional metadata copy in
`GraphicsCreatePrimState` (`Graphics.cpp`). Three further captures of the same
qualified executable each record 31 complete constructor returns, including
13 calls for the captured vertex program with input type 7, no hull shader,
no GS owner, and output zero. The independent owned-memory controls reproduce
19 failures out of 21; the two explicit-GS controls pass. These observations
establish the constructor defect, while final consumer/last-writer attribution
remains unresolved. The qualified source correction
selects explicit GS output when `GS_EN` is set, HS output for tessellation
without GS, and input-derived output otherwise (including 7→3 and 17→4 on
GFX10+). A zero output is not a sentinel: explicit point output survives.
Fixed guest metadata and requested output ranges are validated before
publication; unsupported ownership/input combinations refuse explicitly.
This correction alone does not establish vertex-wave or raster equivalence.
`ShaderNggPassthroughProof` is a CPU-only, count-specific prerequisite. Its
18 tests pass, and it accepts the complete captured 26-instruction program
under explicit guest64/three-vertex/one-primitive premises, retaining the 12
original vertex ALU/export/end instructions. It tracks known scalar bits without choosing unknown launch
values, checks allocation/primitive identity and every live vertex mask, and
returns original indices for scalar-free independent vertex instructions. Its
result does not authorize a renderer strategy: the caller still owes complete
program binding, real launch inputs, primitive assembly and output-state proof.
The current native admission path does not consume it. This result is
conditional on those input counts and identities; it does not establish the
actual initialization or the raster contract.

A partition-independent proof cannot simply replace the missing launch
contract. The captured mask construction has a full active mask at count zero;
its positive-count algebra does not establish which waves hardware can issue.
Allocation payload equality also does not prove allocation ownership or
retirement. Public AMD descriptions scope `THDS_PER_SUBGRP` to fast launch
([PAL ABI](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/g_palPipelineAbiMetadata.h#L910-L915))
and describe NGG partial-ES-wave flushing
([PAL pipeline](https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9GraphicsPipeline.cpp#L1259-L1311)), but do not supply the required
population/allocation rule for the captured ordinary passthrough mode. A
same-mode authoritative contract or fully bound target-hardware observation
is still needed. The available Intel host can validate an eventual translated
implementation, but cannot observe native AMD-initialized launch registers.
Primitive connectivity, type-7 sample coverage and enabled observers remain
separate requirements; strict admission stays in place.

The EXP producer correction passes all 15 focused tests, including 73 complete
Vulkan 1.4 module cases, and the full source-bound build: 115 CPU suites,
1,670 passes and 16 explicit environmental skips from 1,686 enumerated cases.
The required CPU/static gates pass. Both production native32 numerical probes
pass all 12,288 compared words with observed fences and zero validation errors.
Fresh host-main layouts and exact Vulkan 1.4/Wayland device preflights pass.

Strict execution advances past the EXP precondition to
`GraphicsRenderPipeline.cpp:CreatePipelineInternal`'s native-wave admission:
a vertex guest64 program has `ExactSubgroup` proof and requested width 64,
while the tested host provides subgroup32 and cannot select native64. The
reported refusal PC zero is an unset diagnostic field, not an identified
offending instruction. The vertex analyzer currently returns after its broad
wave-state predicate; its more precise mask-use analysis is Pixel-only.

The retained cache contains one vertex candidate whose identity has the same
guest/proof/requested widths. Its generated source includes the existing GS
prolog's fixed `s3=1` and scalar shifts writing architectural EXEC. All three
unique retained modules validate offline. This does not prove width neutrality
or a correct merged launch. The bounded input capture above supplies the actual
first draw counts and mapped program; merged-GE capture and its launch contract
remain separate work. Native-wave admission stays strict, with gameplay and
action-response acceptance pending.

The corrected program owner retains a front view for the existing resource
ABI and a complete view for wave analysis and compilation. The parsed cache
retains at most 256 entries and 64 MiB of accounted IR; larger/debug-bearing
owners bypass retention. Code replacement must participate in the mapping
lifecycle: direct in-place writes without a new generation are not detected on
cache hits. Resource metadata remains shallow, and front/back copying is not
atomic against unsynchronized guest stores. The native report remains a
separate live observation rather than an owner snapshot.

Two compiler leads from the reference follow-up do not demonstrate new defects
in the inspected Kyty paths. The B32/B64 SAVEEXEC templates read their source
words before saving EXEC to the destination; the scalar mask adapter preserves
that ordering and packed representation. The generic paired emitter explicitly
emits both banks and refuses a failed rewrite, rather than skipping the high
bank after an unsuccessful low-bank value lookup. These are source-level
exclusions of specific external defects, not new GPU or launch-equivalence
results.

A source-proven integration gap remains in `DepthStencilCopy.cpp:408-475,617-639`:
the specialized guest-geometry copy path creates a guest vertex stage without
the normal pipeline's native-wave selection, subgroup-capability checks or
required-size attachment. A wave-sensitive VS on that type-4/type-6 route needs
shared admission/attachment logic and a focused reproducer before qualification.
This route is distinct from the captured type-7 draw; immutable program
ownership alone does not close it.

The successful VS probe write in `Shader.cpp` passes a null generated-source pointer to
`ShaderProbeWrite`, recreating the same file and losing its earlier generated
text. Preserve phase evidence with the bounded diagnostic writer; the ordinary
file shader log retains both phases but lacks an internal session quota.

A bounded opt-in native input report now covers the earlier admission seam,
with actual draw arguments and separate front/back program copies; the qualified
capture is recorded above. Its control-character fixture exposed a reader limitation in
`Core/JsonReader.cpp:Json::parse_string`: every `\u` escape is rejected,
including correctly escaped control characters. That fixture uses the existing
Unicode-capable agent parser to validate the unchanged full report and decoded
fields. General Core JSON Unicode support remains separate debt; the native
writer must continue emitting valid JSON escapes.
Same-process GraphicsPackets repetition also exposed process-lifetime
`ShaderInit` in `AcceptsComputeShaderWithoutWorkgroupId`'s parent fixture.
Initialization, mapping, parse assertions and cleanup now belong to its
isolated child; the runtime's one-initialization guard remains intact.

### Export operand-tail correction (2026-10-03, CPU and strict advancement verified)

The source-bound revision-99 build completes 114 separately executed CPU
suites: 1,655 passes and 16 explicit environmental skips from 1,671 enumerated
cases. GraphicsState, SaveData, diagnostic persistence and translation-cache
suites also pass two same-process iterations. Diff, emulator boundaries,
graphics tables, four Python gate suites and three CPU diagnostics-integration
modes pass. The native agent is included in the four-target build identity.
The actual playable gate remains unexecuted.

Both production native32 numerical probes were rebound to those final archives
and pass all 12,288 compared words together, with two observed fences, no
exhaustion and zero validation errors. Fresh binary-bound layouts, host-main
and exact Vulkan 1.4/Wayland device preflights pass. These certify that build,
not the subsequent parser correction.

The first strict guest run then stops before GE capture, at
`ShaderSpirvGenerator.cpp:GenerateSource`'s lowering precondition for a
one-source primitive export. `ShaderParseEXP.cpp:shader_parse_exp` initially
decodes four VGPR operands, then reduces `src_num` without clearing the unused
tail. A sanitized probe linked to the retained production archives reproduces
the refusal; its four-source position-export control passes. The correction
belongs at the parser's final operand construction, preserving the selected
active sources and strict admission. Translator identity 100 separates this
correction from earlier cached modules. The verified results and next strict
failure are recorded above. Merged-GE execution, gameplay/action response and
the two sustained gameplay runs remain unverified.

Related EXP debt remains in `ShaderSpirvVector.cpp:Recompile_Exp`: parameter
exports with EN other than `0xf` are parsed as four physical slots, but their
emitter writes the full vector without applying EN. Add evidenced component
accumulation or precise unsupported admission with value controls. The existing
PRIM path only checks `gs_prolog` and emits no primitive operation; its fixed
prolog initialization and complete-module validation cannot prove merged-GE
launch, allocation or primitive output. The operand-tail correction does not
resolve either contract.

### Integrated contract corrections (2026-10-02, intermediate evidence)

The earlier 1,412-test result below belongs to its earlier binary. Subsequent
arithmetic, architectural mask, resource, lifecycle, publication and I/O changes
use translator identity 99. All three affected targets now link. The latest
completed build passed 1,655 tests across 114 separately executed suites, with
16 explicit platform/runtime skips out of 1,671 enumerated cases. Boundary and
graphics-table gates and the four runner/capture/matrix/playable script suites
also pass. These results bind that completed build, not later fixture edits.
Newly linked suites must appear in the executable's actual test enumeration.
Death tests default to re-execution (`threadsafe`) because earlier threaded
suites can leave locks unsafe to inherit through a plain fork.

Four bounded historical READFIRSTLANE consumer controls have now executed on a
full 32-lane compute subgroup with Vulkan 1.4 validation. Each compares 6,144
words against an independent integer oracle. The packed-bit consumer passes
with packed EXEC and fails with lane-local predicates; the nonzero-predicate
consumer has the opposite results. Both failures retain residual EXEC and
explicitly report exhaustion after exactly 64 shader iterations (963 mismatched
words and 192 exhausted lanes per failing case). All four submissions reach
observed fence completion with zero validation errors; the matching controls
have no mismatched words. The allocator count/capacity guard stays at 75,000,
and the replay stays within the two-GiB, 45-second resource envelope.

Those four historical controls preserve only the consumer instructions. Separate
native32 probes now pass for current production scalar-written and CMPX-derived
EXEC: each compares all 6,144 readback words with zero mismatches or exhaustion,
an observed fence, and zero Vulkan validation errors. The complete generator
supplies mask production, save/restore, comparison, selection and depletion; the
external test scaffold still supplies the fixed 64-iteration loop. Complete
native32/native64 fixture modules assemble and validate, but native subgroup64
is unavailable on the tested host. This proves neither wave64-on32 fragment
equivalence nor the cause of the retained game device loss. Presented gameplay,
reproducible action response and the required sustained runs remain unverified.

An initial CMPX32 replay exposed a harness error: its nonempty test included
the ignored upper EXEC word, and its oracle assumed the scalar-written numeric
high word was zero. All 576 mismatches were saved/restored high words or the
empty wave's count/hash. The corrected harness follows RDNA2 §3.3: wave32
activity ignores upper EXEC while scalar numeric copies remain separately
checked. The original failure is retained; no output fields were discarded.

The GE decoder now preserves exact register words and assignment provenance.
The skipped-state report keeps ES, GS-back and legacy pseudo-stage identities
separate; unknown raw words serialize as null, not an observed zero. An opt-in,
metadata-bounded snapshot can retain the actual skipped programs. Classification
of merged ES+GS is not implementation of its launch, shared LDS, allocation or
primitive-emission contract.

Publication debt remains explicit in `Graphics/Objects/Label.cpp`,
`LabelStoragePublication::Acquire/Copy`, and `Objects/GpuMemoryCreate.cpp`:
an ordered storage writer recorded before label completion cannot reacquire
the newly completed publication while its backing is still in use. Closing
that case needs per-command writer coverage and versioned staging/backing
ownership; queue order alone does not identify each changed byte. Clipped or
unaligned synchronization words remain excluded. Whole-word compare/exchange
protects observed intervening CPU stores, but cannot detect a complete CPU ABA
that finishes before publication without an observed mismatch. The page-cache
copy counters also still include excluded bytes. These limitations are not
resolved by the new quiescent upload/publication controls.

The first integrated CPU batch passes 107 lifecycle/I/O tests, including all
31 descriptor/stdio tests, five AMPR read/error tests, and 46 dirty-tracking
tests. Fault-report persistence and the formerly unlinked suites also pass
same-directory repeats. The shader batch exposes shared admission rejecting
canonical parsed instructions and duplicate image-type declarations; those
failures were corrected and rerun. The GE decoder's 14 controls, resource
suite's 28, mode-propagation suite's ten, native-wave admission suite's 16,
architectural-mask suite's 16 and lane-execution suite's four now pass.
All 12 arithmetic tests pass, including 291 complete-module validation calls
and value-level controls; all 17 LDS tests pass. Wider suite execution exposed
an unsampled MIMG decoder source-tail defect and obsolete mask fixture
expectations. Their producer/fixture corrections pass, including all 26 MIMG
tests and the complete GraphicsPackets suite. Subsequent fixture-isolation
verification and the first native failure are recorded above.

The SaveData mount death-test fixture also removed empty ancestors through
`Core::File::DeleteDirectories`, deleting the temporary root needed by a later
translation-cache test. It now uses a parent-owned unique directory inherited
by the child, retains the parent-side persistence assertion, and removes only
its owned tree. Both SaveData and translation-cache suites pass afterward.
Same-process repeated suite execution then exposed a second fixture issue:
the CommonDialog initialization contract is process-lifetime, while four
graphics resource fixtures retained heap registrations or reused already
completed submission IDs. The dialog probe now runs in a fresh child; graphics
fixtures use monotonic mock submissions and scoped owned-range cleanup.
Four adjacent topology fixtures also freed host arrays before unregistering
their GPU heaps; later allocator address reuse exposed that dangling state.
Scoped teardown now unregisters those owned ranges before releasing the arrays,
and the complete repeat batch passes. GoogleTest's `result="skipped"` XML attribute must be counted as
an omission even when no nested `<skipped>` element exists.

Scoped stdio-buffer leases now cover the registered HLE/native bridge and
normal `CloseAll`; all 31 descriptor controls, including 14 new buffer/export/
capacity cases, pass on the integrated binary. Remaining boundaries: raw global flushes in Core's
`DbgAssert.cpp`/`DbgAssert.h`, test-framework fatal paths and skipped teardown
can bypass those leases. Review exit ordering or restrict generic diagnostics
to host-owned streams without adding a Core-to-Emulator dependency. Windows
CRT append-state mutation still returns an unsupported error; alternate-libc
buffer retention and Windows/macOS execution remain unverified. `Errno.cpp`
still has process-global guest errno storage, and the reserved standard
descriptor policies remain incomplete POSIX redirection/dup/fstat behavior.

Image-module integration now reuses identical sampled/storage image types and
preserves their descriptor bindings. Remaining numeric dispatch debt is in
`ShaderSpirvImage.cpp`: mixed float/uint sampled bindings with array/3D resinfo
paths, and mixed-numeric gather, still lack the per-descriptor numeric branch.
The homogeneous uint shape controls do not cover those cases. Add exact
numeric dispatch and value controls, or precise unsupported admission before
emission; type-uniqueness validation alone cannot prove the sampled view.

Samplerless MIMG decoding now omits the unused sampler operand at its producer,
and admits only zero SSAMP pending evidence for nonzero values. Broader encoding
debt remains in `ShaderParseMIMG.cpp`: its UNRM guard rejects one even on store/
atomic forms whose ISA contract specifies it; A16/D16 and unidentified word-zero
bit 6 / word-one bits 29:26 lack complete validation. The new operand-tail
controls cover the currently admitted flag subset. Complete the evidenced
per-opcode flag/type contract before widening these forms.

Raw-register coverage remains incomplete in `GraphicsRunJmpTables.cpp` and
`GraphicsRunOpParsers.cpp`: individual VS RSRC1 writes and direct PS RSRC1
`SET_SH_REG` starts lack table entries. The generic handler warns about a null
parser and then calls it. Packed setup/update and the existing PS indirect
path carry the recorded overflow mode, but do not cover those missing packet
forms. Add bounded shared full-word decoders and packet controls, or terminate
with an explicit unsupported diagnostic before the null call.

### Pad, keyboard and mouse record contracts (2026-10-03, CPU verified, not gameplay)

An independent native-homebrew write-up of the input libraries (GPL, console
firmware 6.02; a lead, not a dependency) agrees field by field with our 120-byte
`ScePadData`, the 96-byte keyboard record and the 40-byte mouse record, and
differs in four places that the HLE now follows; shadPS4's `scePad` sources
(read locally, behavior only) corroborate the first three.

- `ScePadControllerInformation` is 28 bytes: the structure ends in `reserve[8]`,
  which ours lacked, so the trailing bytes of the guest buffer were never
  written and `scePadGetControllerInformation` did not clear the structure at all.
  The extended record embeds it, which makes it exactly the 0x40 bytes its
  comment already claimed.
- `scePadReadState` and `scePadRead` filled individual fields only, so padding,
  the extension unit and the reserve kept stale guest bytes, and the native
  `connected` (0x4C) reads as a 32-bit boolean. Both now clear each record first.
  They also continued after a null pointer, a bad handle or a capacity outside
  1-64 (the guest's pointer was then dereferenced); they now return `0x80920001` or `0x80920003` like the rest of the pad entry points,
  and so do `scePadSetVibration`, `scePadSetLightBar`, `scePadResetLightBar`,
  `scePadSetMotionSensorState` and `scePadGetControllerInformation`.
- Acceleration is in G, not m/s^2. The virtual pad reported 9.8 from
  `scePadReadState` and 0 from `scePadRead`; both now report 1.0 on the axis the
  HLE already used. The axis is not verified against a physical DualSense, and a
  real sensor path (host gyro/accelerometer scaled to G) is still unimplemented.
- `sceMouseRead` sized a `memset` from the guest's capacity with no bound; it is
  now limited to 64 records (the documented loss-resistant drain) like the
  keyboard's 16.

`sceAudioOutOpen` reports an unknown format as `0x80260007` (the host layer
already refused it, but the guest saw a full port table), and
`sceAudioOutSetVolume` and `sceAudioOutGetPortState` return `0x80260004` for a
null pointer instead of dereferencing it.

Layout assertions pin all four records at compile time. The unit tests fill the
guest buffers with a sentinel and prove that each read writes exactly one complete
zeroed record and nothing beyond it; they fail against the previous
`Controller.cpp` (50, 50 and 19 assertions, and a crash on the null paths).

### Debt resolution and compute-wave contracts (2026-10-02, CPU verified, not gameplay)

The specific debt in that working-tree review is closed, and the 56 recorded
failures (55 compute-wave tests and one GETPC test) are explained and fixed. Nothing here ran on a
GPU or against the guest. Evidence: 91 unit suites run one at a time under a
resource scope (1,412 tests, 0 failures, 8 environmental skips), `git diff
--check`, `check_emulator_boundaries.py`, `check_graphics_tables.py` (13 tables)
and `test_kyty_playable_regression.py` (its guest-root case skips honestly).
Translator 98 invalidates every cached module because admission changed.

Added fallback markers. `git diff -U0 -- source` now adds no "condition ignored
(continuing)" line and no `TODO() check` comment. `EmitMovrelMove` and the
`V_LDEXP_F32` emitter return false for operands they cannot lower, and the
generator then refuses the shader (`shader emitter missing`, exit 65) instead of
emitting invalid SPIR-V; the `IMAGE_SAMPLE_LZ` dmask 0xb type checks are one
refusal. The decoder landmines they sat next to were real: SOPK `S_ADDK_I32`
(15) and `S_CMOVK_I32` (2) fell to "treated as SBarrier (continuing)" and were
silently dropped, while the undefined SOPK opcodes 17 and 20, the hardware-
register opcodes 18, 19 and 21, and SOPC opcode 16 (RDNA2 has no `S_SETVSKIP`)
could be mis-decoded. `S_ADDK_I32` now decodes as `S_ADD_I32 D, D, signext(imm)`
(signed-overflow SCC) and `S_CMOVK_I32` as `S_CMOV_B32` with an inline constant,
per the RDNA2 ISA SOPK table; the rest are `KYTY_UNKNOWN_OP()`.

Diagnostic dumps. `KYTY_TRANSPORT_DUMP` and `KYTY_PIPELINE_FAIL_DUMP` share
`DiagnosticDump.h`: the directory must already exist and is never created; the
file name is a fixed component of `[A-Za-z0-9._-]`, at most 96 characters and not
starting with `.`; the joined path must fit in 1,023 bytes without truncation;
the file is created exclusively (an existing file, symlink or FIFO fails); each
file is capped at 16 MiB and the process at 64 MiB, and a dump cut by a cap is
reported as truncated. `StorageTexture.cpp` prints the swizzle with a 64-bit cast
matching `PRIx64`.

Why the compute-wave tests failed. They were written for the per-opcode paired
allowlist that commit 704f4ad3 replaced, and they predate this working tree: the
layout and runtime sources and tests they exercise are unmodified against HEAD.
Each failure was classified by probing the real analysis and emitters, not by
loosening assertions:

- Rejection moved from analysis to emission. The "first unsupported
  instruction" sentinel was `S_GETPC_B64`, which the paired analysis now admits;
  the tests use `v_interp_p1_f32` (`0xc8000001`, pixel-only), which stays
  outside the compute set, and name it `VInterpP1F32`.
- Layout: a trailing partial wave is admitted (the missing lanes stay
  inactive), so the wave count is `ceil(invocations / 64)`. Runtime: when the
  paired layout is refused but the per-lane route is valid, the plan is
  `Supported` with `native_equivalence_required`; Gen5 `USE_THREAD_DIMENSIONS`
  converts counts with a checked ceiling division, so a zero local size is
  `InvalidLocalSize`.
- Classification: the specialized paths (banked ALU, scalar mask, scalar shift,
  scalar copy, ternary ALU) claim only their exact tuples; anything else falls
  to the generic per-lane lowering, so the tests assert what each path claims
  rather than `Unsupported`. `V_CNDMASK_B32` neg/abs are lowered in ISA order by
  `EmitCndmaskData` and stay on the banked path.
- Buffer loads: `ShaderAnalyzeComputeWaveVectorBufferLoad` and
  `ShaderAnalyzeComputeWaveScalarBufferLoad` remain the descriptor-provenance
  proof of the specialized synchronous route and still refuse unproven
  descriptors; `AnalyzePairedWaveCode` then falls back to the generic load. Both
  layers are asserted. The compare emitter evaluates both banks' predicates
  before storing the mask pair, so a mask destination that overlaps an SGPR
  source is lowered read-before-write instead of rejected.
- Control flow and barriers run through the block dispatcher
  (`ShaderSpirvBlockDispatch.cpp`), which resumes each guest wave at its own
  barrier phase, so branches, back edges and barriers in arms are admitted.
- Retired, not repaired. `ShaderAnalyzeComputeWaveLdsAccesses`,
  `ShaderAnalyzeComputeWaveLdsSafety` (both source files) and
  `ShaderAnalyzeComputeWaveWaitcnt` had no caller outside their tests after
  704f4ad3 (every admitted memory operation completes at its own PC and LDS
  accesses take the ordered generic path), and an unused `lds_safety` stub in
  `AnalyzePairedWaveCode` is gone. The exact-tuple predicates that
  are still used (`ShaderComputeWaveLdsInstructionSupported`,
  `ShaderComputeWaveIsExactWait`, `ShaderComputeWaveIsLgkmZeroOnlyWait`) stay.
  Their descriptions in the LDS paragraph below are history.

Independent of this list, four `EmulatorGraphicsPackets` tests were stale:
three asserted the pre-704f4ad3 DS identifier spelling (`%lds_byte_addr_0`, now
`%lds_byte_addr_0_0`), and the dynamic split storage-image test used no-ops where
descriptor flow analysis follows real `s_load_dwordx4` instructions and an
extended pointer; with real loads the dynamic descriptor shadows the static one.

New fail-closed checks found while doing this. Falling back to the generic path
would otherwise have admitted encodings the lowerings do not model, so
`ShaderComputeWaveGenericVectorSupported` now refuses a MUBUF access whose
preserved flags carry LDS (the data goes to LDS), TFE (an extra status VGPR) or
undefined encoding bits, and both it and the generic compare refuse SDWA control
words with a source select of 7 or reserved bits 22 and 30 (and bits 31:24 of a
one-source VOP1). Neg, abs, omod, clamp, `DST_U` with a DWORD destination and an
SGPR source stay admitted because the decoder folds them into operands. The
tests pin both sides.

Recorded defects (found, not fixed in this session):

1. Generic DS emitters do not bound the LDS index. `ShaderSpirvBuffer.cpp`,
   `LdsWordPointer` (line 2199) and the atomic and read2 emitters that follow it
   (`Recompile_DsAddU32_VaddrVdataOffset` 2245, `Recompile_DsAtomic_XXX_...` 2288,
   `Recompile_DsAtomicIncDec_VaddrOffset` 2332, `Recompile_DsRead2B32_...` 2396) build `OpAccessChain
   %lds` from a guest VGPR address. RDNA2 ISA 3.6.1 says an out-of-range LDS
   write is discarded and a read returns zero. Trigger: any `ds_*` with
   `(ADDR + offset) / 4 >= lds_dwords`, now including paired wave64 programs
   because 704f4ad3 removed the constant-address proof. Direction: compare the
   word index with the existing `lds_length` constant, skip the store or atomic
   and return zero; the ISA leaves a partially out-of-range multi-dword write
   undefined, so needs a numerical GPU probe, not a guess. The banked paired
   emitter already guards its accesses.
2. The decoders drop flags the native lowerings do not model.
   `ShaderParseMUBUF.cpp:29-32` (GLC, SLC, LDS, TFE) and
   `ShaderParseMTBUF.cpp:30-32` continue, and so do the SDWA select checks in
   `ShaderParseVOP1.cpp:41`, `ShaderParseVOP2.cpp:58,63` and
   `ShaderParseVOPC.cpp:39,41`. Paired admission now refuses LDS/TFE/undefined
   MUBUF bits and SDWA selects above 6; the non-paired native path still lowers
   them as if the flag were clear. Direction: have those emitters return false
   on `buffer_flags` LDS/TFE and on reserved SDWA fields, then measure which
   captured shaders change.
3. Not verified: the generic vector predicate does not inspect `dst.clamp` or
   `vop3_op_sel`, so an integer VOP3 with CLAMP (for example `V_ADD3_U32`) is
   admitted without evidence that its lowering saturates. Direction: a decoded
   case per affected opcode, then refuse or implement.
4. `AnalyzePairedWaveCode` consults `ShaderAnalyzeComputeWaveScalarBufferLoad`
   only when the generic scalar predicate refuses an `S_BUFFER_LOAD_DWORD`, so
   the specialized scalar-buffer route is effectively pre-empted for uniform
   operands. Direction: decide whether that route still earns its keep.
5. Inherited inventory at HEAD: 1,885 "ignored (continuing)" lines in 92 files, 573
   "treated as SBarrier (continuing)" lines in 13 files and 82 `TODO() check`
   comments in 3 files (the working tree has 1,877 after the SOPK fixes). They
   are not a list of bugs: triage by decode path reachable from captured
   shaders, and replace with an explicit decode or `KYTY_UNKNOWN_OP()` one file
   at a time with a regression per site, never with a mass rewrite.

### Shader float-mode bits (2026-10-02, partial implementation)

The RDNA2 MODE register (ISA 3.5) carries `FP_ROUND` (bits 3:0, single in [1:0],
double and half in [3:2]; 0 nearest even, 1 +infinity, 2 -infinity, 3 toward
zero), `FP_DENORM` (bits 7:4, same split; 0 flushes input and output denorms, 1
allows input only, 2 allows output only, 3 allows both), `DX10_CLAMP` (bit 8,
clamps NaN to zero), `IEEE` (bit 9, quiet and propagate signaling NaN) and the
sticky status bit `LOD_CLAMPED` (bit 10, set by a texture access whose LOD was
clamped). `S_ROUND_MODE` and `S_DENORM_MODE` (SOPP 36 and 37) set the first two
from an immediate.

`ShaderSpirvF16.h` now consumes the initial graphics controls and recorded
compute controls (`fp_mode_known`). Narrowing models all four static half
rounding modes; half arithmetic admits nearest-even with explicit denormal
and supported clamp handling, and rejects unsupported mode/modifier domains.
Initial FP16 overflow control and explicit provenance now reach compute,
VS/GS and PS metadata and cache identities. The integrated arithmetic suite
passes its numerical and complete-module controls; see the current frontier
for the separate remaining qualification blockers.

Other SPIR-V float operations generally still use host defaults. Outside these
explicit half contracts, a non-default mode can therefore produce incorrect
rounding or denormal behavior without a diagnostic. `FloatClampModifier`
applies the shader's `DX10_CLAMP` setting. `V_LDEXP_F32` is exact apart from results in
the denormal range, so only `FP_DENORM` can change it, and `IEEE` only quiets
signaling NaNs that `Ldexp` passes through. `LOD_CLAMPED` has no producer here
because the parser rejects the image `LWE` bit, the only way to read it;
`IMAGE_SAMPLE_LZ` fixes the level at zero regardless. This is one shader-wide gap
and the comment in `ShaderSpirvVector.cpp` points here instead of carrying a
per-instruction `TODO`. Dynamic `S_ROUND_MODE`, `S_DENORM_MODE` and `S_SETREG`
forms currently terminate in unsupported decoding. Direction: evidenced
decoding and dynamic MODE propagation with a case that programs each mode,
then host float-controls capability checks across AMD, NVIDIA and Intel.

### Arithmetic lowering and cached-module validation (2026-10-01, not gameplay)

Focused production-emitter replays expose separate scalar subtraction contracts:
`S_SUB_U32` must report unsigned borrow, while `S_SUB_I32` must compare sign
bits for signed overflow (zero is not a third sign). The previous lowering
fails 50 and 14 of 162 cases respectively. The corrected parser and emitters
pass both sets. Scalar and vector `BFM_B32` previously used `OpBitFieldInsert`
outside its defined domain when masked offset plus width exceeds 32. Shifted
low-bit masks preserve the ISA's truncation and pass 8,192 cases per form,
including inactive vector destinations. The four modules validate for Vulkan
1.4, and the two focused unit tests pass. Translator version 86 invalidates
the former module results; no guest visual improvement is established yet.

The former `Recompile_VCvtF16F32_SVdstSVsrc0` reversed the conversion direction
and failed 11 of 26 replay cases. The current shared half helper replaces that
implementation; compiled numerical and full-module validation now passes.
General device transcendental accuracy and
paired-wave execution still require separate validation.

Remaining arithmetic debt: signed scalar BFE identities are absent in
`ShaderParseSOP2.cpp`. The suspected unsigned 64-bit BFE zero-width helper
issue is excluded by source review: `FUNC_SHIFT_LEFT` and `FUNC_SHIFT_RIGHT`
branch around shifts at counts 0, 32 and 64, returning zero at 64. That review
is not a compiled value-test result. Packed integer
conversion/V16 templates in `ShaderSpirvVector.cpp` retain uint/float select
type mismatches. Correct each with bounded value and complete-module controls.

Offline Vulkan 1.4 validation of 104 retained translator-85 module entries
finds 17 invalid entries (some share identical payloads), including undefined
IDs, maximal-reconvergence CFG violations and selection-construct escapes.
This identifies invalid cached output, not the first module in the next run
or the cause of black output. Locate the live first failure with shader
validation enabled before another GPU execution of the invalid programs.
The existing control-flow limitation is recorded in
`docs/fragment-wave-execution.md`, Native fragment-wave tier. Preserve those
diagnostics rather than disabling validation to pass the frontier.

The next validated strict run stops before any presentation because its
vertex module is empty after SPIR-V validation rejects an undefined `%buf`.
`ShaderSpirvGenerator.cpp:2693`, `WriteFunctions`, emits legacy load helper
bodies from instruction types alone, even when all raw loads use guest-device
addresses and no storage-buffer interface is declared. The captured module
calls neither helper. Four synthetic raw-load widths with zero storage slots
reproduce the undefined ID; the corresponding one-slot controls validate.
Removing only the two uncalled helper bodies from the captured source makes
that exact source validate for Vulkan 1.4. This is source-level isolation,
not proof of guest pixels. Helper publication must require the backing
interface without removing any guest instruction or inventing a buffer.
The boot-time debugger state is explicitly unpaused and not minimized;
that early failure does not explain the older post-presentation pause stall.

Publishing legacy buffer helpers only with a declared storage interface now
passes eight Vulkan 1.4 fixtures and five focused tests. The next validated
strict run passes the former vertex failure and reaches 77 presents before
rejecting a pixel module. `ShaderSpirvControlFlow.cpp:467` redirects a diamond's
selection merge to its synthetic join only when an outer incoming edge exists.
`WriteLabel` and `Recompile_SBranch_Label` also use synthetic joins for standalone
diamonds, leaving their two-predecessor block undeclared as a merge. This violates
`SPV_KHR_maximal_reconvergence` section 2.11. Changing only the merge declaration
to the existing synthetic join makes the exact failed source validate. Align
selection merge ownership with branch and label ownership; retain maximal
reconvergence, both guest arms and their instructions. This is the new first
validated failure, not evidence that it explains the older black output.

The adjacent three-arm regression fixture also exposes a source-identity
collision: `ScJoinFindParent` and `ScJoinFindOwner` use zero for no source,
although a conditional at the first instruction has PC zero. Its nested link
therefore bypasses the actual parent and violates structured selection entry.
Represent absence separately from valid source PCs in both analysis and emission.
The invalid miniature modules are retained offline and have not been sent to
the GPU. This defect is a focused CFG control case, not a new live failure.

Both CFG corrections now pass six focused tests, five Vulkan 1.4 miniature
modules and 128 GPU branch cases. Translator 88's strict rerun validates the
previously rejected pixel module and reaches 850 presents. The next first
failure is a fragment input collision: a smooth parameter and a flat parameter
are declared at the same location and components. The guest uses both P1/P2
and `V_INTERP_MOV` selector 2; merging the inputs would lose their different
values. `ShaderPixelInterpolator.cpp:171` enables the existing geometry remap
only for raw per-vertex inputs, not for mixed qualifiers referencing the same
parameter export. Route that alias through distinct geometry output locations
while retaining the original export source and interpolation qualifiers.
Captures still fail the visual gate; presentation and the agent's phase name
do not establish menu, input or gameplay. The failing run is unpaused and not
minimized, with an empty flip queue; it does not explain the older pause stall.
Early captures show a readable opening caption. Their low entropy and one
zero midline-difference metric are not evidence of corrupted horizontal bands;
later captures are uniformly black. Do not diagnose tiling from that metric.

The mixed-qualifier route now uses the existing geometry interface to duplicate
the actual vertex parameter into distinct fragment locations. Identical views
still share their canonical input; distinct source exports retain the direct
path. Nine focused tests and six Vulkan 1.4 modules pass. A bounded offscreen
replay compares the production pixel/geometry pair with an independent direct
GLSL reference: six cases rotate the provoking vertex and vary clip W, checking
98,304 components with zero differences. All 24,576 covered pixels distinguish
the smooth and flat values, so the check detects collapsing the views. Translator
89 and the qualified-alias input identity invalidate the old pixel results.
The strict guest rerun validates the three formerly rejected modules and loads
the next scene's resources before rejecting another pixel module. Its single
empty diamond arm jumps directly to the raw guest continuation while
`ShaderSpirvControlFlow.cpp:486` declares the synthetic join as the merge.
Retargeting that one empty arm through the already-emitted synthetic merge
makes the exact full source validate for Vulkan 1.4. The merge and the guest
continuation have the same guest PC; no guest operations may be removed.
Align taken-edge ownership when the taken PC equals the chosen merge PC.
This is the next first failure, not evidence that the older black output or
pause stall is resolved. Focused fixtures still do not establish useful pixels.

Empty diamond arms now branch through their declared synthetic merge, retaining
the continuation at the same guest PC. A focused regression fails before this
change; seven focused tests and eight Vulkan 1.4 modules pass afterward. GPU
replays check both the ordinary and empty diamond over 128 cases each, with zero
differences in destination values or SCC. Translator 90 invalidates the former
CFG output. The bounded strict run validates that formerly rejected full module
and reaches a new fragment failure at 849 flips. It remains unpaused and not
minimized; the flip queue is empty. There is still no gameplay acceptance.

The new failure has three conditional branches to a discard tail. Branch
emission uses local fallthrough merges and separate real discard bodies, but
`ShaderSpirvScJoin.cpp:832` collects two orphan reconvergence merges for that
terminating destination. Their unreachable chain adds an extra predecessor to
the first real discard block, violating maximal reconvergence. Removing only
the two unreferenced synthetic blocks makes the exact source validate for
Vulkan 1.4; all original branches, guest operations and three `OpKill` bodies
are preserved. Exclude terminating discard destinations from merge ownership
instead of merging or omitting guest discard paths.

The discard-destination exclusion passes ten focused tests and six Vulkan 1.4
modules (one/two/three discard edges, complete replay shader and references).
An offscreen GPU comparison exercises each of the three original discard
branches and the non-discard path: 98,304 components match, with 12,288 pixels
discarded and 12,288 covered. Translator 91 invalidates the former orphan-join
output. The next bounded strict run validates all 113 captured modules,
including the former discard-tail failure, but stops with no present progress
after the later resource load. The final native capture times out; the agent
still serves events, threads and diagnostics. Opening-caption captures remain
readable and subsequent captures remain black; all nine visual gates fail and
no guest pad reads are observed. This is not image or gameplay acceptance.

At the last native snapshots, completed pipeline, dispatch and submit counts
stop advancing. The last generated source is a large compute module, but source
order does not prove the active host stack. The debugger stalls while resolving
its first state expression without a compilation-unit scope and the observer terminates the
service; no final pause/flip state is valid from that run. A compilation-unit-
qualified offline lookup obtains the real host-field offsets in half a second.
The next diagnostic run uses relocated PIE globals and bounded raw reads before
stacks, without target function calls or changed runtime behavior. Do not skip
waits or assign the stall to pause or GPU execution without the captured state.

The raw-state rerun confirms unpaused, not minimized and an empty flip queue
at the later stall, with 852 completed flips. Its 28 regenerated modules all
validate for Vulkan 1.4. Top frames are host waits, including one graphics-batch
decode-completion waiter and a file read; there is no active allocator top frame
in that snapshot. The bounded reverse-order stack dump ends before the early
processor threads, so it cannot identify the wait's producer yet. Prioritize
those host ownership stacks next. Pipeline-count asymmetry alone must not be
reported as proof of an allocator stall or a GPU hang. Captures still fail all
visual gates; input and gameplay remain unaccepted.

The host-priority rerun identifies the graphics worker waiting for the shared
render mutex in the EOP clock-counter recorder. Its coordinator waits for that
worker; the window thread waits for a flip request, not a pause event. No flip
is pending at the snapshot. Sixteen regenerated modules validate for Vulkan
1.4, and all nine image gates still fail. The owner has not yet been captured:
obtain the host mutex owner TID and its stack before assigning a lock cycle or
editing synchronization. A verified offline DWARF/glibc layout lookup supports
that bounded diagnostic; it is local evidence, not a portable runtime contract.

The owner-first snapshot identifies a live owner inside the system Intel
Vulkan driver, with a libc top frame. Automatic shared-library loading was
disabled, so the intervening stack is not a reliable call chain yet. This does
not prove a lock cycle or a GPU wait. The next run explicitly loads Vulkan
unwind information and captures the actual pending compute module, layout and
stage flags. The owner snapshot still has no pause or pending flip, and all
nine image gates fail. A debugger's explicit exit zero is not guest success.

Synchronization audit finds an additional baseline defect:
`GraphicsRunHelpers.cpp::SuspendedWaitTimeoutMs` defaults to one second, and
`GraphicsRun.cpp::WaitForSuspendedRuns` can set `skip_wait` and resume after an
unsatisfied guest wait. This contradicts the strict no-skip contract. The recent
diagnostic runs did not explicitly disable this default; activation has not
been evidenced, so do not assume either that a wait was skipped or that these
runs satisfy strict acceptance. Disable the timeout bypass explicitly in the
next diagnostic and replace that policy with a focused regression: a diagnostic
timeout may report or stop, never resume past an unsatisfied guest packet.

With that timeout explicitly disabled, an actual Vulkan-call capture now
identifies the render-mutex owner in compute-pipeline creation. Fifty-eight
calls have returned successfully and one remains pending. Its 2,848,336-byte
module, actual descriptor layout, required subgroup32 and full-subgroup flag
are retained offline. All 101 captured modules validate for Vulkan 1.4; nine
image gates still fail, with no pause or pending flip. The pending module has
not been submitted, so its compilation stall is not evidence of its GPU hang.
Separate two-GiB pipeline-only probes of the system and earlier byte-aware
driver each reach a sixty-second limit without completing. Both initially
request 66,673 allocator nodes; owner-checked probes stop before allocating
that large graph. No guest dispatches are queued by these probes. The earlier
compiler correction is therefore not sufficient evidence for this distinct
module's compilation capacity.

For that distinct module, an owner-checked pre-allocation probe finds 66,669
VGRFs and 107,212 backend instructions, with 1,317 peak interval register units.
Of the values at that peak, 484 span almost the complete dispatcher loop. Their
long intervals do not alone prove dead state. A bounded external native-CFG
experiment retains all 277 guest blocks exactly once and all calls, stores and
host barriers while replacing its three loops and forward branches. The module
validates, but both drivers still miss the sixty-second compilation gate; initial
peak pressure increases to 1,535 units. This excludes native structuring alone
as a sufficient initial-pressure fix for this input. No such runtime strategy
is activated. Trace the retained values' definitions and consumers next.

The backend walk now identifies all 647 values at that peak. The 484
dispatcher-wide values have 328 initial SIMD16 selects, 150 SIMD16 moves,
four scalar NoMask moves and two SIMD16 inversions. All have consumers, so
this census does not justify removing them as dead state. An external fold of
92 bind-time storage-slot metadata loads preserves other descriptor fields
and validates, but only changes interval pressure from 1,317 to 1,314 units.
Neither translation is activated; complete compilation and GPU preservation
of this fold remain unverified. Compare actual block live-in/live-out sets
with those conservative intervals before changing the allocator.

The actual block-boundary live sets still peak at 1,105 incoming and 1,111
outgoing register units. The external native CFG raises both to 1,267. Thus
this pressure is not merely overlapping intervals of mutually exclusive
blocks, and those counts do not authorize a different interference contract.

Further bounded experiments on this same input are also excluded as sufficient
compilation fixes. Broadcasting scalar reads leaves 1,314 peak register units;
113 provably unwritten loads represented as undefined leave 1,305. Direct
Workgroup materialization fits this input's 32 physical invocations and 2,048
LDS bytes: register-major storage uses 36,352 bytes and peaks at 594 units;
invocation-major storage uses 36,608 bytes and peaks at 538. The latter still
has 102,689 VGRFs and 159,296 instructions. Scalar broadcasts raise that peak to
605, native structuring to 1,174, block-local caching to 801, and volatile scalar
reads to 558 with 180,558 VGRFs. None of those large graphs was allocated or
dispatched. The shared peak includes 288 units of SLM send responses and 156
units of address additions, not only retained architectural registers.

Broadcasting the dispatcher PC reads reproduces the original backend exactly.
Real-CFG dead-state clears are valid but leave the original 1,317-unit peak with
undefined values and raise it to 1,953 with concrete definitions. Must-constant
SGPR folding leaves 1,314, and scalar-write broadcasts leave 1,318. A first
constant-folding harness incorrectly specialized the synthetic dispatcher PC;
its two-instruction output is rejected, not reported as a compilation success.
All these experiments remain outside production, without GPU equivalence or
complete compilation. Mutable descriptor fields must not be inferred constant.

A separate graph-phase measurement locates the next bottleneck more precisely:
the original graph has 66,673 nodes and 38,182,554 edges, with 557,177,088 bytes
of neighbor-list capacity. Construction takes about 1.54 seconds; initial
allocation retries fail in about 0.23 seconds each. The second spill attempts
to double node capacity from 66,688 to 133,376, quadrupling the dense adjacency
bitset. A bounded probe stops before that allocation. An external growth fix
covers bulk resize requests and caps spare nodes in large graphs; 96 spills
still leave 46 failed allocations and 68,877 nodes at 29.91 seconds. The input's
register pressure and compile-time barrier remain real.

Focused external allocator tests also reproduce three independent contracts:
64-bit triangular edge indices were truncated during set/clear; bulk resize
could reserve fewer nodes than requested; and reset removed edges but retained
the node's old interference weight and neighbor-list allocation. Owned external
corrections pass 35 contract checks and five upstream allocator tests. The
original pipeline nevertheless misses the same two-GiB, sixty-second gate with
each corrected driver. These are host-allocator findings, not evidence of GPU
preservation, guest integration, a fixed historical host freeze or playability.
No installed driver, guest module, resource ceiling or runtime policy is changed.

An external sparse-row membership experiment preserves the same initial node,
instruction and edge counts and passes 5,132 independent add/reset/migration,
wide-index, weight and coloring checks, in addition to the prior focused tests.
Its complete compile still reaches sixty seconds, with a 1.1-GiB memory peak and
about sixty CPU seconds. Lower reserved adjacency memory alone is not a usable
compile-time fix. No guest dispatch or GPU-preservation claim follows from those
CPU tests. Supported host spill batching at rate one also misses the same
sixty-second gate. A valid bounded phase trace records four failed non-spilling
attempts, followed by spill emission; 8,207 additional spill nodes trigger the
capacity guard at 26.41 seconds. The earlier phase probe lacked a valid owner
at an inlined breakpoint and is harness failure, not compiler evidence.

Further shared-storage experiments are also closed as sufficient fixes. A
case cache produces 79,387 VGRFs and 121,225 instructions with a 793-unit peak;
removing 1,512 dead entry restores leaves identical backend output. Conditional
masked stores validate but do not reach allocation within the 25-second
preflight. Branchless private-cell atomic masking passes 331,410 CPU bit checks
and structural cell-disjointness checks, but raises the backend to 127,485
VGRFs, 197,742 instructions and 1,188 peak units. None was dispatched.

A separate host-frontend census finds 937 NIR register declarations: 673 marked
divergent and 264 marked uniform, all scalar 32-bit. The existing frontend
allocates every declaration as a vector. An owned external candidate retains
scalar representation only for uniform single-component 32-bit declarations;
seven focused frontend contracts pass, including divergent, wide and vector
controls. On the original unchanged module it lowers the interval peak from
1,317 to 1,016 units, with 66,978 VGRFs and 105,684 instructions. Its compile
still stops before capacity 75,200 at 23.22 seconds, with 71,104 actual nodes
and 4,122 spill nodes. This is partial CPU evidence, not a completed pipeline,
GPU equivalence or an integrated fix. Tightening sparse graph growth to a
512-node quantum passes 22,552 new checks and the prior focused contracts, but
the unchanged shader still stops during spill emission at 24.73 seconds:
74,688 nodes, 7,706 spill nodes and 4,416 scratch bytes. This closes spare
capacity as a sufficient fix; do not shrink the quantum repeatedly to evade
the unchanged 75,000-node guard or increase the two-GiB ceiling. A next external
representation experiment tests invocation-private Function arrays with
runtime-bounded indices, retaining all architectural cells and guest behavior.
Its valid complete module has a 553-unit peak but 143,954 VGRFs and 268,707
instructions, so it too stops before graph allocation and remains undispatched.
The next host-compiler seam is reuse of instruction-local spill temporaries,
with unioned original-live interference and distinct simultaneous temporaries.

That external host candidate now completes the original unchanged pipeline.
Six focused reuse contracts pass; the bounded trace retains only nine spill
nodes but initially still reaches its 45-second deadline. A follow-up avoids
redundant hazard-edge setup on instructions untouched by a spill, while a full
rescan after each of three small spills adds no missing edges. Eight contracts
pass. The original Vulkan pipeline then completes in 42.589 seconds, with a
real successful compiler return and real target exit zero, under the same
two-GiB ceiling and 75,000-node guard; the journal reports a one-GiB peak.
No dispatch was queued. This establishes compilation capacity only, not GPU
preservation, the exact guest VkDevice, useful images or gameplay. The driver
remains external and uninstalled. Focused GPU preservation now passes: synthetic
uniform/divergent loops check all final cells at SIMD8/16/32 against a CPU
oracle, both with forced spills and with natural SIMD32 spilling. The corrected
ALU modules also pass with observed forced spilling. There are 69,704 checked
components across these focused runs, without differences. This does not prove
equivalence of the complete private module on arbitrary inputs.

An initialization-only capture stops before logical-device creation and records
the current runtime's actual features, extensions and queue inputs. Reproducing
that configuration exactly also completes the unchanged original pipeline in
42.474 seconds, with a real target exit zero and no dispatch. Window/render
debugger schemas are regenerated for the new wait-policy binary and checked at
host main before guest execution. The next run may now test guest integration
using the isolated external ICD, with unchanged strict shader/runtime settings,
the existing host guard and diagnostic compiler stops, not fabricated progress.
Menu, useful images, response to input, gameplay and host stability remain open.

The first protected integration of that driver and the new wait-policy binary
does not preserve the prior visual frontier: 21 compute pipelines complete and
command processing continues, but the frame counter stays at one with zero
presents until the unchanged 300-second runtime limit. The former pending
module is not reached in this run. Its measured cgroup peak is 2.4 GiB and host
available memory stays above 22 GiB; no resource guard or fatal guest error is
observed. Do not blame the later shader for this earlier failure. The observer
must capture native diagnostics and the raw wait owner for a stalled initial
frame as well as a stalled nonzero presentation count before the hard deadline.

The following raw initial-frame snapshot identifies the main thread in
`XIfEvent` inside SDL's `X11_ShowWindow`, before acquire or present. The render
mutex is free, the guest is unpaused and two flips are queued. This is the
already-documented X11/window-manager blocker below, not a new compiler or
guest-wait regression. The external runner had forced X11 and removed the
active Wayland session variables, preventing the existing native-Wayland
selection. Correct the runner environment, not guest waits or SDL mapping.
The corrected runner's initialization-only gate confirms the native Wayland
backend and an unchanged logical-device profile. Integration then completes the
first flip and captures a black frame; all 44 captured modules validate for
Vulkan 1.4 and its first 21 compute modules match the prior baseline exactly.
The observer stopped between completed compilation and continuing descriptor
work, so that stop does not establish a new hang. Confirm present progress and
recent command-processing work before a diagnostic stall stop; the resource,
compiler and total-runtime ceilings are unchanged. No visual acceptance follows.

The confirmed-progress native run reaches the later resource load with 840
presents and 58 completed compute calls. The unchanged former pending module
begins only about 290 seconds into the run, and the original 300-second cap
stops it ten seconds later. This is neither a compiler-deadline failure nor an
integrated completion. All 101 captured modules validate, all eight retained
image gates fail, and no guest pad reads are observed. No kernel error is
recorded in that run's journal window. Its cgroup peak is about 7.15 GiB, below
the 7.5-GiB stop threshold, with over 17 GiB host available memory. A bounded,
normally keyed driver cache is the next way to separate startup compilation
cost from guest execution without extending the runtime cap. Cache warmth must
be disclosed; cached pipeline success is still not GPU or gameplay proof.
The captured compute inputs now create successfully in 59 isolated calls and
one repeated original call, each with a real target exit zero. The original
cold call takes 42.730 seconds; a normally keyed disk-cache repeat takes 0.010
seconds with no allocator assignment callback. The isolated cache is about
7.1 MiB and bounded to 256 MiB. No dispatch or guest execution occurs in this
cache validation. A checksum-verified copy is used for the next protected guest
run; runtime waits, shaders, binary, driver and resource ceilings are unchanged.
That guest run observes all 59 compute creations complete, including the former
pending module's real successful return in 0.008 seconds. Presentation continues
past that call to at least 885; the native image at frame 886 remains uniform
black. All 101 modules validate and all nine image gates fail. Its resource
peak stays below the 7.5-GiB diagnostic-stop threshold, and no kernel error is
recorded in the correlated interval. No pad read is observed. The compiler
blocker has been passed, but pipeline success and global dispatch counts do not
prove that specific dispatch's completion or output correctness. Correlate its
pipeline, command buffer, successful queue submission and observed fence result
before returning to the upstream-color investigation. A new debugger helper
must keep a private namespace: overwriting the raw-schema `word` helper caused
an instrumentation-only failure before the guest's first main loop.
After helper isolation and focused preflights, the dispatch-specific trace
observes the unchanged pipeline record 390 groups, successfully submit the same
command buffer, and complete its fence on another host thread about 39 ms later.
The later native frame is still uniform black and no pad reads occur. At the
final snapshot the guest is unpaused, the window is visible and the render mutex
is free; that point-in-time snapshot is not a sustained guest-wait diagnosis.
Synchronization tracking is disabled, so its zero blocked count proves nothing.
The protected run records no correlated kernel error. This advances one real
compute dispatch through GPU completion, not arbitrary-input equivalence or
useful images. Next use the existing bounded native material/binding census at
the later scene to follow the color producer; keep strict Silent/Native/None.

That native late-scene census now records 21 distinct pixel draws, including
material targets, intermediate consumers and the final compositor. Binding
records alone do not prove their output values. One existing output-preserving
sparse sample probe then observes 259,200 elected compositor samples with
finite RGB zero and alpha one at its first scene-color sample. This locates
observed black before final color conversion, not a missing final draw or proof
that every texel in the source image is zero. All 101 ordinary and 102 diagnostic
modules respectively validate for Vulkan 1.4; all nine captures in each run fail
their image gate, and guest pad reads remain zero. The next discriminator is the
upstream consumer's scene-color sample, preserving its normal shader outputs,
waits and resource limits. No useful image or gameplay is accepted.

The upstream consumer now also completes its existing sparse observation:
104,576 elected samples read finite RGB zero and alpha one from an exact
storage-texture binding. Its 102 actual modules validate; all nine native image
gates still fail and no pad reads occur. This does not identify the producing
dispatch or exclude bad sample coordinates. The next boundary is the earlier
HDR render-target consumer, before the storage-texture composition chain.
Diagnostic shutdown exhausted its stop grace and was killed; a collected
unit's later default `Result=success` is not the original service result or a
normal guest exit. Keep the journal, live resource limits and observer outcome
as separate evidence.

The earlier HDR sample selector returns zero observations on both conditional
read sites; neither result proves black source pixels. A paired existing vertex
probe then observes three exports with no finite post-divide extrema and a live
GETPC-derived buffer base incorrectly reaching the descriptor-array resolver.
The fault is in register namespace comparison: Gen5 vertex user data is emitted
at API register + 8, but `ShaderHasUnboundBufferLoad` and the vector buffer
emitter compare its unshifted metadata register. Thus an inline descriptor at
s[0:3] collides with API binding zero actually emitted at s[8:11]. Correct both
classification and lowering with the same namespace contract, keeping negative
stream-binding sentinels non-registers; first add focused red tests. This is not
proof of correct HDR content or gameplay after that correction.

Two sanitized GETPC/raw-load regressions reproduce both sides of this namespace
error: the constructed s[0:3] descriptor incorrectly takes the array path, and
the actual bound s[8:11] descriptor incorrectly takes the guest-address path.
The shared binding predicate now applies the caller's user-data base and rejects
negative stream sentinels. Classification and all four raw vector load widths
use it, while pixel/compute retain base zero. Nine focused assembly tests pass,
including both red-to-green cases. Their original synthetic fixtures omitted
ENDPGM, so those early results do not prove Vulkan validation. After explicit
validation and fixture termination, the inline case validates but the bound-only
case exposes a separate undeclared s3 prolog variable; see below. Translator
revision 92 invalidates stale lowering. Fresh DWARF schemas, a raw-host-main
preflight and the exact unchanged native VkDevice contract pass before protected
guest integration; image/input/gameplay advancement is not yet verified.

The protected paired-probe integration confirms the repaired vertex contract:
three finite positions now have W=1, X/Y extrema -1..3 and Z=0, with varying
parameters and no erroneous descriptor-array resolver invocation. All 103
actual modules validate and the original compute dispatch still completes.
The selected alternate HDR sample remains unobserved; it is a conditional site,
not evidence of black texels. Eight available native captures fail the image
gate, but the last is before this delayed color checkpoint and the final
capture times out. No image advance is established. Observe the other HDR read
now that its input geometry is valid; retain unchanged outputs and guest waits.

The next protected repeat completes all 59 computes and validates all 103
actual modules, but stops at present 854 before the diagnostic threshold 855.
No probe event is delivered, so no inference about the selected HDR sample is
possible. Eight available captures fail the image gate and no pad read occurs.
The next bounded diagnostic starts observation at present 840 without changing
guest work or the 300-second ceiling. This is not gameplay acceptance.

Focused test defect (2026-10-02): `ShaderSpirvGenerator.cpp` emits an s3 store in
the fused-vertex prolog (line 1902), but `DetectVariables` (line 3277) only
declares its VGPR system inputs. A valid bound-buffer-only vertex fixture that
does not otherwise reference s3 therefore fails validation with an undefined
store ID. The inline fixture incidentally declares s3 through its descriptor,
and the observed guest prolog also references it; this is not the proven cause
of its remaining black image. The unconditional prolog destination is now
declared in the same variable-discovery branch. Both complete namespace fixtures
validate explicitly, all nine focused tests pass, and translator revision 93
invalidates old modules. Fresh binary-bound DWARF, raw-host-main and exact
Wayland/device preflights pass. The older atomic-only assembly fixture lacks ENDPGM
when run with global validation; do not describe its assembly pass as validation.

Earlier diagnostic selection now observes 65,280 elected finite HDR samples,
all RGBA zero, with three valid fullscreen positions. This locates the observed
black upstream of final composition without proving every source texel. Ten
native captures fail their gate. A subsequent paired vertex/MRT/attachment probe
of an indexed HDR producer observes 1,361 finite exports, all with nonpositive
W, zero color exports and an unchanged all-zero attachment. Depth/stencil is
disabled for that draw. Both runs validate all 103 actual Vulkan 1.4 modules and
complete the original compute submission; no pad read or gameplay gate passes.

New producer defect (2026-10-02): `ShaderParseUsage2` in `Shader.cpp` strips both
words of the vertex descriptor-table and attribute-table pointers from direct
SGPR bindings (lines 2024/2037). When native guest-address S_LOAD is used,
`ShaderSpirvBuffer.cpp:3467` reads those same shader pointer pairs before any
initialization; the observed fused-front prolog never stores s24..s27. Retain
the actual API pointer words in the direct-SGPR namespace and its existing +8
mapping. Prove this with sanitized parser-to-prolog regressions before changing
the producer; do not replace the loads with constants/no-ops, repair positions,
or blame MRT retention for a draw with no raster coverage.
Both sanitized pointer regressions fail on the original parser and pass after
retaining those words. An old SRT-span control also fails on the preserved
revision-93 binary: its ENDPGM-only fixture never reads the descriptor it expects
to be bound. Add a real descriptor consumer to that fixture, preserving the
existing liveness policy and its original span expectation. That complete
fixture and the other focused controls now pass: 18 tests, with explicit Vulkan
1.4 validation of the native pointer-load module. The minimal parser correction
retains actual table pointers as direct API words and uses the existing fused
register mapping; no load, wait, coordinate or color is replaced. Revision 94
invalidates cached metadata layouts. Fresh binary-bound DWARF and exact native
Wayland/device preflights pass. The same paired producer probe is now running
with this build. That repeat confirms stores of all four table-pointer words
before native S_LOAD, but the same producer still has 1,361 nonpositive-W
exports, no MRT coverage/color, and the same all-zero attachment hash. All 103
modules validate and all 59 compute creations plus the selected dispatch
complete; ten capture gates fail and no guest pad read occurs. Pointer retention
is a demonstrated contract correction, not a sufficient fix for this image.
The next observation must distinguish real scalar-buffer results from the later
vertex arithmetic, with original instructions and outputs retained. Do not
repeat the lost-pointer hypothesis or add a matrix/coordinate replacement.

The next output-preserving vertex diagnostic appends eight scalar-buffer load
sites to the existing opt-in clip-probe buffer. Each site elects one invocation,
retains the input descriptor/byte offset before the original instruction and
copies at most sixteen actual result words afterward. Unobserved sites stay
unclaimed; a fence precedes native event publication. Two initially red tests
now pass with explicit Vulkan 1.4 validation, proving instruction retention,
descriptor/destination overlap ordering, disabled normal emission and the site
cap. Sixteen focused tests and the probe layout/serialization integration pass.
Translator revision 95 and a separate diagnostic identity invalidate old layouts;
fresh binary-bound schemas and native Wayland/device preflights pass. The next
private producer run uses no attachment retries so its GPU words can be compared
with the same first draw's bounded CPU constant trace. No guest result, entire
wave equivalence, image or gameplay improvement is yet established.

That native repeat observes six scalar-buffer sites: all 73 result words match
the same first draw's bounded CPU constants bit for bit, including four complete
matrix loads. This excludes zero/corrupted scalar constants at those observed
sites, not the entire guest wave. All 103 modules validate; eight capture gates
still fail and pad reads remain zero. The draw's vertex layout reports failure
and zero attributes: unified format 66 (16_16_16_16_SNORM) is missing from
`VulkanVertexInputFormat.cpp` although the shared image catalog already maps it
to `VK_FORMAT_R16G16B16A16_SNORM`. A single missing format rejects the whole
interleaved stream, including UVs and normals. `GraphicsRenderPipeline.cpp:440`
then warns and proceeds with zero binding/attribute counts. Regression-test the
format and the complete packed layout before adding that evidenced table entry.
The warning-and-continue consumer is separate fail-closed debt; do not treat
zero attributes as a valid fallback or fabricate input data.

Two sanitized regressions now fail on the missing mapping and the complete
packed layout, then pass after adding only unified format 66 to the vertex
format table. It has four normalized floating-point components, shared with the
established image mapping; no unobserved attribute alias or conversion shader
is added. All 24 focused format/layout/fetch/register controls pass. Revision
96, a new preserved binary, freshly extracted schemas and native Wayland/exact
device preflight pass. A repeat of the same producer must establish whether
the layout now binds all three attributes and geometry/color improve. Image,
input and gameplay remain unaccepted until that native evidence exists.

The post-fix native diagnostic confirms one binding and all three packed
attributes, 1,361 finite vertex exports with positive W extrema, varying
parameters and 2,073,600 finite MRT0 observations with nonzero RGB. The same
73 scalar-load words still match the original CPU constants. All 103 modules
validate and the original compute dispatch completes. This advances the
producer's observed geometry/color, not the presented image: all nine scored
captures precede that draw, the final capture times out, and no pad read occurs.
The diagnostic stop's exit 1 is not a guest rendering failure or successful
completion. The next protected run removes all shader probes, binding traces,
shader dumps and the debugger while retaining Native/Silent/None, validation,
the verified external ICD and inherited bounded cache. Confirm a native image
and guest input before claiming this resolves the displayed black frame.

The uninstrumented Native/Silent/None run instead terminates with
`VK_ERROR_DEVICE_LOST` from `GraphicsRenderCommandBuffer.cpp:823` while waiting
on queue 8, slot 2. A correlated Xe coredump identifies a started but incomplete
render job; this is a real host-GPU failure, not the former compile wait or an
observer exit. Resource guards remain intact. Retained batch memory places
IPEHR at the PIPE_CONTROL immediately before ACTHD, rather than at ACTHD itself;
offline decoding must preserve that distinction. Identify the preceding work
and correlate it with native submission evidence before changing shaders or
driver policy. No image, input or stability acceptance follows from this run.
The bounded offline path reaches ACTHD through 816 commands and 27 indexed
draws, with no compute walker in the retained path. One predicated branch is
chosen statically and is not execution proof. The final draw has 7,110 indices
and two instances with the preceding draw's pixel kernel. Native checksum and
submission correlation remain necessary. All eight image gates fail; the 25
retained unique cache payloads validate, but persistence is not a complete live
module census. The native fault report's opt-in logger is suppressed by Silent
(`GraphicsRenderCommandBuffer.cpp:90/107`, `Log.cpp:302`), and the agent does not
expose its trail. A launch-only SHA-bound, call-free fault read is justified for
that missing diagnostic; it must resume the original fatal path and never
count debugger exits as guest success.

The bounded diagnostic repeat stops at frame 839 / present 838, before its
minimum-present 840 census gate. It observes neither a fault report nor the
target's original exit. All 101 captured modules validate and all eight native
image gates fail; pad reads remain zero. This is an inconclusive reproduction,
not evidence that device loss is fixed. Keep the existing runtime limits and
replace the missing Silent-mode fault evidence with an opt-in, bounded native
report rather than another debugger-dependent observation.

That diagnostic gap is now regression-tested: the old implementation loses
the requested report under Silent; the corrected native path writes only the
first device-loss report (eight attempts, 8 KiB maximum, exclusive file
creation) and publishes a native fatal event independently of logging. Fence
failures retain the caller's queue, slot, sequence and guest command context
without claiming submit return or fence completion. Fourteen focused CPU tests
pass, including disabled tracing, existing-file preservation and write failure;
the architecture boundary check and fresh host-main/Wayland/device preflights
pass. This changes observation only, not waits, shaders, submissions or the
original fatal path. The protected native repeat without GDB or shader probes
reproduces the real device loss at a queue-8 fence and successfully preserves
the native report. Its failing caller matches one of the eight attempts whose
submit returned `VK_SUCCESS`; that does not establish GPU completion. A
correlated Xe kernel journal records a render-engine reset and timed-out job.
The draw census contains 21 distinct shaders, but only their first occurrence,
not the failing later guest submission (`GraphicsRenderBind.cpp:170–203`).
Consequently neither the fault shader nor its draw is identified yet. Fifty-eight
logged modules and 25 unique persisted cache payloads validate; neither is a
complete live-module census. All eight image gates fail and pad reads remain
zero. The new coredump read times out with zero bytes; do not claim it was
captured or reuse the older dump as evidence for this job. Next collect bounded
recent draw identities correlated to the native submission, and preserve a new
dump through local authorization when available. No further GPU retry, shader
policy change, image/input/gameplay or cross-vendor acceptance follows from
this diagnostic advance.

That recent-identity diagnostic now exists (2026-10-02, CPU verified, no GPU
run yet). An opt-in ring of 1–256 records (default 64) is filled after each
guest `vkCmdDrawIndexed`, `vkCmdDraw` and `vkCmdDispatch`, not at the command
processor call, because `GraphicsRenderDrawIndex` can still return without
emitting. Records join on queue plus host sequence, which the submission
tracker assigns monotonically from one per queue, so slot reuse cannot merge
recordings. Recording, submit call, submit return and observed fence signal
remain separate stages; unobserved values are `null`. The report is written
once at the existing device-loss latch with exclusive creation; disabled,
existing-file and open-failure paths keep the original fault JSON and fatal
event. Twelve new focused tests and the fourteen existing submit-fault tests
pass; before implementation the test binary failed to link the report and
trace contracts. The runtime binary is rebuilt and versioned separately from
the d48 binary. A fresh binary-bound preflight is required before the single
protected repeat; until then the d48 fault shader and draw remain unknown.

Three defects found during that work, none shown to cause the d48 fault, are
now corrected with red-to-green tests:
- The submit trail's `frame` was `Gpu::Done()`'s count, which the Gen5 path
  never advances, so d48's `frame: 0` was a real but mislabeled counter.
  Fault report version 2 records `presented_frame` instead.
- Attempts without a host submission or command-processor context wrote
  `host_sequence`, `guest_submit` and PM4 fields as zero (d48's queue-9 entry).
  Explicit known flags now emit `null`; the Console mirror uses the same JSON.
- `ShouldSkipUnsupportedGeShader` and the invalid-vertex-shader check dropped
  guest draws with an Info log that Silent hides. Every skipped draw is now
  counted per reason; the first of each reason is a native `draw_skipped`
  Warn event, and the counts are in the recent-draw report. The skip itself is
  retained until a run shows whether it fires: converting it to a fatal
  failure without that evidence could only move the frontier backwards, and
  the correct fix is to implement the observed GE configuration.

Two neighboring unit expectations were stale rather than the code wrong:
`Gen5CodeAvailable...DirectStorageDescriptor` asserted the binding policy
before `342e7938` (direct user-SGPR slots need a storage consumer), and the
tile-9 BC1 assertion predated `b632b49e`, whose package upload was verified
against independently decoded source bytes. Both now assert those contracts;
all 315 `EmulatorGraphicsState` and detile tests pass. A third stale
expectation, the vertex-stage `S_GETPC` rejection test, predates 8d08fb38 and
now asserts that a bound program base resolves while a missing base or an
invalid destination is still rejected.

Lane-local EXEC in cross-lane lowering (2026-10-02; device loss not reproduced
in two runs; not gameplay). The recent-draw ring reproduces the real device
loss once more (d50, queue 8, PM4 op 16 at dword 11828, the same point as
d48) and names the work inside the failing submission: 28 recorded indexed
draws, 18 of them one vertex/pixel pair with different counts. The pixel
module has four backward guest branches, three of whose loop bodies contain
one of its three `V_READFIRSTLANE` sites (waterfall loops); the vertex
module's loops are bounded binary searches of the address table and always
terminate. Outside paired wave banks each
invocation is one guest lane and holds only its own EXEC bit in `exec_lo`
(0 or 1), with `exec_hi` kept at zero, as `V_CMPX`, `S_CBRANCH_EXECZ`, `S_WQM`
and the DPP lowering already assume. `V_READFIRSTLANE` and `V_MBCNT` instead
tested bit `1 << lane` of that pair, which can only ever select subgroup
lane 0. A waterfall loop then keeps re-reading a finished lane and cannot
terminate, matching a render-engine timeout. Both lowerings now test the
lane-local value; a literal or SGPR mask operand of `V_MBCNT` stays a
whole-wave mask indexed by lane. The high-half `V_MBCNT` lane offset also
used its lane value before defining it, which the new tests caught. Four
focused tests (real parser, production generator, real toolchain assembly)
fail on the old lowering with the contract exit code and pass afterwards.
Translator 97 invalidates every cached module.

Two protected runs with the corrected binary (d52, d54) complete the full
264-second observation window with no device loss; the earlier reproductions
lost the device at 214-221 seconds (d45, d48, d50). The correlated kernel
journals of d52 and d54 show no render reset, and the 58 logged modules of each
validate for Vulkan 1.4. Both exit zero only because
the observer stops at its time limit, not because the guest exited. This is
two runs on one Intel Xe host with the isolated experimental driver, not proof
of the cause: no isolated GPU replay of the waterfall loop exists yet, and
AMD and NVIDIA are unvalidated. Every capture is still black at about 3 FPS
in the loading phase, no pad read occurs, and no menu, input or gameplay is
accepted.

The skipped-draw counters expose the next frontier candidate. d50 skipped
4,458 guest draws and d54's first-occurrence report names the state:
`stages=0x00002030`, ES bound, legacy GS base zero, `max_vert=0x48`, `out_prim=0x2`,
`ge_ngg=0x46`, `max_out=0xd8`, primitive and vertex group sizes 3 and 0x18, first
seen at presented frame 1. `ShouldSkipUnsupportedGeShader` models only the
`0x02002000` NGG passthrough mask, so this configuration is an unimplemented
guest state, not a defect of the skip predicate. An omitted draw is a strict-
mode violation and a plausible cause of the black output, but no capture
links it to the missing image. Implement the observed GE/NGG configuration
from evidence (ISA, local captures, independent implementations compared, not
copied) before turning the skip into a fatal failure.

The original `gs=0` diagnostic inspected `gs_regs.data_addr`, the legacy
pseudo-stage field. GS program-register writes populate the separate
`gs_back_addr`. The original report therefore leaves the back-program identity
unknown. The version-2 skipped-draw report records both identities, GE registers
and shader resource state; it does not itself establish support for that state.

Working-tree review (2026-10-02, 83 entries, nothing staged): `git diff
--check` is clean and no added line carries a private path or title
identifier. That review listed eight added
"condition ignored (continuing)" checks and seven `TODO() check ...` comments
(VSKIP, SP_DENORM, SP_ROUND, DX10_CLAMP, IEEE, LOD_CLAMPED), the unbounded
`KYTY_TRANSPORT_DUMP` hook and 56 failing tests (55 compute-wave and one GETPC). All
three are closed; see "Debt resolution and compute-wave contracts" above. The
classifier predicates those tests exercise are no longer unchanged: the generic
vector and compare predicates gained fail-closed checks described there.

Reference-review boundary (2026-10-02): this work corrects Kyty's own contracts.
Other emulators supply behavioral leads to verify against local state, never
drop-in implementations or proof of the current failure's cause. Attachment
write masks and guest-wave versus host-subgroup handling require local
correlation; heuristic mip-stat reports and asynchronous draw omission do not
satisfy Kyty's strict contract. The local
`GraphicsRunOpParsers.cpp:1260` query handler currently consumes GET_LOD_STATS
without a guest-visible producer, and `GraphicsRenderBind.cpp:3095` ignores
mip-counter fields. No live correlation with this fault is established; retain
this debt for an actual packet/counter/sample/fence implementation, not invented
counts. All renderer corrections must remain capability-driven across AMD,
NVIDIA and Intel. Xe dump addresses and the isolated ANV experiment are local
diagnostics only; no vendor branch, host-address rule or cross-vendor validation
claim follows from them.

Separate recorded generator defect (2026-10-02): `ShaderSpirvGenerator.cpp`,
`WriteTypes`' non-compute `lds_addtid` template (around line 1280), passes raw
SPIR-V percent identifiers to `String8::FromPrintf` without escaping them.
The compiler reports consumed `%ld`/missing `%u` arguments. A non-compute
DS-addtid module needs a sanitized validation regression and literal-percent
escaping before that path can be trusted. The currently selected HDR producer
does not contain those operations, so this is not its demonstrated blocker.

The production wait policy now removes the bypass state and downstream resume
cursors entirely. Missing/invalid diagnostic deadlines disable it; a valid
opt-in deadline reports and terminates rather than executing past a pending
guest packet. Parsing also rejects nanosecond overflow and signed values.
Fifty-two focused synchronization/packet tests and seven process-environment
helper cases pass, and the owned Release executable builds. The subsequent
protected guest runs use that versioned executable with explicit zero timeout;
the older versioned binaries still require an explicit zero timeout. The new
integration does not establish correct wait producers, image, input or gameplay.

### Fragment wave compiler (2026-09-29, compiler and GPU replay verified)

An explicit compiler interface retains guest Pixel IR and metadata while
emitting a host compute module with paired wave64 execution on a full physical
subgroup32. Bounded inputs carry allocated lanes, initial EXEC, coverage,
initial vector words, native attributes and raw parameter triples. Optional
per-wave parameter state initializes the system SGPR identified by the PS
user-SGPR count. Its entry copy to M0 must preserve the parameter-cache
identity; later reuse of that SGPR after the copy is allowed.

EXP decoding now retains VM, DONE and COMPR. Compressed masks are restricted
to complete half pairs. VM updates pixel validity independently of color,
and every termination path must have VM and DONE exports. Helpers continue
through guest branches; several terminal blocks converge on one output
epilog without OpKill. Storage writes, LDS, atomics, barriers and unverified
interpolation pairs remain rejected.

The complete 2,300-instruction private pixel input generates a 286,670-word
module that passes assembly and validation for Vulkan 1.4. An original GPU
replay checks 19 modules, 160 cases and 356,862 output words against independent
references with zero differences, including both banks, WQM helpers, partial
exports, VM changes, multiple exits, multiple waves, excess dispatch groups
and truncated buffers. A masked packed conversion followed by EXEC restore
previously exported zero instead of retained VGPR bits (160 replay differences);
refreshing each bank's shadow before conversion eliminates those differences.
Fifteen malformed cases are rejected; a control case
permits SGPR reuse after its M0 copy. The existing paired-wave replay still
passes 320 cases and 42,064 register observations.

This interface does not enable the renderer strategy. Compact raster capture,
required host feature enablement, native MRT/stencil resolve and strict guest
execution of the new module remain pending. There is no new guest FPS or
gameplay claim from these results. Unit suites remain deferred as requested.

### Cold compute compilation capacity (2026-09-29, unresolved)

A strict Silent/Native retry of the compiler-interface build collects native
readiness, doctor, events, last error, threads, synchronization and performance
snapshots. It remains before the first present and reaches a cgroup OOM kill
after 269.732 seconds, with an 8 GiB service peak. Completed compute-pipeline
creation already accumulates 44.875 seconds in the early snapshot; the active
command-processor host stack remains in the Intel driver. Guest thread
liveness and an unsupported metadata diagnostic are not frame-rate evidence.
No warm FPS, new capture or gameplay result exists for this retry.

The 8 GiB bound is insufficient for this cold compile plus guest state. A
read-only debugger snapshot supplies driver instruction addresses, but stripped
symbols do not identify the specific compiler pass. Resolve those addresses
with the matching driver build before choosing another shader transformation.
The matching symbol and string sections now resolve the captured frames to
`update_pq_info`, `add_node_to_stack`, `ra_allocate`, `brw_assign_regs` and
`brw_allocate_registers`. The snapshot therefore locates work in register
allocation rather than identifying a NIR pass. This is one stack sample, not
a complete CPU profile. The prior local-load elimination experiment did not
remove the memory barrier; the next bounded experiment must constrain register
liveness while preserving guest control flow and enabled shared-memory limits.
Unit suites remain deferred, and no compatibility push is authorized yet.

An external checkpoint experiment moves eighty cross-block FP32 variables to
invocation-private cells in Workgroup memory and restores them at case entry.
It retains 181 dispatcher cases, uses 48,656 shared bytes against the queried
49,152-byte device limit, and dispatches no guest GPU work. Its module validates
for Vulkan 1.4. Three original branch/export GPU fixtures pass 24 cases and
52,224 output comparisons with no differences. The large pipeline-only replay
still reaches its 4 GiB cgroup OOM bound after 172.141 seconds. Thus
checkpointing these FP32 variables alone is insufficient; scalar state and the
remaining register-allocation pressure need measured evidence before another
transformation. No checkpoint policy or renderer behavior is shipped from this experiment.

Matching-build debugger probes now measure the allocator graph at entry and
terminate the pipeline-only child before further compilation. The original
module requests 35,442 nodes, 26,788,659 interference edges and 416,195,416 bytes
for adjacency lists and their triangular bitset. Five initial scheduling
attempts fail allocation; the first spill grows graph capacity from 35,456 to
70,912 nodes, quadrupling the bitset to 314,277,552 bytes. These measurements
locate an initial allocation barrier; they do not account for the complete
8 GiB guest peak or prove which later allocation causes the OOM.

The eighty-variable checkpoint increases the initial graph to 69,202 nodes and
1,002,890,076 adjacency bytes. Other external candidates retain the original
guest branches and validate for Vulkan 1.4, but do not reduce this initial
barrier:

| External candidate | Initial nodes | Initial interference edges |
| --- | ---: | ---: |
| CFG-liveness candidate using Undef | 35,442 | 26,788,659 |
| CFG-liveness candidate clearing locals | 35,674 | 27,334,053 |
| Ordered case traversal with barrier guards | 35,506 | 26,901,155 |
| Direct invocation-private Workgroup storage for eighty FP32 locals | 42,137 | 27,358,252 |
| Scalar register loads broadcast within each physical subgroup | 35,502 | 26,189,746 |
| Twelve grouped dispatcher loops | 35,470 | 27,300,973 |
| Local optimizer passes without global SSA promotion | 35,477 | 26,841,149 |

Direct Workgroup storage and scalar broadcasts also fail their first allocation
attempt. These are compiler diagnostics with zero queued guest dispatches;
they supply no pixel-equivalence or runtime-speed result. No such policy is
shipped. Before another structural change, isolate the contribution of repeated
address-translation searches and measure later spill growth. Merely decreasing
SPIR-V byte count or moving variables to shared memory is not a verified fix.
Broadcasting the barrier-generation exit or all dispatcher control values
produces the same initial graph as the original, with a failed first allocation.

Sixty bounded allocator entries expose later growth: the original reaches
72,594 nodes and a capacity of 141,824 after 24.662 debugger seconds. Its bitset
alone is 1,257,119,072 bytes. All 59 completed attempts fail; the probe stops
before allocating further. These debugger durations are not guest frame times.

A separate region experiment preserves the two generation barriers, admits
single-entry cyclic regions and emits direct acyclic paths with distinct
selection merges. The 181-block input has three phase entries, two cyclic
components of 19 and 116 blocks, and 46 acyclic regions. It emits 182 case
copies and validates for Vulkan 1.4. Three original fragment fixtures pass
52,224 GPU comparisons; a compute loop and a loop between two barriers pass
16 cases and 2,096 register comparisons, including partial EXEC and both banks.
The sixty-entry probe reaches 54,033 nodes rather than 72,594. Nevertheless,
the complete pipeline-only run still reaches its 4 GiB OOM bound after
167.393 seconds. Early graph reduction therefore does not establish a capacity
or speed fix, and this region policy is not shipped.

The captured CFG has seven natural loop headers. Both barrier cases are outside
the cyclic components. Five natural loops have one exit and contiguous blocks;
two nested loops have four backedges and two exits. The next structural
experiment must retain their exact exit state while removing internal
dispatchers. An external native-loop prototype now emits all seven loops,
validates for Vulkan 1.4, and passes the same 52,224 fragment and 2,096 compute
register comparisons. Its sixty-entry probe reaches 50,385 nodes, but the
complete compile again exhausts 4 GiB after 153.637 seconds. Removing the
internal dispatchers alone therefore does not resolve compilation capacity.

The two barrier resumes also form a finite chain of three phases with 5, 7 and
170 reachable blocks. A separate external prototype removes the enclosing
phase loop only when each phase has a unique barrier resume and the chain is
acyclic and bounded. It retains both generation barriers and continues retired
waves through empty later phases. Eight original two-wave cases pass 4,096
independent VGPR observations and 4,160 original/transformed word comparisons,
including four early retirements and different per-wave loop counts. The
fixture explicitly initializes observed SGPRs before early retirement; reading
unwritten SGPRs is not an equivalence contract. Nevertheless, its complete
pipeline-only run exhausts 4 GiB after 123.777 seconds. The finite-phase
transformation is therefore not a capacity fix and is not shipped.

Twelve matching-build spill selections from that module occupy classes with
6, 7 or 9 contiguous host registers and interfere with 15,078 to 32,350 nodes.
Those classes are host allocator metadata, not identified guest registers.
Further transformations must measure complete spill growth rather than treat
smaller initial graphs as acceptance. Direct invocation-private Workgroup
storage now also passes 52,224 fragment comparisons and the same 4,096 two-wave
VGPR and 4,160 equivalence-word checks. Its complete pipeline-only compile
nevertheless exhausts 4 GiB after 168.951 seconds. The shared-memory bound
allows eighty variables in that module; offloading them does not establish a
capacity fix. No Workgroup state policy is shipped.

A separate external Private-array candidate uses disjoint two-cell ranges for
102 direct cross-block FP32 locals, indexed by a stable invocation selector.
It requires 816 private bytes per invocation and does not consume Workgroup
memory or add a descriptor. Pointer escapes remain excluded. Its module
validates for Vulkan 1.4 and passes the same 52,224 fragment, 4,096 two-wave VGPR
and 4,160 equivalence-word comparisons. Its complete pipeline-only compile
still exhausts 4 GiB after 174.765 seconds. Its sixty-entry allocator probe
reaches 76,992 nodes, a capacity of 88,256, and 1,053,255,284 adjacency bytes.
Private FP32 storage alone is not a capacity fix and is not shipped.

The next external candidate also moves direct unsigned locals and does not
limit admission to locals referenced from several dispatcher cases. It selects
234 scalar locals (107 FP32, 127 unsigned), requires 1,872 private bytes per
invocation, validates for Vulkan 1.4, and passes the same fragment and two-wave
checks. Pointer escapes remain excluded. Its complete compile nevertheless
exhausts 4 GiB after 176.993 seconds, with 174.134 CPU seconds. A small parallel
read-only liveness calculation makes this wall duration unsuitable for a speed
comparison. Exact liveness finds 102 simultaneous direct FP32 locals and 102
colors; putting all of them in invocation-private Workgroup cells would need
59,920 bytes, above the queried device limit. Neither Private storage nor
Workgroup aliasing is accepted as a capacity fix.

The captured module has 256 guest invocations and 128 physical invocations,
with four complete guest waves and no thread-limit predicates in its initial
lane mask. The next bounded probe examines whether making that verified full
initial mask explicit reduces preserved inactive-register state. Partial waves
and dispatch thread limits must retain their existing predicates. No runtime
policy or new FPS result has been established.

Replacing only the initial lane predicates with true produces the same first
allocator graph and the same sixty-entry node growth as the original. Explicit
full mask words reduce the initial node count only from 35,442 to 35,140 and
still grow to 70,524 nodes before the bounded probe stops. Applying those
verified full mask words to the finite native-phase prototype reaches 57,173
nodes at sixty entries, above that prototype's prior 48,880. These are allocator
measurements, not complete pipeline or frame-time results. Full initial masks
alone therefore do not explain the allocation barrier; no mask optimization
is activated from these observations.

The matching Intel compiler exposes a host spill-batching option. Its default
value is eleven; setting `shader_spilling_rate=0` in one external child makes
each failed allocation spill one value per retry. At sixty allocator entries,
the unchanged original module reaches 46,886 nodes and 690,323,376 adjacency
bytes, compared with 72,594 nodes and 1,859,236,448 bytes under the default.
All 59 completed allocation attempts still fail in both probes. A complete
pipeline-only retry with the single-spill setting reaches its eight-minute
runtime limit without completing compilation. It consumes 475.415 CPU seconds
and peaks at approximately 1.9 GiB; no guest dispatch is queued. Lower spill
growth therefore does not establish a usable compilation time or an FPS fix.
No global driver configuration or runtime spill policy is changed. The next
investigation must measure the live values responsible for allocation failure,
rather than extend an unbounded sequence of spill retries.

A matching-build read-only probe now checks the allocator owner's live-range
metadata and class sizes. The original module has 35,438 virtual registers
and 58,580 backend instructions; its maximum interval overlap is 992 values,
requiring 2,172 contiguous register units. This is the allocator's conservative
interval model, not a count of live guest registers. The finite native-phase
candidate still reaches 761 overlapping values and 1,771 units.

An external rotated Private-array candidate places the same 234 direct locals
in 936 bytes per invocation, using a stable bijection of array indices. The
compiler actually reserves 29,952 scratch bytes for its dispatch, but the
initial graph rises to 51,068 nodes and fails allocation. Its maximum interval
overlap remains 1,063 values and 2,125 register units. Thus preventing all these
locals from becoming SSA does not remove the remaining temporary pressure.
The module validates for Vulkan 1.4, but has not passed GPU equivalence or a
complete compile and is not activated. Investigation proceeds to temporary
lifetimes within address-translation helpers rather than another storage-only
variant.

The owner-checked backend walk identifies all 992 values overlapping the
original peak. Of them, 588 share the interval from instruction 141 to 58,575,
almost the entire enclosing dispatcher loop. Their definition histogram
includes vector selects and additions, single-channel shifts and moves, and
undef initializations. This locates conservative temporary retention but does
not map those values to specific guest instructions.

Further external probes keep zero queued guest dispatches:

| Candidate | Initial graph nodes | Maximum interval register units |
| --- | ---: | ---: |
| Pack 28 low/high word pairs with a bit-preserving vector cast | 35,442 | 2,172 |
| Six-level radix lookup retaining full chunk-fit checks | 38,355 | 2,254 |
| Inline helpers, then clear state dead on the decoded guest CFG | 37,386 | 2,388 |
| XOR-indexed Private arrays for the original 234 direct locals | 49,630 | 2,101 |
| XOR-indexed Private arrays for 492 scalar locals after helper inlining | 79,862 | 2,220 |

All modules validate for Vulkan 1.4. The packed-pair variant produces the same
backend instruction count and initial graph as the original. The radix variant
removes binary-search loops from the address helper but increases the initial
pressure, so that helper's loops alone do not explain the barrier. Inlining
admits previously escaping scalar temporaries to CFG analysis, but adding
59,887 dead-state clears still increases pressure. Private storage also leaves
large temporary intervals even after those helpers are inlined. These probes
do not establish full compilation, GPU equivalence, runtime speed or gameplay;
none is shipped. The next bounded question is whether rematerializing Private
slot addresses at use removes their long intervals, within the shared-memory
limit, before considering any production strategy. That probe now uses 512
additional shared bytes for stable invocation ownership and requests 324,733
allocator nodes before its 3 GiB cgroup OOM kill, after 30.409 wall seconds.
It does not reach live-range measurement. Private-slot rematerialization via
shared ownership is also excluded. The external probe now stops graph requests
above 100,000 nodes before allocating them. Production memory and compiler
policies remain unchanged.

A separate external StorageBuffer candidate materializes 490 direct scalar
locals after helper inlining and retains the two dispatcher-control locals.
Disjoint invocation slices preserve raw 32-bit values; the proposed workspace
requires 262,144 bytes per physical workgroup. Its matching-layout Vulkan 1.4
module validates and reduces peak interval pressure to 1,263 units, but raises
the initial graph to 90,084 nodes and backend size to 124,093 instructions.
Inlining alone reproduces the original graph and pressure, so it is not the
source of that reduction. A complete pipeline-only run still reaches its
180-second limit without finishing, with 177.828 CPU seconds and a 3.6 GiB
service peak. No guest dispatch or runtime workspace is activated.

Combining that materialization with the finite native phases also validates,
but increases the peak to 1,612 units; its initial graph contains 84,377 nodes.
Neither smaller early pressure nor a new buffer representation establishes
compile-time or frame-time acceptance. Further work must preserve the known
subgroup-uniform scalar state when assessing any materialization strategy;
broadcasting arbitrary unsigned temporaries would alter guest vector values.

A fresh bounded strict Silent/Native source capture with Vulkan 1.4 reaches
its 120-second process limit, with 126.545 CPU seconds and a journal-reported
peak of about 4 GiB. Its native fifteen-second watch records zero frame and
present progress. The last generated compute source has 175 dispatcher cases;
assembly reproduces the corresponding captured module body exactly. This
recovers the actual scalar-register names without guessing a map from the
larger 181-case cached fixture. Source order alone is not a complete host stack
or proof of which later pipeline is active at timeout.

That source-matched program requests 34,519 initial allocator nodes and has
2,156 interval register units. External materialization of 487 direct scalar
locals reduces the peak to 1,263, with 86,618 graph nodes. Preserving the known
subgroup-uniform SGPR, EXEC, EXECZ, SCC and VCC state on their materialized loads
reduces the peak only to 1,217 and raises the graph to 87,292. Arbitrary unsigned
temporaries remain excluded from the broadcast. Representing the one dedicated
immutable metadata buffer with point-of-use StorageBuffer reads changes that
peak only to 1,209, with 88,358 nodes. These Vulkan 1.4-valid diagnostics do not
establish complete compilation, GPU preservation or a runtime policy.

Two other bounded address-liveness questions are also excluded as sufficient
fixes. Volatile Uniform loads in the original 181-case module change the peak
from 2,172 to 2,105 and raise nodes to 36,608. In its materialized counterpart,
a stable per-invocation workspace base read at each basic block changes 1,263
to 1,181 while raising nodes to 96,406; the workspace size remains unchanged.
Neither result meets the compile or execution gate.

An external unsigned word-pair representation preserves the 175-case guest
CFG, carry, borrow, high/low unsigned comparisons, the observed constant
shifts and physical pointer bit patterns. It validates for Vulkan 1.4 and
replaces all unsigned 64-bit value operations; an unused signed 64-bit type
and capability remain. The initial graph instead grows to 38,579 nodes, and
interval pressure barely changes from 2,156 to 2,154. Thus native unsigned
64-bit arithmetic alone does not explain the barrier. No GPU equivalence or
complete compile result exists for this representation, and it is not shipped.

Matching driver source excludes partial register writes from full block-def
kills. Read-only probes identify many width-one definitions with intervals
spanning the outer loop, but that does not prove those values are unnecessary
or that the driver is incorrect. Trace their actual producer and consumers
before another shader representation or allocator change. All external
pipeline probes queue zero dispatches; production compiler, memory, descriptor
and driver policies remain unchanged.

The width-one shift producers use an immediate count of eight. Matching
sampler-send lowering constructs packed surface/sampler descriptor bits with
that shift; the traced values have multiple partial writes and depend on
width-one sends. This is a resource-lowering lead, not evidence that guest
64-bit address arithmetic is responsible or that every such interval is
semantically unnecessary.

An external immutable-sampler layout leaves the original 175-case graph and
pressure unchanged: 34,519 nodes and 2,156 units. Folding eleven known sampler
metadata indices validates for Vulkan 1.4 but only changes those values to
34,341 and 2,138. Conservative continuation-CFG propagation, including
function-parameter write effects, resolves sixty image-register reads and
still produces that same backend graph and pressure. Immutable samplers and
these sampler-index constants alone are therefore excluded as sufficient
compile-capacity fixes. No complete pipeline, guest dispatch or FPS improvement
is established by these probes; all remain outside the production renderer.

A read-only resource-layout capture confirms the source-matched metadata
ordering. Folding its thirty-four known texture/sampler index loads validates
for Vulkan 1.4 and changes the initial graph to 33,930 nodes, with 1,740 interval
units. This includes typed image-array tags; other descriptor fields remain
runtime inputs. It is still compiler-only evidence, without complete compilation
or GPU equivalence, and is not activated.

A separate matching-layout descriptor-buffer probe admits the advertised host
feature and keeps the original shader unchanged. Its initial graph has 34,017
nodes and 1,841 units; singleton classes at the peak fall from 308 to 51.
Combining descriptor buffers with the external scalar-state workspace produces
87,278 nodes, 163,928 backend instructions and 937 units. The complete pipeline
still reaches its 175-second limit, after 173.572 CPU seconds and a 3.4 GiB
service peak, without a pipeline result. No guest dispatches are queued. These
representations do not establish a capacity fix or frame-time improvement.

An independently authored descriptor-loop reproducer contains no workload
code or data. Both eight-case and sixty-four-case modules validate for Vulkan
1.4. Their initial interval pressure is 43 and 155 units respectively. In the
larger program, 128 packed-descriptor temporaries span the outer loop from
instruction 5 to 2,826. Each is consumed by width-one OR/AND operations reading
its low word at byte offset zero; the remaining register bytes are not read.
This isolates the partial-register-def question independently of the private
program. The next compiler experiment must account for bytes actually read
and preserve predication, masking, wider reads and indirect access; it does
not authorize treating every long interval in the original program as dead.
No driver modification or system installation is validated by this evidence.

An external Mesa 26.2.3 build now tests that question in
`brw_analysis_liveness.cpp`, without changing the installed driver or Kyty's
renderer policy. It conservatively collects the byte spans of all VGRF reads.
An additional block-def kill requires a contiguous NoMask write covering all
read bytes, and excludes predicated non-select writes and indirect/subregister
addressing. Wider reads, high-word reads, masking, predication, prior reads and
noncontiguous writes retain their existing lifetime. Two independently authored
contract cases fail before the correction and pass afterward; the six negative
cases pass in both versions. All 299 compiler tests pass with the correction.

The same public sixty-four-case module falls from 155 to 28 interval register
units. Its complete pipeline creation changes from 132.950 to 36.105 ms in the
two local builds, with the shader cache disabled. Seven GPU dispatches exercise
distinct sampler LOD clamps and check 14,336 values against expected mip
colors; both builds produce zero differences. These are focused compiler and
GPU-preservation results, not a workload FPS benchmark.

The original 175-case module retains its 56,898 backend instructions and 34,515
VGRFs, while interval pressure falls from 2,156 to 1,896 units. With the external
correction, complete pipeline creation now finishes in 104.767 seconds under
the 4 GiB bound, with a 2.2 GiB service peak and no queued dispatches. This passes
the probe's complete-compilation gate, but remains a long cold compile. A
scoped warm-cache retry of the same module and probe layout without push
constants finishes in 9.236 ms with 28,280 KiB peak RSS. This is one cached pipeline,
not the complete workload. Private-program GPU execution, strict runtime
progress, frame time and gameplay acceptance still require measurement. No system
driver installation or vendor-specific production policy is made.

A strict Silent/Native run with the external driver and scoped cache still
ends in its 4 GiB cgroup OOM bound after 210.015 wall seconds and 221.135 CPU
seconds. At the last native snapshot, 153.961 seconds into the run, seventeen
compute pipelines have completed, totaling 81.357 seconds of Vulkan creation
time; thirty-five dispatches and 272 submissions are recorded, with zero
presents. Completed fence waits total only 0.186 seconds. No usable frame-time
sample or native graphics capture exists. The next evidence must identify the
actual pending compute module and stage flags; successful compilation of an
earlier source-matched fixture does not establish which pipeline exhausts the
runtime bound. This remains an unresolved compile-capacity failure, not proof
of improved gameplay or a current Xe reset.

A subsequent bounded Vulkan-call capture identifies the eighteenth application
compute pipeline before creation. Its SPIR-V matches the 175-case fixture
byte-for-byte; its full-subgroup flag and required size of thirty-two also
match. Its actual layout differs from the earlier probes: seven sampled-image
bindings each reserve seventeen descriptors, including unused typed banks,
whereas the probe layout has only the three declared sampled arrays of ten,
six and one. Both use eight storage buffers, two storage images, seven
samplers and one metadata UBO, with no push range in the runtime layout. Thus
the standalone completion and cache timing must retain that layout qualifier.
`GraphicsRenderDescriptor.cpp:146` reserves the aggregate sampled count for
every sampled bank; measure the captured layout before changing that shared
contract or attributing the remaining capacity failure to different shader
code or subgroup flags. No descriptor compaction is activated.

A warm-prefix retry with an 8 GiB service bound also ends in cgroup OOM,
after 176.239 wall seconds and 185.944 CPU seconds. Seventeen cached compute
pipelines complete in 3.074 ms total at the last native snapshot, but the
eighteenth remains pending; there are still zero presents. Host available
memory stays above 11 GiB. Increasing this service bound or caching the
completed prefix therefore does not establish a capacity fix.

The captured layout's initial interval pressure is 1,888 units, with 34,514
VGRFs and 56,895 instructions. This is slightly lower than the compact probe's
1,896 units, so unused descriptor banks do not explain higher initial pressure.
A bounded allocator trace nevertheless reaches 128,931 graph nodes, 179,395
instructions and 484,608 logical scratch bytes after 124.881 seconds, without
successful allocation. The probe stops at its RSS/time bound and queues zero
dispatches. Compare complete allocation trajectories with the same device
features before attributing that later growth to descriptor padding.

After relocating the runtime executables, external driver, caches and evidence
to a local btrfs filesystem, the compact-layout trace completes the unchanged
module in 94.518 seconds, with 2,351,668 KiB peak RSS and zero dispatches. Its
ninety-third allocation attempt succeeds with 120,677 graph nodes. The two
probe sources differ only in their descriptor-binding include. A complete
captured-layout run instead reaches its 4 GiB cgroup OOM bound after 156.595
wall seconds and 155.318 CPU seconds. Thus this capacity failure is reproduced
without executing the emulator or its caches on NTFS. This is layout-dependent
compiler evidence under the probe's device configuration, not private-program
GPU equivalence or authorization for an unverified renderer change.

### Per-array descriptor layouts (2026-09-29, strict compile progress)

The renderer now derives each layout from the separate image-array sizes
already declared by its shader generator. It preserves sparse binding numbers,
depth arrays, storage resources and every declared numeric dispatch bank.
Layout and descriptor-set caches include the complete binding/type/count key;
per-stage and pipeline limit checks count the same plan. Layout handles remain
live for their dependent sets and pipelines, with a bounded cache. The shader
module and descriptor writes are unchanged.

An independent public image/sampler program compares padded and separate-array
layouts on both the installed and external compiler builds. Distinct image
views and seven sampler LOD clamps produce 57,344 checked GPU values with zero
differences across four runs. This is preservation evidence, not a private
workload rendering result or a frame-rate benchmark. The Release executable
and native agent build successfully in a dedicated btrfs build directory;
boundary and graphics-table provenance gates pass. Focused unit cases for
shape keys, numeric banks, counts and binding collisions compile; their
execution remains deferred until the workload runs, as requested.

A strict Silent/Native run using the external compiler completes eighteen
compute pipelines and reaches thirty-six dispatches and 290 submissions at
the last native snapshot, with zero presents. It ends at its 6 GiB cgroup OOM
bound after 263.393 wall seconds and 277.620 CPU seconds. A bounded Vulkan-call
capture confirms that pipeline eighteen uses the byte-identical earlier
175-case module and unchanged stage/subgroup flags, with sampled descriptor
arrays of ten, six and one instead of seven arrays of seventeen. The next
pending pipeline is a different, larger module using that same corrected
layout. Thus aggregate descriptor padding is a verified compile-capacity
contributor, but is insufficient to solve the remaining compilation cost.
The next investigation must trace this nineteenth pipeline's allocation
trajectory and device feature admission. No current frame-time, graphics
capture, controls or gameplay acceptance exists.

### Immutable resource-index specialization (2026-09-30, strict compile progress)

The nineteenth pending module uses the corrected separate-array layout. A
matching-runtime-device pipeline probe reaches its 120-second bound without
completing compilation, with 118.965 CPU seconds and a 2.6 GiB service peak.
An owner-checked allocator trace separately reaches 125,659 nodes, 174,599
instructions and 416,544 logical scratch bytes after 114.731 seconds; it stops
at its RSS bound without dispatching the program.

A source-matched resource capture recovers the immutable image/sampler indices
that the host binding code writes into metadata word zero. Replacing only
thirty-four such loads, with all other descriptor words retained as runtime
inputs, completes the same module's pipeline in 44.080 seconds and 1,733,468
KiB peak RSS. Layout, subgroup flags and enabled runtime device features match;
the shader cache is disabled and zero private-program dispatches are queued.
A public image/sampler loop compares dynamic and known index loads over seven
metadata variants on both driver builds: 57,344 GPU values have zero
differences. That preservation result does not cover private-program pixels.

The production candidate derives these indices from the bind's existing
spatial/numeric banks, handles mapped scalar loads and initial SGPR metadata
through one emitter, retains mutable loads and versions the module cache.
The image-versus-storage classification participates in module identity.
Focused cases compile but remain deferred. The Release build and boundary/table
provenance gates pass. A strict Silent/Native integration run advances from
eighteen completed compute pipelines and zero presents to forty-eight completed
compute pipelines and 643 observed presents. Its native snapshot has a 172 ms
median and 241 ms p95 frame time; these are current timings, not a matched
frame-rate comparison. A scored native capture at present 643 is entirely
black (`low_entropy`), and cannot establish a visual or gameplay fix. The run
logs the title transition and subsequent map resource load, then reaches its
6 GiB cgroup OOM bound after 356.215 wall seconds and 337.890 CPU seconds.
Separate the remaining workload-memory and visual frontier from the resolved
nineteenth pipeline compilation; a warm-cache run must capture the opening,
transition and later state before input/gameplay acceptance.

### Descriptor-array feature admission (2026-09-29, unresolved)

`Window.cpp:2083` initializes device features to zero and leaves sampled-image,
storage-image and storage-buffer array dynamic indexing disabled. A read-only
Vulkan-call capture confirms those disabled feature values. The pending
compute module contains 194 nonconstant UniformConstant-array access chains;
its declared capabilities also omit the corresponding array dynamic-indexing
capabilities. This is an admission/emission contract to audit against Vulkan's
shader-interface requirements, independently of the compiler capacity issue.
The [Vulkan shader-interface contract](https://docs.vulkan.org/spec/latest/chapters/interfaces.html)
defines the required features and capabilities for each descriptor-index class.
The existing standalone compiler probe enables all supported core features,
so it does not reproduce that device configuration. No validation-layer result
or causal performance claim exists for this discrepancy. Check the resource
index uniformity and required capabilities, then admit supported features and
reject unavailable requirements before enabling a production correction.

### Loaded image resource lifetime (2026-09-29, compiler verified)

Sampled-texture and sampler lookup now checks a live mapped load before the
initial SGPR binding. An initial flat texture and a later array descriptor
share one SGPR home in the captured pixel program. The later load's recorded
consumer lifetime identifies the array descriptor, but lookup previously
returned the initial flat binding. Comparison sampling then rejected the
instruction's four array coordinates. With the lookup priority corrected,
complete-program replay selects the loaded array and emits both lane banks.
This is resource-selection and compiler evidence; resulting guest pixels
and frame time remain unverified.

### Paired wave primitives (2026-09-29, GPU replay verified)

Shared paired execution now implements bitwise DPP rows and row-table
permutations with architectural source EXEC and destination masks, signed
SDWA integer conversion, whole-quad masks and inactive image destination
preservation. The combined Vulkan 1.4 GPU replay covers 320 cases and 42,064
register observations with zero differences against independent ISA
references. Eight malformed DPP tuples are rejected. The local Linux build,
emulator boundary gate and graphics-table provenance gate pass. Details and
remaining fragment adapters are recorded in `fragment-wave-execution.md`.

This does not enable compute fragment execution or provide a new guest FPS
measurement. Strict integration attempts end before any present: one is
terminated by host-wide OOM and another reaches its 6 GiB service limit
during pipeline compilation. Preservation of the strict guest frontier is
therefore still awaiting a run with sufficient memory.

A separate pipeline-only experiment tests whether eliminating redundant
local loads solves the compiler memory barrier. A single-block elimination
pass reduces the large module from 77,860 to 70,865 operations without adding
phi nodes; both variants still reach a 4 GiB cgroup OOM limit after roughly
three minutes. GPU reference checks remain exact. The capacity hypothesis is
disproven, and no optimizer policy change is shipped. The next investigation
must measure the driver's memory producer rather than infer it from SPIR-V
operation counts alone. Unit tests and gameplay gates remain pending.

### Vulkan 1.4 baseline (2026-09-29, opening image verified)

The active build now requires Vulkan 1.4 SDK headers and external SPIRV-Tools
with the Vulkan 1.4 target environment. Instance creation and device selection
reject older runtime versions. Both assembly and optimization use that same
environment, and the persistent cache target changes so older compiled entries
cannot bypass it. Historical Vulkan 1.2 validation results below remain
historical evidence rather than the current build contract.

Configuration verifies the installed SDK and compiler target. The full local
build succeeds. The generated detile module and the complete captured compute
program, including the array comparison correction, validate under Vulkan 1.4
with SPIR-V 1.6. A strict guest run records an actual instance request of 1.4.0
and preserves the opening caption in a scored native present-228 capture. This
first run spends 555 seconds creating compute pipelines; one 358,848-word
module takes 395 seconds inside the Intel driver. A read-only stack locates
that cost at `GraphicsRenderPipeline.cpp:917`; subsequent frames resume. The
warm interval presents 473 through 648 in 57.301 seconds (3.054 per second),
with no new SPIR-V compilation or Vulkan pipeline creation. The corrected
comparison-image dispatch completes on its recorded queue before a later
unsupported scalar instruction stops parsing. Later scene output and gameplay
still require separate evidence. Hosted release builds now install the
required SDK components. Windows and macOS builds and distribution packaging
remain untested locally.

### Dispatch address preparation timing (2026-09-29, native verified)

Native `perf-snapshot` and `diagnostics.performance` now expose call counts,
total time and maximum time for dispatch write-back, guest-address preparation,
residency discovery and tracked-snapshot refresh. Preparation includes registry
locking and table work; residency and refresh are nested intervals. They overlap
with processor and fence timings and are not exclusive frame costs.

The owned Linux build passes, as do strict source boundaries and the graphics
table manifest. A strict Silent/Native run under read-only debugger probes
verifies all twelve new fields in both serializers and their snapshot/reset
window. A 56.366-second warm interval
contains 216 presents, a median frame time of 245 ms and 124 submissions per
present. Dispatch processing accumulates 29.44 seconds, address preparation
1.90 seconds, residency 1.81 seconds and write-back 1.19 seconds. The scored
present-791 capture is uniformly black. Translation still stops on a pixel DPP
row shift at PC 0xfec; no scene or gameplay result is established. These counters
identify costs without changing guest synchronization or residency decisions.

### Compute wave-analysis preparation cost (2026-09-29, bounded verified)

Read-only host stacks locate repeated owned-string allocation and destruction
inside `ShaderComputeWaveNativeEquivalence.cpp`. Enum classification now uses
views of the same static names. Label validation and CFG target lookup use a
binary search after proving decoded PCs are sorted; reordered IR retains the
original linear search, and duplicate PCs retain their first matching index.
No admission condition, shader instruction or synchronization rule changes.

Two actual captured compute programs are replayed thirty times before and after
the change. The 2,924-instruction program retains its rejection, PC and reason;
median analysis time changes from 103.025 to 9.278 microseconds. A separate
137-instruction program captured at live analysis retains its successful result;
its median changes from 142.952 to 72.728 microseconds. These measure this CPU
analysis alone and do not establish an equivalent frame-rate improvement.

`ninja -C _build_linux_astro_play_s1 -j2 fc_script` succeeds. Strict source
boundaries and the graphics-table manifest pass. A fresh strict Silent/Native
run under read-only debugger probes preserves the same first pixel-emitter
failure at PC 0xfec. A 58.031-second warm window contains 229 presents with a
243 ms median frame time; the scored
present-813 capture remains uniformly black. Gameplay and acceptable runtime
performance are still unverified; focused unit tests remain deferred until
gameplay as requested.

### Compute dispatch input preparation (2026-09-29, bounded measured)

The command processor previously performed the complete compute input analysis
once to decide whether to drain guest-address writes, then again to record the
dispatch. Preparation and recording now share that analysis under the render
lock. A guest-address dispatch requests its processor drain before touching the
command buffer, then retries with newly analyzed inputs. Peer submission waits
still release both recording locks and retry with fresh inputs. The initial
processor drain does not consume the existing sixty-four peer retry budget.
No resource snapshot is cached across a wait.

Two strict Silent/Native runs without a debugger measure forty-five-second
warm introduction windows beginning after present 153. Both use Vulkan 1.4,
native resolution, the same compiled shader cache and resource bounds; neither
window compiles SPIR-V. The earlier executable differs only in these dispatch
preparation objects. Its launch uses a PTY while the new run uses a pipe;
logging is Silent in both measured windows. This transport difference and the
single sample per arm limit the comparison.

The earlier window contains 194 presents in 45.038 seconds (4.307 FPS), with a
226 ms median frame time. The new window contains 228 in 45.039 seconds
(5.062 FPS), with a 194 ms median. Input analyses per processor dispatch change
from 2.0001 to 1.0725, and accumulated dispatch processing per present changes
from 128.815 to 97.968 ms. Dispatches per present remain 77.81 and 77.73;
submissions per present remain 126.60 and 127.09. These samples measure a
17.5 percent introduction frame-rate increase, not a gameplay benchmark or
acceptable overall performance.

The owned Linux build, strict source boundaries and graphics-table manifest
pass. Both native captures are uniformly black and their offline score gates
exit 1. The new strict run preserves the first pixel-emitter failure at PC
0xfec and exits 65. The earlier guest also exits 65 according to its service
journal; its PTY proxy loses output, so that arm's final error text is unavailable.
Gameplay and the missing wave64 pixel lowering remain unresolved. Focused unit
tests remain deferred until gameplay as requested.

### Metadata descriptor liveness preparation (2026-09-29, bounded measured)

Read-only host stacks also locate per-instruction hash-node destruction in
`ShaderStorageAnalysis.cpp` while preparing sampler metadata. Each storage or
sampler query rebuilt the same PC-to-instruction index. The liveness traversal
now validates strictly increasing PCs and searches the existing instruction
array directly. Reordered IR retains the previous hash lookup; duplicate PCs
retain the conservative all-live result. Joins, unresolved targets, indirect
transfers and descriptor overwrite rules are unchanged. No analysis is cached
across guest updates.

Two captured programs are replayed thirty times, querying storage and sampler
evidence at fifty-one SGPR starts per iteration. Complete result digests are
identical before and after. The 137-instruction compute program's median query
batch changes from 540.648 to 81.916 microseconds; the 2,300-instruction pixel
program's median changes from 9,545.339 to 1,556.091 microseconds. These measure
analysis alone, not an equivalent frame-rate increase.

The owned Linux build and strict source/table gates pass. A fresh strict
Silent/Native run without a debugger reaches present 750, then preserves the
same first pixel-emitter failure at PC 0xfec and exits 65. Its forty-five-second
warm introduction interval contains 249 presents in 45.039 seconds (5.529 FPS),
with a 169 ms median frame time. The preceding dispatch-preparation run had
228 presents in the same duration (5.062 FPS), with a 194 ms median. Accumulated
dispatch processing per present changes from 97.968 to 76.095 ms; dispatches
and submissions per present remain approximately 77.7 and 127.1. Both windows
start after present 153, use the same resource bounds, native resolution and
compiled shader cache, and contain no SPIR-V compilation. This single-sample
comparison is limited to the introduction. The new native present-403 capture
is uniformly black and its offline score gate exits 1. Acceptable performance,
scene output and gameplay remain unresolved; unit tests remain deferred until
gameplay as requested.

### Sparse physical residency query (2026-09-29, not gameplay)

Read-only sampling found descriptor preparation repeatedly scanning an entirely
unpopulated two-gigabyte physical-backing interval. The owned Linux backing can
prove the complete interval empty with `SEEK_DATA`. A fresh query on each
preparation now avoids that interval's `mincore` scan; unproven intervals and
unsupported hosts keep the existing residency path. No absence is cached.

In a strict Silent/Native comparison, the warm opening interval changes from
0.884 to 2.969 presents per second. Descriptor preparation falls from 423.33 to
5.14 milliseconds per present, or 25.02 to 0.30 milliseconds per draw. The
native present-173 capture preserves the opening caption. The run reaches
present 809 and the same unresolved comparison-image binding, with no observed
earlier failure. The image after the caption and gameplay remain unverified;
this is a measured host-side cost reduction.

### Exercised custom pixel interpolation gap (2026-09-29, unresolved)

An address-correlated native lifetime trace identifies two draws writing the
sampled overlay image before final composition: an indexed six-index material
draw, then a three-vertex draw that exports zero to two channels. The first
draw has four pixel inputs, including control `0x424` for input three, and its
decoded program uses `V_INTERP_MOV_F32` selectors 2, 0 and 1 on that input.
The prior sampled context alone did not identify an executed writer.

`ShaderPixelInterpolator.cpp:104` rejects that parameter-cache pass-through
control. `ShaderSpirvVector.cpp:2802` continues after the failed decode, and
`Recompile_VInterpMovF32_VdstVsrcAttrChan` loads the same ordinary varying for
all three selectors. This general custom-interpolation gap, previously
excluded for a different program with only P1/P2 instructions, is now exercised
by the observed overlay writer. Resolve the producer parameter and distinct
per-vertex values, preserving their raw packed bits and the guest barycentric
inputs. The 89,132-byte pixel module read from the live translation cache fails
`spirv-val --target-env vulkan1.2`: conflicting fragment input location zero,
component zero (`VUID-StandaloneSpirv-OpEntryPoint-08721`). This supplies an
actual-module red validation case. A corrected strict comparison must establish
whether this is the producer of the nonfinite overlay values; the code mismatch
alone does not yet prove that causal result.

The first correction transports raw parameter values through a generated
triangle geometry stage, using distinct flat unsigned vectors for each
vertex and perspective/linear center or centroid coordinates for manual
interpolation. It selects the route from the consumed controls and the host
geometry capability, checks interface limits and gives that route its own
pixel/module identity. Ordinary inputs retain their interpolation modes.
The exercised pixel module (30,488 bytes) and geometry module (1,868 bytes),
read from the live translation cache, both pass Vulkan 1.2 SPIR-V validation;
the pixel source and optimized disassembly also validate. The original
duplicate-location failure is gone, with four component loads from each of
the three vertices and initialized barycentric VGPRs. Native captures at
presents 93 and 184 remain uniformly black. This corrects the exercised
interface and selector translation but does not establish useful rendering
or identify the remaining producer of zero/nonfinite output. PARAM0's
ambiguous custom/default encoding, sample/pull-model barycentrics and layered
geometry still require separate contracts.

A subsequent native probe of the corrected material's MRT0 export at the
100-present threshold counts 409,600 invocations: every result is finite and
RGBA zero. The probe's generated and optimized modules validate for Vulkan
1.2, and capture 157 is still uniformly black. This observes zero at the
material output; it does not yet locate its first zero-valued input or prove
per-pixel correspondence with the earlier nonfinite final sample.

### Storage write-back dependency retry (2026-09-29)

A later Silent/Native diagnostic run passed at least 598 presents, then stopped
after 708 seconds at `Objects/GpuMemoryWriteback.cpp:214`: a writable
`StorageBuffer` still had incomplete exact submission dependencies when
`WriteBackObjectLocked` was requested. This occurred before the JSON assignment
call in that run. An earlier shorter
occurrence is already recorded below. Capture the current caller, resource
dependencies and completed queue sequences before changing synchronization;
neither skipping write-back nor treating a pending fence as complete is valid.

A subsequent run captures the exact cross-queue state. A compute command
buffer on queue 7 calls `GuestDeviceAddressWriteBack` through
`GraphicsRenderBind.cpp:4188`; the selected writable storage object depends
on queue 8 sequence 48643, while the published completion is 48642. Queue 8
sequence 48643 is still the graphics processor's active recording buffer,
not an executing Vulkan submission. `CommandProcessor::DispatchDirect`
already calls `WriteBack`, but that drains its own compute processor only.
The later registry-wide storage publication therefore encounters another
processor's recorded resource use. Capture the access role of that use and
move exact dependency preparation outside recording/mutation locks before
publishing host-visible bytes; adding a fence wait inside `BindDescriptors`
would hold the render lock across the other processor's completion path.
This run stops before exercising the pending JSON string conversion.

Dispatch preparation now checks the same writable storage objects and exact
submission dependencies as publication, while holding the render recording
lock. A pending use returns to the command processor before command-buffer
mutation; the processor releases recording locks, waits for that submission,
and retries with a bounded attempt count. Indirect dispatch shares this path.
A strict live trace exercised three retries against graphics queue 8 sequences
24691, 24692 and 24693. All three dispatch calls returned with queue 8 completed
through 24700, and the run continued through present 807 to an unsupported
image-atomic instruction. This verifies the exercised cross-queue retry;
gameplay acceptance and deferred focused tests remain outstanding.

The exercised custom-interpolation program also reads its incoming
`FRONT_FACE` VGPR. The pixel prolog in `ShaderSpirvGenerator.cpp` initializes
XY position but does not initialize that field. The captured module declares
the register without an initializer and first reads it at an integer compare;
its first store comes after that read. This is undefined input, not a proven
zero-valued front-face flag. Its live `SPI_BARYC_CNTL` is `0x01000000`. AMD's public
[register guide, page 189](https://docs.amd.com/api/khub/documents/9fuBVmqajj07G~5~aeTUig/content)
defines bit 24 as selecting integer one/zero for front/back, versus floating
positive/negative one when clear. Shader metadata now carries that control,
and the enabled input adds a versioned identity with its encoding. The pixel
prolog loads Vulkan `FrontFacing`, selects the requested bit representation
and initializes the VGPR before guest instructions.

A subsequent Silent/Native run validates the live 30,596-byte ordinary pixel
module, its 33,828-byte diagnostic variant and both 1,868-byte geometry
modules for Vulkan 1.2. Their identities contain the new front-face version
and integer encoding. The diagnostic source and optimized module also
validate; the source initializes VGPR4 with integer one/zero before the old
first read. The floating representation still requires focused validation.
At the same 100-present threshold, all 409,600 material invocations remain
finite RGBA zero, and native capture 134 remains uniformly black. This
corrects the missing system input but excludes it as a sufficient fix for
that black output.

A read-only snapshot at the diagnostic draw's actual descriptor bind records
its scalar input buffer. Both opacity factors loaded at byte offsets 16 and
24 are floating one. The zero at offset 16 seen at an earlier first draw is
therefore not evidence for this later all-zero export. Follow the texture
sample and packed per-vertex alpha at this same draw before changing either
buffer contents or guest media state.

The implicit-LOD 2D RGBA sample is now also admitted by the bounded native
sample probe. At the same threshold it records 409,600 finite RGBA-zero
vectors immediately after sampling, before material arithmetic. Its actual
33,300-byte cached diagnostic module, 30,596-byte ordinary pixel module and
both geometry modules pass Vulkan 1.2 validation, as do the generated and
optimized diagnostic modules. Capture 129 remains uniformly black. The
correlated descriptor is a 1600-by-256 BC7 sRGB image with Standard64KB tiling
and eleven allocated levels; all four vertex colors are opaque white.

`Utils.h:155` excludes uncovered Standard64KB BC formats from guest upload.
`Objects/Texture.cpp:533` consequently seeds a zero image when that policy is
selected. The Standard64KB sample path also lacks a compressed mip-chain
layout. This is a concrete unsupported-package defect; confirm the actual
backing role and bytes before attributing the observed sample to it. A later
cache snapshot no longer contains that opening texture, so it does not prove
which materialization path was used at the probed draw. Capture the creation
and implement the evidenced block-coordinate mip layout, preserving live
GPU-owned surface dependencies.

The retired object record does retain the exact opening descriptor: eleven
levels, a 6,553,600-byte claimed span and `skip_guest_upload=1`. Its content
origin is recorded as CPU upload even though that path clears the image.
The still-readable source bytes decode into a coherent nonzero opening
caption with a meaningful alpha channel using Standard64KB block addressing
and the reverse mip-chain placement; they were read after object retirement,
not at the original draw. The
[public GFX10 layout](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/imported/addrlib/src/gfx10/gfx10addrlib.cpp)
and two local implementations agree on a shared tail block followed by the
larger levels in reverse order. For this descriptor the compressed block
model requires 983,040 bytes, with mip zero at byte 524,288 and the tail
beginning at level four. The original texel-based size and zero-seed policy
are therefore both incorrect for this CPU package texture. A corrected run
must capture its upload and sample before claiming visual improvement.

The corrected Silent/Native run captures creation with the expected
983,040-byte span and guest upload enabled. The shared detiler returns all
eleven compact levels; its first level is byte-identical to the independently
decoded source. At the same 100-present threshold, all 409,600 sampled
vectors are finite: RGB ranges from 0.99115 to one and alpha from zero to one.
Native capture 173 now contains the opening caption, although it is blue.
The offline capture gate still rejects it as gameplay. The affected build,
source-boundary gate and thirteen-table provenance check pass; unit tests
remain deferred until the requested runtime milestone. This verifies the
package upload correction, not gameplay or correct final composition.

The remaining color defect has a separate concrete trigger:
`GraphicsRenderHwCheck.cpp:547` incorrectly describes every color mode except
resolve as an ordinary draw, while both draw entry points route only mode
three through fixed-function handling. A bounded live trace captures mode
six with DCC enabled on the same HDR overlay after its material draw. The
ordinary shader exports zero to red and green, preserving blue and alpha;
that shader export is not the decompression operation's color result. Check
the existing expanded host backing and submission dependencies, then handle
the metadata operation before ordinary rasterization. Mode two also occurs
on other targets and denotes fast-clear elimination; its clear-state contract
must be handled separately from DCC decompression.

The expanded-surface handler now consumes mode-six operations before either
indexed or automatic ordinary draws. It requires one existing initialized
render texture with exactly matching extent, format and memory span, a single
sample/mip/layer, and no fast-clear key. It records the submission use through
the GPU-memory lookup. A bounded live probe confirms three enabled DCC
attachments return through this preservation path. With context restoration
corrected, native capture 89 contains the opening caption in white instead of
blue. Both native and offline scores classify this simple caption as low
entropy and not gameplay. The emulator/executable build, boundary gate and
thirteen-table provenance gate pass; focused test execution remains deferred.

The same run reports a separate stencil frontier from
`GraphicsRenderHwCheck.cpp:339`: stencil testing is enabled with neither read
nor write base present. It has not caused a process stop here. Capture the
producer and format before deciding whether that state is inactive hardware
state or a missing attachment; the opening caption does not validate stencil.
A later bounded read-only capture records both depth and stencil formats as
zero, with both stencil base addresses zero. `GraphicsState.cpp:146` still
classifies the enabled test as a missing plane before checking the disabled
format contract. Trace the format and test-enable interaction before changing
this classification; the observation alone does not validate stencil output.

### Register-default lookup and context restoration (2026-09-29)

A strict diagnostic run with expanded-surface DCC handling stops at present
35 on a 2432-by-1368 HDR target with no live GPU-memory object. Its ordinary
postprocess draw still has color mode six after the preceding metadata
operation. Original guest indirect lists and their immutable snapshots agree
at the failure. Creating a zero backing or recognizing operations by shader
words would conceal the missing state restoration.

A producer hardware watchpoint confirms API version thirteen reads seventeen
pairs from the sixteen-pair depth default in `Graphics.cpp`. The seventeenth
read lands on the next `RegisterDefaultInfo` header. This table-size defect
remains unresolved; verify the versioned depth layout before extending it.
The ordinary color-control default itself is copied correctly, and its
observed type lookup reads only the bank and pointer index. Upper index
metadata is not a supported explanation for the stale mode.

A separate missing target-zero blend key supplies an all-ones pair, later
used as an invalid register offset. Adding the key with the existing
`CB_BLEND0_CONTROL` default changes the live normal draw to the valid blend
register and removes the inherited reserved bits. The same DCC failure
remains, excluding this lookup defect as the stale-mode cause. The emulator
and executable builds, source-boundary gate, and graphics-table gate pass;
focused tests remain deferred until gameplay by the active investigation
instruction.

The guest surrounds its metadata helper with context-state operations three
and two. The import resolves to `GraphicsCbType2Pad` in `Graphics.cpp:4079`,
which dropped the operation argument and emitted one padding word. The
corrected HLE encodes clear/push/pop/push-clear and retains the helper's
allocation boundaries and byte-size query. The processor owns one saved CX
bank; shader and UCONFIG banks remain separate. In the strict comparison,
twenty pops restore the complete saved context byte-for-byte, including a
mode-six utility returning to normal mode one. The run passes the old
35-present failure. This verifies context restoration, with the pending
expanded-surface DCC handler present in that comparison; it is not gameplay.

The first comparison also exposes an older, separate reset-queue encoding
problem in `Graphics.cpp:3424`: it writes the third argument's low four bits
as a native `CLEAR_STATE` operation. The live value thirteen is outside the
hardware clear/push/pop/push-clear range. Tightening the native packet decoder
therefore stops earlier at queue initialization. Its existing behavior is
preserved for the context-helper comparison; trace reset-queue arguments and
its actual packet contract before correcting that encoder and decoder.

### Title-transition admission (2026-09-29, not gameplay)

The context-restored Silent/Native run reaches the title transition after
15 minutes 49 seconds. Two threads stop there. The 147-instruction compute
program now decodes scalar absolute value correctly, then fails in
`ShaderComputeWaveAnalysis.cpp:528`: the last mapped EUD load precedes two
separate ordinary writes to the former pointer registers. Both words are
replaced before later arithmetic reads. The prior lifetime proof rejected
any partial write and could not accumulate these definite per-word writes.
The correction tracks possible and definite word masks across the existing
control-flow edges. It retains rejection of mapped loads after either original
pointer word may have changed and of ordinary reads lacking a definite write.

The original and transformed programs now pass admission. With the subsequent
image-descriptor correction, the complete module passes Vulkan 1.2 SPIR-V
validation, and the recorded dispatch's queue advances beyond its submission.
Focused branch, partial-write and back-edge tests remain deferred as requested.

The next strict run admits the original and transformed 147-instruction
programs. Emission then fails at PC `0x298` in `ShaderSpirvImage.cpp`: a
storage-image descriptor has moved from its original SGPR range through four
scalar pair copies. A second store uses a descriptor loaded from the extended
table. Both resources are present as writable 2D arrays; the static register
lookup does not follow either origin. Track complete descriptor words through
copies, mapped loads, and control-flow joins, rejecting partial or conflicting
origins. This is descriptor selection, not a missing image dimensionality.

The first descriptor-origin comparison stops earlier at a different store.
Its producing scalar load maps to a sampled descriptor, while a separate
writable binding contains the identical eight guest words. Treating binding
usage as guest identity therefore rejects a valid sampled/storage alias. The
origin proof must select the unique writable binding with the same complete
T#, preserving the existing split binding model; register coincidence or a
matching address alone is insufficient. This closes the assumption that an
origin must already name a writable binding.

The alias correction selects the existing writable binding in that earlier
program. The next comparison exposes an analysis bug: a scalar load into VCC
was rejected even though it cannot change any tracked ordinary SGPR word.
Such a load preserves the descriptor-origin state; any later copy from that
untracked special register still produces an unknown origin. The captured
failure occurred before a later mapped texture load, so it does not disprove
that store's complete descriptor identity.

The corrected analysis passes both earlier programs and resolves the two
original stores to their expected writable bindings. The actual complete
module passes Vulkan 1.2 validation; its recorded dispatch completes before
the next strict failure. Complete eight-word origins survive scalar copies
and verified loads, while conflicting control-flow joins remain unknown.
Build, strict boundary and all 13 table checks pass. Focused origin-analysis
tests remain deferred, and no gameplay acceptance has been established.

### Image atomic and class translation (2026-09-29, compiler verified)

The next title-transition program stops at `ShaderParseMIMG.cpp:41` with
MIMG opcode `0x11`, 2D dimension, one data component and `GLC=1`. The RDNA2
ISA identifies `IMAGE_ATOMIC_ADD`; for atomics, GLC requests the pre-operation
value in VDATA. The parser also leaves this opcode unimplemented at its switch
case. Implement resource write classification, the atomic read/modify/write
and its conditional old-value return together, preserving EXEC and image
coordinate rules. Removing the GLC rejection alone cannot implement this
instruction. The preceding translated dispatch has completed on its exact
queue; this new failure is a later decoder boundary.

The implementation resolves the atomic's complete descriptor, uses its typed
R32_UINT storage alias, and retains the required VDATA source and conditional
old-value return. Full admission also exposed VOPC/VOP3 opcode `0x88` being
substituted with `SBarrier`. Both parsers now decode `V_CMP_CLASS_F32`; the
emitter classifies the ten binary32 classes from their bits and uses the
existing paired-wave mask packing. The captured SDWA form uses DWORD sources,
an ordinary scalar destination pair and class mask `3`.

Complete-program replay then exposed an array-view gap in
`Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF`. Its existing typed sampler now
handles explicit level zero while preserving the array layer and separately
encoded coordinates. The prior atomic formatting failure was an IR format
that declared two sources while the parser retained the third VDATA read;
the corrected format preserves all three operands.

The complete 1,429-instruction captured compiler input produces a module of
177,518 words that passes `spirv-val --target-env vulkan1.2`. Generated-source
checks cover both atomic lane banks, the numeric class comparisons and array
level-zero samples. The emulator and executable build, strict source boundary
check and all thirteen graphics-table checks pass. A strict Silent/Native run
shows the opening caption and admits the complete program; its cached live
module also passes Vulkan 1.2 validation. At present 805, resource preparation
stops before this dispatch is submitted. Focused unit tests are deferred until
gameplay, as requested. These are compiler milestones, not gameplay acceptance.

The next binding failure is `GraphicsRenderBind.cpp:3760`: a pure comparison
sample selects a 2D-array view of an uploaded format-56 color image.
`ResolveDepthReferenceImageView` rejects that view, while the array emitter
already uses a regular sample followed by an explicit comparison. The live
sampler has linear minification and magnification, so merely relaxing the
view check would leave compare-after-filter semantics. Resolve comparison
filtering and color/depth view compatibility together; do not substitute a
texture or accept the view solely to pass the binding gate.

Array comparison now gathers the four base-level texels, compares each before
bilinear filtering, and admits the floating color-array view through the
existing regular sampled-image path. The complete captured program validates
for Vulkan 1.4. A strict run records its dispatch at queue 8, sequence 96,288;
the completion ledger reaches 96,294 on that same queue. The later parser stop
is SOP1 opcode `0x44` in another compute program, after present 808. This
proves completion of the former blocked dispatch; the black interval and
subsequent scene output are still unverified.

The new stop is `ShaderParseSOP1.cpp:280`, decoding
`S_ANDN1_SAVEEXEC_B32` (RDNA2 SOP1 opcode 68). The captured input is a
32-lane compute dispatch, saving EXEC_LO to a scalar destination and updating
EXEC_LO from the complement of its scalar source. Implement the decoder,
implicit EXEC/SCC effects and mask-aware analysis together; do not map it to
a barrier or widen it to the existing 64-bit instruction.

Captured-program replay then reaches `S_AND_SAVEEXEC_B32` in the same input.
The 32-bit SAVEEXEC family now decodes and emits its operation on EXEC_LO,
saves the old low word before any destination alias can overwrite the source,
and derives SCC from the low result while retaining the high EXEC word.
Implicit-mask analysis excludes these operations from linear storage coverage
and rejects treating a wave64 low-half update as wave-width independent.
The full 2,404-instruction captured program now decodes all seven observed
SAVEEXEC operations (AND and ANDN1). A strict run reaches source generation
and stops earlier in that program at `ShaderSpirvVector.cpp:772`: the floating
comparison emitter unconditionally asks for a second destination word, but a
captured SDWA comparison targets only VCC_HI. The integer comparison path
already handles this single-word form. Preserve that destination width in the
floating path; do not widen VCC_HI to a pair. GPU execution of this program was
still pending at that stop. Focused unit tests remain deferred until gameplay.

After preserving the single-word floating comparison destination, the complete
program emits source and assembles for Vulkan 1.4. Validation exposes a separate
native control-flow defect in `ShaderSpirvWriteLabel.cpp:288`: backward branches
sharing a destination have their loop header deferred to a later exit test.
The back edges consequently target ordinary blocks, rejected by `spirv-val`.
The failing graph contains nested loops with several back edges. Derive one
header and continue target from each loop's complete predecessor set, retaining
its real exit and every guest branch. Do not route this through the paired-wave
dispatcher without proving the native lane and barrier semantics.

The native lowering now recognizes single-entry interval loops with a common
exit, emits the loop header at the real entry, and routes every return through
one continue block. It also keeps selections inside their enclosing loop,
closes loop exits through the owning selection, and proves arm reachability
before nesting shared joins. Header repair alone was insufficient: validation
then exposed a selection merge outside its loop, a skipped inner selection
merge, and sibling cases incorrectly chained as parent/child. Those failures
are preserved in the local compiler evidence. The complete captured program,
both before and after performance optimization, now validates for Vulkan 1.4;
the preceding 1,429-instruction program also still validates. A strict
Silent/Native Vulkan 1.4 run records the new dispatch at queue 8, sequence
96,272, and the completion ledger reaches 96,273 on that queue. Its scored
native present-195 capture preserves the opening caption; present 726 is
black. The runtime then starts the title and requests the map resources,
before a different 2,924-instruction compute program stops in source emission.
These are dispatch and loading milestones, not scene or gameplay acceptance.
The structural rules are defined in the
[SPIR-V specification, section 2.11](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#_structured_control_flow).

The new stop is `ShaderSpirvVector.cpp:4204`: reverse subtraction with borrow
reads VCC_HI in a native 32-lane dispatch, but the provenance guard accepts
only the low VCC/EXEC word or a proven SGPR pair. A preceding single-word
unsigned comparison produces VCC_HI; an intervening reverse subtraction
writes VCC_LO. The shared carry emitter also clears the high destination word,
which may destroy that independent mask. Verify the wave32 mask-width contract
against the ISA and retain the captured producer/consumer sequence before
changing either the guard or the store width.

The captured program reproduces that exact emitter failure independently of
the runtime. RDNA2 wave32 mask rules and the
[LLVM GFX10 assembler's wave32/wave64 carry cases](https://github.com/llvm/llvm-project/blob/main/llvm/test/MC/AMDGPU/gfx10_asm_vop2.s)
confirm that carry inputs and outputs occupy one scalar word in wave32. The
native compute emitter now preserves the adjacent word, decodes an explicit
high-VCC operand as one word and requires a same-block comparison producer
before consuming it as reverse borrow. The full 2,924-instruction program
validates before and after Vulkan 1.4 optimization; its generated consumer
loads the original comparison value without an intervening high-word clear.
A strict Silent/Native run dispatches it at queue 8, sequence 96,168; the
completion ledger reaches 96,171 on that queue. Native capture 225 preserves
the opening caption and capture 592 remains uniformly black. The next stop
is a different 496-instruction compute program at
`ShaderSpirvImage.cpp:3171`: IMAGE_GET_RESINFO reads a descriptor classified
as read/write storage, while its emitter only accepts sampled images. The
captured instruction asks for width and height, and later operations use
the same storage descriptor. Verify the requested mip and descriptor view
before implementing the storage-image query. No scene or gameplay output is
established by the completed carry-mask dispatch. Focused unit tests remain
deferred until gameplay.

The storage-query replay confirms a zero LOD produced by an unmodified vector
move in the entry block, a plain 2D descriptor with base/last/max mip zero,
and a width/height-only result. Its later image store references the same
descriptor. The new lowering uses the proven writable binding and
`OpImageQuerySize` for that single-mip spatial query. Unknown LODs, control-flow
bypasses, other mip views and unimplemented result components remain rejected.
Storage mip metadata participates in the translation key so cache reuse cannot
bypass those checks. The entire 496-instruction program validates before and
after Vulkan 1.4 optimization. A strict Silent/Native run submits it at queue 8,
sequence 96,483; that queue's completion ledger reaches 96,485. Native capture
213 retains the opening caption, capture 707 is uniformly black, and the run
reaches present 810 before a new allocation failure. This establishes execution
of the queried program, not correct scene output or gameplay.

The new stop is `Objects/GpuMemoryCreate.cpp:1895`: a single-mip 32x32 storage
image equals one sampled Texture range and crosses a second sampled Texture.
Both parents have CPU-upload origin, equal CPU/GPU markers and no write-back
function. The first parent's format, extent, pitch, tiling and mip parameters
match the incoming storage image. The multi-parent classifier has no path for
this mixed relation set. Verify guest upload ownership and preserve both sampled
backings before extending the classifier; do not seed from stale GPU aliases.

The classifier now admits a single-mip 2D storage view only when every parent
is a CPU-uploaded sampled Texture with equal CPU/GPU markers, no write-back
function and an Equals or Crosses relation. An exact parent must also match
the full sampled layout. Normal storage creation uploads the guest bytes;
existing links preserve the sampled backings and their pending reads. A strict
Silent/Native run observes this path, dispatches the formerly blocked work at
queue 8, sequence 96,778, and completes through 96,779. Native capture 279
preserves the fading opening caption; no scene capture was obtained before
the next failure. Source/table gates and the Linux build pass.

The next first failure is a 335-instruction pixel shader at PC 0x44c:
`S_GETPC_B64` writes a scalar pair, but `ShaderGetInputInfoPS` never supplies
the program-base metadata required by `ShaderSpirvProgramAddress.cpp:13`.
Vertex and compute input analysis already supply it. Capture its consumer
chain and apply the same runtime base-address contract to the pixel stage;
do not bake the mapped address into the translated module.

The captured consumer builds a 128-byte V# over an inline table immediately
following the program, then reads sixteen pairs at scalar byte offsets. A
second defect in `ShaderSpirvBuffer.cpp`, the scalar-buffer load emitters,
would use that constructed base word as a storage-descriptor index whenever
another bound buffer exists. Merely enabling pixel GETPC metadata is therefore
insufficient. Pixel input analysis now supplies the runtime program base
and routes unbound scalar V# reads through the existing guest-address table.
It preserves statically bound and dynamically materialized buffer paths,
checks each DWORD against the descriptor extent and reads all source words
before writing overlapping destinations. Scalar execution stays independent
of EXEC. Cache policy hints and combined register/immediate forms outside the
admitted subset remain rejected. The complete captured 335-instruction module
passes Vulkan 1.4 validation before and after optimization. A strict
Silent/Native run records that draw at queue 8, sequence 96,400, and the
completion ledger reaches 96,424. Its live metadata has both runtime address
blocks enabled and spills 160 bytes through the existing uniform-buffer path.
Native capture 318 is uniformly black; the run reaches present 809 before
another pixel program stops in `ShaderParse.cpp:131` on family 0x33, word
0xcc20701a at PC 0xb8. The complete 13,920-byte program and stack are captured
for decoding against the ISA. No scene or gameplay output is established;
focused unit tests remain deferred until gameplay.

The next captured family is RDNA2 VOP3P. Its first operation is
V_FMA_MIX_F32; the complete byte range contains twelve independently decoded
instances with different FP16/FP32 source selections. `ShaderParse.cpp` lacks
family 0x33, while `ShaderParseVOP3.cpp` incorrectly treats different-family
opcodes 0x320/0x321/0x322 as ordinary FP32 FMA, including approximated packed
destinations. `ShaderSpirvDispatch.cpp` also routes VFmaMixF32 through a generic
three-FP32 emitter. A correct implementation needs a separate family decoder,
source precision and half selection, inline constants at the selected width,
and modifiers applied after conversion. The RDNA2 operand table and sections
12.10/13.3.6 specify those semantics. LLVM's gfx1030 disassembler independently
confirms all twelve captured operations and places OPSEL_HI source bits at
59, 60 and 14 respectively; the older PDF's prose reverses two of those source
names. The full-program parser replay reproduces the first failure with exit
65 before implementation. The Linux build and source/table gates pass with the
new decoder and mixed-source emitter. Parser replay now reaches PC 0x100c and
rejects VOP3 opcode 0x378 (V_PERMLANEX16_B32); it has decoded all twelve mixed
operations before that stop. Live translation now emits all twelve mixed
operations before stopping at
PC 0xfec on a DPP row shift. The retained source prefix contains twelve Fma
operations and twenty-eight FP16 unpack operations; it is not a complete
validated module or a completed draw. Unknown packed operations, unverified
packed destination forms and upper-half inline FP16 constants stay rejected.

The captured prefix also contains V_OR_B32 DPP row shifts with controls 0x111,
0x112, 0x114 and 0x118, followed by the cross-row permutation and reads of
lanes 31 and 63. `ShaderSpirvOperands.cpp:395` admits only quad permutations,
so adding the missing parser opcode alone cannot translate this program.
The later wave-width and consumer captures below constrain the host subgroup
lowering. `GraphicsRenderPipeline.cpp:371` currently
leaves the fragment stage pNext empty, even when pixel input analysis requests
an exact subgroup width. Its capability check covers quad operations alone;
`ShaderSpirvVector.cpp:3545` also forwards readlane selectors without the ISA
wave-width mask. These are unresolved contract defects. Any correction must
preserve helper participation and inactive-lane rules; a 32-lane host cannot
be assumed equivalent to a 64-lane guest without a program-level proof.

A strict Silent/Native rerun confirms the same full program bytes and stops
at PC 0x100c after 706 decoded instructions. The live SPI_PS_IN_CONTROL value
is 0x4005: PS_W32_EN is clear, so this is a wave64 input. The captured host
reports a maximum subgroup width of 32 and does not support required fragment
subgroup sizes. A new parser representation preserves PERMLANE16/PERMLANEX16
FI and BC bits and rejects their reserved modifiers. Full-program replay now
parses 2,300 instructions through PC 0x3260 with exit 0; those operations still
have no admitted backend lowering. This closes a decoding gap only.

The same run reaches present 808; native capture 328 is uniformly black.
A 152.56-second native interval under read-only debugger probes contains 445
presents, a median frame time of 299 ms and roughly 146 submissions per present. Dispatch
processing accumulates 92.14 seconds; fence waits accumulate 33.38 seconds
and WAIT_REG_MEM waits 39.52 seconds. These times overlap across processors
and must not be added as an exclusive frame-time breakdown. Shader cache hits
are 251 with no misses or SPIR-V compilations in that interval. Coherency and
submission preparation are the next performance measurement, separate from
the missing scene shader. Timers for dispatch write-back, address preparation,
residency discovery and snapshot refresh are now verified through native
diagnostics. The later plain-run comparisons above measure the two dispatch
preparation improvements separately from this debugger interval.

A related summary-operand defect remains unverified in this workload:
`ShaderSpirvOperands.cpp:573` combines VCC_LO and VCC_HI when reading VCCZ,
even for a native wave32 dispatch. RDNA2 defines that summary from the low
word alone in wave32. An independent nonzero high word with a zero low word
therefore supplies the wrong summary. Capture an actual VCCZ consumer before
changing its lowering; scalar branch lowering already has a separate path.

### Integer value references (2026-09-29)

A missing integer getter in `LibJson2.cpp` stops the title-transition caller.
The captured caller accepts signed or unsigned integer tags and dereferences
the returned address. The HLE now validates either integer tag and returns a
reference to its existing payload. A strict Silent/Native run observes the
unsigned value 100, a return address exactly sixteen bytes after the value
object, and the same payload when that address is read. The missing import is
passed; the concurrent compute program now stops later during image-store
emission. The emulator and executable builds, source-boundary gate, and
thirteen-table provenance gate pass. Focused tests remain deferred until the
requested gameplay milestone; this getter observation is not gameplay.

### Zero-LOD HDR sample observation (2026-09-29, not gameplay)

The bounded native sample probe now observes 2D `ImageSampleLz` RGB and RGBA
results as well as the existing biased sample. It records the original sampled
vector after the normal destination stores and does not replace shader output.
The RGB form is verified in a live Silent/Native run: at present 100, all
8,294,400 samples are finite, with RGB zero and alpha one. Generated and
optimized SPIR-V both pass `spirv-val --target-env vulkan1.2`. A native capture
at present 200 remains uniformly black. A second live run selects the RGBA
overlay sample: it records 8,294,400 samples, 409,600 nonfinite vectors and
RGBA zero for every finite vector. Both SPIR-V forms validate for this variant
too. The nonfinite count matches the earlier final-export observation and
places invalid values before final composition, although individual pixels
have not been correlated across those runs.

This locates zero color before the final shader's arithmetic for that opening
occurrence. It does not establish why the sampled storage image contains no
color, whether every later occurrence is black, or whether its producer has
run. The HDR input can legitimately be empty during a logo scene. Follow the
overlay image's writer and lifetime before changing texture decoding,
arithmetic or presentation. A broad format-only lifetime trace selected two
earlier 1024-square targets and did not identify the overlay producer; use a
selector correlated with the actual sampled image.

### JSON value lookup and assignment (2026-09-29, not gameplay)

The title transition now passes `Value::operator[](uint64_t)`. The missing
NID matches the const array-index operator's mangled name. Kyty validates
the value and owned array, bounds the index and returns the existing child;
absent elements share the same immutable null value as object lookup.

A Silent/Native call trace sees index zero of a one-element array and verifies
that the returned object is that owned child. That run then reached the next
unresolved import, `4zrm6VrgIAw[Json2_v1][Json_v1.1]`, the value assignment
operator. Its caller immediately assigns the indexed value to another
32-byte value. The initial copier rejected the host-owned source by asking the
guest virtual-memory registry to read it. Explicit node ownership and string
allocation byte counts now distinguish those HLE allocations from validated
guest mappings. Assignment clones the bounded tree before releasing the
destination, retains its parent context and gives copied children new owners.

A later Silent/Native run verifies the actual return: the destination is
returned, all five nodes have equal content, storage is independent and child
parent links are correct. The copied object contains two strings and two real
values. The run passes the earlier write-back occurrence and reaches the next
missing import after the title transition at 15 minutes 23 seconds:
`Ncel8t2Rrpc[Json2_v1][Json_v1.1]`, `Value::toString(String&) const`. The caller
constructs an empty string, converts a string-valued JSON node and consumes
its `c_str()` before destruction. The string-valued conversion now copies the
bounded contents into independently owned storage, allocating the replacement
before releasing the destination. Other value kinds fail explicitly until
their conversion contract is verified. A later Silent/Native run verifies
two actual returns of 36 and 12 bytes: both match their source, preserve it
and own independent storage. Value assignment's five-node ownership check
also still passes in that run. Nonempty destination replacement and
embedded-NUL cases still need focused tests, deferred until gameplay per the
requested validation order.

The next exercised import, `3qrge7L-AU4[Json2_v1][Json_v1.1]`, is
`Value::getReal() const`. The caller checks for real type four, passes an
owned value and immediately reads a double through the returned pointer.
The getter now validates the value and returns its existing real field.
A Silent/Native run verifies two actual returns: each points into the source
value at offset 16, preserves its exact double bits and leaves all 32 source
bytes unchanged. The earlier string and assignment checks also still pass.
Build, strict boundary and 13-table provenance checks pass; focused tests
remain deferred until gameplay as requested.

That run reaches the title transition at 15 minutes 10 seconds and stops at
`zTwZdI8AZ5Y[Json2_v1][Json_v1.1]`, `Value::getBoolean() const`. The caller
checks type one, passes an owned boolean value and reads one byte through
the return pointer. The boolean getter now validates that type and exposes
the existing field. The next Silent/Native run verifies its actual return:
the pointer is source plus 16, the byte agrees and all 32 source bytes are
unchanged. Real, string and assignment return checks still pass.

After the title transition at 15 minutes 18 seconds, that run stops at
`wLsJlmgEIaI[Json2_v1][Json_v1.1]`, the mutable
`Value::referValue(const String&)` lookup. The caller constructs a string
wrapper, passes an owned object, destroys the wrapper and checks the returned
pointer for null before further lookup. Its ten-byte key is absent from the
five-member object. The bounded wrapped-key lookup now returns the existing
owned child or null without insertion. A Silent/Native trace verifies twelve
actual returns: five existing children and seven absent keys, with source
and key contents preserved on every call. The earlier string, boolean, real
and assignment checks still pass. At the title transition after 16 minutes,
the next missing import is `rDMyAf1Jhug[libc_v1][libc_v1.1]`, whose name hash
matches `__isinff`. The caller passes a float in XMM0, tests the integer return
and clamps only when it is zero. The float predicate follows the public
[libc contract](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/lib/libc/gen/isinf.c):
integer one for either infinity and zero otherwise. The next Silent/Native
run verifies twelve actual finite inputs and zero returns, matching their
captured float bits. It advances beyond all previously missing JSON imports
and this libc call, then stops in compute-wave admission after the title
transition. Infinity and NaN cases still need the deferred focused tests.
Useful rendering, controls and both gameplay acceptance windows remain
unverified.

The new compute gate at `Shader.cpp:3129` reports `SBarrier` at PC 0x40,
but the raw word is SOP1 opcode 0x34, `S_ABS_I32`, according to the local
RDNA2 ISA. The parser's placeholder is not a real barrier and must remain
rejected until the scalar instruction and its SCC result are implemented.
The captured workgroup is 5 by 5 by 1 with one paired guest wave; native
equivalence separately rejects a cross-lane operation at PC 0x90. Preserve
that rejection while correcting the first decoded instruction.

The scalar parser and emitter now implement the ISA's signed absolute value,
including the unchanged INT_MIN bits and SCC for a null destination. With the
subsequent EUD lifetime and image-descriptor corrections, the actual
147-instruction program translates to an 8,105-word module that passes
`spirv-val --target-env vulkan1.2`. Its dispatch is recorded and the same
queue's completed sequence advances beyond that submission before the next
strict failure. Native-equivalence rejection of the cross-lane operation is
preserved. This verifies the exercised translation and completion; focused
integer edge-case tests remain deferred until gameplay as requested.

Two existing string-contract gaps were found while reviewing value ownership:
`LibJson2.cpp:394` uses `strlen` for `JsonStringLength`, although the parser
retains embedded NUL bytes. Verify the length export's guest contract and use
the owned byte count if the full parsed string is required. Separately,
`LibJson2.cpp:406` frees a string's data before `JsonStringAssign` measures its
input; passing that same data pointer or a suffix therefore reads freed
storage. Build a replacement before release when correcting that API. Neither
case is yet observed in the live workload, and neither is gameplay evidence.

### Unsupported-swizzle diagnostic (2026-09-29, corrected 2026-10-02)

The build identified a diagnostic-only type mismatch at
`Objects/StorageTexture.cpp:536`: rejection of an unsupported component
swizzle passed its 32-bit value to a `PRIx64` format, so the printed swizzle
read undefined upper bits. The argument is now widened explicitly, matching
the neighboring unsupported-format report (line 484). The rejection path has
no cheap unit fixture (it needs a live storage-texture object), so the change
is verified by the clean build only. It was never exercised by a run and is
not evidence of the black-output cause.

### Mixed sampled-image numeric selection (2026-09-29, compiler verified)

A separate captured-program review exposes a numeric-selection gap in
`ShaderSpirvImage.cpp:3236`, `Recompile_ImageLoad_VdataVaddr3StDmask`: runtime
unsigned/float selection is emitted only when all sampled views are flat 2D.
With both flat and array views present, the shape branch uses the shader-wide
numeric choice and ignores the per-descriptor unsigned tag. The captured
program copies a complete direct R32_UINT descriptor into the image-load
registers while also binding floating-point array views. Keep numeric selection
inside each selected shape's fetch path. Complete-program replay confirmed
that the unsigned predicate was previously defined but never consumed. The
corrected emitter selects the typed view in each shape, preserves integer bits
at the numeric join and uses that join as the outer phi predecessor. Both lane
banks now emit the unsigned fetch and the complete module passes Vulkan 1.2
validation. Resulting pixels and live GPU completion remain unverified.

### Residency preparation cost (2026-09-29, not gameplay)

`GuestDeviceAddress` now queries every interval of pages still awaiting
import, while existing aliases and tracked snapshots keep their established
refresh and invalidation paths. Newly faulted pages are checked on every
preparation; quiesced invalidation clears the imported-page bitmap.

One warm Silent/Native comparison in the same opening phase measures draw
descriptor finalization at 459.4 ms per present before and 426.8 ms afterward.
The 79- and 73-present windows have 23.04 and 23.00 draws per present and
78.0 and 77.5 dispatches per present; observed throughput is 0.836 and 0.890
FPS. This is a modest local CPU improvement, with one comparison and no
gameplay acceptance. The corrected run passes 800 presents without an import
failure. `fc_script`, the strict boundary gate and the 13-table gate pass.

### Pixel NULL export admission (2026-09-29, not gameplay)

The clean Silent/Native run stopped after 347 seconds at pixel target 9,
`DONE=1`, `VM=1`, `COMPR=0`, `EN=0`. RDNA2 defines this NULL export as a
valid-mask update without a color or depth payload. The decoder now admits
that form for Gen5, preserves per-invocation discard, retains the pixel stage and
omits color outputs for programs whose exports are all NULL. Such programs
also disable Vulkan color writes: live guest state leaves both CB masks
enabled, which otherwise permits undefined fragment outputs to reach color.
Admission is limited to the Gen5 path that carries this export analysis into
the pipeline's write mask.

The actual failing program now executes. Its generated and optimized SPIR-V
both pass `spirv-val --target-env vulkan1.2`, with discard and no color output.
The first corrected run reaches the title transition and exits after 969
seconds on missing `XlWbvieLj2M[Json2_v1][Json_v1.1]`, the const array-index
operator. `LibJson2.cpp` lacks that export despite retaining parsed array
elements; the next HLE work is bounded lookup returning the owned element.
The final build with color writes suppressed also passes the former export
failure and reaches present 324. `fc_script` and the strict boundary gate pass.

The media pause is an explicit guest preload transition after its first
decoded frame, not a decoder failure. Native captures remain black. An
output-preserving probe of the final color export observes 8,294,400
invocations across the full 3840-by-2160 target, 409,600 nonfinite results,
and RGB zero in all finite results. Its CPU constants are populated and its
three textures are bound; the remaining color investigation is upstream of
presentation. Neither the scene transition nor these probes prove gameplay.

### Media file replacement callbacks (2026-09-29, not gameplay)

The later startup scene writes the same two packed-color images that VideoOut
presents, but its media player had no decoder. A live entry trace proves that
the guest supplied a complete file replacement table, which initialization
discarded. The source URI was therefore mapped to a nonexistent host path.

AvPlayer now retains that table and provides its bytes to the host demuxer
through a bounded random-access source. The decoder owns the source until its
worker has stopped, then closes it outside player publication locks. Failed
opens return failure without announcing READY or accepting playback.

A Silent/Native run now opens the actual 4K stream, prepares six decoded frames
and delivers its first frame. Subsequent live state is paused by the guest,
with no backend error; captures at presents 36 and 103 are still black.
The next frontier is the pause/resume sequence and its guest-side condition.
Rendering, controls and the two gameplay acceptance windows remain unverified.

### Async DMA argument layout and counter reset (2026-09-29, not gameplay)

The async compute DMA export used the graphics entry point even though its
ABI omits the engine selector and trailing parser-block argument. Live entry
registers and stack words show four-byte immediate clears; the wrong function
interprets those arguments as one-byte requests and rejects them. The async
entry now decodes its own arguments and uses the shared hardware DMA encoder.

Combined with the GDS byte-address correction in `b3288fb7`, a bounded live
trace now observes the intended counter clears and 48 indirect dispatches
with group counts of one across consecutive cycles. The uncontrolled counter
accumulation is no longer present in that sample. A Silent/Native run reaches
its five-minute limit, advances into the first startup scene and has scored
black captures at presents 28, 172 and 222. The largest completed fence in
the retained diagnostic sample is about 65 ms. These are execution and
contract observations, not visible rendering, controls or gameplay acceptance.
The next color investigation must correlate the later startup scene's final
output with the presented buffer; the older present-20 black-source evidence
alone does not establish that later producer.

### Physical aliases across CPU protection boundaries (2026-09-29, not gameplay)

The intermittent guest-device import failure was reproduced with a 16 KiB
resident span: its first three host pages belonged to a CPU NoAccess segment,
and the fourth to a read/write segment of the same physical mapping. The
alias lookup incorrectly bounded the request by the first protection segment;
its subsequent CPU snapshot could not read those NoAccess pages. The alias
now validates containment against the physical mapping while holding its lock.
It follows the backing offset without changing the guest view's permissions.

The focused reproduction fails on the previous implementation and passes with
the correction, checking byte identity, coherent alias writes, unchanged guest
rights and rejection beyond the backing mapping. A strict 120-second run
passed the former import exit and reached a scored, still-black present 43.
This does not establish gameplay or long-run device stability.

The remaining slow-GPU trace identifies four device-addressed indirect
dispatches whose X group counts grow across consecutive cycles: initially
1/1/2/3, then 4/5/6/7, then 8/9/10/11. Their argument values are unchanged
before and after the existing write-back boundary. A later writer watchpoint
and binding trace identifies a GPU shader whose four GDS appends collapse
onto one incorrectly scaled counter address. The decoder drops their byte
offsets, and both emitters omit conversion from bytes to dwords. The correction
preserves the offset and shares the byte address calculation between both
emitters. A clean Silent/Native rerun reaches 70 presents with a black scored
capture. Four counters now advance separately by one per cycle, so reset
remains broken. A live ABI trace finds that the async DMA entry is incorrectly
aliased to the graphics entry and rejects valid counter clears. The handoff
records that next correction and the separate native-wave limitation. This
is not gameplay acceptance.

### Guest-address imports creating their own residency (2026-09-29, not gameplay)

A clean strict Silent/Native run of `ae78d70a` reached 45 presents with a
uniform-black scored capture, then lost the device in `vkQueueSubmit` after
172 seconds. A bounded timing probe subsequently found about 98 ms per
`GuestDeviceAddressPrepare` call, 63,588 chunk entries and 499 retained tables
at call 500; that run ended with Mesa `execbuf` ENOMEM. A second sample had
64,200 entries, of which 64,066 adjoined another chunk.

A synthetic Vulkan reproduction proves a self-induced growth mechanism:
importing a resident run plus one guard page makes that next page resident.
The next preparation imports it and another guard page, without any new guest
write. The correction imports exact page spans and translates the two pages
of a crossing shader load separately. Two native Vulkan regressions fail on
the previous implementation and pass with the correction: repeated preparation
does not populate untouched neighbours, and 128-byte loads preserve both
pages (or the zero tail when the second page is absent).

The strict 120-second measurement reached a scored, still-black present 39
and stopped at its configured time limit, with a 6.1 GiB service memory peak.
At call 500 it had 1,804 entries, 120 retained tables and 76.5 ms mean
preparation time. This bounds the artificial import growth in that run; it
does not establish long-run stability, correct color output or gameplay.
The experimental physical read-ahead was removed before this run because it
would amplify the newly proven residency mechanism.

Later cache inspection qualifies that runtime evidence: `d8950414` did not
invalidate translator version 56, so retained modules could still use the
old single-span shader loads. The focused Vulkan regression exercised the
new split-load emitter, but the cached workload run did not establish its
integration. Regeneration during the GDS investigation produces a larger,
valid SPIR-V module and spends several minutes in the host driver's compute
pipeline compiler before dispatch. Shader semantic edits must invalidate
the module cache, and compilation time is distinct from a GPU fence stall.

Recorded separately: `ShaderSpirvGuestAddress.cpp`, in the lookup after
`gda_merge`, reads entry zero even when the table count is zero. An empty
registry allocates only the zero prefix, so those entry reads are outside the
buffer. Guard the empty-table path before reading entry fields; the current
workload evidence has not identified an empty table at its remaining failure.

Validation also exposed a pre-existing test mismatch at
`UnitTestEmulatorShaderGetpc.cpp:170`: the rejection test expects a vertex
`s_getpc_b64` with valid program-base metadata to exit, although that path is
now admitted. The same assertion fails with the guest-address source restored
to `ae78d70a`. Update that case to validate successful vertex lowering while
retaining the missing-metadata and invalid-destination rejection checks.

A subsequent clean `d8950414` run stopped after 64 seconds on a 24 KiB
non-writable resident import. The later per-page reproduction and physical
alias correction are recorded above; the residency correction alone did not
resolve that separate failure.

### Raw render alias composition and explicit mip fetch (2026-09-29, not gameplay)

Commit `28db4aa3` materializes a partially written linear storage image from
the raw bytes of its overlapping render-target parents, ordered by observed GPU
write time. A live Vulkan test compared every byte of a mixed-format,
two-block alias against the CPU tile-address equation; the focused geometry
tests and source gates passed. A strict Silent/Native run crossed the former
mixed-parent creation exit and stopped later at Gen5 `IMAGE_LOAD_MIP` opcode
`0x01`. Commit `e169d95b` decodes its explicit level and emits an
`OpImageFetch` LOD operand. Its focused parse/SPIR-V test and the MIMG test
filter passed; a strict run crossed that parser failure and reached 42
presents. This proves the two former exits were passed, not correct pixels.

Native captures at presents 20, 40, and 42 were identically uniform black
(`entropy=0`, one quantized color). The longer follow-up run stopped after
40 presents and 189 seconds at `GraphicsRenderCommandBuffer.cpp:823` with
`vkWaitForFences` device loss. The kernel recorded an Xe timed-out job and
then a high-order atomic page-allocation failure while capturing its
devcoredump. The latter occurred on the coredump path and does not establish
the cause of the timed-out GPU job. The follow-up binary had the tested shader
change but still reported the pre-commit build revision; rebuild before the
next strict run. At present 20, a bounded readback of the packed VideoOut
source found zero RGB bits in all 8,294,400 pixels; its first word was
`0xc0000000`. The PNG conversion did not erase color. The exact registered
range had one VideoOut image and one CPU-uploaded storage-buffer view, with no
live render-target view. An opt-in GPU write-history recorder covered the same
range and found zero direct guest-memory writes before that present. The agent
still reported the loading phase, so this identifies an unwritten early output
buffer but does not prove which later compositor or transition is missing.
The next visual step is to correlate the first final-color producer with the
VideoOut range. No controlled gameplay, five-minute stable run, or playable
regression gate has passed.

Two shorter strict runs also stopped at `GraphicsRenderBind.cpp:4260` because
`GuestDeviceAddress.cpp:288` could not import a resident, non-writable guest
chunk. A bounded stage probe emitted no host-pointer Vulkan failure before
that exit; a later run did not reproduce it and showed optional guard-page
imports crossing unreadable next pages before successful unguarded retries.
The earlier fatal chunk's exact copy/protection failure remains unproven.
Measure its unguarded retry and mapping protection before changing the
device-address table or guest access policy. All temporary probes were removed.

### Scalar and vector lowering, RGBA16 storage, and mixed image ownership (2026-09-29, not gameplay)

The strict Silent/Native workload passed the earlier absolute async indirect
dispatch, scalar 64-bit equality, single-word high-VCC mask, and vector bit-count
failures. The vector bit-count emitter had loaded a float-typed VGPR pointer as
an unsigned integer and stored an unsigned integer through it. Commit
`f9b56b0c` bitcasts at the VGPR boundary; a red/green SPIR-V toolchain test
and a strict run passed the former pipeline-creation failure. That run stopped
at an unsupported Gen5 storage image with format 65. The local format table
and independent decoder observations identify four 16-bit UNORM channels. The
host Vulkan device reports storage-image and formatless read/write support for
`VK_FORMAT_R16G16B16A16_UNORM`. Commit `f4db885b` maps the storage use to that
format. Its focused test failed before the change and passes afterward.

A strict run with that format reached 36 presents and then stopped at
`GpuMemoryCreate.cpp:1825` while creating a linear 960×540 RGBA16 UNORM
storage image with `skip_seed=0`. Its range crosses 12 existing views: storage
buffers, render targets, sampled textures, and storage textures. The saved
GPU-memory database records a larger tiled RGBA16F storage image covering the
whole incoming range. A bounded diagnostic found its content origin is
`Unknown`, as are the render targets'; their object-wide update markers do not
prove which image owns the latest bytes of every tile. The first compute
writer uses a table of tile positions and dispatches 140 groups of 8×8
threads. Its two full-channel image stores can touch at most 8,960 of the
518,400 texels in the incoming image; the table gate can reduce that count.
Skipping the seed is therefore incorrect. The incoming range crosses two
recently GPU-written render targets with different pixel widths, so a single
covering image or object-wide timestamp cannot supply its prior bytes. Trace
per-range ownership and a format-aware raw materialization path before
extending the mixed-parent alias policy. A native frame-36 capture
scored `entropy=0`, one quantized color, and `gameplay_like=false` with
`scripts/kyty_capture.py`; presents are not gameplay evidence. Other bounded
retries stopped earlier at intermittent Xe `execbuf` ENOMEM / `vkQueueSubmit`
device loss in `GraphicsRenderCommandBuffer.cpp:656`. No controlled gameplay
or visual acceptance has been observed.

### Depth mip storage views, D16 arrays, and paired compute frontier (2026-09-28, not gameplay)

A strict Silent/Native run passed a mixed-parent storage-image creation that
previously stopped at `!create_all_the_same`. The incoming R32 image lay inside
live render targets but used depth tiling, so their color texels could not be
copied into it. The parsed compute program has one image store and no other
side-effecting operation. An exact read-only scalar binding supplies a uniform
value that is currently zero; its unsigned compare clears `EXEC`, and the
branch restores `EXEC` from the same zero `VCC` before every store. A bounded
snapshot confirmed the guest value, and the new control-flow check omits only
dispatches for which that value remains zero and the source range can be read
without a writable GPU alias. Focused tests reject a bypass around the gate,
an altered restore, other writes, an unproved binding, and a nonzero compare.
The next R32 depth-tiled image has eight mips (480×270, pitch 512). The old
size fallback covered only mip zero (768 KiB); the GFX10 depth-64KB mip layout
occupies 1152 KiB, with the smaller levels in a shared 64 KiB tail. A focused
red/green size test, detile test, public AddrLib layout comparison, and the
adjacent guest allocation boundary support that layout. The strict run passed
the former upload exit.

The consuming compute shader uses `IMAGE_STORE` with an 8×8 group and a
resource view at base level 6; another descriptor selects base level 7 of the
same eight-level backing. Storage writes now use single-level Vulkan views of
one mipmapped image, while its sampled view retains the full chain. Reuse
requires matching format, extent, pitch, tiling, mip count, and seed policy;
the mixed older color surfaces remain linked without being copied as depth.
Four focused tests and a strict Silent/Native run passed both former
`!create_all_the_same` exits. The next depth-reference binding had a 2048×2048
D16 depth-64KB array descriptor with raw depth two, normalized to three
layers. A bounded trace found the complete 24 MiB span physically mapped and
overlapping two CPU-uploaded Texture views with no write-back ownership.
Overlap summaries group equal type/relation objects; source admission now
requires matching individual provenance and rejects GPU-materialized or
write-back-capable Texture aliases. The full overlap count also bounds a
provenance scan that hits its page cap after visiting every matching object.
The matched GFX10 16-pipe Z_X equation XORs array-layer bits into within-block
address bits 8–11; CPU upload now detiles every layer with its slice index.
The storage-backed inline path remains limited to one layer because its shader
fills only layer zero. Focused source-classification and layout tests pass.

A subsequent strict Silent/Native run passed that D16 binding and reached
present 32, then stopped at `paired-wave dispatch admission unsupported` for
an instruction represented as `SBarrier` at PC 0x66c. A bounded retry
identified SOP2 opcode 0x25, `S_BFM_B64`: the parser had substituted a barrier
for this bitfield-mask operation. Its real 64-bit mask semantics are now
lowered in two 32-bit SPIR-V words without writing SCC, and paired admission
requires an exact scalar tuple. The barrier gate also requires the real SOPP
opcode 0x0a; a placeholder opcode stays rejected. Focused red/green tests
and SPIR-V toolchain validation passed. A strict run with this change passed
PC 0x66c and stopped next at `BufferAtomicUmax` at PC 0x74c. The captured
atomic is an aligned, no-index, no-offset, no-return unsigned dword maximum
through a unique, writable, guarded raw V# binding. Paired admission now
requires that exact tuple and binding; the existing Gen5 atomic emitter emits
one guarded operation per bank. Focused red/green admission, resource-use, and
SPIR-V tests pass, including negative return, index, and binding cases. Two
strict runs passed the former PC 0x74c gate and stopped at PC 0x560. A
bounded trace of that 368-instruction program found six mapped EUD loads, a
full write to their former `s12:s13` base by `SBufferLoadDwordx2` at PC
0x560, two later ordinary reads, and 18 static branches, all forward.
Paired admission now tracks possible and definite writes through the parsed
control-flow graph: every mapped EUD load requires the original pair on all
paths, while an ordinary read requires a full replacement on all paths.
Partial writes and unresolved branch targets stay rejected. Eight focused
tests cover reuse, a back edge, a skipped write, and the earlier guard cases.
A strict diagnostic explicitly recorded admission of this program before a
later storage-texture overlap exit. This confirms admission, not correct GPU
execution or gameplay. A separate retry stopped earlier at a storage
write-back dependency, which needs a causal reproduction. Native wave-width
equivalence remains rejected at PC 0x208 for a lane-crossing read. A native
frame-10 capture was uniformly black (`entropy=0`, one color). No controlled
gameplay has been observed. Several bounded retries also stopped before shader
admission when Xe reported `execbuf` ENOMEM and `vkQueueSubmit` failed in
`GraphicsRenderCommandBuffer.cpp:656`; correlate live GPU allocation budget
and competing Vulkan clients before changing submission policy.

### Bounded storage-image overwrite and next mixed alias (2026-09-28, not gameplay)

Linking older render, storage, and sampled views around a new storage image
depends on proof that its first dispatch writes every texel. The prior
write-only image-use bit alone did not provide that proof: it allowed seed
skipping when symbolic tile coverage was zero. Commit `4f8f0719` makes zero
coverage retain the seed and admits a bounded-grid pattern only when group
and lane IDs form the full 2D grid, the shader's sole `EXEC` guard excludes
coordinates beyond a scalar width/height pair, a full-channel image store
reaches every in-bounds lane, and a read-only snapshot of that pair matches
the destination extent at dispatch time. Altered branches, coordinates,
masks, source binding, or missing snapshots fail closed. A focused test failed
on the old fallback and 16 focused graphics/image tests now pass.

The strict Silent/Native run passed the former three-parent storage overlap
after that runtime proof. It entered the logo-level startup and stopped at a
different storage-image overlap with nine linked parents: six render targets,
two storage images, and one storage buffer; eight relations cross the new
range and one existing render target is contained within it. No native
capture or controlled gameplay was obtained. Determine the new image's
actual first-write coverage and each parent's content provenance before
extending the overlap policy. A prior uniformly black native frame remains
scored evidence from this bring-up phase.

Commit `69586773` proves a second whole-image pattern in paired wave64
compute. A symbolic 64-lane check follows the shader's local-coordinate
permutation, verifies that the tile contains each texel exactly once, and
requires two full-channel stores to the same dynamic image descriptor on
complementary `EXEC` paths. It rejects destination reads, missing or altered
branches, coordinate clobbers, incomplete masks, and unbound image reads.
Only a matching 2D dispatch can skip the initial seed. The positive test
failed before the change; the focused tests and a strict Silent/Native run
passed the former nine-parent overlap. The next first strict failure is in
`GpuMemoryCreate.cpp` at `!create_all_the_same`: a 768 KiB storage image lies
inside six older views (four render targets, one storage image, and one
storage buffer). The new image's format, first-write coverage, and live byte
owner remain to be established. No native capture or gameplay was observed.

Commit `2c589a47` proves the first writer of that six-parent storage image is
also a full overwrite. Its paired wave64 shader has one full-channel store
to the destination, no destination read or divergent path, and a bijective
permutation of the 64 local lane coordinates. The 2D dispatch covers every
valid destination texel. A positive test failed before the change, 19 focused
graphics/image tests and the source-boundary and graphics-table gates passed,
and a strict Silent/Native run crossed the former alias. That run stopped
later at unknown Gen5 VOP3 opcode `0x369` in `ShaderParseVOP3.cpp`; the local
RDNA2 ISA identifies it as `V_CVT_PKNORM_U16_F32`. Its actual encoded tuple
and lowering must be verified before adding parser support. The run reached
only the logo; it did not establish a playable or correctly rendered state.

A bounded trace found two VGPR float sources, a VGPR destination, and no
modifiers for opcode `0x369`. Commit `f41b2f06` decodes the two-source tuple
and uses the SPIR-V normalized-pair conversion, which clamps, scales, rounds,
and packs the first source into the low half. The prior emitter converted
clamped floats directly to integers and had a float/uint type mismatch.
The synthetic parser/emitter test failed before the change and passes with
SPIR-V validation; both affected targets build and the source/table gates
pass. The strict Silent/Native run passed `0x369` and stopped at the adjacent
unknown VOP3 opcode `0x368` later in the same program. The local RDNA2 ISA
identifies that opcode as the signed normalized pair conversion. Verify its
observed tuple and numeric lowering before enabling it. No gameplay or
correct-rendering claim follows from this parser advance.

The bounded trace for `0x368` found a tied VGPR source, an inline zero
second source, a VGPR destination, and no modifiers. Commit `6e0600ea`
decodes the two-source form and lowers it with validated SPIR-V
`PackSnorm2x16`; the old shared emitter also omitted the normalization
scale and passed a uint to a float selection. The synthetic red/green test
and prior unsigned test pass. Both targets build and the source/table gates
pass. One strict run was interrupted by the previously observed Xe
`execbuf` ENOMEM / `VK_ERROR_DEVICE_LOST` in `GraphicsRenderCommandBuffer.cpp`.
A single strict retry passed the former `0x368` parser exit and stopped at a
lazy `Agc_v1.1` import in `RuntimeLinker.cpp:1081`. Its exact contract was
unverified at that point. This is still logo-stage execution with no controlled gameplay.

A bounded live call trace and independent local implementations established
that the async indirect-dispatch import receives an ACB, a full 64-bit
argument address, and a modifier. The native packet carries the absolute
address in two dwords; the DCB form remains relative to the indirect-argument
base. The new synthetic packet test failed to link before implementation and
passes with its full high address preserved. Both affected targets build and
the source-boundary and graphics-table gates pass. A strict Silent/Native run
crossed the former lazy import and both indirect-dispatch packet forms,
reached the logo-stage level, then stopped at unrecognized SOPC opcode
`0x12` in `ShaderParseSOPC.cpp`. A native capture at 28 presents scored
`low_entropy` (one color bin). Neither the presentations nor that capture
prove correct rendering or gameplay; the SOPC operands and 64-bit equality
lowering are the next verified frontier.

### Strict compute/storage and libc string frontier (2026-09-28, not gameplay)

On the reference Gen5 workload, the owned Linux build in strict Silent/Native
mode now passes the former dynamic-storage write-back, fused ES+GS register,
offset image-sample, storage-image alias, render-target-format, first
audio-propagation imports, bounded libc string imports, and three later
storage-image overlap cases. A blockwise GPU copy now carries the live bytes
from a compatible tiled render target into the first partially written storage
image. The strict run passed the former single-parent
`RenderTexture Crosses StorageTexture` exit. With SDL's Wayland backend, a raw
32-to-16-bit tiled alias transfer now passes the former `!create_all_the_same`
exit for the second partially written storage output. It reads the live source
image, swizzles its raw words, and uploads the destination in the current
command buffer. The multi-parent policy requires a newer packed render target
whose guest range covers every other parent's overlapping bytes and whose
full host extent received a render-pass `CLEAR`. The destination still takes
its uncovered prefix from guest memory. A focused red/green test exposed that
the old two-byte tile-27 path mapped 32,768 texels to only 8,192 distinct
offsets per block. The revised mapping is bijective and aligned over 64 KiB;
a synthetic cross-format test checks the raw word address mapping, and the
embedded compute shader passes `spirv-val`.

The 180-second strict run reached present 17 without the former structured
exit. Its native capture is uniform black (`entropy=0`, one quantized color,
`gameplay_like=false`). A second 110-second run with the full-clear gate
reached present 9 and recorded the five-parent storage creation as linked;
it ended at its runtime limit without a guest error. Performance telemetry
measured that creation near 30 ms while frames took about 8 seconds. The
source image bytes, revised two-byte tiling on real hardware, and cause of
black/slow output still need direct validation. There is no controllable
gameplay evidence.

A temporary audio probe counted 3,000 empty blocking pushes in about 34
seconds at 512 samples and 48 kHz, close to a real-time audio cadence. GDB
placed that wait on thread 60 and the internal flip on thread 6 in the same
strict run. This rules out a direct sleep on the flip thread; it does not
identify the remaining frame-time cost. The probe was removed.

Bounded GDB and packet probes now attribute much of the roughly eight-second
frame interval to repeated one-second `WAIT_MEM64` fallbacks in the graphics
ring. The slow waits compare a 64-bit label against `1` with a low-32-bit mask;
the live word stays `0` and the submission tracker finds no pending producer.
In contrast, waits paired with a preceding immediate `RELEASE_MEM` find their
current submission's producer. A guest store initializes the slow label to
`0`; a later graphics-label completion writes `1` only after several fallback
timeouts. No compute ring was mapped or advanced during the measured opening
window, and the first 40 dispatches belonged to the graphics queue. Returning
from DCB submit immediately after command-buffer snapshotting did not improve
the measured pace (about 0.11 FPS in a bounded A/B run), so that timing change
was removed. The earlier producer had not yet been identified; changing the
wait timeout or fabricating the label would conceal it. This explains slow
progress, not the uniform black native captures.

The first color-chain readback located a separate raster-state defect. The
context register decoders handle viewport indices 0 through 15, but
`HW::ScreenViewport` allocated only 15 entries. Writes to viewport 15 landed
in the following generic-scissor fields; a strict trace observed the bit
patterns for `0.5` and `1.0` as its left and top coordinates, leaving Vulkan
with a zero-area scissor. A focused test reproduced those exact integers
before the array was expanded to 16 and passed afterward. The corrected
strict trace resolves the HDR writer's scissor to its full 2432×1368 target.
Nevertheless, a 125-second strict run reached present 15 with every retained
capture uniformly black (`entropy=0`, `gameplay_like=false`), and its measured
pace remained about 0.12 FPS. Direct readbacks showed the packed VideoOut
image, its 2432×1368 HDR source, and the preceding 1920×1080 HDR source all
zero in the opening presents. The viewport defect is corrected, but visual
output and playability are not established. Trace the first nonzero producer
upstream of the HDR postprocess chain before changing the final compositor.

The next strict probes reached the first full-screen HDR source writer. Its
2,073,600 fragment exports and the immediate 1920×1080 attachment were all
RGB-zero. The guest-uploaded single-channel input was readable but contained
zero in every word of its 8,847,360-byte span at that draw. A second
full-screen branch likewise exported zero RGB over 2,073,600 fragments; its
storage-image producer is a compute upscaler that reads an earlier HDR target,
so it is not an independent source of scene color. First-draw censuses at the
opening present and at present 11 each found the same 14 pixel shaders; every
first occurrence used three nonindexed vertices and no depth attachment.
Those samples do not prove that every repeated draw has the same shape. A
scheduled diagnostic `cross` was delivered at present 2, yet a present-6
native capture still scored zero entropy and the input counters recorded no
ordinary guest pad reads. At this point the live boundary was the missing nonzero scene
source or scene submission, together with the unresolved graphics-label wait;
no evidence justifies changing the compositor, sampled-image selection, or
audio memory contract to force visible pixels. The temporary probes were
removed before the clean build.

A later paired-submit trace identified the first slow label's producer. The
opening ACB on one async queue contains five waits for that label; a later
DCB contains its matching `RELEASE_MEM`. Kyty had sent both through the
graphics ring, so the producer could run only after the ACB's timeout
fallback. ACB handles now retain separate ordered compute command processors,
and GPU write-back waits until every queue using a writable object has
completed. Two focused tests and the boundary/table gates passed. In the
first strict run after the correction, the agent reached present 15 in about
36 seconds (snapshot FPS about 0.44); 32 `WAIT_REG_MEM` suspensions remained.
The native present-19 capture still scored `entropy=0`, one color bin, and
`healthy=false`. The run then exited on missing strict import
`S5JxQnoGF3E[Json2_v1][Json_v1.1]`, identified as `Parser::parse`. This
does not establish gameplay or a controlled performance benchmark. The next
work is a contract-backed JSON data path and the still-missing scene source.

The JSON data path now parses bounded UTF-8 input into owned values and
supports object lookup by a bounded C-string key. A focused test covers a
nested document, numeric value, missing key, Unicode escape, malformed trailing
input, and destructor cleanup. A strict Silent/Native probe reached the former
parser import with a 14-byte document: parsing returned success with an object,
and a seven-byte key lookup found an array. It then stopped at the next
unresolved `SHtAad20YYM[Json2_v1][Json_v1.1]` (`Value::getType`) import, after
19 presents in about 53 seconds. That type accessor now returns the value's
guest-visible enum, with its own focused assertion. The bounded probe's
result-only diagnostics were removed. Error codes for malformed input and
invalid arguments remain inferred from secondary implementations rather than
measured on hardware. A subsequent strict Silent/Native run with the final
JSON path passed the former `getType` import and reached present 22, but the
host Vulkan submission failed with `vkQueueSubmit result=-4` at
`GraphicsRenderCommandBuffer.cpp:656` after the Intel driver reported repeated
`execbuf` ENOMEM. The service used 3.8 GiB at peak under a 16 GiB limit, while
the host reported 15 GiB of available RAM and full swap. These counters do not
identify whether device memory, driver address space, or another resource was
exhausted. Reproduce and measure the Vulkan allocation/submission state before
changing the queue or memory policy. A repeat with the same binary did not
reproduce ENOMEM: the agent reached 21 presents in strict mode, then the guest
stopped at missing `RBw+4NukeGQ[Json2_v1][Json_v1.1]` (`Value::count`). Near
that point, the agent reported 159 live GPU memory objects and a maximum of
three submissions in flight; Xe fdinfo attributed about 1.58 GiB resident
VRAM and 665 MiB resident GTT to the process. This excludes deterministic
failure at sequence 2000 and leaves intermittent host/device failure to
measure separately. No captured image or controlled gameplay validates the
scene.

`Value::count` now reports parsed array and object sizes, with focused checks
for array, object, and scalar values. In the next strict Silent/Native run, the
guest passed the former count import, logged that a level had started, and then
stopped at missing `CPLV6G-eXmk[AudioPropagation_v1][AudioPropagation_v1.0]`,
catalogued as `sceAudioPropagationSystemRegisterMaterial`. The 61-second run
did not establish that the level image was displayed or controlled. The audio
material ABI, output handle, lifetime, and acoustic behavior require a guest
call trace before implementation; secondary implementations in the local
comparison either return success without state or depend on unverified record
sizes.

A bounded strict call probe of `SystemRegisterMaterial` found a live system,
a readable 64-byte material record with descriptor ID `0x010107d1`, and a
writable eight-byte output handle. Kyty now retains the complete opaque record
under that system, enforces its material count, and releases it with the
system. A focused lifecycle test passed. The next strict Silent/Native run
passed the former material import and stopped at missing
`kIdb+iQUzCs[AudioPropagation_v1][AudioPropagation_v1.0]`, catalogued as
`sceAudioPropagationSystemSetAttributes`. Registration alone does not
implement acoustic propagation. The request for a native capture did not
complete before the missing import stopped the run; no visual result is
established.

The next strict call to `SystemSetAttributes` supplied one 24-byte entry:
ID `0x20000`, an eight-byte value pointer, and size eight. Guest code wrote
only the low ID dword, so the following dword cannot be treated as a required
flag. A bounded live-object probe proved the pointed-to value was a room
handle owned by the system, not a material handle. Kyty retains that room
association and clears it when the room is destroyed; unsupported attribute
IDs remain rejected. A focused red/green lifecycle test passed, and a strict
Silent/Native run passed this import before stopping at missing
`ht-QXT3zGxo[AudioPropagation_v1][AudioPropagation_v1.0]`, catalogued as
`sceAudioPropagationSystemGetRays`. A native capture from the preceding probe
was uniform black (`entropy=0`, `healthy=false`); there is still no gameplay
evidence.

The strict `SystemGetRays` call passes a writable count initialized to 64 and
a caller-owned array of 64 records, each initialized by the guest with a
0x58-byte descriptor. Guest code scans the resulting records for nonzero
coordinates. Kyty's CPU-only propagation system has no ray producer, so its
current result is an empty set: it writes count zero and leaves the caller's
array untouched. A focused red/green test passed. The following strict run
passed this import and reached a logged level start, then stopped in graphics
at `unknown mimg format for opcode: 0x24`, `dmask: 0x1`. The present-32 native
capture still scored `entropy=0`, `healthy=false`. The empty result does not
establish acoustic propagation or a visually correct level.

The graphics decode failure was a Gen5 `IMAGE_SAMPLE_L` (`0x24`) with
`DIM=1` (2D), one scalar destination selected by `DMASK=0x1`, and NSA
addresses. The local RDNA2 ISA defines this as an explicit-LOD texture
sample; the parser previously accepted only three- or four-component forms.
The new scalar emitter reads the three selected address VGPRs, samples with
the explicit LOD, and writes only the red component. A synthetic parse/SPIR-V
source test passed, and the strict run passed the former MIMG exit. It next
stopped at `!create_all_the_same` for a storage texture with three containing
GPU-memory objects. The present-34 native capture remained uniform black
(`entropy=0`, `healthy=false`). This is shader admission, not visual proof.

The SDL X11 backend can stop earlier at zero presents on this host.
At 78 draws, 133 dispatches, and 549 submissions, the graphics worker waits
for flip completion while the main thread waits inside SDL's X11 window show
for a map event; the guest submitter then waits for decode completion while
holding a guest mutex. The X11 window remained unmapped. A standalone program
linked to the same SDL build reproduced the blocked hidden-window show with
and without `SDL_WINDOW_VULKAN`; the same program returned normally with
`SDL_VIDEODRIVER=wayland`. On Linux Wayland sessions, the host window now asks
SDL to try `wayland,x11` when no video driver was explicitly selected. A strict
Silent/Native run with the driver unset reached the storage overlap exit above.
This identifies and routes around an environment-dependent host-window blocker,
not a rendering or gameplay fix.
Showing the window early did not resolve the X11 stall and was reverted.

`AudioOut2ContextPush` also returned immediately for empty PCM in a blocking
context: a bounded strict probe observed 10,000 calls in 529 ms. A focused
red/green test now requires one grain of pacing, and the implementation releases
the context mutex while waiting and revalidates its lifetime afterward. The
strict run confirmed the changed path executes, but X11 still stopped at the
first flip; audio pacing alone did not advance the graphics frontier.

- A scalar-loaded storage descriptor consumed by vector stores or atomics is
  now classified writable and merged with its equal-descriptor users. Compute
  dispatches using guest device addresses receive ordered GPU write-back
  before descriptor preparation. The strict run passed the former zeroed
  linked-list input; standalone shader replay alone was not the acceptance.
- The observed fused ES+GS program receives the GS user-data address in
  `s0:s1`; the indirect ES resource register is parsed. The observed 2D
  `image_sample_lz_o` masks 1 and 2 now produce their selected components.
  Translator identity 54 separates these modules from older cached SPIR-V.
- For an observed wave64 storage-image writer, symbolic lane/group analysis
  proved four stores cover each 16×16 tile without reading that destination.
  The 152×86 dispatch covers the 2432×1368 image, so it may omit an initial
  guest-memory seed while retaining overlapping live render and storage
  parents. GDB confirmed skip mask `0x2` at the actual dispatch. The strict
  run passed the former `!create_all_the_same` exit. The proof rejects
  incomplete coverage and destination reads in focused tests.
- A later native 16×16 reduction dispatch loads its destination descriptor
  after sampling a separate source, then writes one R32 texel per selected
  2×2 block. A bounded symbolic proof tracks the EXEC reset, even-coordinate
  predicate, and 8×8 output tile; 152×86 and 76×43 dispatches cover the
  observed output extents. The fully overwritten images can retain live render,
  storage, and sampled-image parents in the captured overlap relations. Another
  8×8 dispatch writes two destinations under bounds read from a buffer; the
  observed bounds match its 1216×684 output and the computed 152×86 grid
  covers it. Focused tests and strict runs passed all three former exits.
- The next render target was 3840×2160, tile `0x1b`, with color format
  `0x9`, UNORM type `0`, and alternate component order `1`. A focused
  red/green test and the next strict run confirmed the 4-byte
  `VK_FORMAT_A2R10G10B10_UNORM_PACK32` mapping moves beyond that exit.
- The first partial storage output has the same RGBA16F format, sample count,
  and 64 KiB tile layout as its single live render-target parent. Their
  overlap begins on a tile boundary. A copy plan maps each shared guest tile
  to source and destination image rectangles, while CPU upload seeds the
  non-overlapping range. It rejects partial tiles, mismatched formats,
  incompatible extents, and unaligned ranges. A red/green byte-address test
  and the strict run verified the former exit is passed; neither proves the
  resulting pixels are visually correct.

The guest's `sceAudioPropagationSystemQueryMemory` call passes a 56-byte option
record and a 48-byte memory record, then reads the CPU size from memory-record
offset `+0x18`. The new CPU-only HLE reports its own 64-byte caller-owned
system state, validates and initializes that memory in `SystemCreate`, and
tracks room lifetime under a live system. A focused test and strict run passed
the former query and room imports. This establishes only those lifecycle
operations: no acoustic rendering or broader library compatibility is proven.
Two local secondary emulators report a 1 MiB size and 256-byte alignment and
return success from the remaining entry points; one explicitly ports the
other. They provide no independent hardware measurement of the size, alignment,
creation behavior, or return codes. Keep the guest-traced record layout and
Kyty's current CPU-only HLE until a real trace or independent published
contract establishes a broader one.
The former `.agx` graph-name assertion came from a wrong libc NID binding:
`NC4MSB+BRQg` was handled as an errno-style formatter, which overwrote the
destination. The guest passes destination, capacity, source, and count and
expects `strncat_s` to append; an independent local export catalog labels the
same NID `strncat_s`. A hardware watchpoint showed the previous binding
replaced an existing name with only `.agx`. A focused test and strict run
confirmed the corrected append contract and advanced to the substring import.
Keep raw guest strings, disassembly, and addresses in scratch.

The subsequent `Xnrfb2-WhVw` import is `strnstr`: two independent local
emulator catalogs agree on the name, and the live guest passes haystack,
needle, and a haystack byte limit. A focused bound test and strict run passed
that import, reached later load phases, then stopped at the separate mixed
`StorageTexture` overlap. The later reduction and bounded dual-output writers
were traced separately and passed as described above. Their proofs do not
apply to the current partially written image.

The first present uses `VK_FORMAT_A2R10G10B10_UNORM_PACK32` at 3840×2160.
Native capture now converts both packed 10:10:10:2 channel orders to RGBA8 and
also accepts 8-bit UNORM sources. A focused channel-order test passed, and the
strict Silent/Native run saved four 1280×720 PNGs with matching metadata. All
four have the same black-image hash and fail the official scene/gameplay score;
presentation and capture alone do not establish rendering or playability.

### Generic wave64 compute frontier (2026-09-26, not gameplay)

Strict Silent/Native runs on the reference workload now admit, translate and
create pipelines for several wave64 compute programs that previously stopped at
the paired allowlist. No presentation, input or gameplay state is claimed.

- A wave64 program that is proven wave-width independent runs one guest lane
  per invocation on the native recompiler over the unchanged guest workgroup
  (`ShaderComputeWaveNativeEquivalence`). The fail-closed proof rejects lane
  crossing, mask/data mixing (flow-sensitive), SCC produced by mask logic and
  any scalar value written inside an EXEC/VCC-conditional region that is live
  at its join, which is what makes an independently skipping host subgroup
  equivalent to the guest wave.
- Paired lanes no longer depend on a per-opcode allowlist: per-lane VALU,
  vector memory and DPP16 instructions re-emit the native lowering once per
  bank; all VOPC compares ballot a native lane predicate into the architectural
  mask; DS accesses are followed by a subgroup barrier (one guest wave is one
  converged subgroup); uniform scalar instructions, saveexec and GETPC use the
  native lowering on the architectural SGPR/VCC/EXEC words; `v_mbcnt`, GDS
  `ds_append`/`ds_consume` and `v_*_co_ci_u32` have dedicated wave emitters.
- Partial waves and `USE_THREAD_DIMENSIONS` dispatches are admitted: the
  initial EXEC is the ballot of lanes inside the workgroup and inside the
  per-dispatch thread limits (runtime metadata, not a cache key), and every
  EXEC write is clamped to it.
- ISA decoder corrections (RDNA2 tables, confirmed against the live guest
  operands): VOP3 `0x365`/`0x366` are `v_mbcnt_lo`/`v_mbcnt_hi` with a VGPR
  destination (the former `v_bcnt_i32_b32` decode did not exist); single-address
  DS instructions use the 16-bit `{OFFSET1, OFFSET0}` offset; `ds_read`/`ds_write`
  `b64`/`b96`/`b128` move all their dwords; `ds_wrxchg_rtn_b32` exchanges;
  `s_bitcmp*`, `s_ff1_i32_b*`, `s_trap` (no trap handler: not taken) and
  `v_cmp_*_u64` are decoded instead of the `SBarrier` placeholder.
- Guest-memory device addressing: GPU-visible guest mappings are imported as
  host-pointer device memory (`GuestDeviceAddress`); shaders translate computed
  guest pointers through a device-resident range table and read through
  `PhysicalStorageBuffer` pointers (scalar loads off the extended pointer,
  `image_bvh_intersect_ray`). Write-back coherency before such reads is not
  implemented yet.
- `image_bvh_intersect_ray` (RTIP 1.1, 32-bit pointer, non-A16) follows the ISA
  T#/VGPR contract and AMD's open-source ray-tracing library for node layouts,
  box/triangle arithmetic, sorting and barycentric rotation. Return mode 0's
  `triangle_id` is taken from the node's triangle-id dword (unverified).
- Paired programs with branches run as a block dispatcher (loop around a switch
  on the guest block id); branch conditions are uniform per subgroup. Their
  SPIR-V is not run through spirv-opt: SSA promotion of the per-bank registers
  across the dispatcher loop makes the driver compile unbounded (offline: -O and
  -Os exceed 12 GB; unoptimized compiles in 25 s / 0.9 GB).
- Wave64 programs proven wave-width independent now take the native route even
  when a paired layout exists (half the generated code).
- Current first failure (strict Silent, frame 1, 0 presents): the compute
  dispatches that use guest device addressing and `image_bvh_intersect_ray`
  reset the render engine (`Engine reset engine_class=rcs`, `Timedout job`,
  `vkWaitForFences` result -4, sequence 54). Excluded in
  `docs/kyty-runtime-graphics-investigation-handoff.md`: an infinite block
  dispatcher, a device-address load that merely overruns its imported span,
  and `s_barrier` scope. A scalar load whose descriptor base is a guest
  address still indexes the storage-buffer array with that word; decoding the
  descriptor in a per-load helper is valid SPIR-V but pipeline creation does
  not finish. Skipping those dispatches reaches a vertex `s_getpc_b64` with
  no compute program base (`ShaderSpirvProgramAddress.cpp`). Not gameplay.

Recorded, not yet fixed:
- `source/emulator/src/Graphics/ShaderComputeWaveAnalysis.cpp:759` uses a
  broad scalar fallback after exact paired-wave classifiers reject a tuple.
  A malformed `SMovB64` pair and `SGetpcB64` were reclassified as
  `ScalarGeneric` in the existing compute-wave tests. Replace the fallback
  with audited instruction/tuple admission before using those tests as a
  strictness gate; do not treat a generic scalar register shape as sufficient.
- `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp:6495`
  expects the older direct LDS `ds_write_b32` SPIR-V text. The current emitter
  no longer produces its two asserted instruction strings in that fixture.
  Inspect the emitted program and update the test to check the actual LDS
  address/write contract; a source-string mismatch alone does not prove a
  runtime LDS defect.
- `source/emulator/src/Graphics/ShaderParse*.cpp` still map about 600 opcodes
  to an `SBarrier` placeholder and the DS parser keeps "treated as" substitutes
  (for example `ds_write2_b32`, `ds_rsub_u32`, `ds_cmpst_b32`). Each silently
  changes guest semantics; decode each from the ISA when it is reached.
- The native `ds_read`/`ds_write` emitters previously ignored EXEC; they now gate
  on it, but the other native DS atomics without a return value still do not.
- Storage image stores used the sampled-3D flag to pick three coordinates; they
  now follow the declared 2D (optionally arrayed) storage type. Real 3D storage
  images remain unsupported.
- Kyty names float constants with six decimals, so distinct small values (for
  example 2^-24 and 0) collide; emitters must use bit patterns for them.

### Paired compute admission frontier (2026-09-22, not gameplay)

The restricted wave64-on-native32 compute path is connected to direct and
indirect dispatch admission on this dirty `main`: checked guest/physical local
sizes and host capabilities precede pipeline and descriptor work, and
translation identity separates the two layouts. This remains an instruction
allowlist, not general wave64 emulation. Strict Silent/Native runs first
stopped at PC `0x0` (`S_INST_PREFETCH`), then PC `0x4`
(`S_LOAD_DWORDX4`), then PC `0xC` (`S_LSHL_B32`). Exact SOPP opcode
`0x20` modes 1–3 are admitted as instruction-cache hints, with parser aliases
rejected. The latest owned build and strict rerun completes resident loading
and stops at PC `0x10`: `SWaitcnt is outside the paired compute-wave admission
set` (dispatch mode `0x41`). The cache-hint interpretation follows the
[AMD RDNA2 ISA](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture).
This is admission progress only; no guest compute
dispatch, presentation, input-to-gameplay, capture, or playability gate passed.

At PC `0x4`, the captured tuple loads `s16..s19` from type-5 EUD pointer
`s12:s13` at byte offset `0x50`. A paused diagnostic found the nonzero pointer,
24 declared EUD dwords, zero GLC/DLC, and the normal resource collector's
storage-buffer descriptor mapping for EUD dwords 20–23 with a consumer at PC
`0x14`. The paired admission now runs **after** resource mapping and requires
that exact per-PC producer, offset, span, flags, binding source, and consumer
record. The emitter fails closed if any mapped word is missing. A separate
128-logical-lane Vulkan probe on Arc A770 passes all four descriptor words in
both banks of two waves, but supplies its mapping manually: it proves mapped
SPIR-V lowering, not production mapping or a guest dispatch. The collector
diagnostic and its focused mapping tests provide distinct provenance evidence.
Because the EUD base pair is not initialized as ordinary SGPR data, paired
analysis rejects each ordinary word read until every control-flow path writes
that word. It rejects mapped EUD loads after any path writes either word.
The later per-word lifetime correction permits separate ordinary writes once
mapped EUD loads are dead. The gate keeps combined SGPR plus
`smem_imm_offset` loads rejected; `recompile_sload_from_extended` does not add
that extra offset. Unrepresented SMEM reserved bits still require a fail-closed
decoder check. A later EUD load at PC `0x490` lacks a verified mapping; inspect
its actual consumer only if it becomes the first strict frontier.

The PC `0xC` scalar shift is admitted only for a plain one-word SGPR input,
inline shift 0–31, and `VccHi` destination. Two synthetic GPU cases on Arc
A770 verify nonzero/zero results, SCC, VCC-low and EXEC preservation across
128 logical lanes. The preceding complete regression checkpoint passed 1221
unit tests with eight skips, three compute/graphics integrations, emulator
boundaries, graphics-table provenance and `git diff --check`; the newer EUD
guard passed 23 focused units and the shift passed 20 focused units and the
paired GPU integration. Full regression after these additions is still due.
A separate portability risk remains in
`GraphicsRenderPipeline.cpp`: native W32 compute shaders using subgroup
operations do not currently request subgroup size 32, so a host whose default
subgroup is not 32 can execute incorrect lane-width semantics. The Arc A770
used for these probes defaults to 32, so this has not reproduced locally.
Require size 32 only for subgroup-sensitive W32 modules when supported, without
the paired path's full-subgroup flag; otherwise reject that sensitive pipeline.
Even a forced size 32 does not by itself prove subgroup lane ordering.

### Latest startup shader frontier (2026-09-22, not gameplay)

The latest strict run passes the texture-resource and scalar-load mapping
ceilings described below, then stops at the missing `S_GETPC_B64` emitter.
Dynamic scalar-load mappings now use copy-on-write records bounded by parsed
instruction count, not a 64-entry parallel-array table. A 65-load shared-resource
fixture failed on the prior ceiling and passes with preserved mapping lifetimes
and copy isolation. Independent review found no blocking migration findings.
Three separate regression processes passed 49 shader, 225 state and 231 packet
tests, and both compute and graphics integrations passed. This does not erase
the separately recorded combined-suite retirement-counter failures or establish
any presentation/gameplay. The next contract is a runtime, cache-relocatable
compute program address; fused graphics program addresses remain unrepresented.

The subsequent compute GETPC implementation passes that stop and reaches an
unsupported vector 64-bit comparison. Its program base is per-dispatch metadata
in the existing push/UBO path, while cache identity contains only presence and
layout. The 55 shader tests pass. On Intel Arc A770, thirteen numeric GPU cases
pass, including two-base same-pipeline GETPC dispatches through push and UBO
metadata, low-word carry, empty EXEC execution and preservation of an active
EXEC low bit. These do not prove nonzero EXEC high-half behavior. Independent
production and harness reviews found no remaining blocking findings. No
presentation or gameplay is proven.

The next comparison consumes a vector-comparison-produced VCC mask. The current
backend stores that mask as a per-invocation boolean, not the architectural
packed wave mask; comparing that boolean numerically with zero would therefore
be incorrect. A bounded dispatch-entry capture reports initiator `0x41` and a
256-invocation workgroup, while the host supports subgroups of at most 32.
The public [GFX10.3 register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_3_0_sh_mask.h)
place `CS_W32_EN` at bit 15; the [LLVM PAL wave-size regression](https://github.com/llvm/llvm-project/blob/main/llvm/test/CodeGen/AMDGPU/mixed_wave32_wave64.ll)
corroborates its use for wave32. The captured dispatch requests wave64.

Do not replace the numeric whole-wave comparison with a native subgroup32
vote or request an unsupported native subgroup64. The actual parsed shader
contains LDS reads, writes, an atomic and two real workgroup barriers, so
splitting its workgroup into independent waves is not justified. A preceding
EXEC-dependent branch can bypass the comparison, so inserting a workgroup
barrier at the comparison also requires a convergence proof that is not
currently available. Complete wave64 execution remains an architectural
prerequisite; the bounded implementation below does not yet admit this shader.
Retain these exclusions when continuing.
The observed comparison also aliases its VCC source and destination. RDNA2
ISA section 6.2.4 warns that wave64 VALU same-SGPR read/write can be
unpredictable; the generic packed-mask rule alone does not establish this
aliased sequence's hardware result. Do not infer per-half write behavior from
the instruction-level statement that VCC is fully written.
Follow-up comparison with public implementations did not close this gap:
snapshotting U64 sources in another emulator and host-Vulkan tests with
authored expected values are not target-hardware measurements. No verified
same-SGPR exception or half-pass visibility rule was found. A lawful synthetic
target-device result with initial/final masks and repeatability is still
needed before choosing snapshot or forwarding semantics for this alias.

The checked paired-layout module and optional Vulkan feature path now build.
A real Vulkan host-layout probe verifies four guest waves per workgroup across
two workgroups, with 512 distinct logical-lane output slots, on the Arc A770.
The thirteen existing scalar/GETPC probes remain passing. The production
paired prolog and non-CMPX U32 comparisons now pass eleven numerical GPU cases:
packed lane63, zero/low/high/both/partial EXEC, ordinary SGPR mask destination,
and three guest-coordinate axes across two waves. The full unit command reports
1185 passes and eight skips. This is a bounded compare-only capability, not
complete wave execution or gameplay; runtime paired admission remains disabled.

The GPU canary caught an integration defect before admission: the execution
mode used physical32 but the decorated WorkgroupSize constant retained guest64.
[WorkgroupSize takes precedence over LocalSize](https://docs.vulkan.org/refpages/latest/refpages/source/WorkgroupSize.html),
so duplicate wave records overwrote the canary. Both now use physical dimensions;
all eleven numerical cases preserve the canary. Do not weaken that oracle or
attribute this reproduced source mismatch to the probe's readback.

The lane-operation stage reproduced and corrected an existing contract in
`ShaderSpirvVector.cpp`, `Recompile_VReadfirstlaneB32_SVdstSVsrc0`: empty EXEC
returned literal zero instead of lane0's value. A native32 GPU regression first
returned zero for lane0=42; removing the post-broadcast zero selection makes it
return42. Twelve paired read/write/readfirst cases also pass, including upper
halves, modulo64 and EXEC-independent lane accesses. This does not establish
complete wave execution or runtime admission. After the related parser fixes,
all thirteen paired lane/move cases pass. The integrated checkpoint has82
focused passes and1191 full-suite passes with8 known skips; both scalar and
wave GPU integration targets pass without skips.

`ShaderParseDS.cpp`, opcode0x20 (DS_ADD_RTN_U32), previously discarded VDST
and the upper byte of its offset. Focused parser regressions reproduced both
losses; the parser now retains a distinct return tuple and full byte offset.
Paired LDS lowering has passed its bounded numerical and safety checks.
The isolated native `ShaderSpirvLdsAtomic.cpp` handler now preserves the old
value, full offset, operand aliases and inactive EXEC. Eight numerical GPU
cases pass, including 32 contending invocations with distinct returned values.
Translator version 46 invalidates binaries with the earlier native semantics.
The paired LDS numerical counter probe passes128 logical increments with
all returned old values0..127, but independent review found an admission gap:
`ShaderComputeWaveLds.cpp`, `ValidateLdsAddress`, checks bounds without proving
that non-atomic constant-address writes have a unique active writer or that
later cross-invocation effects are synchronized. Such accepted programs can
produce a host data race under the [Vulkan memory model](https://docs.vulkan.org/spec/latest/appendices/memorymodel.html).
This finding is now closed for the narrow admitted subset by the separate
`ShaderComputeWaveLdsSafety.cpp` proof: whole-workgroup singleton writer
provenance, generic SGPR-half invalidation, and real-barrier effect phases.
Independent review and numerical tests pass, including a lane63-only atomic
that preserves inactive destinations. Current validation:91 focused passes,
1199 full-suite passes and8 known skips; both GPU targets pass on Arc A770.
Do not strengthen DS writes into invented
atomics, insert extra barriers, or treat the passing counter as general LDS
acceptance. Runtime paired admission stays disabled.
*Superseded (2026-10-02):* commit 704f4ad3 moved LDS accesses to the ordered
generic path and the address and singleton-writer proofs described above no
longer gate admission; `ShaderComputeWaveLdsSafety.cpp` and the LDS address
analysis were removed as unreferenced. The index bound they implied is a
recorded defect in "Debt resolution and compute-wave contracts".
The strict native-return rerun passes that missing-emitter frontier and again
reaches the unresolved U64 comparison after resident-load completion. There
is still no presentation or gameplay evidence.
Related native defect recorded, not fixed here: `ShaderSpirvBuffer.cpp`,
`Recompile_DsWriteB32_VaddrVdataOffset` (line2067) and
`Recompile_DsReadB32_VdstVaddrOffset` (line2228) emit unconditional accesses;
an inactive EXEC can therefore still write LDS or change a VGPR. Existing
non-returning LDS atomics share this gap. Add focused inactive-effect tests
and native predicate guards before claiming general native LDS correctness.

The paired scalar subset now admits exact S_AND/OR/XOR_B64 and
S_AND_SAVEEXEC_B64 (ordinary SGPR save destination only). Twelve numerical
cases exercise high-half masks, SCC/EXECZ, source/destination aliasing and
vector EXEC consumption. Full-EXEC initialization and singleton-writer
proofs are conservatively invalidated by their explicit/implicit writes;
four RED regressions reproduced stale-proof acceptance before correction.
Current validation: 93 focused passes, 1201 full passes and 8 known skips,
both compute GPU targets passing. These remain opt-in translation probes;
paired runtime admission and general control-flow/memory support are pending.
The next bounded control-flow slice now parses SOPP `0x09` as a real
`S_CBRANCH_EXECNZ` and admits only one forward EXECZ/EXECNZ diamond with a
shared join. A canonical workgroup barrier may precede or follow that diamond,
but not appear inside either divergent arm; a branch-containing shader rejects
all LDS effects until a CFG-aware LDS proof exists. The paired branch predicate
tests both EXEC words, while the native subgroup path remains unchanged. A
real compare-to-lane-63 fixture failed numerically on the old low-word-only
predicate, then passed both EXECZ and EXECNZ across two guest waves on Arc A770
with a 64-lane output canary. The three structural tests, 50 focused tests,
1205 full tests (8 existing skips), and three compute/diagnostics integration
targets pass. Translator version 47 invalidates earlier branch binaries.
The newly decoded EXECNZ remains fail-closed outside paired mode: a native
low-word-only predicate cannot prove a wave64 result.
This is probe-only progress: the strict Silent workload still stops after
resident load at the unsupported aliased U64 comparison, PC `0x784`, with no
presentation or gameplay evidence. Paired runtime admission is still disabled.

Independent review found a separate native LDS atomic host-safety gap in
`ShaderSpirvLdsAtomic.cpp`: dynamic `vaddr + offset` is shifted into an
`OpAccessChain` without proving it is within `lds_dwords`; an address at the
allocation end can therefore access outside the host Workgroup array. A
native shader with 128 allocated dwords and `vaddr=512` bytes is a synthetic
trigger. The [RDNA2 ISA reference](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
describes general LDS out-of-range reads/writes but does not establish the
returned value of this atomic form. Do not clamp or synthesize a return value.
Next: establish that return contract or a sound address-range proof, add an
in-range/boundary/wrap red test, and guard before constructing the host pointer.
The passing in-range native atomic probes are not a general OOB-safety claim.
The restricted forward diamond does not generalize to loops or multiway
branches. Linear LDS proofs remain unusable across arbitrary joins, including
an LDS operation only in the suffix after a divergent arm.
The
[RDNA2 ISA reference](https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture)
sections12.8,12.12,12.13 also specify that readlane/writelane ignore EXEC and
select modulo64 in wave64; writelane must not acquire an ordinary vector EXEC guard.

DS decoder hypothesis excluded: do **not** move the Gen5 OP/GDS fields to
bits17/16 based on the old PDF table94. AMD's newer
[machine-readable RDNA2 specification](https://gpuopen.com/machine-readable-isa/)
defines OP at bit18 (8bits), GDS at bit17 and VDST at bit56 (8bits). Its XML
SHA256 is `d671ecbc36543674ab59e9b2feffd56718fd2137e7ad913c314f2e2508b9f4f7`.
LLVM22 gfx1030 decoding of independent synthetic read/write/add-return words
agrees, as do28 DS instructions in the private workload capture. The source
also agrees with [LLVM's GFX10 DS encoding](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/DSInstructions.td).
Retain the current field positions; the defect to fix is the lost return
destination and unsupported aliases, not these bit positions.

The new exact-admission tests also exposed inactive DPP fields populated from
the next instruction by the VOP1/VOP2/VOPC parsers. Plain V_MOV then appeared
modified and was rejected. The parser-local correction reads DPP controls only
for actual DPP encoding. Preserve SDWA encoding and VOP3 OMOD metadata before
destination replacement, and reject those unsupported variants in paired
analysis; erased controls are not evidence of a plain instruction.

The dispatch audit additionally found an existing size-validation defect in
`GraphicsRenderDraw.cpp:2533`, `GraphicsRenderDispatchDirect`: thread-dimension
ceil division uses `(count + local_size - 1) / local_size` in `uint32_t` and
only warns for a zero local size. Large counts can overflow and zero local
dimensions can reach division by zero. A focused checked-ceil-division
regression and validation against host dispatch limits are needed; no size
semantics were changed during the wave investigation.

The full unit command now reports 1164 passes and 8 skips (1172 total), and both
compute/graphics integrations pass. Its initial isolated tap failure was a stale
test: the existing dirty controller/header/docs explicitly hold two pressed
guest samples, while the unchanged test expected one. The test now asserts
release/press/press/release and one delivered tap without changing controller
behavior. The earlier order-dependent retirement-counter failures remain a
separate recorded subset-order issue; they did not reproduce in this full run.

A fresh Linux Release run of the current working tree, using Native resolution,
Silent output and no permissive flags, passes the earlier allocator/audio import
boundaries and completes resident loading. This supersedes the startup stop
described in the older bounded-repair entries below, not the independent original
reference-workload frontier. No presentation or controllable gameplay has been
established for this startup workload.

Two incomplete depth-array edits prevented compilation: the D16 span variable
escaped its declaration scope in `GraphicsRenderBind.cpp`, and the
`UtilFillDepthImage` definition in `Utils.cpp` omitted the layer parameter already
declared in its header. Those declaration/signature errors are repaired without
changing the array layout contract.

The next observed shader failure was Gen5 `V_SUBREV_CO_CI_U32`. The decoder now
handles its VOP2 and VOP3B forms; lowering computes `src1 - src0 - borrow_in` with
two unsigned subtract-with-borrow operations, combining their borrow outputs.
The existing scalarized mask is normalized to zero/one. Unimplemented modifiers
and SDWA controls that would be discarded fail closed for this new opcode.
Synthetic regressions demonstrated the original parser rejection and validate
the generated SPIR-V. The workload still stops during whole-shader parsing,
before its full shader can be lowered or dispatched; this is not a workload
emitter-execution or GPU numeric-conformance claim.

Independent review caught and corrected an unconditional VGPR destination
write in the new lowering: inactive EXEC lanes now preserve their old value.
The older shared `V_ADD_CO_CI_U32` text in `ShaderSpirvVector.cpp` still writes
its destination unconditionally before testing EXEC; that pre-existing defect
needs a separate inactive-lane regression and repair. The observed reverse-
borrow SGPR mask is produced by a vector comparison, which this backend stores
as a per-invocation boolean. A raw packed SGPR mask is a different representation;
blindly extracting a lane bit from a comparison-produced boolean is incorrect.
`ShaderMaskAnalysis.cpp` now admits ordinary SGPR-pair inputs only when a
full-pair supported comparison is proven in the same straight-line block;
unknown producers, partial overwrites and branch-entry bypasses fail closed.
VCC/EXEC retain the existing backend mask representation. The translator cache
version is incremented so older generated modules cannot bypass the new policy.
These are deliberately not claims of arbitrary packed-wave-mask support.

The new tests are separate shader-focused modules, leaving the pre-existing
graphics-packet test file unchanged relative to the starting working tree.
The earlier Linux build and 317 focused shader/cache/graphics/heap/audio tests
passed. Subsequent parser/scalar work below advances the failure to shader
emission, still with zero presentations; a playable/capture-scoring gate cannot
pass in this state.

The subsequent strict run stops at `IMAGE_BVH_INTERSECT_RAY`. GFX10 MIMG encodes
the eighth opcode bit separately in word-zero bit zero; masking to seven bits
misidentifies opcode `0xe6` as `0x66`. The decoder now preserves that bit and
reports the unsupported BVH instruction explicitly rather than treating its
128-bit resource as a texture. The [LLVM encoding definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/SIInstrFormats.td)
and [opcode definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/MIMGInstructions.td)
corroborate the identity. Removing the `r128` rejection, inventing a hit/miss,
or skipping the instruction would not implement it. AMD's public
[legacy GPURT node definitions](https://github.com/GPUOpen-Drivers/gpurt/tree/7b226d48b46b7e92fec3b9ecc5712e5bf2bf3dd9/src/shadersClean/common/gfx10)
provide the FP16/FP32 box and two-triangle node layouts. The
[software intersection path](https://github.com/GPUOpen-Drivers/gpurt/blob/7b226d48b46b7e92fec3b9ecc5712e5bf2bf3dd9/src/shaders/IntersectCommon.hlsl)
is a vendor reference for arithmetic, not proof of every hardware edge case.
Mode-zero triangle-ID/status encoding remains unverified.

The live compute shader constructs the BVH descriptor after a 16-dword scalar
load through a computed pointer, then extracts the base field and forms the
size/type/mode words with scalar arithmetic. It is not a four-dword BVH
descriptor copied directly from the EUD table. Extending the existing dynamic
V# resource classifier alone would therefore be insufficient. Required next
work is bounds-checked, coherent memory access for this computed resource plus
real box/triangle intersection lowering; Vulkan ray query is not a drop-in
replacement for this per-node instruction.

In particular, the current `ShaderSpirvBuffer.cpp::recompile_sload_from_extended`
reads snapshotted descriptor metadata, not arbitrary computed guest pointers.
It only warns when the source register is not the EUD register, then continues
with metadata indexing. `S_LOAD_DWORDX16` also has no dispatch-table emitter.
Do not request sixteen outputs from the eight-element local destination array,
or reinterpret a computed pointer load as EUD metadata. Add a bounded
address-to-backing contract and dedicated computed-load regressions first.

A further live metadata capture exposes a preceding resource-classification
defect: the same compute stage declares nonempty SRT and EUD regions together,
with a type-5 pointer and scalar loads through that pair. However,
`ShaderResources.cpp::Gen5HasEudPointer` previously required `srt_size_dw == 0`, so this
dispatch reaches emission with `extended.used == false`. Its direct pointer
also enters the generic four-word storage path in `ShaderParseUsage2`. Do not
use the resulting candidate bindings as proof of computed-pointer coverage.
The mixed-region predicate now has a red/green regression and recognizes the
explicit EUD pointer. The generic four-word fallback now preserves a partially
overlapping SRT span as raw scalar data. A conflicting direct buffer use is
rejected only when existing CFG analysis proves an incoming descriptor reaches
the consumer; later SGPR overwrites are not evidence about the incoming pointer.
Focused red/green cases cover the two-word pointer, a live-in contradiction,
and descriptor reuse after overwrite. This is initialization preservation, not
an implementation of computed memory resolution.
The next snapshot guard incorrectly capped API sharp slots by dividing backing
dwords by four. Sparse or aliased slots need not occupy distinct storage; both
span passes now accept them while retaining the actual 256-dword access bounds.
A separate small resource-pointer suite covers sparse aliases and out-of-range
spans.

The full-precision, 32-bit-node BVH form now has a dedicated parser with eleven
address registers and a four-word resource tuple. Contiguous and NSA addressing
have synthetic regressions; other formats remain unsupported. This is parser
support only, not an intersection emitter. Fresh raw SOP1 opcode evidence also
identifies two descriptor-producing instructions as `S_BITSET1_B32`, not GETPC
or FF1: the earlier probe printed their inline bit indices, not opcodes. A
dedicated bitset emitter preserves the other destination bits and SCC. The next
parser stop, `S_CMP_LG_U64`, now compares both scalar words and writes SCC without
requiring host Int64 support. Ordinary pair bounds are checked. Six focused
parser/scalar tests pass, including SPIR-V validation; GPU numeric validation
is still pending.

The fresh strict run now reaches `Spirv::WriteInstructions`. A bounded backtrace
identified its first unknown-format instruction as `S_PACK_LL_B32_B16`.
That instruction now has a dedicated scalar packing module, tested with distinct
halves, aliased inputs/destination and VCC_HI. Ten hermetic Vulkan numerical
cases covering bitset, scalar inequality and packing pass on an Intel Arc A770,
including scalar execution with EXEC zero and SCC preservation where specified.
These synthetic shader results are not proof of workload shader dispatch.
The formatter assertion at `ShaderDebug.cpp:216` can
hide the underlying unimplemented instruction when printing placeholder IR.

Admitting the real EUD table exposed a host memory-corruption defect:
`ShaderGetTextureBuffer` warned at its sixteen-entry capacity but still appended,
corrupting counters later read by `ShaderAddDynamicTextureResource`. The strict
launch produced an access violation in that later loop; a focused capacity
regression proves an append at capacity previously returned instead of rejecting
the write. The subsequent strict run identifies a seventeenth combined
sampled/storage resource, not a Vulkan device limit. A focused test reproduces
sixteen sampled entries followed by a writable entry. The logical table is
being expanded to thirty-two entries while preserving separate sampler and
storage-buffer bounds and the thirty-two-bit image-write mask. Do not truncate
resources or merge distinct sampled and writable uses. The dense layout cache
remains bounded (approximately 29 MiB across the three stages).
Generated layouts need checks against actual host limits, including all seven
sampled-image arrays, and pipeline layouts need aggregate checks across sets.
Standalone samplers do not contribute to `maxPerStageResources`; they retain
their own per-stage and pipeline limits (see the
[Vulkan limits specification](https://docs.vulkan.org/spec/latest/chapters/limits.html)).
The expanded table passes focused capacity/limit tests, ten numerical Vulkan
shader checks, and the graphics diagnostics integration. A strict run now
retains eighteen textures and advances to a different ceiling: dynamic scalar
resource collection exhausts its sixty-four instruction-PC mapping records.
Those mappings represent producers, not distinct Vulkan resources. Repeated
loads may share one resource while requiring distinct lifetime mappings; replace
the fixed mapping ceiling with instruction-count-bounded storage rather than
dropping producers. The SRT-preserving build reaches the same mapping frontier.
Presentation and gameplay remain unverified.

An additional broad-test limitation remains recorded: the two
`EmulatorGraphicsState` linked-buffer retirement tests pass individually,
together, and in the entire graphics-state suite, but adding the graphics-packet
suite before the pair produces nine extra storage-buffer delete callbacks in
the second test. `EnsureGpuMemoryForTests` initializes a process-wide singleton
once, while these assertions reset global callback counters and compare global
free deltas. The origin of the prior objects is unproven; isolate ownership or
drain only test-owned objects before interpreting this as a retirement-contract
regression. No production retirement fix or complete broad-suite pass is claimed.

The two retirement tests now use fixture-owned callback tokens and exact backing
presence, with one reusable dedicated range and scoped cleanup. The ordered
packet-suite plus pair run passes 233 tests; the pair passes three repetitions.
The broader run passes 1182 tests with eight skips when excluding only the
currently pending wave-mask RED case. No production retirement policy changed.
An additional repeat-only fixture defect remains: repeating the entire packet
suite reaches `AcceptsComputeShaderWithoutWorkgroupId` a second time and calls
`ShaderInit()` again, failing its `g_shader_map != nullptr` guard in
`Shader.cpp:770`. Isolate that test's shader-global initialization or make its
fixture lifecycle explicit; do not weaken the production initialization guard.

Important strictness limitation: `ShaderParseSOP1.cpp` and the other scalar
parsers still map several unimplemented operations to `SBarrier` with warning-
only diagnostics, including get-PC and find-first-one. They retain an unknown format rather than
the real barrier's empty format; parser survival is not evidence that an emitter
can execute them. A launch without permissive environment flags does not validate
these unimplemented semantics. Identify and implement the actual producer
operations from their encoded instructions before claiming a correct BVH binding
or compatibility; do not perpetuate the placeholders.

Related depth-array limits still open:

- The source span now uses the normalized layer count and CPU upload detiles
  each slice with the matched Z_X layer term. The inline storage-backed D16
  path still fills only layer zero and is therefore admitted only for a
  single-layer descriptor. Layered GPU-owned depth sources need a separate
  evidenced multi-layer producer and upload contract.
- The type-13 one-layer case (raw DEPTH zero) remains rejected by the D16
  gate; the depth-array view is created only when logical depth exceeds one.
  Establish the guest descriptor/view contract before extending this case.

The new speaker/privacy HLE paths pass the observed startup route, but their
broader ABI assumptions remain unverified: `Audio.cpp::AudioOut2GetSpeakerInfo`
places availability bits at output offset four whereas the earlier caller
trace consumes offset eight; full write extent, angles and additional selector
semantics are not established by that trace. `LibUserService.cpp` interprets
the privacy query's first argument as a user ID without an independently
verified invocation contract. Preserve these as open evidence requirements;
passing self-consistent HLE tests does not establish native ABI equivalence.

### Process allocator startup (bounded repair, not gameplay acceptance)

Libc startup now consumes the declared `PT_OS_PROCPARAM -> libc parameters ->
malloc replacement` chain. The replacement record has a two-qword header;
its initializer is not the first entry of the direct kernel heap API. A
captured failure reached the first allocation without ever calling this
initializer. Calling the declared initializer before publishing the direct
API fixes that null-mspace boundary without scanning load segments, replaying
main-image constructors, or substituting a host heap.
Both HLE libc need-flags were already set in the failing run, and the main CRT
already called its constructors: changing those flags or adding a second
constructor pass does not explain the missing allocator producer.

The common pointer offsets and version-one record are corroborated by the
[public OpenOrbis CRT](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/af1619c2d1bfffe85a9534c9abb3fb43eed45dfd/src/crt/crt1.S).
The version-two tail and successful initializer return were checked against
local runtime evidence. Publish-after-success is Kyty's safety policy, not a
claim about undocumented system ordering. Empty default tables preserve the
default allocator. Reentry and concurrent startup have focused regressions;
guest callbacks run outside host publication locks.

Known unresolved lifecycle defect: `Libs/ApplicationHeap.cpp`'s process-global
API and startup identity outlive `Loader/RuntimeLinker.cpp`'s destructor and
`Clear()`. Reusing one host process for successive or nested guest runtimes can
retain callbacks to unmapped guest code; the new once-state can also reject a
different parameter address or mistake a reused address for the old process.
The direct-API lifetime defect predates this startup repair. Fix direction:
owner-generation-scoped state, owner-conditional retirement, and quiescence of
in-flight allocator callbacks before guest unmapping. An unconditional reset
in a nested linker's destructor is not safe. Current verification uses a fresh
host process per guest run and does not establish multi-runtime reuse safety.

### Nested arenas and AudioOut2 startup (bounded repair)

The next allocation failure was a legitimate child mspace rejected by the
registry's blanket overlap check in `Core/MSpace.cpp::MSpaceRegister`. Admission
now requires a live canonical parent allocation containing the entire child;
invalid overlaps are rejected before writing the child header. Backing storage
cannot be freed, moved, or destroyed while a child is registered. A transient
realloc pin prevents an OOM callback from creating a child on a moving chunk,
without holding the global registry lock across that callback. Copied alignment
markers and raw-prefix subpointers have red-to-green regressions. The allocator
and adjacent libc/loader suite passes 135 tests; arbitrary allocator-metadata
corruption is not a supported contract.

The strict workload then reached an audio-startup polling loop with zero GPU
submissions. This was not a nested-heap deadlock: the context-parameter whitelist
in `Audio.cpp::ReadSupportedContextParam` rejected a second captured 0x40-byte
configuration. The guest ignored the failed memory query, passed a zero-size
workspace to context creation, and retried port creation on a missing context.
Do not force a successful port result or bypass this guest wait.

The decoder now accepts only the two complete measured configurations. The
second configuration's producer/consumer trace establishes 512 frames per
submission; the corresponding F32 eight-channel snapshot is 16384 bytes, not
the legacy 8192. Header fields remain opaque and unknown blocks still fail
closed. A guard-page regression checks the full PCM read, alongside mutation
tests that leave rejected outputs untouched. The existing 64 KiB workspace is
explicitly a shared HLE reservation policy, **not** a measured native query
result or native workspace layout for the new configuration. Native-equivalent
workspace sizing and audible PCM routing remain unverified.

The final core/audio/heap/loader/symbol validation ran 197 tests: 192 passed and
five media-fixture-dependent tests skipped. The next strict original-workload run
exited at the unresolved lazy import `sceAudioOut2GetSpeakerInfo` in
`AudioOut2_v1 / AudioOut_v1.1`, with no draws or presentations. The export is
deliberately absent from `Libs/LibAudio.cpp` until its argument/output contract
is evidenced; a public name/NID mapping alone does not justify a success stub.
The caller explicitly prepares an output pointer and a zero second argument,
then consumes a byte at output offset zero and bit zero of a dword at offset
eight; it does not test the return value. This proves neither the complete
output size nor the meanings of these fields. The old log-and-success binding
from `908d78d4` was deliberately removed in `ea839c67`; restoring it, zeroing a
guessed structure, or returning an error without an evidenced output contract
does not solve this boundary. Public HLE layouts are research leads, not a
verified native contract.

Next requirement: a valid contract or a successful reference trace for the
observed invocation, with return value, pre/post output, actual write extent,
and a second controlled speaker configuration to identify the consumed fields.
No speaker implementation is added in this repair. Gameplay is still unproven
for this workload. The original reference-workload frontier below is independent
of these startup repairs.

### Original reference workload

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
- The storage-analysis inconsistency is closed conservatively: an unmatched
  `DirectResource` remains `Unknown` with `NoMatchingInstruction`, while only
  proven-unused `MetadataSharp` entries may be removed before binding. Focused
  unit and graphics-integration coverage enforce that distinction. An earlier
  strict private run with the same conservative policy did not reach its present
  gate before timeout, so it is not compatibility evidence. Before changing
  global storage pruning, isolate the exact direct binding and prove its
  ownership, size, and real shader consumer.
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
Resolved later: each idle read-only buffer member now retires on its own (see
"Linked read-only buffers retire one at a time" above).

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
- `KYTY_PIPELINE_FAIL_DUMP=<directory>` and `KYTY_TRANSPORT_DUMP=<directory>`:
  opt-in local evidence dumps (SPIR-V of a failed pipeline; the guest program
  that needs fragment wave transport). The directory must already exist and is
  never created. File names are fixed components, created exclusively (an
  existing file, symlink or FIFO is not overwritten), each file is capped at
  16 MiB and the process at 64 MiB, and a cut dump is reported as truncated.
  Each file's status is reported in the fatal transport message or the debug
  log; a rejected dump never changes the outcome of the run. Keep the directory
  in untracked scratch.

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
