# Performance architecture plan (2026-11-12)

Status: plan, stage 0 in progress. Maintainer decision of 2026-11-12: build the changes below, and
the 2x build gate of `parallel-recording-design.md` is withdrawn. Every gain in this document is an
estimate until its stage is measured. Each stage names its measurement and its correctness gate.

Scope: (A) memory release without a full drain, (B) a submit that returns once its batch's guest
reads are done, (C) staged command processing, (D) the per-draw binding floor, (E) internal
resolution as a functional option, (F) an option that turns ray tracing off.

## 1. Where the frame goes

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, branch `perf/guest-memory-binding`, strict runs,
60 fps present limit.

| Title and scene | Frame | Command processing | Outside command processing | GPU |
| --- | --- | --- | --- | --- |
| `PPSA17221 - Minecraft`, world, 5,200-5,700 draws | 48-51 ms | 34-38 ms, 6.3 us per draw | about 14 ms (frame minus command processing) | not saturated |
| `PPSA21567 - ASTRO BOT`, dunes, 370-630 draws | 48-52 ms | 51-53 ms, of which 15-25 ms are waits on the GPU | not measured | 83-90 % busy, 42-48 ms; one program with BVH intersections takes 19.7 of 44.4 ms |

In the first row the frame is the sum of the guest's own work and the command processor's run of
its batch, because a submit returns only after the batch is decoded (`cross-title.md`, "A graphics
submit returns after its batch is decoded"). In the second row the GPU bounds the frame.

## 2. Leads from three independent implementations

Three independent open-source implementations of the same guest interface were read on 2026-11-12.
They are behaviour leads (`AGENTS.md`): nothing is copied, and a guest contract is taken from them
only where Kyty's own traces agree.

| Question | First | Second | Third |
| --- | --- | --- | --- |
| Submit returns | before decode | before decode | before decode |
| Copied at submit | command words | nothing | command words, indirect register tables, index and vertex data up to a cap |
| Bound on guest run-ahead | flips in flight | one frame boundary | one frame boundary |
| Guest releases GPU-visible memory | waits only for batches that hold a lease on the range | waits for the GPU only when GPU state covers the range | unregisters at once, destroys after submitted work completes |
| Threads | a worker per queue, recorder threads | one GPU thread | command thread, render thread, a pool that resolves draws |
| Texture binding | cached, hash and write watch | one sampled-image array per guest table, page protection | one sampled-image heap, per-draw memos, page guards |
| Resolution option | reported output size | window size, upscaler | window size |
| Ray tracing option | every intersection reports a miss | none | compute programs with intersection instructions are skipped |

- **Agreement.** All three return from a submit before decode and bound how far the guest runs
  ahead. None drains every queue on every memory release.
- **Disagreement.** What a submit must copy: nothing, the command words, or the command words plus
  tables and geometry. Kyty's own trace outranks all three: `PPSA21402 - Balatro` read descriptor
  memory the guest had already reused when a submit returned at once (`cross-title.md`,
  2026-10-23). The read set is therefore derived from Kyty's titles (stage B0), and that title's
  boot is the correctness gate of workstream B.
- **Not adopted.** Two of them bound run-ahead at the guest's suspend-point call. That is guest
  behaviour known only from secondary sources. Kyty bounds run-ahead inside the submit, which is
  emulator policy and needs no guest contract.

## 3. Workstreams

### A. Memory release without a full drain (first)

- **Problem.** `release_direct_memory` (`Memory.cpp`) reaches `GraphicsInvalidateGpuMappingRanges`,
  which runs under `Gpu::RunQuiesced`; `Gpu::WaitLocked` drains every ring and the host GPU. The
  same quiesce serves unmap, fixed-map replacement and protection changes. The world scene of
  `PPSA17221 - Minecraft` drains 33 times a second. With the serial submit the rings are already
  idle there (under 0.5 ms a frame); with a submit that returns early the guest thread spent 691 ms
  of each second in this wait (`docs/titles/sandbox.md`). The quiesce protects the emulator's own
  objects, not a guest contract.
- **Why first.** Without it the sandbox title cannot show any overlap, and with it the
  one-batch-ahead experiment (B0) measures the real ceiling before the capture is built.
- **A1, measure.** Per release, one run per title: size, whether a memory object, a device-address
  entry or an import window overlaps the range, whether the command processor is inside a host read
  of the range, and whether the next mapping reuses the addresses before the queued batches end.
- **A2, the two global steps.** `GpuMemory::Free` runs `WriteBackAllCompleted` and the
  device-address release runs `DestroyRetiredTables`; both assume an idle host GPU. Each retired
  item gets the submission that last used it and is freed when that submission completes.
- **A3, ordered invalidation.** The release posts the range into the ring in submission order and
  the command processor applies it between batches. The guest thread waits only while the command
  processor is inside a host access of that range, or when a new mapping would reuse addresses that
  queued batches still hold; then it waits for the ring as today. The host pages stay mapped until
  the ring has passed the invalidation.
- **Gate.** The strict regression set; a unit test for a release between two queued batches; a
  hand-made access after release that the check must report.

### B. A submit returns when its batch's guest reads are done

- **Contract kept.** The guest gets control back only after every read its batch makes of memory
  the guest may rewrite. Today that is the end of decode. The change moves those reads earlier. The
  serial submit is run-ahead depth 0 of the same path; there is no second mode to keep.
- **B0, measure before building.**
  1. Serial submit: per batch, the position of the last draw that cannot take prepared inputs
     (geometry passes, indirect draws, draws after a host write). If it sits near the end, the draw
     front cannot be the capture as it stands.
  2. One batch ahead (each submit waits for the previous batch): the bytes at analysis against the
     bytes at use, per class (register tables, descriptor tables, index, vertex, storage, texture,
     shader code), on `PPSA21402 - Balatro`, `PPSA17221 - Minecraft` and `PPSA21567 - ASTRO BOT`.
     The 2026-11-11 result that no data range changed was measured with the serial submit and says
     nothing about a released submitter.
  3. One batch ahead after workstream A: the frame time, and whether the guest starts its next
     frame earlier. This is not established for any title; the fighting title waited on an event
     flag instead (`docs/titles/anime-fighter.md`).
- **B1, capture.** The draw front becomes the batch's capture: register state and descriptor words
  gathered before the submit returns, plus a copy, in a bounded arena owned by the batch, of each
  class B0 finds rewritten. A batch over the bound stays serial.
- **B2, return rule.** Each batch keeps a mark of the last guest read still pending. A read that
  depends on an earlier command of the same batch (the host-write-epoch rule) stays with the
  command processor at its position, and the submit waits until the mark passes it.
- **B3, write-side audit.** Labels, interrupts, flip events and write-backs must be emitted in
  stream order after the work they certify, and the order across the graphics and compute rings
  kept. Each path is listed and checked before B2 is enabled.
- **Estimate.** Sandbox world: the frame becomes the larger of guest time plus capture and command
  processing, about 28-36 fps from 20.

### C. Staged command processing

- The decision of `parallel-recording-design.md` is amended: build. Stage one resolves in guest
  order; stage two records on workers into secondary command buffers, one per pass segment.
- The bound of 1.2-1.7x of command-processing time was measured at 18.8 us per draw; the classes
  are measured again at today's 6.3 us before the build. With B in place command processing is the
  frame, so the factor reaches the frame rate.
- Unmeasured: the cost of a secondary per segment of 22-26 draws and of the state each one re-emits.

### D. Per-draw binding floor

- **Measured 2026-11-12, sandbox world.** Reusing the bound state when a (stage, shader) pair
  repeats every descriptor word is not a lever (`cross-title.md`). That test is all-or-nothing per
  shader; it does not price one persistent slot per texture.
- **Direction.** A persistent slot per guest texture descriptor in one sampled-image array, with
  the per-draw indices as dynamic data; buffers are by address since 2026-11-11. First step: the
  repeat rate and cost of resolving one texture descriptor, in the robot platformer, whose texture
  stage costs 8.9 us per draw.

### E. Internal resolution as a functional option

- **State.** `RenderResolutionScope = RenderTargets` scales only targets of the display extent and
  has two hard exits (`RequireScaledImageReads`). The user run of 2026-11-12 at 960x540 stayed near
  20 fps in gameplay and exited at `GraphicsRenderAttachments.cpp:824`.
- **E1, functional first.** Repeat the `:824` exit at native resolution to learn whether scaling
  causes it. Make the two refusals demote the framebuffer group to native at planning time instead
  of stopping the run.
- **E2, one run.** Whether the title's own grid follows the output size the guest is told
  (`ScreenWidth`, `ScreenHeight`). If it does, the output size becomes an option separate from the
  window size.
- **E3, scaled texel access.** Lower integer texel access (fetch, image load and store, fragment
  coordinate, size queries) with the scale, so storage and fetch paths stay scaled.
- **Limit.** A compute program's grid is fixed by the guest. Emulator-side scaling cannot shrink
  the BVH program's 2,087,936 invocations; only the guest's own configuration (a game patch
  measured 16.6-18.1 fps against 12-13) or workstream F does.

### F. Ray tracing off

- **Detection.** Programs that contain `ImageBvhIntersectRay` (opcode 0xE6). Opcode 0xE7 is not
  decoded, and a BVH walked with plain loads cannot be detected.
- **Evidence.** A forced miss at every node saved about 1 ms of 24 in the robot platformer, so a
  mode that reports misses does not deliver. Not running the program is what removes the cost.
- **F0, one run.** Skip the programs that contain the instruction; take a capture and the frame
  rate.
- **F1, option.** `RayTracing = Guest` (default) or `Off`, in the configuration, the launcher and
  an environment variable. `Off` is not a strict mode and makes no compatibility claim, like a game
  patch. The content of a skipped program's outputs is defined by F0's capture.
- **Expectation.** The robot platformer's command processing without waits was 28-36 ms a frame in
  the user run, so about 30 fps even with the GPU freed.

## 4. Order

1. Stage 0: A1, B0.1, E2 and F0, four short runs.
2. A2 and A3, then B0.2 and B0.3, then B1 to B3, then C, then D.
3. E1 and F1 are small and independent; they go in between.

## 5. Verification of every stage

The unit suites and the three CTest gates, `kyty_agent playable-regression`, a strict run of
`PPSA17221 - Minecraft` and of `PPSA21567 - ASTRO BOT` with scored captures, `PPSA21402 - Balatro`
for workstream B, and the AMD integrated GPU as a second driver.
