# Performance architecture plan (2026-11-12)

Status: plan, stage 0 measured (section 6), next B0.2, then A2 and A3. Maintainer decision of 2026-11-12:
build the changes below, and the 2x build gate of `parallel-recording-design.md` is withdrawn.
Every gain in this document is an estimate until its stage is measured. Each stage names its
measurement and its correctness gate.

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
  of each second in this wait (`docs/titles/sandbox.md`).
- **What the drain also protects (read 2026-11-12).** Host writes that land when a submission
  completes: label stores (`Label.cpp:481-491`) and end-of-pipe callbacks (for example the clock
  value at `GraphicsRenderEop.cpp:290`) write through guest pointers checked when they were
  recorded. A physical release keeps the heap, so the allocated-range check before a label store
  still passes, and the label fence registry lists label stores only, not callback writes. On Gen5
  `PhysicalMemory::Release` (`Memory.cpp:1154-1273`) takes the range out of the allocated set at
  once and keeps the views that map it; the backing goes only when no view maps it. Without the
  drain a late write can land in a range the guest has allocated again. No test over memory objects
  and device-address ranges can therefore show that a release is safe without the drain.
- **Why first.** Without it the sandbox title cannot show any overlap, and with it the
  one-batch-ahead experiment (B0) measures the real ceiling before the capture is built.
- **A1, measure.** Per release, one run per title: size, whether a memory object, a device-address
  entry or an import window overlaps the range, whether the command processor is inside a host read
  of the range, and whether the next mapping reuses the addresses before the queued batches end.
- **A2, the two global steps.** `GpuMemory::Free` runs `WriteBackAllCompleted` and the
  device-address release runs `DestroyRetiredTables`; both assume an idle host GPU. Each retired
  item gets the submission that last used it and is freed when that submission completes; the
  device-address registry already retires tables by reader (`GuestDeviceAddress.cpp:2229-2238`).
  These steps alone do not let the drain go: the late host writes above need A3's range lifetime.
- **A3, ordered invalidation.** The release posts the range into the ring in submission order and
  the command processor applies it between batches. The guest thread waits only while the command
  processor is inside a host access of that range, or when a new mapping would reuse addresses that
  queued batches still hold; then it waits for the ring as today. The host pages stay mapped, and
  the physical range is not handed out again, until the submissions outstanding at the release
  are complete.
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
  class B0 finds rewritten. A batch over the bound stays serial. Stage 0 (section 6) amends this:
  the capture has to cover auto draws and dispatches as well as indexed draws and has to reach the
  end of the batch before the submit returns, because in both titles the last item the draw front
  does not prepare sits in the last hundredths of the batch.
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
  window size. Result (section 6): it does not in the robot platformer, so that option is not
  built.
- **E3, scaled texel access.** Lower integer texel access (fetch, image load and store, fragment
  coordinate, size queries) with the scale, so storage and fetch paths stay scaled.
- **Limit.** A compute program's grid is fixed by the guest. Emulator-side scaling cannot shrink
  the BVH program's 2,087,936 invocations; only the guest's own configuration (a game patch
  measured 16.6-18.1 fps against 12-13) or workstream F does.

### F. Ray tracing off

- **Detection.** Programs that contain `ImageBvhIntersectRay` (opcode 0xE6). Opcode 0xE7 is not
  decoded, and a BVH walked with plain loads cannot be detected.
- **Evidence.** A forced miss at every node saved about 1 ms of 24 in the robot platformer: the
  traversal is a small part of that program, the rest is shading that runs either way.
- **F0, one run, done: skipping is rejected.** Not running the programs that contain the
  instruction raised the dunes from 20.0 to 26.0 fps and left the image wrong (section 6): the
  programs' outputs stay unwritten and the passes that read them show dark terrain and translucent
  models. Which output each later pass reads was not traced. A skip is therefore not an option
  Kyty offers.
- **F1, option.** `RayTracing = Guest` (default) or `Off`, in the configuration, the launcher and
  an environment variable. `Off` makes every intersection report a miss: the programs still run
  and write every output, so the image keeps its lighting and loses what the rays add (traced
  shadows and reflections). `Off` is not a strict mode and makes no compatibility claim, like a
  game patch. Expected saving in the robot platformer: about 1 ms of 24 per call of the heavy
  program, by the record above; it is built for the titles where traversal is the cost.
- **Expectation, amended.** With the GPU cost of those programs gone entirely (the rejected skip)
  the dunes ran at 26.0 fps with 36.1 ms of command processing a frame. No GPU-side lever alone
  passes about 26-28 fps in that scene; the rest is command processing (workstreams B to D). The
  title's own renderer selection (a game patch, `docs/game-patches.md`) is the other way to drop
  the ray-traced passes. It measured 20.6-20.9 fps against 18.5 in the hub and its image was not
  verified (`docs/titles/robot-platformer.md`, "Game patch trial and geometry cost", 2026-11-01).
  It is the guest's configuration, not an emulator option.

