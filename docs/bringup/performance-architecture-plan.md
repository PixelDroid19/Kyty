# Performance architecture plan (2026-11-12)

Status: plan, stage 0, B0.2 and the one-batch-ahead probes measured (section 6); an own-producer rule for B1 passed three boot gates, next the read-set writer and the in-flight overlap of A3. Maintainer decision of 2026-11-12:
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

### A. Memory release without a full drain (in design, section 6)

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
- **Order.** First. A timeline estimate briefly put B1 ahead; a probe with one batch in flight
  (section 6) disproved it: the release drains on the submitting thread took 22 ms of each guest gap
  and the frame rate did not move. A3 is necessary for any gain from B; that it is sufficient is not
  shown.
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

### B. A submit returns when its batch's guest reads are done (in design, section 6)

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
     nothing about a released submitter. Measured 2026-11-12 over program intervals of the serial
     submit (section 6, B0.2): one batch ahead no descriptor table or shader code changed in the
     three titles; vertex and index copies did; the sandbox title's tables change two batches ahead.
  3. One batch ahead: the frame time, and whether the guest starts its next
     frame earlier. This is not established for any title; the fighting title waited on an event
     flag instead (`docs/titles/anime-fighter.md`). The serial submit's timeline (section 6)
     estimated x1.43 for the sandbox title and at most x1.08 for the robot platformer; a probe
     with one batch in flight measured no gain in the sandbox title, held by the release drains.
- **B1, capture.** The draw front becomes the batch's capture: register state and descriptor words
  gathered before the submit returns, plus a copy, in a bounded arena owned by the batch, of each
  class B0 finds rewritten. A batch over the bound stays serial. Stage 0 (section 6) amends this:
  the capture has to cover auto draws and dispatches as well as indexed draws and has to reach the
  end of the batch before the submit returns, because in both titles the last item the draw front
  does not prepare sits in the last hundredths of the batch. B0.2 amends it again: one batch
  ahead, the copies the capture must take are the vertex and index copies; a depth beyond one adds
  the descriptor tables.
- **B2, return rule.** Each batch keeps a mark of the last guest read still pending. A read that
  depends on an earlier command of the same batch (the host-write-epoch rule) stays with the
  command processor at its position, and the submit waits until the mark passes it.
- **B3, write-side audit.** Labels, interrupts, flip events and write-backs must be emitted in
  stream order after the work they certify, and the order across the graphics and compute rings
  kept. Each path is listed and checked before B2 is enabled.
- **Estimate.** Sandbox world: the frame becomes the larger of guest time plus capture and command
  processing, about 28-36 fps from 20. Section 6 narrows it to at most about 28 fps, bounded by the
  command processor, with the capture costed at up to 0.4 of the decode time (the draw front's
  replay time does not cover the vertex and index analysis; section 6).

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
2. B0.2, the one-batch-ahead timeline and probes (done); the own-producer rule (section 6). Next the
   writer of the copies that change one batch ahead and the in-flight overlap of A3, then B1 and A3
   together, then the fence wait, then C, then D.
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

- **Reading, release.** Most of the drain is ring waits. A ring stays busy after its decode signal
  for the rest of the batch (the wait for the submission, the publication), so a guest that releases
  memory right after its own submit returns can wait on its own ring; which ring the time belongs to
  was not identified. The command processors' `SubmitAndWait` is 0.2 ms per frame. Under the serial
  submit, A removes at most about
  1.2 ms per frame and the drain's 30 ms worst case. A is a prerequisite of B, not a frame-rate
  lever on its own.
- **Reading, submit.** If a capture costs what the replay costs per record, a submit that returns
  after the capture holds the sandbox title's guest about 14 ms per frame instead of the command
  processor's 35 ms. The frame then follows the command processor, at most about 28 fps. That is an
  estimate: the capture's own cost and the copies of B0.2 are not in it. The robot platformer is
  bounded by the GPU at 89 % busy; B carries no frame-rate estimate there.
- **Consequence.** B alone does not reach 30 fps in the sandbox title; the command processor's time
  per frame has to fall as well (C and D).

### B0.2, guest bytes a batch read and what the guest rewrites after the submit

Serial submit, `0ce88de2b` plus a temporary diagnostic (not committed). The guest reads that the
command processor and the draw front make at these sites, while decoding a batch whose submit
waits for its decode, are recorded with their bytes' hash: nested command words, indirect register tables outside
the submit snapshot, descriptor tables (the usage parse's tables, vertex attribute tables and
vertex-buffer descriptors), shader code, scalar-memory spans, and the index, vertex and storage
copies. Each entry is rehashed through the same checked guest copy at three points: when the next
submit on the ring enters (one batch ahead), when that submit returns, and when the submit after
it enters (two batches ahead). A hand-made entry that the diagnostic changes before each check was
reported changed in every window. Textures and indirect arguments are not covered (the GPU reads
the arguments at execution); the submit already copies the top-level indirect register tables, and
texture, sampler and buffer descriptors held in user registers come with the command words it
copies. The other direct guest reads on the draw path are trace-only or shader-code reads. The
windows are program intervals of the serial submit: a guest released earlier runs the same
intervals, sooner.

| | `PPSA21402 - Balatro`, boot | `PPSA17221 - Minecraft`, world (2 runs) | `PPSA21567 - ASTRO BOT`, dunes |
| --- | --- | --- | --- |
| Windows; mean length one batch ahead; two ahead | 5,771; 1.3 ms; 18.0 ms | 5,172 and 4,741; 16.1-16.8 ms; 44.9-47.3 ms | 18,055; 0.94 ms; 8.7 ms |
| Descriptor entries checked; changed one batch ahead | 1.77 M; 0 | 31.3 M and 28.5 M; 0 | 1.57 M; 0 |
| Descriptor entries changed by the next submit's return | 0 | 0 (second run) | 0 |
| Descriptor entries changed two batches ahead | 0 | 24.7 M (79 %) and 20.6 M (72 %) | 0 |
| Shader code: entries, changed; nested command words and their register tables | 23, 0; none read | 180-203, 0; none read | 619, 0; none read |
| Vertex copies: entries; changed one batch ahead | 162 k; 8 (16 two ahead), 128 bytes each | 253 k and 225 k; 5,702 and 5,578 (about 0.9 MiB) | 1,451; 0 |
| Index copies: entries; changed one batch ahead | 1; 0 | 1,765 and 854; 14 and 5 | 2,339; 0 |
| Storage copies: changed one batch ahead; by the return | none read | 50 and 8; 5,204 (19.4 GiB, second run) | 1,170 (1.3 GiB, 32-byte and 2 MiB ranges); 1,168 |
| Entries recorded on the draw-front thread | 0 | 30.0 M and 27.4 M | 1.0 M |

- **Reading, one batch ahead.** In the three titles no descriptor table, shader code or nested
  command word that a batch read had changed when the next submit entered or returned. The
  sandbox title rewrites 72-79 % of its descriptor entries before the submit after that: its
  tables are reused two submits later. A submit that returns before
  decode and waits for the previous batch's decode (one batch ahead) therefore needs no copy of
  these classes in these runs. That is a lower bound: equal bytes at two instants do not prove
  that nothing was written in between, and other scenes may reuse sooner.
- **Reading, copies.** Vertex and index copies change one batch ahead (the sandbox title about
  5,600 vertex and 5-14 index entries per run, the card title 8 vertex entries). The writer, guest
  or GPU, was not identified, so B1 takes those copies before the submit returns. Storage changes
  include the GPU's own writes (most land while the next submit waits, with the submitter
  blocked) and are not attributed.
- **Reading, run-ahead.** Two batches ahead the sandbox title's tables are already rewritten, so
  any depth beyond one needs a descriptor capture. The card title's failure of 2026-10-23 (an
  unbounded early return) does not reproduce within two batches here: no descriptor it read had
  changed two batches ahead. What it read then remains unexplained by this measurement.
- **Consequence for B1.** One batch ahead, the capture is the analysis that finds the batch's
  vertex and index copies and those copies. The draw front does not do that analysis: it computes
  the shader input layout and the shader ids (`GraphicsRenderPrepareIndexedDrawInputs`), reading
  descriptor and attribute tables, while the index scan and the vertex and index copies run on the
  command processor. Its replay time above is not the capture's cost. The release drain still waits for the
  in-flight decode, about 1.5 times a frame in the sandbox title, priced by the timeline below.