## 4. Order

1. Stage 0: A1, B0.1, E2 and F0, four short runs.
2. A2 and A3, then B0.2 and B0.3, then B1 to B3, then C, then D.
3. E1 and F1 are small and independent; they go in between.

## 5. Verification of every stage

The unit suites and the three CTest gates, `kyty_agent playable-regression`, a strict run of
`PPSA17221 - Minecraft` and of `PPSA21567 - ASTRO BOT` with scored captures, `PPSA21402 - Balatro`
for workstream B, and the AMD integrated GPU as a second driver.

## 6. Stage 0 results (2026-11-12)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, `bbc559839` plus a temporary diagnostic (not
committed) that prints five counter lines every 4 s, serial submit, strict runs, 60 fps present
limit. Windows: `PPSA17221 - Minecraft` world, 40 s; `PPSA21567 - ASTRO BOT` dunes route, 105-148 s
(36 s of counters).
Frame rates are comparable only inside this section.

### A1, release census (partial: serial submit only)

`PPSA17221 - Minecraft`, two runs. The per-batch sweep in `ObjectsOnly` mode is not a guest release
and is left out.

| | Run 1 | Run 2 |
| --- | --- | --- |
| Quiesce transactions per second | 40.5 | 52.6 |
| Ranges released by `release_direct_memory` per second | 98.3, 8.7 MiB | 188.2, 14.6 MiB |
| Ranges that overlap a memory object | 114 of 4,333 (2.6 %) | 103 of 7,529 (1.4 %) |
| Ranges that overlap a device-address range | 337 (7.8 %), one chunk each | 343 (4.6 %), one chunk each |
| Drain per transaction, mean and maximum | 0.42 ms, 15.9 ms | 0.35 ms, 20.8 ms |
| Drain per second of run | 17.2 ms | 18.5 ms |
| Action per transaction; write-back step | 0.08 ms; about 1 ms per second | 0.06 ms; about 1 ms per second |

- Run 2 sizes: 7,219 ranges up to 64 KiB, 297 up to 1 MiB, 13 up to 16 MiB. 6,860 of 7,529 (91 %)
  released an address that was among the 256 addresses released before it. No release in unmap
  mode.
- `PPSA21567 - ASTRO BOT`, dunes: no guest release in the window.
- **Reading.** About nine released ranges in ten overlap no memory object and no device-address
  range, and the action itself is short; the drain is the cost, and its 16-21 ms maximum is a frame
  hitch even with the serial submit. Overlapping nothing does not make a release safe without the
  drain (section A, late host writes).
- **Not measured.** Import windows; whether a queued batch reads a released range; whether the next
  mapping reuses the addresses before the queued batches end. The last two need a submit that
  returns early. The 91 % address reuse says the "wait as today" case of A3 may be the common one,
  so A3 carries no gain claim until that is measured.

### B0.1, last item without prepared inputs

Batches of at least 64 draws. Position is the fraction of the batch's time at which the item was
processed.

| | `PPSA17221 - Minecraft`, world | `PPSA21567 - ASTRO BOT`, dunes |
| --- | --- | --- |
| Batches per second, time each | 32, 22.4 ms | 70 (160 with the small ones, over several rings), 11.2 ms |
| Indexed draws with prepared inputs, per batch | 2,522 | 50.2 (all batches) |
| Indexed draws without, per batch | 291 (10.4 %) | 9.1 (15.3 %) |
| Auto draws, dispatches per batch | 18.9, 47.1 | 13.1, 11.6 |
| Position of the last item without prepared inputs | 0.998, in the last tenth in 1,280 of 1,280 batches (1,367 of 1,367 in run 1) | 0.988, in the last tenth in 2,528 of 2,532 |
| Kind of that last item | draw front behind the command processor, 1,280 of 1,280 | auto draw 2,165, dispatch 367 of 2,532 |

Reasons a draw or dispatch had no prepared inputs, per second, with the mean position of the last
one of its kind in a batch:

| Reason | Minecraft | Position | ASTRO BOT | Position |
| --- | --- | --- | --- | --- |
| Draw front restarted | 1,170 | 0.872 | 285 | 0.693 |
| Record not prepared yet | 422 | 0.076 | 0 | |
| Stale by a host write to guest memory | 2,160 | 0.145 | 948 | 0.874 |
| Draw front behind the command processor | 5,567 | 0.998 | 221 | 0.918 |
| Auto draw (never prepared) | 605 | 0.845 | 2,102 | 0.980 |
| Dispatch (never prepared) | 1,507 | 0.865 | 1,865 | 0.912 |

- **Reading.** The draw front as it stands is not the capture: it does not reach the end of any
  batch ahead of the command processor in the sandbox title, and it never prepares the auto draws
  and dispatches that end the robot platformer's batches. Reads that depend on a host write end
  early in the sandbox title's batches and late in the robot platformer's.
- **Not measured.** The draw front's own time per batch, which is the time a submit would still
  hold the guest once the front is the capture. The replay time per prepared record stands in for it
  in the next subsection.

### Drain split and draw-front replay time

One run per title on `a6c3df9f1` with a second temporary diagnostic (not committed): the drain timed
in two parts, the ring waits and the command processors' `SubmitAndWait`, and the draw front's
`Replay` timed against the records it prepared. `Replay` holds no wait; the front parks outside it.

| | `PPSA17221 - Minecraft`, world, 40 s | `PPSA21567 - ASTRO BOT`, dunes, 44 s |
| --- | --- | --- |
| Frame rate; draws and dispatches per frame | 20.2; 5,054 draws | 18.3; 593 draws, 84 dispatches |
| Command processor per frame; GPU busy | 35.3 ms; 65 % | 56.0 ms (all pipes); 89 % |
| Quiesce drains per second | 29.3 | 0 |
| Ring waits per second; per frame | 19.4 ms; 0.96 ms | |
| `SubmitAndWait` per second; per frame | 4.0 ms; 0.20 ms | |
| Ring waits over 1 ms per second; longest ring wait; longest `SubmitAndWait` | 2.7; 29.9 ms; 2.4 ms | |
| Replay per second; records prepared per second | 288 ms; 100,276 | 176 ms; 8,730 |
| Replay per prepared record; per frame | 2.88 us; 14.3 ms | 20.2 us; 9.6 ms |

- **Reading, release.** Most of the drain waits for other rings to finish a batch in flight; the
  command processors' own wait is 0.2 ms per frame. Under the serial submit, A removes at most about
  1.2 ms per frame and the drain's 30 ms worst case. A is a prerequisite of B, not a frame-rate
  lever on its own.
- **Reading, submit.** If a capture costs what the replay costs per record, a submit that returns
  after the capture holds the sandbox title's guest about 14 ms per frame instead of the command
  processor's 35 ms. The frame then follows the command processor, about 28 fps. That is an
  estimate: the capture's own cost and the copies of B0.2 are not in it. The robot platformer is
  bounded by the GPU at 89 % busy; B carries no frame-rate estimate there.
- **Consequence.** B alone does not reach 30 fps in the sandbox title; the command processor's time
  per frame has to fall as well (C and D).

### E2, reported output size (excluded for the robot platformer)

Census of the first colour target's extent per indexed draw, `PPSA21567 - ASTRO BOT`, dunes route.
The diagnostic printed the screen size it read, so the variable engaged.

| Reported output size | 1920x1080 | 2432x1368 | 3840x2160 | 960x540 | Frame rate | GPU busy |
| --- | --- | --- | --- | --- | --- | --- |
| 1280x720 | 451,615 | 2,117 | 7,179 | 73,443 | 19.5 fps | 92 % |
| 960x540 | 466,605 | 3,340 | 7,566 | 75,545 | 20.4 fps | 95 % |

The title's targets do not follow the reported size, and its display buffer is 3840x2160 in both
runs. `PPSA17221 - Minecraft` also draws mainly at 1920x1080 with 1280x720 reported. The robot
platformer starts at 2432x1368 and steps to 1920x1080 by itself (2,117 draws at the larger size
with 1280x720 reported; 3328x1872 appeared only with 960x540 reported, 6 draws).

### F0, programs with intersection instructions not run (rejected)

`PPSA21567 - ASTRO BOT`, dunes route, one pair of runs.

| | Guest | Programs skipped |
| --- | --- | --- |
| Frame rate | 20.0 fps | 26.0 fps |
| Command processing per frame | 54.1 ms | 36.1 ms |
| Of which fence waits, register waits | 15.6 ms, 11.6 ms | 1.2 ms, 1.3 ms |
| GPU busy | 94 % | 82 % |

18,024 dispatches were skipped. The image is wrong: dark terrain, a translucent player model and
washed-out objects, in the scored capture pair and under user play (26-30 fps reported by the
window). The cause is recorded as not traced: the skipped programs' outputs stay unwritten, and
which later pass reads which output was not followed.

### Guest GPU clock marks

The robot platformer writes about 8,700 end-of-pipe clock values a second, about 440 a frame, in
pairs 0x20 bytes apart. Kyty samples the clock when the label callback runs at the completion of
the host submission (`GraphicsRenderEop.cpp:284-296`), so every mark of one submission gets nearly
the same value and the two values of a pair differ by about 0.02 ms. Recorded as an open defect in
`cross-title.md` with a suggested direction (a host timestamp per end-of-pipe position).

One run with every mark written at least 0.1 ms after the previous one: 6 draws at 2432x1368
against 2,117 in the run without the spacing, and no main target below 1920x1080 in 143 s. That
run is not comparable beyond its first frames: its captures at 108, 125 and 145 s show the intro
cinematic where the other run is in gameplay, and it measured 17.4 fps with 369 draws a frame
against 19.5 and 592. Open: whether the title's dynamic resolution follows the marks, whether
1920x1080 is its lowest step, and whether correct marks change the frame rate.