### Per-batch timeline of the serial submit and a one-batch-ahead estimate

Serial submit, `b05446080` plus a temporary diagnostic (not committed) that stamps, per graphics
ring, the submit's entry, the decode thread's dequeue, its decode signal, the submit's return and
the decode thread's next dequeue. Only submits that wait for their decode are stamped. The
estimate replays the measured decode times and guest gaps (submit return to the next entry) with
a submitter that returns once the previous batch is decoded, the capture costed at zero and at 0.4
of the decode time; it ignores the release drain, which also waits for the in-flight decode.

| | `PPSA17221 - Minecraft`, world (40 s) | `PPSA21567 - ASTRO BOT`, dunes (40 s) |
| --- | --- | --- |
| Batches per second | 20.3 | 97.1 |
| Decode: mean, median; share of the span | 34.4 ms, 33.6 ms; 70.0 % | 9.5 ms, 8.0 ms; 92.1 % |
| Guest gap: mean, median; share of the span | 14.4 ms, 13.5 ms; 29.3 % | 0.56 ms, 0.04 ms; 5.4 % |
| Entry to dequeue; signal to return; signal to next dequeue (medians) | 0.26 ms; 0.07 ms; 0.10 ms | 0.09 ms; 0.04 ms; 0.05 ms |
| One batch ahead, capture 0 and 0.4 of decode | x1.43 and x1.43 | x1.08 and x1.08 |

- **Reading.** In the sandbox title, under the serial submit, a batch's decode is 70 % of the
  span and the guest's own work between submits the other 29 %. The decode
  thread takes the next batch about 0.1 ms after its signal, so no tail of work after the signal
  holds the ring. With one batch in flight the frame becomes the decode time, about x1.43 (about
  20 to at most 28-29 batches per second). The estimate holds for a capture up to 0.4 of the decode
  because the decode, not the submitter, sets the pace. A first run (shorter window) gave x1.42.
- **Reading, robot platformer.** The guest submits about five batches per frame (about 19 frames per
  second by the captures' frame counters) and re-enters the submit almost at once (median gap 0.04
  ms): the decode thread is busy 92 % of the span. One batch ahead gains at most x1.08 there, and
  the title is GPU-bound (89 % in the stage 0 run), so its frame needs less GPU and
  command-processor time (workstreams C and D, and F), not an earlier return. The decode time
  includes the decode thread's waits on the GPU, so x1.08 is an upper bound.
- **Release drains with one batch in flight** (one more sandbox run, the same stamps plus each
  quiesce's entry and exit with its thread). The submit came from one thread (828 of 829 submits).
  In 40 s, 979 quiesces ran on that thread, in 671 of 827 guest gaps, 0.22 ms median each; none ran
  inside a submit. They start late in the gap, at a median 0.82 of its length. Replayed with each
  such drain waiting for the batch in flight, one batch ahead gave an estimated x1.29 instead of
  x1.42 in the same run; the probe below disproved that estimate. Another 115 quiesces ran on other threads (0.16 ms median, 10 ms mean); they hold
  submissions back only while they run and are not modelled. A hand-made drain at the start of every
  gap gives x1.00 and one at its end x1.41, so the replay engages.
- **Probe, one batch in flight** (one more sandbox run; a temporary change, not committed, made a
  graphics submit wait for the previous batch's decode instead of its own, with the same stamps).
  The player was killed about 9 s after the world loaded (no input route, night) and the death
  screen draws half as much, so only the 8 s before it compare with the serial runs at the same
  scene (5,200-5,800 draws a frame): 19-23 fps against 18-22, no gain. Over those 8 s against the
  serial run: decode 37-42 ms against 33-34 ms (command-processor time 38.4 against 33.6 ms a frame,
  fence waits 1.5 against 0.3 ms); guest gap 47.2 ms against 14.9 ms; submit 0-4 ms against 34 ms.
  Quiesces on the submitting thread: 390 against 210, 9.5 ms mean against 0.7 ms, 22.4 ms of each
  gap against 0.9 ms. The long ones end a median 4.3 ms after the in-flight batch's decode signal:
  they wait for that decode and its tail on the host GPU. Per gap (medians), 11.8 ms of guest work
  come before the first drain, about 30 ms of drains follow, then 2.2 ms; the guest's own work
  outside the drains stays about the same (15.3 against about 14 ms). Why the quiesce count doubled
  was not found.
- **Consequence.** The x1.29 estimate is disproved: it ended each drain at the decode's end and kept
  the serial decode and gap lengths. One batch in flight alone gains nothing in this title, and the
  release path (A3) is necessary before B can gain; that A3 is sufficient is not shown. With no
  drain waiting, the frame would still follow the decode, now 37-42 ms, about 24-27 fps at most, so
  reaching 30 in this title also needs a cheaper decode (C, D). A3 went first again; the next
  subsection puts A3 and B1 on hold.
- **Open, unattributed.** On the death screen of the probe run the text lacks letters (the killer's
  name and the respawn button). No serial run reached that screen, so whether one batch in flight
  caused it is not known. The next subsection shows it with the serial return as well.

### Release drain skip, one batch in flight on the robot platformer, and a wait census

Temporary changes on `6baccd17e`, not committed, one run each:

- **Drain skip (A3 probe).** A release whose range overlaps no tracked GPU memory, fence label,
  guest device-address table or VideoOut buffer skips the drain and runs only the label release and
  the texture-layout invalidation. It is unsafe against late host writes (label stores and
  end-of-pipe callbacks write through guest pointers when their submission is published), so it is
  a measurement only.
- **Wait rule.** A batch whose top-level command words hold a memory wait (`WAIT_REG_MEM` or the
  32- and 64-bit memory-wait packets) or a nested command buffer keeps the serial return; any other
  batch's submit waits for the previous batch's decode.
- **Census, serial semantics.** For each graphics batch at submit entry: its top-level waits, and
  whether each is already satisfied by the command processor's own compare. Later in the run:
  whether its decode was released because no submitted GPU work writes the awaited value. In every
  run, four hand-built packets (one wait satisfied, one unsatisfied, one with address 0, one with no
  wait) were classified correctly.

| Title, configuration | Result |
| --- | --- |
| Sandbox, one batch in flight with the drain skip | 9,552 of 10,095 releases skipped. At the same scene (5,380-5,455 draws a frame), 24.5-26.5 fps against 20.3-21.2 in the serial run, about x1.21. Command-processor time 38.7 ms a frame, fence waits 8.8 against 0.3 ms, GPU busy 76 %. Drains remain in 57 of 1,000 gaps, median 39.5 ms. |
| Robot platformer, one batch in flight with the drain skip | Stops at boot: frame 1, no present. On the graphics ring, 5 batches are dequeued and 4 signalled. |
| Robot platformer, one batch in flight only | Same stop. |
| Robot platformer, wait rule, version that also released the suspended batch's own submitter | Same stop. |
| Robot platformer, wait rule | Boots and plays: 3,599 presents at 145 s, 18,014 batches dequeued and signalled. No batch returned early. |
| Sandbox, wait rule with the drain skip | No batch returned early (5,741 serial). Drains: 12,434 skipped, 827 run. In three 5-second slices from 5 to 20 s after the world loaded, before the player died: 17.8-20.2 fps at 5,290-5,450 draws a frame, against 20.6-21.0 serial. |
| Robot platformer, census | 18,512 batches, each with at least one wait unsatisfied at entry or outside mapped memory (the two are not separated; about 20 32-bit and 5 64-bit memory waits per batch). 3,083 (17 %) were released for a host-produced value. The command processor's wait counters in the same run, 90-145 s only (dunes, not the boot): 148,478 waits at decode, 91.8 % with the producer in the command processor's current host submission, 5.8 % already satisfied, 0.8 % in a queued submission, 1.6 % suspended with no producer. |
| Sandbox, census | 5,659 batches, each with a wait unsatisfied at entry or outside mapped memory (about 64 32-bit memory waits per batch). None was released for a host-produced value. The command processor's wait counters, 20-151 s (menu and world): 323,443 waits at decode, all with the producer in the current host submission. |

- **Reading, robot platformer.** The stop has the signature of the one recorded on 2026-10-26
  (`docs/titles/robot-platformer.md`, "A suspended submitter must not pass the command
  processor"). There, a batch suspended on a CPU-written label must not return its own submitter
  before decode passes the wait. One batch in flight returns that submitter before the decode even
  reaches the wait. The two wait-rule runs are an A/B of that contract at this revision: with every
  batch serial, releasing the suspended batch's own submitter stops the title, and keeping it boots
  the title. The mechanism is inferred from the record: thread states and suspended waits were not
  taken in these runs.
- **Reading, census.** In neither title are a batch's waits already satisfied at submit, so a
  rule of that kind lets no batch return early. The census lumps two other cases in with
  "unsatisfied": waits outside mapped memory, and waits whose value an earlier packet of the same
  batch writes, which are unsatisfied at entry by construction. The command processor's counters
  point to the second case: every wait of the sandbox title and 91.8 % of the robot platformer's
  (dunes window, not the boot) have their producer in the command processor's current host
  submission, which is inferred, not shown, to be the same guest batch. Only the robot platformer has
  waits with no producer (1.6 %). A submit-time rule that a batch's waits are all written by its own earlier
  packets would therefore let every sandbox batch return early and keep serial the robot
  platformer's batches that wait on other work. That rule is neither built nor measured. Hardware
  returns from a submit at once, so which ordering in Kyty makes the early return unsafe is still
  unidentified.
- **Reading, drain skip alone.** With the serial return the drains cost 0.9 ms of each guest gap
  (serial timeline above), so skipping them alone cannot move the frame much. The one run agrees: no
  gain in three 5-second slices while the player was being hit. x1.21 came only from the combination
  that stops the robot platformer.
- **Consequence.** B1 (return before decode) and A3 (release without a drain) are on hold. The only
  measured gain needs both, the probe of the release is unsafe, and the combination stops the robot
  platformer at boot. The own-submission rule above is the open way back for B1 (taken in the next
  subsection). The 8.8 ms of fence
  waits a frame appear only with one batch in flight (0.4 ms with the serial return), so they are
  not a lead for the serial submit. Next are C and D, the sandbox title's decode cost. The robot
  platformer is GPU-bound (89-91 % busy) and B gives it at most x1.08; its frame-rate levers are E1
  and F1, and C's rule for it is command-processor time per frame.
- **Missing letters.** The sandbox title's death screen lacked letters again in the run with the
  serial return and the drain skip, and also in the census run, whose submit and release behaviour
  is unchanged (it only reads the command words and the awaited values). The defect exists at
  `6baccd17e` without either probe (`docs/titles/sandbox.md`).

### Own-producer rule for the early return

Temporary changes on `b0003cd26`, not committed, one run per title and configuration:

- **Scan at submit.** A batch is eligible when it has no nested command buffer and every top-level
  memory wait's address is written by an earlier packet of the same batch. The packets that count
  are `WRITE_DATA` (both forms), `RELEASE_MEM` (both forms, when their data selector writes),
  end-of-pipe and end-of-shader event writes, the custom DMA packet, `DUMP_CONST_RAM` and the two
  label-writing markers. A 64-bit wait with address 0 takes the destination of the release packet
  just before it, as the command processor does. `DMA_DATA` is left out (its destination selector
  was not checked). Hand-built packets were classified correctly in every run: a wait after its
  release, a wait alone, and an address-0 wait after a release.
- **Census at decode** (serial semantics). Per batch, whether any wait was classed other than
  current-producer by the command processor, and whether the decode was released for a host-produced
  value.
- **Early return.** An eligible batch's submit waits for the previous batch's decode; any other
  batch keeps the serial return.

| Title, configuration | Result |
| --- | --- |
| Robot platformer, census | 15,642 eligible batches, none with a wait already satisfied, from a queued submission, mismatched or without a producer, and none released. All 3,352 releases and 3,386 waits without a producer are in the 3,910 ineligible batches. |
| Sandbox, census | All 5,264 batches eligible, all 337,595 waits with an earlier producer in the batch, no release. |
| Robot platformer, early return | 16,153 batches early, 4,038 serial. Boots and plays the dunes route; the three captures render the scene. 105-145 s: 17.98 fps at 605 draws a frame, against 19.44 at 579 in the serial run (route variation; no gain, as estimated). |
| `PPSA21402 - Balatro`, boot, early return | All 5,765 batches early. 60 fps; at 100 s the same game state as the serial boot. |
| Sandbox, early return with the drain skip | All 5,289 batches early; drains 10,698 skipped, 591 run. Over a full minute in the world (5,374-5,460 draws a frame): 23.0-25.5 fps, mostly 25.0, against 20.0-21.2 serial, about x1.20. Command-processor time 38-42 ms a frame, fence waits 5.9-9.0 ms, GPU busy 65-68 %. The last capture is clean. |

- **Reading.** The rule separates the batches that wait on work outside themselves from those that
  produce their own labels, in all three titles: the batches whose decode is released for a
  host-produced value are all kept serial, the robot platformer boots, and the card title boots with
  every batch early (its 2026-10-23 failure under an unbounded early return does not reproduce at
  depth one). The sandbox title's gain is the earlier probe's again, and it exists only with the drain
  skip, which is unsafe.
- **Before B1 is built.** In the sandbox title, B0.2 found about 5,600 vertex and 5-14 index
  entries per run that change one batch ahead, writer unknown; with the early return, decode reads
  them while the guest runs. One clean frame does not cover that. The writer decides the design: a
  host write needs no capture, a guest write must be copied before the return, at a cost to price.
  The real rule must use the command processor's own decision that a packet stores a value that
  satisfies the wait (reference and mask), not a parallel scan of addresses. An eligible batch that
  reaches a host-produced release must fail explicitly. The rule earns a unit test.
- **Before A3 is built.** Under the serial submit, every batch from the releasing thread is decoded
  before the release. With one batch in flight, its reads and label writes are not registered yet,
  so the drain skip's overlap test cannot see them, and both sandbox runs with the drain skip ran
  exactly that. The next run
  checks the in-flight batch's waits, releases and guest reads against the skipped ranges.
- **Claims.** Neither change alone gives the sandbox title a frame (the in-flight probe and the drain-skip run
  above), so the first code
  commit claims no frame rate. With both, the 6-9 ms of fence waits a frame are about the distance
  to 30 fps and come before C and D.

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

### B1 and A3 probes (2026-11-13)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, `7a7f9e6c8` plus temporary diagnostics (not
committed), strict runs, 60 fps present limit. `PPSA17221 - Minecraft` world window 100-140 s,
`PPSA21402 - Balatro` boot.

- **In-flight hazards of a skipped drain, Minecraft.** With the early return and the A3 ceiling
  probe (a release that overlaps no memory object, device-address range, label fence or display
  buffer skips the drain), 25,809 releases skipped the drain. Of 56.3 M reads and waits checked
  inside batches entered before a skip, 28.8 M had a skip in flight, and none touched a skipped
  range; 721,936 publication stores (64- and 32-bit labels, end-of-pipe clock, GDS publication)
  found none in the last 500 ms of skips. A live injection (every 256th skip, its range offered
  to the next check on the real path) was detected 52 of 64 times. Not hooked: the label
  `store_bytes` branch, the write-backs before label publication and the label callbacks; the
  store window has no injection. Balatro had no release that the probe skips, and the dunes route
  of `PPSA21567 - ASTRO BOT` has none, so the result is Minecraft only.
- **Where the drain goes.** Splitting the quiesce into ring idle (the command processors finish
  their queued batches) and device waits: serial submit 17.2 s of ring idle against 1.8 s of
  device waits over 11,206 quiesces (maximum 25 ms); with the early return 64.1 s against 1.8 s
  over 10,673 (maximum 317 ms). Skipping only the device wait saves little; an A3 with a gain must
  not wait for the queued batches, which is the variant whose read hazards are only measured.
- **The early return alone.** 21.41 fps against 20.53 serial (5,438 and 5,439 draws per frame,
  one run each, inside noise); frame time median 44.0 against 48.1 ms, 95th percentile 67.8
  against 53.1 ms. The early return moves the wait into each release, which now waits for the
  whole queue. B1 is not an improvement without an A3 that skips the queued batches.
