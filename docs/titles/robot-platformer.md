# Robot platformer

Title: PPSA21567 - ASTRO BOT.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Robot platformer: BVH fetch merge and loop-enclosing selection cut drop 0x3666fa39 from 16.4 to 6.0 ms and lift the dunes cliff to 31.9-33.6 fps (2026-11-18, fixed)

Intel Arc A770, Mesa 26.3.0 (`git-34ad0a45aa`) on Linux 7.3.4, `60 fps`
present limit, `KYTY_PRINTF_DIRECTION=Silent`, `origin/main` `87bbe5570` +
translator changes (`kShaderTranslatorVersion = 231`).

- **Baseline (`87bbe5570`, `csprof-base`, warm cache, temporary Vulkan compute
  timestamps).** In the dunes rock-cliff scene (`19.72 fps`
  mean across `145-216 s`), compute work spanned `43.6-48.3 ms` per frame.
  Dispatch `0x3666fa39` (`8160x1x1`, 1 call/frame) alone took `16.23-17.77 ms`
  (`16.36 ms` mean in the in-run A/B baseline arm).
- **Changes (general translator path, no title branches).**
  1. `Spirv::EmitGuestLoad64Aligned` (`ShaderSpirvGuestAddress.cpp`) + BVH node
     load rewrite (`ShaderSpirvBvh.cpp`): loads 64B-aligned BVH node words as
     `OpLoad %v4uint ... Aligned 4` (16 dwords direct without page-cross
     checks; 28 dwords with a single `tail_device` check at
     `(guest & 4095) + 64 >= 4096`), branches on `exec != 0` before loading
     arguments, splits `box16` / `box32` handling with per-child unpacks, and
     sinks loop-invariant operands into the case arms. Backed by
     `EXIT_IF((table.device & 15ull) != 0ull)` on GDA table allocation
     (`GuestDeviceAddress.cpp`).
  2. `SpirvCutSelectionLiveRanges` (`ShaderSpirvSelectionCut.cpp`): cuts live
     ranges of loop-invariant scalar `Function` variables (must-initialized across
     reachable predecessors via `OpStore` or `OpFunctionCall`) before and after
     the highest-scoring loop-enclosing one-armed selection merge when
     `device_address_used && push_constant_size > 0`, `total_fn_vars >= 60`,
     `body_blocks >= 100` enclosing at least one `OpLoopMerge`, and at least 32
     parked scalars exist, using a runtime zero derived from
     `(%vsharp[0][device_address_block][0] & 15u)`.
- **Offline cross-recipe verification and exclusions.** Global unrestricted live-range
  cuts regress other workloads (`30/91`), and reading `NumWorkgroups.y >> 16` for the
  runtime zero was excluded in favour of `(%vsharp[0][gda_blk][0] & 15u)`. Across
  all 16 `PPSA21567` BVH compute shaders, `16/16` dropped peak live registers
  (`-35` to `-71`) and `-111` `SEND`s and `14/16` dropped spills+fills (`0x3666fa39`:
  `392:1663 -> 282:1049`, scratch `8192 -> 4096 B`, peak live `245`). Across 431
  compute recipes from 26 titles, only one non-BVH shader (`PPSA17221`, without
  GDA) matched the structural thresholds and is skipped by `device_address_used`.
- **Verified.**
  - Live Vulkan timestamps (`t231-prof`, temporary profiler removed afterwards):
    `0x3666fa39` (`8160x1x1`) dropped from `16.23-17.77 ms` to `5.76-6.19 ms`
    per frame; A/B arm `arm=3` pixel diff vs baseline in the same cliff frame was
    `mean_abs <= 0.35`, `p99 = 1.00`, preserving direct and bounce lighting.
  - Strict warm-cache run without diagnostics (`strict-t231`, `214 s`, exit `0`):
    sustained `32.6-34.6 fps` (`33.63 fps` steady across `t=164-209 s` outside
    synchronous manual capture frames; `31.88 fps` mean over `170-215 s`), with
    `3/3` healthy captures (`entropy 4.70-4.72`, `stripey: false`).
  - Gates & cross-title checks on the final source: CTest `5/5` passed
    (`KytyUnitTest`, `KytyHostShaders.CheckedInIncludes`,
    `KytyArchitectureBoundaryIntegration.Repository`,
    `KytyArchiveDagIntegration.Repository`, `KytyIntegration._build`);
    `kyty_unit_test`: `2724` passed, `11` skipped (`CoreVirtualMemory` x3,
    `EmulatorAudio` x6, `EmulatorAudioDecoderAdmission` x1,
    `EmulatorKernelLifetimes` x1), `0` failed, including 2 new tests in
    `EmulatorShaderSpirvSelectionCut`. `kyty_agent capture-run` exited `0` on
    both `PPSA21567 - ASTRO BOT` and `PPSA17221 - Minecraft`; `kyty_agent
    playable-regression` passed `11/14` non-external gates on both (`exit=3` due
    to absent external baseline/checkpoint JSON), and `PPSA17221` scored
    `material_healthy: true`.
- **Remaining gap / open items.** At ~33 fps (~30 ms/frame), closing the gap to
  `60 fps` (`16.67 ms`) requires non-BVH GPU pass work and command-processor cost
  reduction; in close-up dune views compute shader `0x874bd13f` (`120x68x1`)
  reaches `10.4-12.5 ms`. Untested on AMD RADV and Windows.

## Robot platformer: progress writes to save-data memory were dropped (2026-11-16, fixed)

- **Observed (live trace, late dunes route).** Every `sceSaveDataSetSaveDataMemory2` call passed a valid
  data pointer with an entry count of zero, and plausible ranges (13397 bytes at 0, about 3.3 KB at 393216,
  148267 bytes at 0, and others). Kyty took the count literally and wrote nothing, so the 2 MiB slot stayed
  zero and progress could not survive a restart.
- **Fix (`36b94dd3f`).** A zero count with a data pointer writes that one entry; the getter already reads a
  single entry without a count. The save-data unit test writes through a zero count and reads it back.
- **Verified.** After the same route the slot file holds 30095 non-zero bytes. Not checked: that the title
  restores the progress on the next boot.

## Robot platformer: the studio intro video was black, a UNORM target sampled as sRGB (2026-11-16, fixed)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, strict runs of 14-18 s from boot.

- **Observed.** After the publisher card the title plays its 8.5 s studio video (3840x2160 H.264); Kyty
  presented black or the background instead. A video in another title plays, so the decoder was a lead only.
- **Localized, stage by stage, with temporary prints.** The player calls, events and `GetVideoDataEx` frames
  are correct (NV12, luma means 17-61). The guest copies each frame with `DMA_DATA` into two ping-pong
  buffers; the copy matches its source. The copy is sampled as a linear R8 texture that is updated every
  frame, and the YUV-to-RGB pixel program (image samples with non-sequential addresses) reads it correctly
  into a 3840x2160 `R8G8B8A8_UNORM` render target. The next pass samples that target's address as
  `8_8_8_8_SRGB` (tile 27, same extent): `Gen5PickSampleSurfaceAliases` rejected the target because its
  format differs only in gamma, and the sample got a separate texture that skips its guest upload
  (`guest-skip-live-cover`), so it read transparent black.
- **Causal check.** Binding the rejected target for that sample (wrong gamma, behind a temporary variable)
  showed the video, too bright; without it the frames were black.
- **Fix (`13fafeac5`).** Render targets whose format has an 8-bit gamma twin (`R8G8B8A8`/`B8G8R8A8`
  UNORM and sRGB) are created mutable with a two-format list (that pair only, so the driver keeps colour
  compression). A sampled descriptor rejected only by gamma reads a mutable target whose twin is the sample
  format, at the exact extent with every viewed level and layer, through a sampled view in the twin format
  (`Gen5PickGammaTwinSurfaceAlias`); red/blue-exchanged targets compose their selectors as before. Storage
  descriptors keep the old path.
- **Verified.** Captures at 12-14 s match frames extracted from the source video in content and tone.
  On the late dunes route a temporary counter on the new branch fired only for the video target and only
  in the first 10 s. `PPSA17221 - Minecraft` world route 80-108 s: 30.35 fps against 30.31 before, 0 errors.
  Unit suite 2703 tests pass and the three repository gates pass. A validation-layer intro run (layer
  confirmed loaded) reported no image, view or format-list error; it reported two existing dispatch errors,
  recorded below.
- **Not verified.** There is no same-scene frame-rate comparison for this title: the 150 s route reaches
  different scenes from run to run (tunnel, space, dunes). AMD/RADV is untested; window creation failed on
  that driver before graphics initialization, so this is a host problem.
- **Open, found on the way.** One of four 150 s route runs captured an all-black frame (RGB 0) at frame 3211,
  with the title steady at 18.6 fps in the dunes. Three other runs, with captures every 4-10 s, showed none.
  The new alias branch was not active then, so the cause is unexplained (a fade on a diverged route is
  possible). The validation layer reports, during the intro, a storage image view `R16G16B16A16_SFLOAT` used
  by an atomic (`VUID-vkCmdDispatch-None-02691`) and an `R8_UNORM` view bound to a descriptor that requires
  UINT (`VUID-vkCmdDispatch-format-07753`), 20 times each. Neither was investigated.

## Robot platformer: unpatched frame rate by scene, the transient command-bound dip, and a replay-worker poll A/B (2026-11-15, measured)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, silent console, late dunes route from the
world-map slot, `1f28471be` source (later commits are documentation), no game patch unless stated.
The wall-clock route reaches different scenes from run to run; each figure names its run and
window.

- **Unpatched frame rate depends on the scene.** a390 170-210 s: 25.5 fps at 349 draws per frame,
  engine 94 % busy. a391 and a392 150-215 s: 20-22 fps at 510-610 draws, engine 90-96 % busy.
  The 19-20 fps quoted in earlier records belongs to their scenes (525-772 draws). No unpatched
  window reached 30 fps.
- **A transient dip set by command processing.** a390 150-158 s (1080p) and a385 150-159 s
  (960x540 patch) drop to 13-17 fps with the engine 56-69 % busy. Draws per second stay at about
  13,000-14,500 while the cost per draw rises from about 29 us to 54-63 us; it falls back over
  5-6 s as the scene settles. Every stage of a draw grows (a390, per draw, dip against 170-210 s:
  state setup 20.2 against 9.2 us, resource binding 18.8 against 12.1, pipeline setup 5.6 against
  2.1, emission 4.6 against 2.4). Two counters move with it: guest-address preparations that
  re-query physical population (64-71 % of draws against 16-20 %; they run when an ordering point
  or a host write intervened since the previous preparation), and compute-pipeline lookups (706
  per frame against 105, with 104 dispatches in both), which points at draws that carry a compute
  geometry front. Neither is shown to be the cause: no profile of the dip exists, because a391
  and a392 never reached it (no window above 800 draws per frame). Its ceiling at about 29 us per
  draw would be about 24 fps, not 30.
- **Stops to zero frames per second.** a389 (960x540 patch) 208-210 s: 0.5-0.7 s of pipeline
  creation and 0.24-0.29 s of SPIR-V work in each second, at an area change. a388 (probe build,
  960x540 patch) 176, 181 and 183 s: command runs of 0.73, 1.96 and 0.85 s with under 0.5 ms of
  pipeline creation; what they waited on is not established.
- **Replay-worker poll, in-run A/B** (a390, temporary probe `diag-poll-20261115.patch`: the
  worker's 200 us poll window off in odd 4 s wall-clock blocks, on in even ones; a snapshot counts
  only inside one block). Steady scene 165-212 s: 25.46 against 25.42 fps, 28.6 against 28.9 us
  per draw, engine 94 % in both, worker 33.7 % against 3.8 % of a core. In the dip (150-164 s,
  five snapshots per arm, 866 against 900 draws per frame) poll off read 56.8 against 51.5 us per
  draw and 15.1 against 16.8 fps. The dip result rests on too few, unequal snapshots: a cost of
  the poll's removal is not established and neither is its absence. Not shipped.
  **Excluded (2026-11-15, m472):** the same probe on the PPSA17221 - Minecraft world route (about
  5,450 draws per frame, 20 and 21 one-second samples inside single 4 s blocks) read 30.5 fps with
  the poll and 23.5 fps without it (5.8 against 7.3 us of command processing per draw). The poll's
  half core buys about 7 fps on a dense draw stream; removing it is not a resource fix.
- **960x540 patch, fourth run** (a389, 166-215 s): 25.4 fps; 20-28 fps over 150-192 s at 330-480
  draws, 41-53 fps at 195-201 s with 82-254 draws per frame before a load. The 37-41 fps of the
  older record (469 draws, `6a6a5818a`) was not reproduced; its cause is not established.
- **Host render-target scaling, unpatched** (a394, `Fixed`, `RenderTargets`, 1280x720, `Fsr`):
  23.0 fps over 60-215 s, the same by scene as unscaled runs (21 fps at 550-700 draws, 25 at about
  370, a dip to 16 at 836-885), engine 83-95 % busy. The status reports the display cohort scaled
  at 1/3 of the 3840x2160 display; no counter shows any offscreen target scaled. This repeats the
  2026-11-02 exclusion below: the scope plans only targets of the display extent, and the main
  passes and the lighting kernel's grid keep the guest's 1920x1080. A presentation filter cannot
  change guest work, so neither `Fsr` nor `Fsr3` is a frame-rate lever here. The bound for any cut
  of internal resolution is the guest patch, which misses 30 fps in the dunes (27.8) and the
  close-up (23.4) at 960x540.
- **Tools** (evidence directory, outside the repository): `pollab.py`, `perf_on_draws.py`
  (enables the profiler on draws per frame instead of the clock).

## Robot platformer: at 960x540 the draw ring's cycle is the frame; the scheduler holds 0.2 ms of it (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, silent console, late dunes route from the
world-map slot, the user-supplied game patch that sets a 1920x1080 output and a 960x540 internal
resolution (same terms as the record below: not a strict run, no compatibility claim, picture not
compared). The wall-clock route lands in a different scene from run to run, so each row names its
scene from the run's captures. Three runs, one instrument each:

- Run a385, `1f28471be` built clean: once a second, `/proc/<pid>/task/<tid>/schedstat` of every
  task (on-CPU time, run-queue wait, timeslices) and 18 `kyty_agent threads` snapshots 2 s apart.
- Run a386, same build started inside the user-space sampling profiler, which samples only in
  190-194 s (199 Hz, stacks): it names the host threads by thread id, because every task reports
  the same name. Every a386 figure below comes from that wrapped run, read outside those seconds.
- Run a388, the same source with a temporary per-second bill of each graphics ring thread (wait for
  the next batch, submit-to-dequeue latency, run-lock wait, batch body in wall and thread CPU time,
  suspended-run waits, submission waits). The host was not quiet after 174 s of that run (all tasks
  waited 48 ms per frame for a core against 17 ms in a386, and a draw cost 48 us against 28), so
  only 153-171 s is quoted as figures. The scheduler samples start at 168 s: the host is known
  quiet for 168-171 s only (18.6 ms per frame over all tasks); before that the claim rests on a
  flat frame rate. A run before it stopped at 161 s on the title's own abort at hub load and gave
  no window.

| Run, seconds | Scene (draws per frame) | fps | Frame ms | GPU work ms/frame | Engine busy | Command runs ms/frame, all rings |
| --- | --- | --- | --- | --- | --- | --- |
| a385, 176-206 | free play beside a rock (357) | 33.9 | 29.5 | 23.0 | 78.2 % | 28.6, 4.6 in fence waits |
| a386 (profiler-wrapped), 196-214 | dunes with the tutorial prompt (758) | 27.8 | 35.9 | 29.9 | 83.3 % | 34.1, 1.5 in fence waits |
| a386 (profiler-wrapped), 151-177 | close-up landing (392) | 23.4 | 42.7 | 37.2 | 87.0 % | 36.7, 12.2 in fence waits |
| a388 (probe build), 153-171 | close-up landing (386) | 23.6 | 42.3 | 37.4 | 88.4 % | 34.7, 15.8 in fence waits |

- **The 960x540 internal resolution does not hold 30 fps on this route.** The dunes scene gains
  about 1 fps over 1280x720 (27.8 against 26.2-27.2 in the record below) and the close-up landing
  scene still spends 37.2-37.4 ms of GPU work per frame in two runs (40.3 at 1280x720, 50.8
  unpatched). The 37-41 fps recorded for this patch earlier on 2026-11-14 is another scene (walking
  the dunes, 469 draws per frame) on an older commit (`6a6a5818a`) and was not reproduced: the
  first row has fewer draws and a lower frame rate, and that gap is unexplained.
- **Host threads in the dunes scene** (a386, the profiler-wrapped run, 197-211 s, 28.0 fps, frame
  35.7 ms; names from the profile stacks):

  | Thread | CPU ms/frame | Run-queue wait ms/frame | Share of a core | Timeslices per frame |
  | --- | --- | --- | --- | --- |
  | Graphics ring thread that carries the draws | 28.19 | 0.17 | 79.0 % | 122 |
  | Deferred replay worker | 17.89 | 0.09 | 50.1 % | 18 |
  | Draw-front thread | 10.47 | 0.13 | 29.3 % | 224 |
  | Guest thread that yields in a loop | 4.27 | 0.17 | 12.0 % | 7 |
  | Graphics ring thread that carries the dispatches | 3.93 | 0.35 | 11.0 % | 339 |
  | Guest job workers, each of 11 | 1.54 | 0.71-0.83 | 4.3 % | 49 |
  | Guest thread that submits command buffers | 1.52 | 0.15 | 4.3 % | 12 |

  The draw ring thread waits 0.17 ms per frame for a core: the scheduler is not what it waits on.
  It is off the CPU 7.3 ms of each 35.7 ms frame; the wait counters hold 3.2 ms of that (fence
  1.50, render lock 0.83, WAIT_REG_MEM 0.48, flip done 0.37; the counters do not say which ring).
- **The draw ring's cycle** (a388, 153-171 s, frame 42.3 ms; four graphics ring threads exist, the
  one with the draws takes 5 batches per frame):

  | Per frame | ms |
  | --- | --- |
  | No batch queued, waiting for the next submit (0.45 ms of it on the CPU, publishing completions) | 2.71 |
  | Waiting for the run lock | 0.00 |
  | Batch bodies, wall | 39.45 |
  | - on the CPU | 18.97 |
  | - blocked | 20.48 |
  | - - of which waits of a suspended run for a value not yet written | 7.72 |
  | Cycle | 42.16 |

  A batch waits 4.1 ms on average between its submit and its dequeue, so the guest submits ahead of
  the ring. In the disturbed window of the same run (196-212 s, 708 draws, 20.2 fps) the shape is
  the same with the CPU share larger: no batch queued 3.47 ms, bodies 45.89 ms of which 35.88 on
  the CPU and 1.42 in suspended-run waits, cycle 49.37 of 49.62 ms.
- **What this prices.** In these runs the draw ring has no batch queued for 2.7-3.5 ms per frame;
  the rest of its cycle is batch bodies, where the thread either works (dunes: 28.2 of 35.7 ms) or
  is blocked (close-up landing: 20.5 of 42.3 ms). Inference, not measured on the same run: the
  6.6 ms that the earlier accounting left "outside the runs" (subsection "The command thread"
  below; unpatched resolution, lighting program left out, another scene) is this wait for the next
  batch plus waits between the runs of one batch. That accounting cannot be exact either way: its
  run counter adds the runs of every ring thread.
- **Guest side** (a385, 18 snapshots): the main thread was in a mutex wait in 10 and asleep in 7;
  the 11 job workers were idle on their semaphore in 16 or 17; three guest threads were never in a
  wait. `sync-waits` reported its tracking disabled and gave nothing.
- **Deferred replay worker.** 359 of its 374 profile samples are its poll loop (clock reads and the
  loop itself): half a core that replays almost nothing in this scene
  (`DeferredReplayWorker::Run`, `kPollWindow`, `GraphicsRenderCommandBuffer.cpp`). It is a resource
  cost, not a measured frame cost. A shorter window moves the cost to a wake-up on the draw ring
  thread for every hand-off; handing work off in larger pieces would let the worker sleep. Neither
  is built.
- **Not established.** What the guest does in the 2.7-3.5 ms before its next submit. Which ring
  holds each counted wait. The split of the 20.5 ms blocked in the close-up scene beyond its
  suspended-run waits. Each scene has one window of one run; a388 is a probe build.
- **Tools** (evidence directory, outside the repository): `schedsample.py`, `schedperf.py`,
  `ringbill.py`, `diag-ring-20261114.patch`.

## Robot platformer: a 1920x1080 output with a 1280x720 internal resolution removes under a third of the GPU work and still misses 30 fps (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, 60 fps present limit, silent console, `1f28471be` built clean (no
probe), late dunes route from the world-map slot, one run per patch, per-second performance
snapshots and the render engine's busy counters over the stated seconds. The guest resolution is
changed with a user-supplied game patch file that rewrites the title's own resolution table
(`docs/game-patches.md`). A run with a game patch is not a strict run and supports no compatibility
claim; the patch is not shipped and its picture was not compared against the unpatched one.

| Guest output / internal | Scene (draws per frame) | fps | Frame ms | GPU work ms/frame | Engine busy | Command runs ms/frame | us per draw |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 3840x2160 / 1920x1080 (unpatched), 196-213 s | dunes (772) | 19.1 | 52.3 | 48.0 | 91.7 % | 42.7, 6.4 in fence waits | 35.9 |
| 1920x1080 / 1280x720, 183-213 s | dunes (765) | 26.2 | 38.1 | 34.3 | 90.0 % | 33.4, 3.1 in fence waits | 27.8 |
| 3840x2160 / 1920x1080 (unpatched), 153-180 s | before the dunes (386) | 18.9 | 52.9 | 50.8 | 96.0 % | | |
| 1920x1080 / 1280x720, 150-174 s | before the dunes (390) | 22.7 | 44.1 | 40.3 | 91.3 % | | |
| 1920x1080 / 1920x1080, 150-215 s | another scene (550) | 18.6 | 53.7 | 46.2 | 86.0 % | 48.9, 17.0 in fence waits | |

- **Most of the GPU frame does not follow the internal resolution.** The patched rows change two
  things at once: the output from 3840x2160 to 1920x1080 and the internal resolution to 0.44 times
  the pixels. Together they take 13.7 ms off 48.0 ms in the dunes and 10.5 ms off 50.8 ms in the
  scene before them. A line through each pair leaves 22 ms and 32 ms of GPU work per frame at zero
  internal pixels (two points per scene, not a fit). Inference, not measured: 13.7 ms is what two
  lighting programs of about 25 ms would lose if they scaled with the pixel count while draws and
  the other dispatches stayed; only `0x3666fa39` was measured in a dunes scene (13.65 ms, record
  below), `0x874bd13f` was not.
- **The output resolution alone is not the cost.** With the output at 1920x1080 and the internal
  resolution unchanged the frame stays at 53.7 ms. The route landed in a different scene in that
  run, so the row excludes a large gain and prices nothing.
- **At 1280x720 both limits sit together.** The engine is still 90 % busy at 38.1 ms and the
  command thread runs 33.4 ms of the frame, so 30 fps is not held at that resolution either. An
  earlier run at 960x540 on `6a6a5818a` held 37-41 fps with the engine 78 % busy (record below); later
  runs at 960x540 held 33.9, 27.8 and 23.4-23.6 fps in three other scenes (record above).
- **Command work per draw is not constant.** The same draw count costs 35.9 us per draw at
  1920x1080 and 27.8 us at 1280x720, with every stage lower by a similar share (state setup 10.5
  against 8.2, binding 14.8 against 12.2). The cause is not established; the thread sleeps in fence
  waits for 6.4 ms against 3.1 ms per frame. Later check (2026-11-15, dunes route at 1920x1080): it is not
  the host slowing an idle thread. The busiest emulator thread ran at 4.5-5.1 GHz in every second, with no relation
  between its clock and the cost per draw (r = 0.23), nor with a busy SMT sibling or core migrations (|r| < 0.1).
  Within one draw band the cost per draw falls as the engine gets busier (r = -0.91 to -0.95 in four runs): the
  costly seconds are the ones where the command thread limits the GPU. In them every stage rises, most of all
  texture and storage descriptors and state setup, which points to the draws themselves. The check sampled only the
  emulator's three busiest threads; other host load was not measured.
- **Two further command-thread costs (2026-11-15, same run, agent counters).** Guest-address residency rises from
  about 0.9 ms to 37-65 ms per second in five slow seconds at an unchanged call count. Each such second has a
  longest single preparation of 13-19 ms against under 5 us in quiet seconds, so the time is a few long calls,
  not a repeated per-preparation cost; the timed span is the import of physical pages the title populates while it
  streams (page query, window export and import), and which part of a long call dominates was not measured. Memory-object
  lookups run about 10 calls per draw at a measured 0.6-1.4 us each, timing overhead included, in steady seconds
  (8-12 us per draw, 10-19 % of command-processor run time; the paying thread was not established). In steady
  seconds 90-93 % are cached reuses and the rest fast reuses; the slow ones are texture
  loads, whose staging cost db6d7c132 already reduced. Neither names a cause with a small fix; moving residency off
  the command thread or folding the per-draw lookups are larger changes, not started. The residency
  imports are timed in `cross-title.md` ("Physical windows are whole 64 MiB chunks", 2026-11-15 entry).
- **Colour compression is not the GPU cost (2026-11-15, weak exclusion).** One run with the driver's
  colour compression disabled (`INTEL_DEBUG=noccs`, passed through the inherited environment; the
  child's environment was not read) against one without, same binary, save and route, compared by
  draws per flip: 650 draws per flip 20.4 → 17.3 fps, 600 23.3 → 19.3, 250 32.7 → 29.0 (2-25
  seconds per band). Compression is a net gain on this scene, so its resolves are not where the GPU time
  goes. One run per arm.
- **Storage buffers rehashed on edge-only changes (2026-11-15, excluded as a fix).** Storage buffers of
  8 MiB and more at page-unaligned addresses were hashed in full 30-60 times a second because a
  neighbour wrote their shared first or last page; their interior pages were unchanged. A change that
  let the existing storage-edge hashes declare such a buffer clean cut storage hashing from 1,680 to
  355 ms over the run (48.8 to 9.4 GiB) with unchanged fps in light bands, but in the 525-700 draws
  per flip band command-buffer fence waits rose from about 1.7 to 6.3 per frame (50-150 to 250-335
  ms per second) while the GPU's busy share fell from 82-96 % to 67-82 %, and fps fell (650 draws per
  flip 20.4 → 14.3). A check run that hashed every edge-clean buffer in full anyway found 22,765
  mismatches in 28,000 decisions (decisions, not buffers: a skipped buffer mismatches again on every
  later visit), all on buffers whose content came from a device write-back and none on CPU uploads:
  their recorded hash predates the write-back, so the full hash was re-uploading the written-back
  bytes. That check run, slowed by its own hashing, reached only 750-800 draws per flip, where it
  again waited on fences 6-8 times per frame, but with the GPU 92-94 % busy. Why skipping those
  re-uploads adds fence waits is not established. The change was reverted. Direction: a narrower rule
  needs its own measurement of the write-back path.
- **Short dips on the dunes route (2026-11-15, cause open).** Two classes:
  - Scene-linked: two runs fall to 9-15 fps with the GPU 40-56 % busy at 78-80 s, just before draws
    per flip fall to about 45; the 31-68 ms window exports recorded near 77-80 s
    ([cross-title](../bringup/cross-title.md)) are the lead. A third run reached that point about 9 s
    later and ended before it.
  - Unattributed: that third run fell to 9-17 fps at 56-67 s (GPU 39-75 % busy, few fence waits), where
    each per-draw phase took about twice as long and command emission about seven times as long (4 to
    29 us per draw). No scheduler samples cover it. Samples that did cover 72-86 s showed the
    emulator's threads waiting 2,400 ms a second to run at 74 s (300 normally), but fps was 28.7 then,
    and agent snapshots were being taken in that second.
  - Passive samples (five runs, per-second scheduler, pressure, disk and process IO; one run with a
    user-space CPU profile). In the one clean run the 78 s dip (12 fps) coincides with a burst of
    first-use texture creation on the command thread: 111 creates, `upload_ns` 215 ms (27 ms for one upload),
    `guest_address_residency_ns` 86 ms, `detile_ns` 54 ms, `draw_descriptor_texture_ns` 353 ms,
    `command_processor_run_max_ns` 291 ms, fence waits 19 ms, and GPU busy cycles fall to about a
    fifth. The command thread's user-space samples stay at about 540 a second in steady and dip
    windows; in the dip they move to detile, memcpy, XXH3 hashing and block creation. Whether the
    rest of `upload_ns` is kernel time (first-touch faults on new staging) or a wait for in-flight
    fills is not measured, and the three counters above are not shown to be disjoint.
  - A later clean run repeats the dip at 78 s (10 fps) and its slow-frame records give the split per
    frame: the 484 ms frame carries 97 ms of GPU memory creation and 50 ms of upload, the next
    (208 ms) frame 10 ms of creation; the command thread's longest run is 243 ms. Creation is under
    half of the lost time, so the dip stays open. The slow-create records (tagged
    `temporal_not_causal`) show 22 MB textures taking 6-16 ms each: upload 1.5-11 ms, Vulkan
    allocation 0.2-8 ms, hashing about 0.8 ms. The host sampler's seconds and the snapshot seconds
    differ by one or two, so per-thread fault spikes are not paired with the dip second.
  - Excluded for these dips: swap activity, cgroup memory events (all zero) and process block reads
    in the dip second (0 MB at 78 s; reads of 0.4-0.9 GB/s come one to three seconds later).
  - Guest file IO runs synchronously on the calling thread: `SubmitCommandBuffer`
    (`Libs/LibAmpr.cpp:786`) executes its reads inline (`ExecuteActions`, line 812; `ExecuteRead`,
    line 671, 4 MiB chunks), the kernel AIO submit (`Kernel/Aio.cpp:66`) runs each request inline and
    reports it completed, and `KernelPread` (`Kernel/FileSystem.cpp:1587`) holds the file mutex. A
    temporary probe measured up to 300-700 ms a second of such blocking per streaming thread at about
    0.9 GB/s. The blocking did not line up with the dips (a 698 ms block at 77 s came with no dip), so
    it is not shown to cause them. The install volume is a FUSE NTFS mount: 1.16 GB/s cold serial,
    0.70 GB/s cold with eight readers, 10.6 GB/s from page cache; no claim that it causes the dips.
  - Open: a 0 fps freeze at 104-108 s in two runs with a foreign build loading the host (IO
    pressure 270-525), not repeated in three later runs.
  - Package stats: from about 30 s to 90 s the guest calls `KernelStat` on `/app0` files 700-1,000
    times a second, and the FUSE daemon serving the install uses 525-570 ms of CPU a second in the
    runs measured with and without the change below. Early on, 65-90 % of the paths repeat; later
    the calls are about 700 distinct absent paths a second. `KernelStat` now takes the package fast
    path that APR resolution already used (an exactly spelled regular file below `/app0` with no
    patch directory: a status, a size and a time lookup, plus an existence check for `.odx` paths),
    which answered 20-50 % of the early calls. No effect on stat rate, CPU or fps was measured: the
    stat rate varies between runs of each arm more than between arms. Open:
    an absent path still runs the full resolve (patch, extension alias, data segment, companion and
    font candidates, each a host lookup); answering it fast needs a cached, bounded index of the
    read-only package that proves every candidate absent.
  A dip on a shared host needs passive scheduler and pressure samples over the whole run before it is
  attributed.
- **Standing against the 30 fps target at the unpatched resolution.** The frame needs about 17 ms
  less GPU work, and the command thread, which runs 33.4 ms for the same draw count once the GPU
  stops limiting it (second row), has no margin left under 33.3 ms. The measured GPU levers are the
  range split of the lighting program (5.9 ms, probe only, fails its sweep) and nothing else above 1
  ms; no general rule recorded so far closes the gap on this GPU.
- **Runs.** The first attempt with the 1280x720 patch exited with status 139 at 144 s in the hub
  load, a fault in guest code; whether it is the hub-start assert recorded on 2026-10-30 was not
  checked. The repeat ran clean.

## Robot platformer: the loop-boundary range split fails the cross-recipe sweep and stays a probe result (2026-11-14, offline)

Intel Arc A770, Mesa ANV on Xe (Mesa 26.3 development build), SIMD8, driver shader cache disabled. No
title runs: every stored compute recipe whose entry function holds a loop was compiled as translated
and again after the rewrite of the record below, applied as a rule: at every outermost loop of the
entry function, in the block that enters the loop (when it has a single one that only falls into the
header) and at the start of the loop's merge block, every scalar function variable is stored back as
itself plus a run-time zero. The gate was that no recipe gets worse. It fails, so the rule is not
written into the translator.

| Store | Recipes with a loop | Fewer spills + fills | Same | More |
| --- | --- | --- | --- | --- |
| PPSA21567 - ASTRO BOT | 60 | 36 | 9 | 15 |
| PPSA03969 - F.I.S.T.: Forged In Shadow Torch | 9 | 0 | 3 | 6 |
| PPSA07393 - Gori: Cuddly Carnage | 9 | 1 | 3 | 5 |
| PPSA01341 - Demon's Souls | 5 | 0 | 4 | 1 |
| PPSA17221 - Minecraft | 4 | 0 | 3 | 1 |
| PPSA09809 - OCTOPATH TRAVELER II | 3 | 0 | 1 | 2 |
| PPSA04220 - JoJo's Bizarre Adventure: All-Star Battle R | 1 | 0 | 1 | 0 |
| All | 91 | 37 | 24 | 30 |

- **The gains sit in this title.** 13 of the 36 gains in this title's store are compilations of
  the lighting program itself (the same module shape, including the arms the probe runs stored);
  the 60 rows hold 46 distinct module shapes. Outside this title the rule gains once and loses 15
  times.
- **Losses are not marginal.** Spills:fills go 104:321 -> 597:1184 on a Minecraft program,
  53:161 -> 93:1224 on a Demon's Souls program, 622:1593 -> 1380:2236 and 2901:9982 -> 4848:14221
  on two other programs of this title, and 0:0 -> 41:107 on a Gori: Cuddly Carnage program that
  did not spill at all.
- **No narrower form passes.** Over nine losing and five gaining recipes, three narrower forms were
  compiled: the exit only, only the registers the loop never references, only the registers the loop
  never writes. They make 6, 5 and 7 of the 14 worse; four recipes get worse under every form.
- **No structural property separates the two groups.** The share of the entry function before,
  inside and after its outermost loops, the number of outermost loops and the number of registers
  written inside them overlap between gains and losses (a program with 23 % / 51 % / 24 % loses, one
  with 32 % / 52 % / 14 % gains).
- **The driver's cycle estimate cannot choose.** For the lighting program it reads 1.536e9 as
  translated, 1.574e9 with the split that measured 5.9 ms faster, and 0.808e9 for the parked form
  that the statistics of the record below exclude.
- **What stays open.** Choosing per pipeline from the spill and fill counts the driver reports for
  both compilations would never pick a worse count, at the price of compiling such programs twice.
  It is not built: the only evidence that the static count tracks GPU time is three compilations of
  this one program (1634, 938 and 214 fills against 13.65, 7.75 and 3.3 ms per frame).
- **Tools** (evidence directory, never in the repository): `splitloops.py` (the rule and its
  narrower forms on a SPIR-V listing), `sweep_split.py` (the before/after table over recipe stores),
  `loopshape.py` (where the entry function's code sits relative to its loops).

### The command thread: wait counters hold 1.2 ms, 6.6 ms outside its runs is unpriced

A later record the same day ("at 960x540 the draw ring's cycle is the frame", above) measures the
draw ring's cycle directly, in 960x540 runs of other scenes; that it accounts for this 6.6 ms is
an inference.

The per-second performance snapshots of run a379 (record below) were mapped to the arm that was
live, keeping only snapshots that lie inside one 2 s block (`armperf.py`; block edges from the
probe's lines, shifted by the sub-second offset that best matches flips to presents, mean error
1.6 fps). In the second scene of that run (blocks 56-67, three snapshots per arm, about 630 draws
and 105 dispatches per frame):

| Per frame, lighting program left out | ms |
| --- | --- |
| Frame | 38.3 |
| Command processor runs | 31.7 |
| Draw processing (632 calls, 33 us each) | 21.1 |
| - resource binding (texture descriptors 4.7, storage 1.5, finalize 1.5) | 8.4 |
| - draw state setup | 6.8 |
| - pipeline setup, emission, materialization | 3.9 |
| Dispatch processing (105 calls, 57 us each) | 6.0 |
| Fence waits | 0.84 |
| WAIT_REG_MEM | 0.35 |
| Flip-done waits | 0.04 |

The wait counters hold 1.2 ms of the frame, and fence waits are timed inside the runs. That leaves
6.6 ms per frame outside the runs and outside every wait counter (38.3 - 31.7), which matches the
75-80 % of a core the thread used in the causal test of 2026-11-14. That time is not priced: it may
be the gap between a guest submit and the run that serves it (about 20 runs per frame), run-queue
delay, or the guest's own pace. With the program as translated the same counters read 33.2 ms of
runs and 4.9 ms of fence waits in a 43.9 ms frame. Three snapshots per arm on a probe build; the
figures size the parts, they are not a benchmark.

## Robot platformer: splitting register ranges at the light loop recovers 5.9 ms of GPU time (2026-11-14, probe only)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot,
`d25d2e373` plus the temporary probe of the record below (not committed), four arms in 2 s blocks in
one run. The fourth arm runs the translated module of `0x3666fa39` after an offline rewrite of its
SPIR-V: in the block that enters the light loop and in the loop's merge block, every scalar function
variable (a guest register) is stored back as itself plus a run-time zero. The rewrite changes no
result of the program. No translator change is committed; this record prices the rule.

| Arm (blocks 4-48, 11-12 per arm) | Engine ms/frame | Frame ms | Against full |
| --- | --- | --- | --- |
| Program as translated | 50.69 | 53.98 | |
| Program left out | 37.04 | 41.70 | -13.65 |
| Trace region folded out | 40.36 | 43.19 | -10.33 |
| Ranges split at the light loop | 44.79 | 46.71 | -5.90 |

The split arm is -4.66 in blocks 4-27 and -6.23 in blocks 28-48, and -5.45 (3 blocks per arm) in the
scene the route reaches in blocks 56-67. Blocks 49-55 and 68 onwards hold a scene change and are
left out.

Driver statistics of the same module variants, compiled offline from the stored recipe (method of
the record below, SIMD8); fills are counted before, inside and after the light loop in the
generated code:

| Variant | Instructions | Spills:Fills | Max live | Fills before / inside / after |
| --- | --- | --- | --- | --- |
| As translated | 34,087 | 390:1634 | 283 | 413 / 666 / 555 |
| Trace region folded out | 12,149 | 86:214 | 168 | 142 / 6 / 66 |
| Split at the loop exit | 31,034 | 280:1139 | 270 | 348 / 646 / 145 |
| Split at the loop entry | 32,651 | 340:1305 | 264 | not counted |
| Split at entry and exit (the arm above) | 30,423 | 253:938 | 262 | 222 / 577 / 139 |
| Same, every variable without a liveness test | 30,423 | 253:938 | 262 | same module statistics |
| Parked in scratch, one index base (19 variables) | 32,704 | 357:1393 | 275 | 399 / 457 / 537 |
| Parked in scratch, separate reload base | 33,345 | 354:1491 | 259 | 342 / 611 / 538 |
| Parked with constant indices (control) | 34,087 | 390:1634 | 283 | as translated |
| Split at every loop | 43,837 | 1322:2687 | 671 | 399 / 2105 / 183 |
| Split at every loop and selection holding a loop | 61,708 | 2793:4708 | 1361 | 940 / 3528 / 240 |
| Entry and exit, plus copies of read-only registers after the trace region (9) | 30,306 | 254:921 | 262 | 221 / 563 / 137 |
| Same, plus copies at the top of the loop body (27) | 31,345 | 285:1108 | 263 | 223 / 572 / 313 |

- **What the rewrite does.** `V = V + z`, with `z` a value the driver cannot fold (the probe uses
  `NumWorkgroups.y >> 16`, which is zero for any dispatch below 65,536 groups in y), in bit form for
  float registers. The value after the boundary is a new value for the driver. The record below
  measured that the driver counts one register range per value (283 against an exact 243), so a
  register written before the loop and read after it holds one range across the loop; the split
  ends that range at the boundary.
- **Engaged.** The run log reports the fourth arm's module loaded from the offline file (200,194
  words against 196,783) and the arm differs from full in every window.
- **Where it acts.** Fills after the loop fall from 555 to 139 and before it from 413 to 222; inside
  the loop they stay (666 to 577). The 5.9 ms is close to the about 7 ms the record below assigns to
  the code before and after the light loop; the about 4.5 ms it assigns to the loop itself remain.
- **No liveness needed.** Rewriting all 267 scalar variables at both boundaries gives the same
  module statistics as rewriting only the 98 and 88 live ones: the driver drops the dead ones.
- **Excluded without a run: parking in per-thread scratch.** Only 19 variables are untouched by the
  loop and live after it (the driver's 44 loop-through values include its own subexpressions). With
  one index base the driver keeps 17 slot addresses in registers across the loop; with a separate
  reload base the maximum falls to 259 but fills stay at 1491 and the fills after the loop do not
  move (538). With constant indices the driver folds the array back into values (identical
  statistics), so a run-time index is what makes a scratch array stick.
- **Excluded without a run: splitting inside the loop.** A split at every loop, or at every loop and
  every selection holding a loop, multiplies registers and fills (maximum 671 and 1361). A value
  rewritten inside a loop becomes one more range across that loop.
- **Excluded without a run: copies of read-only registers.** Only 9 registers that the loop only
  reads have two or more reads after the trace region; copying them there changes nothing, and
  copies at the top of the loop body bring fills back after the loop.
- **Not verified.** Picture equality of the split arm against full (the rewrite is a no-op by
  construction; no capture was compared). The rule in the translator, other programs of this title,
  other titles and other drivers.
- **Tools** (evidence directory, not in the repository): the SPIR-V rewriter, the fill counter by
  loop position and the loop-through lister.

## Robot platformer: the trace region costs GPU time by being compiled in, not by running (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot,
`c5cf3199e` plus the temporary probe of the record below (not committed), six arms in 2 s blocks in
the same scene. Two more kinds of arm: every `image_bvh_intersect_ray` reports a miss without
loading a node, and a guest branch taken under a condition the compiler cannot fold (the count word
of the guest address table in the program's uniform block is not zero: true on every dispatch).

Run 1, medians of blocks 18 to 92, 12-13 per arm:

| Arm (instruction index) | Frame | Engine work a frame | Removed |
| --- | --- | --- | --- |
| As translated | 52.8 ms | 48.1 ms (47.2-49.3) | |
| Dispatch left out | 37.0 ms | 32.6 ms (32.1-33.6) | 15.4 ms |
| Trace region skipped, branch folded at compile time (1437) | 40.9 ms | 36.5 ms (36.2-39.6) | 11.6 ms |
| Every intersect reports a miss (1550, 1582, 1767) | 52.6 ms | 47.9 ms (46.5-48.6) | 0.2 ms |
| Bottom-level walk skipped, branch folded (1485) | 46.6 ms | 42.1 ms (41.2-42.9) | 6.0 ms |
| Trace region skipped under the run-time condition (1437) | 52.7 ms | 48.0 ms (47.0-49.5) | 0.0 ms |

Run 2, the control, medians of blocks 18 to 79, 10-11 per arm. The first half of the window is
about 4 ms a frame heavier in every arm; the differences hold within 1.7 ms in each half:

| Arm (instruction index) | Frame | Engine work a frame | Removed |
| --- | --- | --- | --- |
| As translated | 61.3 ms | 50.7 ms (45.7-54.1) | |
| Dispatch left out | 45.2 ms | 35.5 ms (30.1-37.3) | 15.2 ms |
| Trace region skipped, branch folded (1437) | 49.0 ms | 39.1 ms (33.6-41.4) | 11.6 ms |
| Trace region skipped under the run-time condition (1437) | 57.2 ms | 50.1 ms (45.0-53.6) | 0.5 ms |
| Leaves after the light cull under the run-time condition (250) | 44.5 ms | 35.8 ms (29.9-37.8) | 14.9 ms |
| Light loop never entered under the run-time condition (1361) | 51.8 ms | 45.6 ms (41.5-48.7) | 5.1 ms |

- **The run-time condition engages.** Leaving after the light cull under it recovers 14.9 of the
  15.2 ms of leaving the dispatch out, so the arms that use it do skip their region on every thread.
- **Running the region costs nothing measurable here.** Skipped at run time it returns 0.0 and
  0.5 ms; with every intersect a miss, 0.2 ms. The method resolves about 2 ms.
- **Having it in the compiled program costs 11.6 ms a frame**, in both runs: the same skip folded at
  compile time removes the region from the program and with it three quarters of the program's cost.
  Half the region (the bottom-level walk) removes half of that.
- **The cost lands on the rest of the program.** With the light loop never entered at run time
  5.1 ms come back, against 12.6 ms when the loop is folded out (record below). About 7 ms of the
  region's presence is paid in the code before and after the light loop, about 4.5 ms in the light
  loop outside the region.
- **Driver statistics of the same variants** (their stored compute recipes compiled offline,
  `VK_KHR_pipeline_executable_properties`, shader cache disabled, SIMD8):

  | Variant | Instructions | Loops | Spills:fills | Scratch a thread | Maximum live registers | Removed |
  | --- | --- | --- | --- | --- | --- | --- |
  | As translated | 34,087 | 137 | 390:1634 | 8,192 B | 283 | |
  | Trace region skipped at run time | 34,086 | 137 | 390:1634 | 8,192 B | 283 | 0.0-0.5 ms |
  | Every intersect a miss | 28,602 | 125 | 412:1390 | 8,192 B | 228 | 0.2 ms |
  | Bottom-level walk folded out | 23,255 | 88 | 271:1042 | 4,096 B | 242 | 6.0 ms |
  | Trace region folded out | 12,149 | 39 | 86:214 | 2,048 B | 168 | 11.6 ms |

  The region is 65 % of the compiled program for 22 % of the guest instructions. The GPU time follows
  the scratch traffic, not the instructions that run. By position in the assembly of the translated
  program, the first 12,000 instructions hold 487 scratch loads, the next 18,000 (the light loop)
  369 and the last 10,300 (the shading after the loop) 778; the variant without the region holds 214
  in all.
- **Why the rest of the program pays** (final NIR from `INTEL_DEBUG=cs`, exact CFG liveness, one
  register per value at SIMD8 against 128 registers):

  | Point | Live values |
  | --- | --- |
  | Before the light loop | 150 (31 uniform, 119 per-lane) |
  | Trace region entry | 164 |
  | Peak, inside the region | 243 (83 uniform, 160 per-lane) |
  | Trace region exit | 160 |
  | After the light loop | 112 |
  | Peak without the region | 158 (47 uniform, 111 per-lane) |

  At the peak 95 values live only inside the region, 95 pass through it untouched (18 uniform, 77
  per-lane; 44 of them are not touched by the whole light loop) and 51 are used on both sides. The
  pass-through values have 162 definitions and 1,900 uses outside the region, 268 of them inside
  the light loop. One continuous range per value peaks at 282, the driver reports 283: ranges with
  holes add about 40 to the exact figure, not the gap the 2026-11-12 record below claimed. Which
  values the allocator sends to scratch was not traced; 1,634 fills against 1,900 uses of values
  that only pass through is consistent with those values being loaded from scratch at every use.
- **Tool defect, earlier figures withdrawn.** The liveness script behind the 2026-11-12 table read
  successors only from the line that closes a block; an empty block prints them on its header line,
  so liveness never crossed an empty else arm (471 of 1,431 blocks). Fixed, it matches a hand count
  on a 60-instruction test program (10 values: 3 uniform, 7 per-lane). "Exact 126", "51 under
  exact" and "exact 102" in that record are wrong; see the correction there. The guest-listing pass
  of that record (122 registers) uses another script and was not re-checked.
- **Scratch is reachable from a translation.** A function-local array indexed by a run-time value
  compiles on this driver to scratch stores and loads (test program: a 96-element array becomes
  `store_scratch_intel` / `load_scratch_intel`, 384 bytes of scratch a thread). Not tried: keeping
  the pass-through values there across the region.
- **What it bounds.** 11.6 ms a frame is the ceiling of any translation change that stops the
  region from pushing other values out of registers. At that ceiling the frame is about 40 ms
  (25 fps) and the graphics command thread sets it (causal test below).
- **Not measured.** How often a thread enters the region in this scene; the same split for
  `0x874bd13f`; any other GPU or driver. The arms change the picture; no image was scored.
- **Discarded run.** One run left the dunes route for the opening cinematic; the program was not
  dispatched after block 40.

## Robot platformer: removing the trace region removes most of the lighting program's GPU time (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot,
`451e7d292` plus a temporary probe (not committed). Starting 35 s after the first dispatch of
`0x3666fa39` it rotates seven arms in 2 s blocks: the program as translated, its dispatch call left
out, and five translations of the same program, compiled outside the translation cache under their
own pipeline keys, in which one guest conditional branch is always taken. At every block change the
probe reads the present count and the process's render-engine cycles from its DRM client records,
which gives engine work per frame. 117 blocks ran; the figures are medians of blocks 14 to 96 (12 per
arm, the scene where the robot lies on the dune under the tutorial prompt), with the block range in
brackets. The last three cycles agree within 0.5 ms.

| Arm (instruction index of the forced branch) | Frame | Engine busy | Engine work a frame | Removed |
| --- | --- | --- | --- | --- |
| As translated | 51.4 ms | 89 % | 46.1 ms (45.2-47.3) | |
| Dispatch left out | 37.3 ms | 82 % | 30.5 ms (29.6-31.0) | 15.6 ms |
| Leaves after the tile's light cull (250) | 37.3 ms | 83 % | 30.9 ms (30.7-31.9) | 15.2 ms |
| Light loop never entered (1361) | 38.5 ms | 88 % | 33.5 ms (32.6-35.0) | 12.6 ms |
| Trace region skipped (1437) | 39.6 ms | 88 % | 34.7 ms (28.2-35.3) | 11.4 ms |
| Shading block skipped (2229) | 51.5 ms | 89 % | 45.8 ms (41.2-47.0) | 0.4 ms |
| Part of the shading block skipped (2236) | 50.1 ms | 89 % | 43.8 ms (41.3-44.8) | 2.3 ms |

- **Where the time goes away.** The light cull costs about 0.4 ms and, with the light loop folded
  out, everything outside it about 3 ms. Folding the light loop out removes 12.6 ms, and 11.4 ms of
  that goes with the span the guest itself skips when no lane of the wave traces: 665 of the
  program's 3,066 instructions, from the branch at index 1437 to its join. The record above shows
  that this is the cost of having the span in the compiled program, not of running it. The span holds the three `image_bvh_intersect_ray` instructions, the header loads that
  build the two BVH descriptors, the two-level walk (instances, then triangle lists) with its shared
  stack in lane reads and writes, and the use of the hit. The two shading-block arms disagree with
  each other (a part cannot cost more than the whole), so 2 ms is the resolution of this method: each
  arm is its own compilation, and the record of the doubled lookups (2026-11-14) saw register
  allocation alone move this program by 15 %.
- **Corrects the earlier price.** The forced-miss build of 2026-11-09 (23.6 against 24.7 ms a call)
  priced the node visits after the first one, not the trace: the header loads, the first visit and
  the lane exchanges ran in both arms. "A host-written traversal is worth about 1 ms" in the record
  below is withdrawn; removing the trace region from the compiled program removes 11.4 of the
  program's 15.6 ms in this scene.
- **What it bounds.** With the region left out the frame is 39.6 ms (25 fps), where the causal test
  below shows the graphics command thread setting the frame. 30 fps still needs both: engine work at
  or under about 30 ms a frame and 6 ms less command processing.
- **Not measured.** How many light iterations trace per tile and how many nodes a trace visits in
  this scene (the 2026-10-30 counts are from another window). The forced-miss arm and the split
  between running the span and compiling it in are in the record above. The arms change the
  picture (lighting on and off every cycle); no image was scored.
- **Discarded run.** The first attempt started the arms at a fixed 201 s. Boot was slow, the hub
  loaded during the first cycle, each first use of a variant stalled the command thread for 1-3 s
  while it compiled, and the run ended at 231 s on the known intermittent guest jump to address 0
  (`rip 0x9004a0c3e`). The probe was then anchored to the program's first dispatch.

## Robot platformer: the GPU limits the frame with the lighting program, command processing without it (2026-11-14, causal test)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot,
`fe853a27b` plus a temporary probe (not committed). From 201 s it rotates four arms in 3 s blocks:
the dispatch call of `0x3666fa39` issued or left out, crossed with a busy-wait of 10 us after every
draw on the graphics command thread (the probe counts the waits: 7.5-7.7 ms a frame). Figures are
block medians of blocks 16 to 31, four per arm, in the scene where the robot lies on the dune under
the tutorial prompt (about 752 draws a frame). The first 16 blocks include two at 10-15 fps whatever
the arm and are left out.

| Arm | Frame | Engine busy | Engine work a frame | Fence waits a frame |
| --- | --- | --- | --- | --- |
| Program runs | 52.6 ms | 88 % | 46.6 ms | 10.0 ms |
| Program runs, +7.6 ms on the command thread | 52.7 ms | 87 % | 46.4 ms | 5.9 ms |
| Program left out | 39.3 ms | 76 % | 30.1 ms | 1.5 ms |
| Program left out, +7.5 ms on the command thread | 46.9 ms | 65 % | 31.0 ms | 1.0 ms |

- **With the program the GPU sets the frame.** 7.6 ms more command processing changes nothing
  (k = 0.01 ms of frame per ms added): the command thread has at least that much slack and the engine
  is busy 46.6 ms of every 52.6 ms frame.
- **Without it the graphics command thread sets the frame.** Every millisecond added to it is a
  millisecond of frame (k = 1.02), although the thread uses only 75-80 % of one core.
- **Budget for 30 fps in this scene.** Both limits are above 33.3 ms: engine work has to fall from
  46.6 ms to about 30 ms (the program accounts for 16.5 ms of it), and the command path from 39.3 ms
  by 6 ms. Either alone leaves the frame above 33.3 ms.
- **Where the command path spends its time** (user-space CPU profile, 199 Hz for 20 s, program left
  out for the whole window, 42 ms frames; 1 % of the samples is about 1 ms a frame):
  - Graphics command thread, 31.6 %: indexed draws 18.8 % (descriptor binding 12.5 %, of which
    texture preparation 6.2 %, guest memory object lookup 4.6 %, storage buffers 3.4 %), handlers
    reached through nop packets 7.3 %, auto-index draws 5.8 %, dispatches 4.1 %, completed-submission
    publication 2.2 %. Shader input analysis repeated on every call is 3.2 % for pixel programs and
    1.6 % for compute programs. No single function is above 1.6 % by itself.
  - Draw-front thread, 13.8 %: 10.3 % is the same pixel-program input analysis (usage parsing 7.4 %).
  - Deferred replay worker, 19 %: 14.7 % is clock reads inside its 200 us poll window
    (`DeferredReplayWorker::Run`, `GraphicsRenderCommandBuffer.cpp`). Draws arrive tens of
    microseconds apart here, so the worker polls through the whole frame: 45 % of one core that does
    no work.
  - Guest threads: none above 2 % of the samples. In 20 `threads` snapshots the guest main thread
    was in a mutex wait in 13 and asleep in 5. One guest thread yields in a loop; its CPU share
    follows the fence waits (36 % of a core with the program, 8-9 % without).
- **Discarded runs.** A first four-arm run with 5 s blocks and 3.9 ms a frame placed nothing: live
  input moved the scene (567 draws a frame, another cinematic at the end). One run stopped at 160 s
  on the title's own buffer-full abort recorded below, before the probe window.
- **Open.** The region of `0x3666fa39` that holds its GPU time is priced in the record above; what
  inside that region costs it is not. The per-call shader input analysis and
  the replay worker's poll are host costs that a memo and a shorter poll would remove; neither is
  measured as a frame-rate gain while the GPU sets the frame.

## Robot platformer: without the heavy lighting program the close-up scene holds 25 fps (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot,
`3faa704ed` plus a temporary probe (not committed) that leaves out only the dispatch call of chosen
compute programs and keeps their bindings and all other command processing. From 201 s it rotates
three arms in 5 s blocks for 95 s; the figures are medians of the 1 s agent samples inside blocks 6
to 18 (two of the first six blocks ran at 8-12 fps whatever the arm). The capture before the window
shows the robot lying on the dune after landing (the close-up scene); both programs were dispatched
once a frame in every block.

| Arm | Frame rate | Command processor run a frame | Fence waits a frame | Render engine busy |
| --- | --- | --- | --- | --- |
| Everything runs | 19.0 fps | 38.7 ms | 8.7 ms | 86 % |
| `0x3666fa39` not dispatched | 25.0 fps | 35.8 ms | 1.4 ms | 74 % |
| `0x3666fa39` and `0x874bd13f` not dispatched | 24.0 fps | 37.8 ms | 1.0 ms | 71 % |

- **`0x3666fa39` is 15.7 ms of engine time a frame here** (86 % of 52.6 ms against 74 % of 40.0 ms).
  Without it the frame no longer waits on the GPU (fence waits about 1 ms) and holds 25 fps; the
  causal test above shows the graphics command thread then sets the frame.
- **`0x874bd13f` frees no engine time (t216, close-up view; amended 2026-11-19 below: about 10 ms at the crash site).** Leaving it out as well leaves the engine's busy time a frame
  at 29.6 ms in both arms and the frame rate unchanged. The about 14 ms that dispatch stamps gave it
  (bill record below) is elapsed time that overlaps other queued work, not exclusive engine time.
- **What this bounds.** 30 fps in this scene needs both sides at once: the GPU frame under 33 ms,
  which is the other work (about 30 ms) plus at most about 3 ms for `0x3666fa39`, and command
  processing under 33 ms. No change to these two programs alone reaches it. The 2026-11-12 record
  below (26.0 fps and 36.1 ms with every intersection program left out) measured the same ceiling.
- **What `0x3666fa39` is.** A decode of its guest listing (3,066 instructions; notes kept outside the
  repository) shows the title's tiled deferred lighting pass, not a traversal kernel: one 16x16 pixel
  tile per 256-thread group from a compacted tile list, a per-tile light list built in LDS by the
  group's threads between two barriers, G-buffer reads, a loop over the tile's lights, probe and
  reflection lookups and two storage-image stores. Its three intersection instructions sit inside
  the light loop, share one wave-wide stack through lane reads and writes, and only bend a probe
  lookup direction for glossy pixels; every lane's result depends on its own ray alone. The record of
  2026-11-09 below priced the node visits after the first at about 1 ms of 24 a call (forced misses);
  the region-pricing records above show that removing the whole trace region from the compiled
  program removes 11.4 of the program's 15.6 ms, while running it costs nothing measurable.
  Earlier records call the program "the BVH kernel"; the trace is 665 of its 3,066 instructions. A host-written copy of the whole program would be a transcription of the
  guest program, which the data boundary keeps out of the repository.
- **Measured since.** The region that holds the time: region-pricing record above. The command path
  without the program is profiled in the causal-test record above.

## Robot platformer: where the GPU frame goes in dunes gameplay (2026-11-14, measured)

Intel Arc A770, Mesa ANV on Xe, strict, 60 fps present limit, dunes route from the world-map slot.
Baseline: `23c501b05` without diagnostics, 1 s agent samples over 150-210 s. Bill: the same commit plus
two temporary diagnostics (not committed) that bracket every guest dispatch and every indexed or
pull draw with bottom-of-pipe timestamps, engaged only from 150 s so the route keeps its timing;
5 s dumps. The bill run's captures at 160, 185 and 210 s show the robot walking in the dunes; the
baseline's show it standing still against a rock in the dunes (the timed input route ends at 140.5 s,
so where the robot stands depends on how long the loads took).

- **Baseline.** 18.8 fps in the steady windows, the render engine 89-91 % busy (about 48 ms of GPU
  per frame), 530 draws per frame. The command processor spends about 27 ms per frame outside
  fence and register waits (summed thread time) and waits 19-21 ms per frame on fences, so the
  GPU is the limit. About a third of the windows dip to 15.8-16.8 fps with the engine 77-83 % busy;
  there the command processor's own work rises to 38-45 ms per frame at the same draw count.
  Two of the three dip windows start at the capture times (160, 185 s); the one at 177-181 s does not.
- **Bill, 155-210 s** (19-20 fps with the probes, engine 73-83 % busy, about 40 ms per frame):

  | Per frame | Time |
  | --- | --- |
  | Guest dispatches | 18.5-24.5 ms |
  | of which `0xbf8ee9fc` (13,072 groups, the 2432x1368 grid) | 4.1-5.3 ms |
  | of which BVH traversal `0x3666fa39` (480-7,700 groups, 2.2 us per group) | 2.8-19.6 ms |
  | of which `0x61f4e366` | 3.2-3.8 ms when present |
  | of which `0x874bd13f` (8,160 groups) | 2.2-2.7 ms |
  | of which `0x8f3da857` | 0.8-2.0 ms |
  | Indexed and pull draws | 7.4-10.0 ms |
  | of which on the 1920x1080 targets | 7.6 ms (105 draws) |
  | Not bracketed (other draws, clears, copies, barriers, gaps) | about 10 ms |

- **Reading.** The BVH kernel's cost follows its group count, which the guest varies with the
  view: 7,700 groups and 19.6 ms on entry, 1,100-2,700 groups and 2-6 ms on the walk. No program
  takes more than about 6 ms per frame on the walk; 30 fps needs about 17 ms off a 48 ms frame,
  so no single program is that lever.
- **The composite runs on the larger grid.** In every dump of the bill run where it appears (9),
  `0xbf8ee9fc` dispatched 13,072 groups per call (152x86, the 2432x1368 grid), while the indexed
  draws render at 1920x1080. Whether the guest picks that grid from its dynamic resolution, and so from the
  end-of-pipe clock marks (`cross-title.md`, open defect), is not traced.
- **Close-up scene, every draw path stamped (second run, same route).** With the non-indexed draw path
  bracketed too, the stamps cover 366-384 of 381 draws per frame. That run's captures show the robot close
  up in the dunes, at 15.2 fps. Per frame: compute 43-46 ms, draws 11.4-12.0 ms (non-indexed ones 2.1-2.4
  ms). `0x3666fa39` and `0x874bd13f` each dispatched 8,160 groups per call, one call per frame, at 2.5 and
  1.7 us per group: about 20 and 14 ms. 8,160 is the 120x68 grid; the program's
  workgroup is 256 threads, one 16x16 pixel tile, so 8,160 groups are 1920x1088: the full-resolution
  tile list, all of it when the robot fills the view (corrected 2026-11-14; this bullet first read
  the grid as 8x8 tiles of a 960x540 image). Both programs spill on
  this host (records of 2026-11-06 and 2026-11-12 below); no translator change was tried in this run.
- **The BVH grids follow the guest's resolution (2026-11-15, one run with the 960x540 measurement
  patch).** With the same per-program GPU stamps on the dunes route, `0x874bd13f` dispatched 2,040 groups
  per call (60x34 tiles, exactly a quarter of the native 8,160) at 1.2 ms a call. `0x3666fa39` walks a
  variable tile list bounded by that grid (480-7,700 groups at native) and dispatched 920-1,000 groups
  at 1.4-1.5 ms; scenes were not matched. The guest sizes the grid from its render resolution, so
  emulator-side scaling cannot shrink it; at native resolution only the cost per group is open. That
  cost was lower at 540p (about 1.5 against 2.2-2.5 us for `0x3666fa39`, 0.4-0.6 against 0.8-1.7 for
  `0x874bd13f`) with the same programs, which is a lead, not a result. Per frame in the patched run
  the two kernels took about 2.4 ms and all stamped compute about 8.5 ms while the engine was busy about
  30 ms; draws were not stamped, so where 540p's GPU time goes is open. A compiler from an unrelated
  process ran from 96 s of that run.
- **Placement of `0x3666fa39`'s storage buffers gave no gain above about 1.2-1.3x (2026-11-15, small
  group counts only).** The kernel binds five storage buffers, all small read-only per-submit copies
  (16 B to 17.9 KB, descriptor-bound) in host-cached system memory; none is imported guest memory. Its
  image inputs and their placement were not examined. A temporary probe copied the five into
  device-local memory before each dispatch and bound the copies; the log showed every binding swapped,
  not that the kernel read the right bytes. The device-local arm stamped the kernel in one 5 s window:
  93 calls at about 650 groups per call, 1.56 us per group. The nearest system-memory figure at a small
  group count is 1.85 us (45,002 groups in 54 calls, an older native build), and the control run gave
  2.0-2.1 us at about 7,300 groups per call; no device-local sample reached the large calls that set
  the close-up frame. `0x874bd13f` was not covered: the probe never saw it bind a storage buffer. Its
  equal time in both arms (6.4-6.9 against 6.5 ms per call at 8,160 groups) only shows that the GPU
  clock was comparable. One run per arm; an unrelated compiler ran during parts of both.
- **Storage placement census and one device-local A/B (2026-11-15, native, dunes route).** Placement
  is closed on this route as a fps lever: the exposed time is under 9 ms a frame, and the one program
  tested ran no faster in device-local memory where the two arms could be compared.
  - Census (one run, 310 frames in the last 15 s). A temporary count of the storage range every
    program binds, by placement, was joined to per-program GPU stamps. Draws are keyed by vertex
    program. Bound range is not bytes read, and every stamp is an upper bound.
    - Compute programs binding 1 MiB or more of system memory per stamped call took 3.65 ms a frame:
      `0x61f4e366` 1.26, `0x90c95e2d` 0.87, `0x108c4737` 0.79 and `0x51b2a70f` 0.46. All four read
      and write in-place guest imports of 11-29 MiB per call.
    - Draws whose vertex program binds 1 MiB or more took 4.77 ms a frame. That is whole-draw time
      (raster and pixel work included), so it is only a ceiling on vertex fetch from system memory.
    - No program bound a storage object, the class the 2026-10-29 probe moved; every byte was an
      in-place import or a small per-submit copy.
    - `0x874bd13f` (2 storage slots), `0xbf8ee9fc` (1) and `0xe1b1315d` (1) took 7.1 ms of compute a
      frame. In the counted classes they bound under 0.05 MiB per 5 s, the print resolution; what
      their slots are and where they live is not established. `0xff2adfc8` binds no storage.
    - An unrelated compiler ran during part of that run.
  - A/B on `0x61f4e366`, the largest exposed program. A temporary probe copied four of its five
    storage slots, the descriptor-bound ones, into device-local memory before each dispatch and bound
    the copies: 32 B, 4.5 MiB read-only, and 1.5 MiB and 16 MiB written. Its by-address range stayed
    in system memory. The written slots were copied back after the dispatch, outside the stamps. The
    log showed the four slots moved from host-cached to device-local memory for the first ten calls;
    later calls were not logged. The route diverged between the arms: the control reached a cutscene
    and the probe stayed in gameplay.
    - At the matched moment, the first 5 s window in which `0x3666fa39` runs, the kernel took
      1.24 ms per call in the control and 1.25 in the probe.
    - In the probe's gameplay windows it took 0.10-0.17 ms per call. An earlier no-shadow run in the
      same gameplay held it below the 8th-ranked program, under about 0.2-0.3 ms per call, so those
      windows agree with system memory.
    - Ceiling: the saving is at most exposure x (1 - 1/k). With about 8.4 ms exposed, saving 5 ms a
      frame needs k of about 3 or more, and neither comparison shows that.
    - One run per arm. The probe's output was not verified (its capture shows a different scene), and
      a kernel reading stale data would also run faster.
- **Register spills across the title's compute programs (offline, 2026-11-14).** The 105 compute
  pipeline recipes recorded for the title (translator store t215) compiled on ANV: 60 spill. Width 16:
  60 recipes, 15 of them with 870-1,309 spills and 3,589-5,927 fills; width 8: 39, the BVH kernel's
  recipe at 622:2,426 and 358 maximum live registers; width 32: 6, one at 2,901:9,982 (the native
  wave32 program recorded below as `0x874bd13f`). Recipes carry no program hash, so only the BVH
  kernel and that wave32 program are matched to their GPU time; how much frame time the other spilling
  programs take is not established.
- **Split-wave rendezvous result trimmed (2026-11-14, translator t216).** The split-wave publish
  function returned the minimum of the sequence words its spin loop read; callers use only the parity
  of the exchange number, which equals that minimum whenever the spin succeeds. Returning the number
  itself frees the loop's loads after it. Offline, the BVH kernel went from 358 to 285 maximum live
  registers and 622:2,426 to 474:2,133 spills:fills, and four other width-8 recipes compiled at
  283-287; the 41 recipes shared with the t215 census are unchanged. One run on the dunes route
  (capture checked, close-up dune scene) measured 17.7-18.1 fps against 16.4-16.9 fps the run before
  without the change: within run-to-run noise, so no frame-rate gain is claimed.
- **Split-wave rendezvous cost priced: small (2026-11-14, t216).** A temporary translator probe added two
  sequence-only rendezvous rounds (no value slot written, parity kept) to every exchange of the two BVH
  kernels only, compiled outside the translation cache so every other program kept its t216 SPIR-V; a
  stderr line confirmed the first BVH kernel took it (the second, 874bd13f, has no split-wave exchange:
  its SPIR-V is identical with and without the probe). GPU timestamps around each dispatch, on the walking-dunes
  view of the dunes route (capture checked, about 8,000 thread groups a call, the second BVH kernel
  absent in both runs): 1.913-1.919 us per group and 15.4 ms per call without the probe, 2.092-2.095 us
  and 16.7 ms with it. Two extra rounds per exchange cost 1.3 ms, so the fixed mechanics of one
  rendezvous are a floor of about 0.65 ms of the 15.4 ms (about 4 %). The added rounds start right after
  a completed rendezvous, when both halves arrive together, so the time one half waits for the other at
  the real exchanges is not priced, nor is the data they move. Both runs used the stamped probe build.
- **Frame budget on the dunes route (2026-11-14, t216, stamped probe builds).** Close-up dune scene:
  compute about 46 ms a frame, 34 ms of it the two BVH kernels, and draws about 12 ms (7 ms at
  1920x1080, 2.9 ms in the 2048x2048 shadow pass). Walking view: the BVH kernel 15.4 ms and other
  compute about 6 ms a frame, with the render engine about 19 % idle. Offline, removing the whole
  address-table lookup from the BVH kernel (not a valid translation, a ceiling only) takes it from
  475:2,022 to 378:1,488 spills:fills and 285 to 271 live registers with 14 % fewer instructions: a
  static estimate of at most about 2 ms of the 15.4 ms; the dynamic share was measured later and the
  live A/B below shows the driver's register allocation, not the lookup work, moves the time. 30 fps at native resolution needs more than that ceiling in the
  walking view and about half of the close-up GPU time, so no single translator lever measured so far
  reaches it; the kernel compiles at SIMD8 with spills because the guest program alone sits at the
  register limit (see the BVH pressure record above). Earlier probe runs that doubled the exchanges of every split-wave program from boot left the
  route (ship cinematic, world map, the title's command-buffer assert), so the probe was narrowed. The
  walking view differs from the close-up scene of the earlier per-group figures (2.5 us), so the two are
  not compared.
- **BVH kernel time follows driver spill placement, not lookup cost (2026-11-14, t216).** A temporary
  probe compiled the first BVH kernel (3666fa39) in a second variant outside the translation cache and
  alternated the two every four dispatches in one run, so both arms see the same view; the second BVH
  kernel, compiled twice with identical SPIR-V, timed equal in both arms (1.593 us a group), so the
  alternation has no bias. Variants: a fresh plain translation timed equal to the cached one; wrapping
  the address-table lookup so each call runs it twice, the second dependent on the first, was about
  15 % faster (1.67 against 1.97 us a group, every 5 s window of two runs, about 8,100 groups a call);
  the same wrapper returning the first result (second call dead) was 4-9 % faster. Driver statistics of
  the dumped live SPIR-V: plain 390:1,634 spills:fills, single wrapper 385:1,577, doubled 363:1,509,
  all 283 live registers; the doubled variant moves spills out of the deepest loops (fills at loop depth
  5: 84 to 57, spills 18 to 4). More lookup work being faster means the kernel's time is set by where the
  Intel register allocator places spills, not by the lookup. Not shipped: it is an artifact of one
  driver's heuristic, with no general translation that reproduces it. Hand-edited offline variants did
  not match the live translator output (plain 40,923 against 34,087 instructions live), so offline
  figures in this record that came from hand edits are not comparable with live ones.
  The loop-depth split came from a parser that resolved 36 to 66 of the driver's 137 loops, and the
  doubled variant's statistics came from the dumped single-wrapper binary with its return operand
  switched to the second call, not from a separate live dump.
- **Inlining the address-table helper is not a lever (2026-11-14, t216, offline on the live SPIR-V).**
  The translator emits the guest-address lookup as one function marked `DontInline`, called 379 times
  in the first BVH kernel. Recompiling the dumped live SPIR-V with that control changed to `Inline`
  gives the same driver statistics as the original (34,087 instructions, 390:1,634 spills:fills, 283
  live registers): ANV inlines the call either way, so the control does not shape register allocation.
  Replacing every `OpSelect` with its first arm (not a valid translation) collapses the program to
  3,866 instructions, so it bounds nothing about the guard selects alone: what a sound subset of them
  is worth stays not measured. `0x874bd13f` has no intersection instruction (217 texture samples); it
  is a shading kernel, not a second BVH kernel, and earlier bullets that call it one are wrong on that
  point. With the doubled-lookup gain above unshippable and inlining excluded, no measured translator
  lever on `0x3666fa39` approaches the about 17 ms a frame that 30 fps at native resolution needs in
  the close-up scene.
- **Close-up dunes GPU time is command work, not submit gaps or guest flush barriers (2026-11-14, t216,
  probe builds; captures show the close-up dune scene, not the walking view).** Per-command-buffer GPU stamps (start of pipe at begin, end of pipe at end): about 600
  buffers a second, their union covers 94-96 % of wall time, the gaps between buffers 4-5 %; the render
  engine is 86-91 % busy, so the GPU is not waiting for the command processor in this scene. Draw and
  dispatch stamps in the same view account for about 39 of 47 busy ms a frame (BVH kernel 16 ms, other
  compute 8, 1920x1080 draws 11.8, shadow pass 2.4); the rest was not attributed further, and per-draw
  stamps add their own drains. A Vulkan call counter showed about 937 pipeline barriers a frame, 615
  with all-commands stages, about 320 of them from guest cache-flush packets (`ACQUIRE_MEM` /
  `RELEASE_MEM` mapped to one full memory barrier). Skipping that barrier in alternate 5 s blocks of one
  run (a pricing probe, not a valid translation) moved GPU time from 51.3 to 50.8 ms a frame and fps
  from 17.3 to 17.7; a repeat with the call counter confirmed the skip engaged (all-commands barriers
  372 to 20 a frame, render passes unchanged at about 52) and gave 50.8 to 49.9 ms, 17.2 to 17.7 fps:
  guest flush barriers cost about 0.5-1 ms a frame here and are not the lever. The other all-commands
  barriers, which bracket buffer fills, copies and guest-address publishes, drain around the transfers
  they bracket; stamp pairs around the transfers themselves (same scene, a separate run) gave fills
  0.34-0.37 ms a frame (64 calls), buffer copies 0.11-0.12 ms (21) and render-pass begins with their
  load and clear 0.06 ms (52): together about 0.5 ms, not the lever either. About 6 ms of busy time a
  frame stays unattributed (18 image clears, image copies and blits were not stamped). The BVH kernel
  arrives on the guest graphics ring (Kyty queue 8); the guest compute queues also run on the render
  engine on this host because the compute-only family gate stays closed (Arc compute engine record
  below), so all of it executes on one engine. A Mesa
  per-event timing run (`INTEL_MEASURE`) crashed inside the driver and gave no data.
- **Excluded: remembering each invocation's last address-table entry (2026-11-14).** A translator
  variant tried the entry of the invocation's previous guest-address translation before the directory
  and the binary search (entries are disjoint, so a containing entry is the search's answer). Unit tests
  with real device loads passed; one run on the dunes route (capture checked, close-up dune scene)
  measured 17.6-18.4 fps against 17.7-18.1 fps without it. The memo's hit rate and the kernels' time per
  group were not measured, so this shows no frame-rate change, not that the address search costs
  nothing; the variant was not kept.
- **Status 139 off the route is the title's own command-buffer assert (2026-11-14).** It ended two
  diagnostic runs and one run of a build without probes (ship cinematic, then `hub_crashsite_tutorial`
  loading). The crash context's "execute fault at address 0" is how the host reports the guest's
  `int 0x42` abort: the faulting instruction is that trap in the title's "buffer full" handler of its
  command-buffer writer, reached when a reservation does not fit ("There are too many objects/batches in
  this scene."). This is the hub-start assert recorded in this file (bone matrices embedded per draw when
  the builder reads a skeleton between the palette clear and its publish), not a null call and not caused
  by the probes. The dunes route avoids it only when it stays on time.
- **Excluded as a measurement: doubled exchanges off-route.** The run with doubled exchanges
  slowed boot so much that the timed route ended in the ship cinematic; its per-group figures
  come from another scene and price nothing.

## Robot platformer: a guest 960x540 render resolution re-measured on the dunes (2026-11-14, measured)

Intel Arc A770, Mesa ANV, Ryzen 5 7600, `6a6a5818a`, 60 fps present limit, dunes route from the
world-map slot, 1 s agent samples over 155-215 s. The patched run applies one game patch: a mod
that fixes the guest's render resolution at 960x540 (13 writes to the 01.018.000 executable). A
run with a game patch is not a strict run and supports no compatibility claim.

| Run | Mean fps | Steady fps | GPU busy (rcs) | Draws per frame |
| --- | --- | --- | --- | --- |
| No patch | 18.9 | 20-21 | 91 % | 525 |
| 960x540 patch | 31.9 | 37-41 after 166 s; 14-16 before | 78 % | 469 |

Later the same day the same patch held 33.9, 27.8 and 23.4-23.6 fps in three other scenes of this
route (record "at 960x540 the draw ring's cycle is the frame" at the top); the 37-41 fps was not
reproduced.

- **Scenes differ.** The input route is timed, and the faster run walks the dunes and the cliff
  path. The unpatched run stands next to a rock in the dunes during the window. Both include one
  load pause (0-2 fps). The captures at 160, 185 and 210 s show correct dunes, the robot and the
  cliffs in both runs; no scored compare was made.
- **Against the 2026-11-02 figure.** The same lever measured 16.6-18.1 fps then. The
  command-processor work cut since then lets the smaller GPU frame show: the GPU is no longer busy
  all the time (78 % against 91 %).
- **Repeated.** A second patched run held 37-39 fps from 170 s to the end of the window, after the
  same slow start (13-20 fps), without a load pause.
- **Not done.** The same scene was not reached in both runs, and the slow start of the patched
  window was not traced to a scene.

## Robot platformer: transport shade layout destroyed while in use (2026-11-14, fixed)

Intel Arc A770, Mesa ANV, strict run, 60 fps present limit, pause-menu route, 300 s, with the Vulkan
validation layer's thread-safety, unique-handle and object-lifetime checks.

- **Cause.** `FragmentTransportCreateShadePipeline` built the shade pipeline layout from an empty
  descriptor set layout and destroyed that layout right after pipeline creation. Without
  `maintenance4` the set layouts of a pipeline layout must stay alive while it is used, so every
  bind of such a pipeline was invalid (`VUID-vkCmdBindPipeline-pipeline-parameter`).
- **Fix.** The shade pipelines use the device-lifetime empty layout that the graphics pipelines
  already share.
- **Verified.** The same route reports nothing under those checks; a temporary entry report
  confirmed fragment transport was recorded in both runs, and the three captures have the same
  health metrics as before the fix.

## Robot platformer: draw tail recorded on a replay thread (2026-11-14, measured)

`docs/bringup/performance-architecture-plan.md`, entry "Built: C stages 1 and 2". Intel Arc A770, strict
run, 60 fps present limit, dunes gameplay: 20.3 fps against 19.1 fps with the draw tail recorded inline
(one run each; the scene is GPU-bound, so this is not a speed claim). The Vulkan validation layer with
only its thread-safety checks is clean.

## Robot platformer: a depth array layer partly outside its depth image (2026-11-13, fixed)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, branch `perf/guest-memory-binding` at `05f53da47` plus the
fix, strict runs, `KYTY_PRESENT_FPS_LIMIT=60`, native resolution, the user playing with a real pad.

- **Cause.** Leaving the dunes, a 64 KiB tiled depth view reads layer 4 (D32, 1024x1024, 4 MiB at
  0x529c30000). One depth image (1536x1536, plane 0x529530000-0x529e30000) holds the layer's first 2 MiB;
  the rest lies in no image's plane. The layer planner exited on the first such block (the record of
  2026-11-12 below).
- **Fix.** The planner keeps the blocks outside every image as merged address ranges. When a layer has such
  ranges, each range must classify as guest-owned (the same classification the plain guest layer uses); the
  layer's guest bytes are uploaded, then the images' blocks are copied over them. The layer is rebuilt on
  each use, since its digest does not cover guest bytes. A range that is not guest-owned still exits, naming
  the range and the classification.
- **First attempt rejected.** Classifying the whole layer returned unsupported, because the depth image that
  holds the first half overlaps it; that build exited at 270 s on the same layer (e77).
- **Checked.** A user-played run on the fixed build held 321 s with no exit (e78), where the previous build
  exited at 270 s (e77); `kyty_unit_test` 2652 passed; the three CTest gates passed. Not confirmed: that the
  mixed-layer branch ran in e78 (no counter was compiled in, and an earlier 893 s run also had no exit),
  the image of the layer's consumer, a recorded route to that scene, and other titles. The mixed layer
  re-uploads 4 MiB of guest bytes on every use; that cost was not measured.

## Robot platformer: Arc compute engine not usable for the compute queues (2026-11-13, excluded)

Same host and build with temporary counters (removed), dune route 105-145 s.

- On this host the compute-only family is selected only when every renderable depth format declares
  `DEPTH_COPY_ON_COMPUTE_QUEUE` (`VK_KHR_maintenance10`). ANV declares it for neither D16 (124) nor D32
  (126), so every queue runs on the render engine: rcs 93-94 %, ccs 0 %.
- Counted depth copies: D16 copies were recorded only on the graphics queue (7,966-8,794 per run), none on
  Kyty compute queues 0-7. The compute queues recorded 4,212-4,276 D32 copies per run, all from the texel-bit
  reinterpretation copy (`vkCmdCopyImageToBuffer` then `vkCmdCopyBufferToImage` through a scratch
  buffer); the source-side and destination-side counters were equal, so whether one or both sides were
  depth is not settled. Either way a buffer copy with a depth aspect needs the format's copy-on-compute
  bit on a queue without graphics. Of the image-to-image sites only the depth array layer copy was
  counted (none on the compute queues).
- Skipping only the D16 check (e75) left the gate closed on D32: 19.63 fps, rcs 93 %, ccs 0 %, against
  20.04 fps on the base (e71).
- The BVH kernel `0x3666fa39` is recorded on the graphics queue (Kyty queue 8) and would stay on the render
  engine in any case; the compute queues held about 6 ms of the frame in the record of 2026-10-30 (gw137,
  older build, not re-measured). Using the compute engine would need those copies moved to the graphics
  queue with cross-queue synchronization, for at most about 2-3 fps.

## Robot platformer: one batch in flight stops the boot (2026-11-12, probe, own-producer rule passes)

- **Observed.** A temporary change made a graphics submit wait for the previous batch's decode
  instead of its own. The title stopped at frame 1 with no present, with or without the release-drain
  skip. On the graphics ring, 5 batches were dequeued and 4 signalled.
- **Contract.** It is the signature of the 2026-10-26 stop ("A suspended submitter must not pass
  the command processor", below). Keeping serial every batch with a top-level memory wait boots the
  title, but also releasing the suspended batch's own submitter at the host-producer release stops it
  again. Over a whole boot-to-dunes run, 17 % of 18,512 batches were released for a host-produced
  value, so those batches cannot return early. In the dunes window (90-145 s), 1.6 % of 148,478 waits
  found no producer and 91.8 % had it in the command processor's current host submission (inferred
  to be the same batch, not shown). The mechanism is inferred from the record;
  thread states were not taken.
- **Own-producer rule.** Returning early only the batches whose every memory wait is written by an
  earlier packet of the same batch: 16,153 batches early, 4,038 serial, and every host-produced
  release falls in a serial batch. The title boots and plays the dunes route; 17.98 fps at 105-145 s
  against 19.44 serial (no gain, as estimated).
- **Record.** `docs/bringup/performance-architecture-plan.md`, section 6.

## Robot platformer: not running the programs that contain intersection instructions (2026-11-12, rejected)

Intel Arc A770, Mesa 26.2.6 on Xe, `bbc559839` plus a temporary diagnostic (not committed) that returned
from a compute dispatch without running it when its program contains `ImageBvhIntersectRay`. Dunes route,
105-148 s, one pair of runs, 60 fps present limit.

| | Guest | Programs not run |
| --- | --- | --- |
| Frame rate | 20.0 fps | 26.0 fps |
| Command processing per frame | 54.1 ms | 36.1 ms |
| Of which fence waits, register waits | 15.6 ms, 11.6 ms | 1.2 ms, 1.3 ms |
| GPU busy | 94 % | 82 % |

- **Rejected on the image.** 18,024 dispatches were not run. The scored capture pair and a user-played
  session (26-30 fps on the window) show dark terrain, a translucent player model and washed-out objects.
  The outputs of the skipped programs stay unwritten; which later pass reads which output was not traced.
- **What it bounds.** With that GPU cost gone entirely the scene runs at 26 fps with 36 ms of command
  processing a frame, so no GPU-side change alone passes about 26-28 fps here.
- **What stays possible.** Every intersection reporting a miss keeps all outputs written; the record of
  2026-11-09 below ("paired kernel measurement") priced it at about 1 ms of 24 per call of the heavy
  program. That is the price of the node visits after the first, not of the trace: the trace region as
  a whole is 11.4 of the program's 15.6 ms (region-pricing record, 2026-11-14). The title's own renderer selection through a game
  patch is the other route and is the guest's configuration; its image was not verified ("Game patch
  trial and geometry cost", 2026-11-01, below).

## Robot platformer: the reported output size does not move the title's render targets (2026-11-12, excluded)

Question of the performance plan (E2): does the title size its targets from the output size the guest is
told? Census of the first colour target's extent per indexed draw over the dunes route, with a temporary
diagnostic (not committed) that also printed the screen size it read.

| Reported output size | 1920x1080 | 2432x1368 | 3840x2160 | 960x540 | Frame rate | GPU busy |
| --- | --- | --- | --- | --- | --- | --- |
| 1280x720 | 451,615 | 2,117 | 7,179 | 73,443 | 19.5 fps | 92 % |
| 960x540 | 466,605 | 3,340 | 7,566 | 75,545 | 20.4 fps | 95 % |

The display buffer is 3840x2160 in both runs. The title does not follow the reported size, so an
output-size option separate from the window size is not built for it.

## Robot platformer: GPU clock marks and the title's dynamic resolution (2026-11-12, open, frame-rate effect not established)

The title starts with main targets of 2432x1368 and steps to 1920x1080 by itself (2,117 draws at the larger
size over the dunes route). Its end-of-pipe clock marks all carry nearly the same value inside one host
submission (`cross-title.md`, open defect). One run with a temporary diagnostic (not committed) that wrote
every mark at least 0.1 ms after the previous one drew 6 draws at 2432x1368 and no main target below
1920x1080 in 143 s.

- **Not comparable past the first frames.** Its captures at 108, 125 and 145 s show a black frame and the
  intro cinematic where the run without the spacing is in gameplay; it measured 17.4 fps, 369 draws a
  frame and 31 ms a frame in each of fence and register waits, against 19.5 fps, 592 draws, 12 and 8 ms.
  Whether the spacing delayed the route or the host did was not separated.
- **Excluded in the dunes (2026-11-14, one run each, scene checked by capture).** With every mark at least
  0.15 ms after the previous one from 150 s (after the input route, so the route kept its timing), about
  66 ms of reported GPU time a frame, the close-up dune scene drew 113,027 draws at 1920x1080 against
  106,479 without the spacing, the same handful at 2432x1368, nothing new below 1080p, and 15.8 against
  15.2 fps. The title's resolution does not drop in the dunes when the marks report over-budget frames,
  so correct marks are a correctness fix there, not a frame-rate lever.
- **Marks fixed (2026-11-14).** Device timestamps now back every mark (`cross-title.md`); one run in the
  close-up dune scene measured 16.4-16.9 fps with main targets unchanged.

## Robot platformer: render-target scaling to 960x540 under user play, and a depth-array exit (2026-11-12, exit fixed 2026-11-13)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, branch `perf/guest-memory-binding` at `0fe51e855`,
strict run, `KYTY_PRESENT_FPS_LIMIT=60`, `RenderResolutionMode = Fixed`, `RenderResolutionScope =
RenderTargets`, 960x540, the user playing with a real pad (no input route), per-second agent samples.
The binary also held a temporary binding diagnostic, compiled in and switched off.

- **Frame rate.** It never reached 60 fps. Menus and the first scene: 30.5 fps, GPU busy 79 %, command
  processor 34.5 ms/f, of which 13.8 ms were waits. Gameplay at 370-630 draws per frame: 19.6-20.7 fps, GPU
  busy 83-90 %, command processor 51 ms/f, of which 15-23 ms were waits. Short peaks of 58-60 fps appeared
  only in scenes with about 170-260 draws.
- **Why the scaling does not help.** This scope scales only render targets of the display extent; the main
  passes and the BVH kernel's guest grid keep their size (see the 640x360 test below). The internal-resolution
  lever is the game patch that lowers the guest grid, and it measured 16.6-18.1 fps in the dunes, not 60.
- **Defect, not fixed.** After 223 s the run exited at `GraphicsRenderAttachments.cpp:824`: "depth array layer
  4 ... lies partly outside depth images" (block 0x529e30000 of the layer at 0x529c30000). No depth image
  covers that 64 KiB block. It is not known whether native resolution reaches the same state, since the route
  was the user's and was not recorded. Suggested direction: record a pad route to that scene, repeat at native
  resolution, and log the depth images near that address when the layer is assembled.

## Robot platformer: BVH kernel register pressure, guest against host (2026-11-12, open)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.15, translator 215, offline.
The question: the 2026-11-10 record below closes the BVH traversal kernel
`0x3666fa39` on "the guest program's live ranges at 128 registers". How much
of the driver's 358 live registers does the guest itself need?

- **Module.** The stored compute recipe of the dunes route whose descriptor
  layout matches `0x3666fa39` (10+10 2D, 5+5 2D-array, 1+1 3D textures, 7
  samplers, 2 storage images, workgroup 256) compiles at SIMD8 to 41,398
  instructions, 1352 sends, 145 loops, spills:fills 622:2426 and 358 maximum
  live registers (`VK_KHR_pipeline_executable_properties`, shader cache
  disabled). That is heavier than the translator 210 baseline below (37,309,
  1219, 137, 556:2138, 354); the cause was not traced.
- **The guest alone sits at the limit.** The program names v0-v73 and
  s0-s54. A backward CFG liveness pass over the guest listing (3,066
  instructions; `exec` and `scc` left out, every write counted as a full
  definition, so a lower bound) peaks at 53 scalar and 69 vector registers,
  122 in all, against 128 registers per thread at SIMD8.
- **Host pressure by model** (final NIR from `INTEL_DEBUG=cs`):

  | Model | Peak, 32-bit words |
  | --- | --- |
  | Exact CFG liveness | 126 (49 uniform, 77 per-lane) |
  | One continuous range per value, the hull of where it is live | 267 |
  | Driver, maximum live registers | 358 |

  The hull is how the back end's allocator sees a value. It agrees with the
  2026-10-29 estimate below (about 265 against 362). At the hull peak 104
  per-lane NIR registers span the point, against 51 under exact liveness:
  register webs whose live ranges have holes. The last 90 registers come from
  the back end's own lowering; not traced.

  **Correction (2026-11-14).** The "exact" row and the two exact figures after
  it are wrong. The script read a block's successors only from the line that
  closes it; an empty block prints them on its header line, so liveness never
  crossed an empty else arm and the exact figure came out far too low. Fixed
  and re-run on the translator 216 module (34,087 instructions, 390:1634, 283
  maximum live): exact 243 (83 uniform, 160 per-lane), one continuous range
  per value 282, driver 283. Holes in register webs add about 40, not 140: the
  program's values are live, and 95 of them only pass through the trace region
  (record of 2026-11-14 above). "51 under exact" and the holes conclusion are
  withdrawn. The exact figure of the guard ablation below (102) has the same
  defect and was not re-run; its compiled statistics stand.
- **Guards explain little of the holes.** The same module with every select
  on the guest EXEC replaced by its new value (unsound, offline ceiling)
  compiles to 33,141 instructions, 462:1464, 331 maximum live; hull 247,
  exact 102. The 2026-11-02 exclusion of removing EXEC guards by register
  liveness stands: even all of them leave the hull near 250.
- **Driver back end.** `INTEL_JAY=cs` leaves the same brw output on this GPU;
  the alternative back end does not engage.
- **Cross-lane operations.** The guest uses only `v_readlane` (44),
  `v_writelane` (36), `v_readfirstlane` (9) and `s_ff1` on 64-bit masks (30):
  no DPP, `v_permlane16`, `v_permlanex16`, `ds_swizzle` or `ds_bpermute`. The
  ISA confines those to 16-lane rows or 32-lane halves, so that confinement
  cannot keep any of this kernel's exchanges inside a part.
- **Excluded: a workgroup barrier as the exchange between parts.** The lane
  reads already use `OpControlBarrier` with subgroup execution scope, which
  orders one part's memory and does not wait for the others
  (`ShaderSpirvSplitWave.cpp`, lane-read helpers); the only workgroup-scope
  barrier is in the prologue. A workgroup barrier at each exchange needs the
  four guest waves of the workgroup to make the same exchanges between
  barriers, and the traversal loop of each wave exits on its own wave-uniform
  stack state.
- **Excluded: one guest wave64 as one 32-lane host subgroup with two guest
  lanes per invocation.** It removes the exchange between parts, but on 128
  registers per thread a SIMD32 value takes 4 registers, and the kernel keeps
  about 69 vector values live per lane, twice per invocation. The records
  below measured the nearer variants: 16-lane parts compiled to 1574:9325, and
  32-lane parts gave wrong output at 101 ms per call.
- **Open.** The largest static gain on record is still the rendezvous
  ablation (362 to 258 live, 2225 to 1353 fills, 2026-10-30), which has no
  valid design. Even the guest's own registers fill the thread, so no
  translation reaches zero spills; a gain here is a few ms per call at most,
  and only per-call GPU timestamps on a matched scene with an image compare
  can show it.

## Robot platformer: banded frame in the intro cinematic (2026-11-10, open)

- Observed (Intel Arc A770, ANV Mesa 26.2.6, strict, `6dab9a32e` plus the uncommitted
  min/max change of the cross-title float min/max record, translator 211): on the full
  dunes route, from a save that continues on the world map, a capture at 150 s (frame
  3881) showed the ship over dark horizontal bands, 38 s and about 800 frames after
  that run's space-map capture. The row-mean luminance has a 42-43 px period (spectral
  peak 61 times the median). The shot was not identified. The space scene record of
  2026-10-31 had the same symptom; its clean captures are of earlier shots.
- No clean `6dab9a32e` run is a replay of that run. One clean run of the same route
  was on the dunes by 140 s; another exited at 117 s (below). Two clean runs whose
  input stops after Continue showed, one, the space map at 125-130 s, a slate frame at
  135 s and the ship falling to the dunes from 140 s, and the other the warp effect
  from 128 to 142 s. The slate frame has no row pattern (row-to-row steps below 0.05
  of 255). None of the 21 clean captures shows bands. The run with the change and
  that input stayed on the title's press-any-button screen: its presses came before
  the title accepted input.
- So whether the bands come from the change, are intermittent, or are a regression
  since 2026-10-31 is not known. The change was not committed.
- One clean run of the dunes route exited by itself with code 0 at 117 s, with no
  error event and no crash report, resident memory 9.7 GB against the run's 16 GB
  limit, after the route's Options, Down and Cross presses during the cinematic.
  Whether those presses chose an exit item was not checked; one occurrence.
- Direction: same route and save as the banded run, clean HEAD first, captures every
  3 s from 100 to 175 s, each file moved out of the capture directory as it lands
  (native captures keep the newest eight), each scored by the row profile; run the
  change the same way only if those captures are clean.

## Robot platformer: the guest does not pace the still dune view (2026-11-10)

Intel Arc A770 at 2400 MHz without throttling, Mesa 26.2.6 on Xe, Linux
7.2.14; other processes used 0.7-1.6 % of the render engine. Idle dunes route,
strict runs with the 60 fps cap. Two runs on `0b02f53a1` (32 slots) carry a
temporary probe, not shipped: timers on each graphics ring thread around its
wait for the next guest batch, the run lock, suspended label waits and the
time it holds a batch, plus the per-site fence probe of the records below. The
third run compared is the ring record's "after" run below, with the per-site
fence probe only.

- **Excluded: the guest paces the frame.** In the probed runs the main
  graphics ring thread waited for a guest batch 2.9 ms per frame in the
  alias-wait regime below (100-110 s, 556 draws per frame) and 3.8 ms per
  frame without it (100-110 s, a window that also holds a 5 s hitch before the
  still view). It held its batch 95-97 % of the frame, and its suspended label
  waits took 0.3 and 1.1 ms per frame. An asynchronous graphics ring thread
  spent 49-63 ms per frame suspended on a host-produced label. The fence
  counter also holds the window thread's present wait (about 2.8 ms per frame
  in the ring record's "after" run), so frame time minus the CP thread's CPU
  time, fence wait and render lock does not bound the batch wait; the direct
  timer above does.
- **The second wait regime recurs with 32 slots.** It is the `publish-host`
  wait of `docs/bringup/cp-gpu-sync-chain.md` ("Attribution moves"): a draw
  reads a storage buffer whose pending GPU write has a Texture alias, so
  `StorageIsWholeGuestContent` fails and the draw waits on the host. One of
  three probed 32-slot runs took it; a fourth 32-slot run of the view, under
  a profiler, cannot show the site. It first fired at 80-90 s, 179 waits of
  about 0.15 ms each; in the still view (100-110 s) it waited 4.9 times per
  frame, about 4.5 ms each, 21.9 ms per frame. The wait's length follows how
  far the GPU is behind, which fits the untested reading in the sync-chain
  record that the first wait of a frame absorbs the GPU catch-up.

  | Per frame | Ring record "after" run, 103-110 s | Probed, without the alias wait, 106-116 s | Probed, with it, 103-110 s |
  | --- | --- | --- | --- |
  | Frame time, ms | 52.0 | 60.4 | 62.8 |
  | Graphics CP thread CPU, ms | 36.9 | 40.9 | 32.9 |
  | Fence wait, ms | 8.7 | 10.8 | 24.1 |
  | `WAIT_REG_MEM` wait, ms | 6.9 | 7.9 | 2.0 |
  | Submits | 45.9 | 44.4 | 60.8 |
  | Render engine busy, ms | 42.1 | 48.2 | 44.3 |

  The submit rate tracks the regime: about 15 more per frame with the alias
  wait, three per alias wait; the mechanism is not separated. A fourth run
  (2026-11-10, no alias wait, 107-116 s) gave 44.4 per frame again; other
  DRM clients used 1.7 % of the render engine's cycles over 90-116 s, with
  no 3D renderer running on the host, so a busy one was not measured. By the
  code, one wait submits at most two graphics recordings, both conditional:
  the open recording at the same-queue wait (`WaitSubmission`) if the
  awaited one is still recording, and the idle-GPU submit after the retried
  draw (`SubmitRecordedWorkIfGpuIdle`, every 8 operations and 500 us); the
  wait itself blocks on the fences through the awaited submission
  (`CompleteSubmittedThroughLocked`). The third submit is not on the wait's
  path. The per-site counter of the GPU bill run below put its 56.5 submits
  per flip mostly on the idle-GPU rule (29.3, against about 9 in the earlier
  runs of about 36), so the extra submits are that rule's. Whether that run
  took alias waits was not probed, so the link from the alias wait to the
  idle-GPU flushes is inferred, not measured. The alias itself is not identified: the run that took this wait
  predates the probe that names it, and the probed run did not take it.

  The middle column ran a different workload through 116 s: 55-58 render
  pass begins and about 1470 uploads per frame against 69-91 and 1720-1830 in
  the other two, and no object creation. The two probed runs therefore do not
  isolate the alias wait, and its frame cost is not established. Draws,
  dispatches, uploads, hashed bytes and object creation per frame match
  within 2 % between the first and third columns. Captures at 126 s (first
  column) and 110 s (third) show the same view rendered correctly. The guest
  thread that spins in a sleep loop (about 18 ms of CPU per frame) runs in the
  first two columns only.
- **Ceiling.** The render engine is busy 42-45 ms per frame in the runs with
  the usual workload (the first seconds of the view are not more expensive)
  and 49 ms in the run with the different one, so work on synchronization
  alone ends near 21-24 fps here, and 30 fps needs the GPU's work under 33 ms
  per frame.
- **Open.** Which texture aliases the buffer is not identified: a capped probe
  line in the failing branch (storage and alias ranges, alias parameters,
  dependency queue) did not fire in the repeat run, which stayed in the
  regime without the wait.

## Robot platformer: the still dune view waited on a full submission ring (2026-11-10, fixed)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.14. Idle dunes route, window
96-126 s (the still view, 559-561 draws per frame), one strict run per arm on
`1c707186c` with the temporary per-site wait probe of the record below; the
second arm adds the ring change.

- **Before.** 14.9 fps, 16.0 ms of fence wait per frame, render engine 66 %
  busy (about 44.0 ms per frame). In a 20 s stretch the full submission ring
  held 4333 of the 4739 ms of fence wait. A slot is reused only after its
  submission completes, and in the GPU-bill run below the graphics queue
  submitted 56.5 times per flip (99-109 s), so 8 slots fill while one long
  submission runs.
- **Change (`b7af968cb`).** `CommandProcessorSubmissionSlots::SlotCount` 8
  to 32, with the transient snapshot pool at one recording per slot to match.
  Retention stays at 256 MiB (`kRetainedBytes`, before half of the 8-slot
  pool budget); pool entries are created on demand, so the larger budget is a
  cap, not a reservation. The slot-count contract test now expects 32.
- **After.** 18.6 fps, 7.7 ms of fence wait per frame, render engine 79 %
  busy (about 42.6 ms per frame); full-ring waits 0. The largest remaining
  waits are the `WAIT_REG_MEM` producer wait (1735 ms per 20 s) and the
  window thread's present wait (1047 ms per 20 s). Resident memory peaked at
  10.16 GB against 10.01 GB. With the render engine near 80 % busy at about
  43 ms per frame, this view's GPU bound is about 23 fps.
- **Earlier result reconciled.** The probe-arm table in
  `docs/bringup/cp-gpu-sync-chain.md` found that 32 slots alone changed
  nothing while the host read of the geometry record blocked; that read is
  gone since the device-side geometry draw (`fdd43102c`), so the ring became
  the wait.
- **Other titles.** These runs are on `b7af968cb`, so they cover both
  `b6b1acb58` (the fill split below) and the ring; the previous runs were on
  `fdd43102c`. The binary was built from that tree before the commit, so
  its embedded revision reads `1c707186c`. One strict 125 s run each of `PPSA28311 - Absolum`,
  `PPSA09076 - DREDGE`, `PPSA03969 - F.I.S.T.: Forged In Shadow Torch`,
  `PPSA09809 - OCTOPATH TRAVELER II` and `PPSA21402 - Balatro` on the
  generic route: all exit 0 at the interactive frontier with both captures
  healthy.

  | Title | Frames in 60-116 s, before | After | Peak resident memory, before / after |
  | --- | --- | --- | --- |
  | Absolum | 2839 | 2854; repeat 3274 | 7.94 / 7.66 and 8.30 GB |
  | DREDGE | 1723 | 1611 | 2.75 / 2.77 GB |
  | F.I.S.T. | 312 | 412 | 4.71 / 4.87 GB |
  | OCTOPATH TRAVELER II | 3182 | 3247 | 4.01 / 4.08 GB |
  | Balatro | 3306 | 3230 | 0.75 / 0.79 GB |

  - The first Absolum run reported 50.2 and 48.8 fps at the two captures
    against 60.0 and 60.1. That status figure is a 0.25 s window at the
    query instant; over the window the frame count was the same, and the
    per-sample median frame time was 18.0 ms against 19.2 ms. The repeat
    held 59.7 fps over the window.
  - DREDGE has 6.5 % fewer frames in the window (29.9 against 38.9 fps at
    the 60 s capture, 31.7 against 30.4 at 115 s), one run each; it was not
    repeated.
- **Playable regression.** `kyty_agent playable-regression` on
  `PPSA06084 - Superliminal` exited 3. Its failed gates were
  `visual_compare` (no baseline is recorded on this host, so there is no
  visual comparison), `scene_checkpoint` and `action_response`; the input
  gates passed. The previous reference run failed those three and two input
  gates.
- **Tests.** `ctest --no-tests=error`: 108 tests, 100 % passed
  (`PairedWave64` skipped), exit 0.

## Robot platformer: geometry output fills ran a driver divisor search (2026-11-10, fixed)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.14. Strict runs of the idle
dunes route on `1323c0915`; the dune approach and crash-site cinematic
(700-826 draws per frame, 6-10 fps) came before the still view in every run
by 115 s, so these figures describe that stretch, not the still view.

- **Before the fix, fence waits were not the lever there.** A temporary
  per-site wait probe measured 1-12 ms of fence wait per frame against
  106-157 ms of summed command-processor time, with the render engine
  41-56 % busy. Per 10 s the largest sites were the full submission ring
  (214-389 ms) and the `WAIT_REG_MEM` producer wait (116-333 ms); untagged
  waits added 120-180 ms. After the fix the same stretch shows 16-24 ms of
  fence wait per frame with the render engine 63-74 % busy: the wait moved
  once the command processor got faster, and its sites are not yet split on
  the fixed binary.
- **Profile.** `perf` (user time, 199 Hz, 6 s from the first sample at
  >= 500 draws per frame) on the graphics command-processor thread:
  `CommandBuffer::CloseGeometrySegment` 16.2 % of its samples, all in the
  driver below `vkCmdFillBuffer`, 130 of 131 samples in one two-instruction
  loop of `isl_surf_from_mem` that increments a candidate and divides until
  it divides the element count. The driver records the fill as a 2D surface
  and searches for a width of at most 16384 elements; an element count with a
  large prime factor costs one division per candidate.
- **Fix (`b6b1acb58`).** `VulkanCmdFillBuffer` records explicit-size fills as a body in
  whole 64 KiB (about one division per 64 KiB, about 16k at the 1 GiB
  geometry output cap) plus a tail below 16384 words that fits one row; the
  filled bytes are unchanged. Only the geometry segment call site was shown
  exercised; the device-address, detile and fragment-transport sites use the
  same helper without a measurement. Same route and profile window: `CloseGeometrySegment` 0 of 731
  samples, `isl_surf_from_mem` 0 samples (130 before). Rendering at 116 s is
  the same cinematic, scored healthy.
- **Frame rate: indicative only.** At 737-799 draws per frame the summed
  command-processor time fell from 90-138 to 75-106 ms per frame and the
  frame rate rose from 7.5-11.7 to 10.0-14.3 fps, but the heavy stretch began
  at 101 s in one run and 109 s in the other, so the windows are not the same
  part of the cinematic.
- **Remaining on that thread.** Transient snapshot capture (copy and compare
  of read-only buffers) 15-17 %, `ShaderParseUsage2` under
  `ShaderGetInputInfoPS` 10-12 %, `GpuMemory::CreateObject` 11 %.
- **Not changed.** The GDS fill is at most `kGraphicsGdsDwords` (12288)
  words and fits one row. The guest fill in `GraphicsRenderAttachments.cpp`
  (`vkCmdFillBuffer(cmd, identity.buffer->buffer, ..., fill.size, ...)`) has
  a guest-controlled size, so the driver search is still reachable there; it
  was left because that file carries unrelated uncommitted work on the shared
  branch. Direction: route it through `VulkanCmdFillBuffer` once that work
  lands.

## Robot platformer: BVH kernel spills are the guest's live ranges, no translation lever (2026-11-10)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.14, translator 210, offline.
The question: can a different translation of the BVH traversal kernel
`0x3666fa39`, 19.7 of the 44.4 ms dunes GPU bill (record below), cut its
scratch spills enough to save 3 ms per frame?

**Later (2026-11-14).** The record at the top of this file prices the scratch
traffic of this program at 11.6 ms a frame and counts 95 values that only pass
through its trace region. "No translation lever" covers the variants tried
here, not a translation that keeps those values out of the allocator's way.

- **Module.** A strict 211 s run of the dunes route on `aa282cbf4` with
  temporary probes and a private copy of the shader store logged the
  kernel's SPIR-V assembly through the shader probe. Assembled without
  optimization (`spirv-as`, Vulkan 1.4) it is byte-identical to the module
  the store holds for that program, so the pipeline compiles that text.
- **Method.** Each variant is the logged text rewritten offline and
  compiled by ANV at SIMD8, shader cache disabled, with the kernel's
  descriptor layout; statistics and GEN assembly come from
  `VK_KHR_pipeline_executable_properties`. Outer-loop fills are counted in
  the assembly. Variants that break the exchange's semantics only bound
  what a translation change could reach; none is a candidate fix.

  | Variant | Instructions | Sends | Spills:fills | Outer-loop fills |
  | --- | --- | --- | --- | --- |
  | Baseline | 37,309 | 1219 | 556:2138 | 1072 |
  | 19 cold scalars packed | 36,248 | 1223 | 520:1955 | 1012 |
  | 26 cold scalars packed | 35,992 | 1226 | 488:1878 | 923 |
  | 36 cold scalars packed | 38,132 | 1249 | 516:2123 | 1013 |
  | 19 cold scalars in shared memory | 38,433 | 1294 | 547:2401 | 1299 |
  | Relaxed exchange ordering (invalid) | 35,487 | 907 | 555:2171 | 1076 |

- **Cold scalar registers.** The translation gives each guest scalar
  register its own function variable, and ANV holds each in a whole register
  at SIMD8. The packed variants keep the least-accessed scalar registers in
  the lanes of a few per-lane variables: a read becomes a constant-lane
  broadcast, a write a lane select. That assumes the packed values are
  uniform in the subgroup, which was not checked. Packing all 172 declared
  scalar registers gave no gain. The best set (26 registers) removes 149 of
  the 1072 outer-loop fills. 89 of them sit in the loop at nesting depth 2
  that holds 264 of the outer loop's 318 global loads, by its loads the node
  traversal: 28% of that loop's own fills. The frame effect was not
  measured. The saving is not monotonic in the set (36 registers gives
  almost all of it back), so it yields no general selection rule; a
  hand-picked set per program would not be a general translation, and
  packing is only valid for values proven uniform, which the translator does
  not establish. One workgroup-memory slot per subgroup instead of a lane is
  worse.
- **Rendezvous and ordering.** Removing the split-wave spin loops entirely
  (invalid, a ceiling) still leaves 1740 fills, and the fills outside loops
  do not move. Relaxed ordering on the exchange atomics without the acquire
  barrier (invalid: acquire and release make the exchange defined) cuts
  sends by 312 and SLM fences by 303 (318 to 15 in the assembly), but no
  fills: ordering does not drive the spills. Plain loads in the spin loops,
  and the acquire barrier moved after the loop, compiled to one identical
  program; these and the relaxed variant have 128 loops instead of 137, for
  a reason not established. Fence cost is bounded by the halving of the
  rendezvous (record of 2026-10-29 below: 2399 to 1486 sends, at most about
  10% in dune windows).
- **Allocator noise.** The relaxed variant moved fills by 33 with no
  register change, so differences of a few dozen fills are noise.
- **Excluded: marking scalar registers uniform (2026-11-10).** A general
  variant passes every value stored to a guest scalar register (and to
  `exec`, `vcc`, `scc`) through `OpGroupNonUniformBroadcastFirst`, and a
  second one also every load: 561:2170 and 560:2188 spills:fills against
  556:2138, 353 maximum live registers against 354. In the final NIR of the
  stores variant one of the 1663 inserted broadcasts remains: the compiler
  removed the rest because their sources were already convergent, and the
  phis stay at 274 convergent and 1118 divergent as in the baseline. The
  stored values were therefore already uniform where they are defined. That
  the backend then gives each such value a whole SIMD8 register is a reading
  of a few assembly lines (`<0>` source regions, no-mask SIMD8 writes), not
  established for the scalar registers.
- **Conclusion.** The spills follow from the guest program's live ranges at
  128 registers per thread. No variant tried gives a general translation
  rule that cuts them, so reworking the BVH translation is closed; the
  frame effect of the best packed set was not measured.

## Robot platformer: the command processor is worth at most 3 fps in the dunes (2026-11-10)

Intel Arc A770, Mesa 26.2.6 on Xe, Linux 7.2.14, the user-recorded dunes
route, 60 fps limit. The binary is `799fb76fd` (Release, without link-time
optimisation) plus temporary diagnostics, not committed: a 40 us busy-wait
after each draw in alternate wall-clock windows, and fence time per wait
site.

- **Causal spin on the title screen** (the logo over the ship, 241-248
  draws and 83 dispatches per frame, alternate 4 s windows from 56 s). The
  two samples taken before the spin was armed are left out.

  | Arm | Frame | GPU busy | Fence waits |
  | --- | --- | --- | --- |
  | Off (6 s) | 38.5 ms | 75% | 11.5 ms |
  | On (6 s) | 43.9 ms | 69% | 7.9 ms |

  - The spin added 10.3 ms per frame and the frame grew 5.3 ms: k = 0.51.
    The rest came out of fence waits and command-processor idle time.
  - The 2026-11-05 figure (k = 0.82) was taken in the space cinematic, a
    different scene, so the two are not a before and after.
- **Fence wait sites on the title screen** (50-60 s, 243 frames, 4.0 ms per
  frame in all): the wait inside `CommandProcessor::WriteBack` 1.7 ms,
  `WAIT_REG_MEM` on a producer submission 1.2 ms, waits outside the tagged
  functions 0.7 ms, a full submission ring 0.3 ms.
  - No site reaches 2 ms and the top two wait for GPU results.
  - The frame was 44.4 ms with the GPU busy 62%: the processor computes
    there rather than waits.
  - The same scene ran 38.5 ms per frame in the spin run, so one pair cannot
    show a change under about 6 ms per frame.
- **Fence wait sites in the dunes** (the crash-site dunes, 98-108 s,
  135 frames, 74.9 ms per frame, fence waits 15.9 ms per frame). The
  per-site dump does not align exactly with the per-second samples, so the
  sites are given as shares of the dumped fence time:

  | Site | Share of fence time |
  | --- | --- |
  | Device-address draw retry, storage publication refused: a Texture object aliases the storage bytes (`StorageIsWholeGuestContent`) | 49% (774 waits) |
  | Full submission ring (`SubmitCurrentLocked`) | 20% |
  | Outside the tagged functions | 15% |
  | `WriteBack` after a guest read (`ProcessorWriteBackRequired`) | 9% |
  | `WAIT_REG_MEM` on a producer submission | 7% |

  - This window is the ramp after the level load (6 to 19 fps). Its first
    seconds upload 420-550 MB per second at 6-12 fps with the GPU 39-62%
    busy, a load transient bound by the command processor.
- **The steady dunes are mostly GPU-bound.** From 105 to 111 s the frame
  rate held 19 fps (53.2 ms per frame) at 526-571 draws per frame, with the
  render engine busy 84-88% (fdinfo cycles; the compute engine was idle).
  - Draw and dispatch processing minus the 17.8 ms of fence waits came to
    25.2 ms per frame. That figure sums the graphics processor and the
    compute-pipe processors, which run on their own threads (see the last
    bullet), so it is not serial frame time.
  - Two earlier runs agree: seconds 112-115 of the 2026-11-10 vertex-search
    run (18-20 fps, 87-88% busy, 24-28 ms of summed processor work) and
    second 191 of the GPU-timestamp run (88% busy).
  - The command processor waits behind a nearly full GPU, so removing one
    wait site moves the wait.
  - This agrees with the exclusion of 2026-10-18, where allowing publication
    with older texture aliases moved the wait into `WaitRegMem32`.
- **Scene.** The capture at 117 s is black, because the route then opens a
  pause cinematic. The dunes are identified by the level the guest log
  reports loading and by counters that match the earlier dunes runs (500-780
  draws per frame).
- **Two dune views reconcile the GPU records.** In the GPU-timestamp run
  (`e82878229`, an earlier build), the 5 s dumps at 12.6-14.8 fps hold both
  large compute programs every frame: the BVH kernel `0x3666fa39` 23-27 ms
  per call, `0x874bd13f` 11-13 ms, and 25-27 ms of everything else, 93% of
  wall time. That is the first column of the 2026-11-09 table ("where the
  dunes frame goes"). The steady 19 fps view matches its dunes column
  instead: GPU 46-50 ms with `0x3666fa39` about 20 ms and `0x874bd13f`
  absent. Work outside the pair is 25-30 ms in all three measurements. No
  timestamp run confirms the split on the current build.
- **Ceiling at native resolution.** 60 fps needs, together, the BVH kernel
  from about 20 ms to near zero, the rest of the GPU from about 26 to under
  16 ms with no other program above about 2.4 ms per frame, and a command
  processor that keeps up with a 16.7 ms frame; no serial measurement here
  shows whether it can. With the BVH kernel gone entirely
  the GPU would still take about 26 ms per frame.
  The recorded levers on this program class are 8-14% each, and a native
  lane mapping needs reconvergence guarantees that are not established, so
  no wave64 lowering design is written. The one lever of that size is
  internal resolution: at a 960x540 game patch the BVH kernel took 6.45
  instead of 22.64 ms ("Internal resolution is the lever", measured then at
  16.6-18.1 fps against 12-13; 37-41 fps in one scene on 2026-11-14 and 23-34 fps in three others,
  records at the top). The GPU sum there was
  still 36.0 ms per frame, so lower resolution gains fps but does not reach
  60 either.
- **Decision: no command-processor change for the dunes.**
  - At 84-88% busy the GPU idles about 7 ms per frame. Removing all of that
    idle time would take the steady dunes from 19 to about 22 fps at native
    resolution and today's GPU cost. That is the most any command-processor
    change can give there.
  - On the title screen, k is 0.5 and every remaining CPU leaf is under 2 ms
    per frame. The per-shader recipe stays excluded (stage 2 in
    `docs/bringup/cp-draw-plan.md`).
  - Publication with a Texture alias is not built.
  - The remaining levers are GPU work.
- **Revisited on an idle dune view (2026-11-10).** The decision above holds for the
  moving route at 19 fps; it does not hold where the view stands still.
  - With the route's input cut at 90 s, the robot stays at the crash-site spawn
    point. There a frame costs about 72 ms (13.1-14.1 fps over five runs), the
    render engine is 58-62 % busy, and the graphics CP thread is blocked about
    35 ms per frame.
  - The block is the host read of one `DRAW_INDEX_INDIRECT` record per frame
    behind the BVH kernel. Removing it together with a deeper submission ring
    gave 17.6 fps in a probe that also skipped an empty draw, so that figure is
    an upper bound and not a gain.
  - The record, the probe arms and the device-side draw are in
    `docs/bringup/cp-gpu-sync-chain.md` ("Link 2 reopened"). The device-side
    draw was first deferred and then built: a device converter writes the
    geometry block and the group count, and the pass reserves only the groups
    that fit the 1 GiB output cap. In one run that switched from the CPU path
    to the device path at 101 s, the host argument reads disappeared and the
    view went from 11.9 to 15.1 fps. That rate change is an upper bound,
    because the first arm carried the comparison harness. Without the harness
    the device path gave 16.1 fps, inside the range of earlier runs, so the
    frame-rate gain is not established. Render-engine time stayed at 43.4 ms
    per frame, and the processor (64-68 ms per frame) is still the limit.
  - Every device-read record in the dune runs was empty. Records with work
    occur only during the boot logos and are host-current. Forced down the
    device path, 191 of them matched the host header exactly, and 307 passes
    of 512 groups ran without a fault.
  - The frame rate moved without an intended change: two later runs of a
    rebuilt binary gave 16.5 and 17.3 fps, with the audio thread about 15 %
    cheaper per second. The cause is open (rebuild, time and host load are
    confounded). The 13.1-14.1 fps baseline is not current; re-measure before
    comparing. Render-engine time stayed at 42-44 ms per frame, which caps
    this view near 23 fps whatever the processor does.
- **GPU bill on the idle dune view (2026-11-10).** Same idle route, window
  96-116 s, a temporary probe writing a bottom-of-pipe timestamp after every
  draw and dispatch in every chunk of a recording. Each item takes the
  interval since the previous mark in GPU time order, so it includes gaps and
  unmarked work before it; "tail" is the mark at the end of each recording.
  The binary was built from 799fb76fd plus the command-processor counter fix
  (88c21e1b9, the only code commit on the branch after it) and the temporary
  probes. The run held 15.2 fps (the probe costs about 2-3 ms of engine time
  per frame). The timestamps sum to 44.4 ms per frame against 44.7 ms of
  render-engine time (busy x frame time) in the same run, so the bill
  accounts for the engine time. The compute engine stayed idle, so every
  queue's work ran on the render engine and that engine time covers all of
  it. Recordings never overlapped (span union = span sum). The 67 recordings
  per frame match the run's per-site submit counter, normalized by the flip
  flush count: 56.5 submits per flip on the graphics queue and 6.3 on three
  compute queues. Of the graphics ones, 29.3 come from
  `SubmitRecordedWorkIfGpuIdle` (it flushes when the queue has nothing
  pending, after at least 8 operations and 500 us), 19.5 from
  `SubmitRecordedDispatchesBeforeDraw` and 7.7 from the other flush sites
  (5.0 of them at the end of each guest submission, 1.0 at the flip).
  These are the run's 99-109 s window. The "about 36" graphics submits
  quoted in `cp-gpu-sync-chain.md` came from the earlier probe-arm runs,
  about 20 before draws and 9 idle-GPU flushes, so the difference is the
  idle-GPU rule firing more often. In this run that rule gave 6.9, 20.1 and
  29.3 flushes per flip in the last three windows, while ring-full waits
  held the command processor 388, 5 and 0 ms of each 10 s; the rule's
  trigger was not traced.
- **Same window on `aa282cbf4`** (strict, temporary probes, 2026-11-10,
  97-116 s): 13.4 fps and 554 draws per frame (554-561 in the runs above),
  but the guest's work differed: 121.7 guest dispatch packets per frame
  against 103.1-103.5, 168 `WAIT_REG_MEM` against 152-153, new pipelines
  in the window and 23 MB of uploads per frame against about 3 MB. The
  graphics queue flushed 44.9 times per flip in the 99-109 s window against
  56.5; the difference is the idle-GPU rule (19.5 against 28.2-29.3), and
  no flush site disappeared. Render-engine time was about 50 ms per frame
  against 42-45 ms; with different guest work, 1.5 against 0.5 CPUs of host
  load outside the process and no GPU clock sampled, that is not a build
  effect. Other DRM clients (read from the xe `fdinfo` cycle counters) used
  0.6-1.6% of the render engine's cycles over the run.
- **After about 116 s the route leaves the still view.** The robot lies
  down on the sand with the camera close (capture at 205 s), draws fall to
  about 370 per frame and render-engine time rises to 57-59 ms per frame.
  Measurements of the still dune view must end by 116 s. A capture shortly
  after that cut can be entirely black (all channels 0): runs on seven
  older binaries (`dc07a6bcb`, `f53efbf48`, `d7828ae27`, `ab946a887`,
  `d4b3548bf`, `a90c9c906`, `7b2e91397`) show it at 340-430 draws per
  frame, and later captures in the same scene show the image again. It is
  the title's fade, not a rendering failure.
- **Excluded: a uniform per-draw GPU overhead behind the 20 us mean of
  indexed draws.** Same run, 100-116 s, split by program pair (the probe's
  top list covers 98 % of indexed-draw time). The time follows the
  programs: the largest pair takes 48.6 us per draw and 30 % of the indexed
  time, others take 115 and 525 us, and frequent pairs take 4.5 and 7.9 us
  per draw including the probe's own mark. A floor that low excludes only a
  cost every draw pays. A cost tied to pass begins or to dispatch-to-draw
  switches would land on particular pairs and is not separated from their
  shading, because the probe's output does not record which draw or
  dispatch preceded each one.

  | Per frame | Time | Count | Mean |
  | --- | --- | --- | --- |
  | Compute dispatches | 25.9 ms | 56 | 465 us |
  | of which BVH traversal `0x3666fa39` | 19.7 ms | 1.0 | 19.7 ms |
  | of which the next one, `0xff2adfc8` | 1.4 ms | 1.0 | 1.4 ms |
  | Indexed draws | 10.1 ms | 508 | 20 us |
  | of which the largest program pair | 3.0 ms | 61 | 49 us |
  | Recording tails | 2.9 ms | 67 | 44 us |
  | Non-indexed draws | 2.9 ms | 71 | 41 us |
  | Geometry passes | 2.0 ms | 33 | 61 us |
  | Device-fed dispatches | 0.6 ms | 5 | 112 us |

  Outside the BVH kernel no single item reaches 5 ms per frame: the other
  half of the GPU time is spread over about 740 marked items. 30 fps on this
  view needs about 10 ms per frame off the GPU. The BVH levers in the records
  do not reach that: the shared arrival counter and the eight dropped LDS
  orderings are excluded below (about 0.2 and 0.5 ms per call), and keeping
  cold guest registers in shared memory is untested with no estimate.
  - A run with the 960x540 patch on this date lost its timing on the moving
    route: the window held a load, not the dunes. It measured nothing, so the
    figures in the ceiling bullet above remain the last ones.
- **Not attributed.** The processor's counters exceed its user-mode samples
  by 3.6-7.3 ms per frame (cpu-clock:u at 199 Hz, 66-72 s). The dispatch
  counters include the compute-pipe threads, so part of that gap may be
  theirs. Kernel stacks need `kernel.perf_event_paranoid` at 1 or lower.
- **Processor counters are thread time summed over processors.**
  - `88c21e1b9` times only the outermost `CommandProcessor::Run`. Before
    it, the nested runs of `COND_INDIRECT_BUFFER` and `INDIRECT_BUFFER`
    (`GraphicsRunOpParsers.cpp:804` and `:831`) were counted twice.
  - That double count does not explain the excess seen in the dunes (83.1
    against 74.6 ms per frame). The route shows no measurable nested runs:
    runs per present stayed at 16.9-18.5 in every 10 s window from 60 to 116 s,
    before and after the change. From 105 to 116 s the counter was 1.06
    times wall time in both runs (17.0 and 17.3 fps).
  - The change is verified by the build and by reading the code. No test
    and no run on this route exercises a nested run.
  - The excess has another source. The graphics ring and each compute pipe
    have their own processor (`Gpu::GetRing`), and each runs on its own
    thread (`ComputeRing::ThreadRun`) under its own run lock. The counter
    adds them up, so it can exceed wall time. The dispatch and fence
    counters read the same way.

## Robot platformer: geometry passes batched into compute segments (2026-11-09)

Same title, host, route and settings as the next record. The change is
`59e3640cf` on `40d8c109f`; the comparison arm is `40d8c109f` built clean in
the same build directory (Release, without link-time optimisation).

- **Change.** A geometry pass whose pipeline writes no guest memory (no
  buffer or image store or atomic, no GDS buffer bound) is recorded into a
  compute-only command buffer, a segment, that is submitted ahead of the
  chunk holding its pull draw. The render pass is no longer split per pass.
  Consecutive passes join one segment while they write contiguous ranges of
  the same output buffer and no hazard was recorded between them. A hazard
  is any command recorded outside a render pass, a command-processor packet
  outside draws, register writes, index state and buffer chaining, a draw
  whose pipeline writes guest memory, or a pending pass that samples an
  image written by a render pass begun since the segment opened. The chunk
  before the segment zero-fills the segment's output range; the segment
  ends with a compute-to-vertex barrier. A pass is recorded inline as before
  when its pipeline writes guest memory or when the slot's chunk budget is
  spent. The parser decodes no GLOBAL or FLAT store: such an opcode ends
  the run as unknown, so it cannot reach a pass unclassified.
- **Segments over the route** (temporary counters, whole run): 120,938
  passes in 15,880 segments, 7.6 passes per segment. Segments of 1 pass:
  5,371; 2: 888; 3-4: 866; 5-8: 4,553; 9-16: 2,341; 17-32: 853; 33 or more:
  1,008. 4,175 draws broke a segment because their pipeline writes guest
  memory. No geometry pipeline wrote guest memory and no pass hit the chunk
  budget, so neither inline branch ran in this title. The sampled-image rule
  found 0 hits in 108,182 checks against a non-empty attachment list; a
  temporary build that seeded the list with each sampled image hit 1,691 of
  1,691.
- **Segment results equal inline results.** A temporary build (not
  committed) re-ran sampled segment passes inline into a
  scratch buffer and compared the bytes. Controls (the same pass with one
  group fewer) were detected 40 of 40 times. In a bracket run, which
  re-ran each sampled pass inline right before (P) and right after (A) its
  segment execution (S), no row had P equal to A with S different (0 of
  422). Every program except `360038d0f4cfb9a5` matched in every run.
- **Open item: `360038d0f4cfb9a5` output depends on execution order.** It
  binds one constant storage buffer and one sampled texture and is not
  indexed. Under the compare instrument the first of its executions is the
  one that differs most often, whether that is the segment or an inline
  copy. One run executed the segment (S), then two inline copies (A, B):
  S was the odd one out 44 times, A 11 and B 3. The bracket run executed P,
  S, then A: P was odd 65 times, S 0 and A 4. This is independent of
  batching. It was observed under the compare instrument only and was not
  measured on the base build. Cause unknown.
- **Validation.** One strict 105 s run of the route under the Khronos
  validation layer, built before the sampled-image rule was added, ended
  with exit status 0. It is not known whether that slower run reached the
  dunes. The layer was loaded: it reported the recorded VUIDs 09600
  (`docs/bringup/cross-title.md`) and 07753, 07752 and 02691 (this file).
  Its only other message was
  `VUID-VkPipelineDepthStencilStateCreateInfo-depthBoundsTestEnable-00598`
  (12 times), from `GraphicsRenderPipeline.cpp:828`, which enables the
  depth bounds test from guest state; this change does not touch that path,
  and it was not checked on the base build. Suggested direction: enable the
  `depthBounds` device feature where the host has it, and refuse explicitly
  where it does not.
- **Frame rate**, one matched pair on the user's dunes route. Settings:
  60 fps cap, `PrintfDirection = Silent`, translator unchanged.
  Independent runs of one build earlier ranged from 11 to 13.5 fps in the
  same scene, so one pair does not separate a gain of this size from noise.

  | Route window | `40d8c109f` | `59e3640cf` |
  | --- | --- | --- |
  | 60-118 s: render engine busy per frame | 38.7 ms | 34.6 ms |
  | 60-118 s: command processor running per frame | 55.2 ms | 50.5 ms |
  | 60-118 s: frame rate | 17.3 fps | 18.9 fps |
  | Dunes (samples from 60 s with 500 or more draws per frame, 24 each) | 11.42 fps | 13.19 fps |

  The skip-the-pass bound in the next record allows about +22 %. 60 fps is
  still far away: the command processor stays above 50 ms per frame in
  this build without link-time optimisation. The gain on an optimised build
  was not measured.
- **Other titles**: `docs/bringup/cross-title.md`, "Geometry segments on
  other titles (2026-11-09)".

## Robot platformer: geometry passes are latency-bound and overlap (2026-11-09)

Same title, host, route and settings as the next record. Executables built
from `b76f4a71e` plus `4cf83a445` and temporary diagnostics only.

- **Shape.** Geometry passes emulate the guest's merged ES+GS stage as a
  compute dispatch before each pull draw. In the dunes, `0xf4cfb9a5` passes
  all have one group: one guest wave of 64 lanes, eight 8-lane host
  subgroups, at most 16 primitives. Across the whole route its passes have
  1-128 groups.
- **Compile statistics** (ANV through `VK_KHR_pipeline_executable_properties`,
  a copy of each geometry pipeline created with statistics capture):

  | Geometry program | Width | Instructions | SENDs | Loops | Spills:fills | Max live |
  | --- | --- | --- | --- | --- | --- | --- |
  | `0xf4cfb9a5` | SIMD8 | 44,571 | 2,151 | 292 | 862:1,132 | 274 |
  | `0x8e51dd94` | SIMD8 | 46,438 | 2,335 | 315 | 791:1,053 | 287 |
  | `0x0e89d742` | SIMD8 | 18,069 | 490 | 35 | 367:975 | 242 |
  | `0xfc3d1580` | SIMD16 | 18,195 | 953 | 123 | 311:1,067 | 286 |

  The rule set before the run (at least 100 fills means register pressure;
  under about 16k invocations per pass means the pass cannot fill the GPU)
  fired on both branches, so it did not choose a lever. The next run did.
- **Latency-bound.** GPU timestamps by group count, whole route: passes of
  2, 3, 4, 5, 9, 50 and 63 groups all cost 106-113 us; 128 groups cost
  360 us. A pass of 2 groups runs for about as long as a pass of 63.
  One-group passes cost more, 165 us over the route and 225 us in the dunes.
- **Consecutive passes overlap.** Prediction set before the run: two copies
  of the same dispatch, recorded back to back with no barrier, cost under
  1.3x one copy if passes can overlap and over 1.7x if they serialize. A
  temporary build issued every geometry dispatch twice in alternate 5 s
  windows of the dunes, only for programs whose guest code has no memory
  store, atomic or GDS instruction (all four above). `0xf4cfb9a5`: 233 us
  doubled against 225 us single (1.04x, 10,470 and 13,903 passes);
  `0x8e51dd94`: 97 against 98 us.
- **Passes come in long runs.** A counter ended a run at every packet other
  than draws, register writes, index state and buffer chaining. In 40 s of
  dunes (494 frames): 65.6 geometry passes and 3.6 runs per frame, mean run
  18.3 passes; 24,116 of 32,400 passes were in runs of 32-63. Every run ended
  at `EVENT_WRITE`.
- **What this bounds.** Today each pass is followed by its pull draw, with a
  render pass break and barriers around it, so the GPU runs one wave for
  about 225 us about 66 times a frame. Running the passes of a run together
  would cost about one pass latency per run. That bounds the gain at about
  10-11 ms of GPU time per frame (about 13.6 to 15.8 fps at the recording's
  73 ms frame), not counting the render pass breaks it would also remove.
  Not built at the time (built since: see the record above); the hazards a
  design must respect are the guest's
  synchronization events that end each run.
- **Passes per recording.** A batch cannot cross a host recording, so a
  second counter closed its count at every command-buffer end. In 40 s of
  dunes, 94 % of 38,895 passes were in recordings that held 8 or more
  passes: 64 % in 8-15, 17 % in 16-31, 13 % in 32-63. Most recordings hold
  none (29,758 of 34,000). The recordings that hold passes are closed by
  the idle-GPU submit before a draw (passes of 20,450) and by the submit of
  recorded dispatches before a draw (18,006).
- **Upper bound by skipping the pass.** In alternate 5 s windows of the
  dunes a temporary build skipped every geometry dispatch and kept the fill,
  barriers and pull draw. A zero-filled group allocates no primitive, so
  those draws drew nothing. Skip windows ran 15.00 against 12.31 fps (arm
  medians); each skip window against the mean of its two neighbours gave
  1.11-1.26, median 1.19. Fence waits were 22.3 against 31.0 ms per frame.
  This also drops the draws' pixel work, so it bounds any geometry-pass
  change from above; the prediction set beforehand was to exclude batching
  below +8 %.

## Robot platformer: a dunes route, and where the dunes frame goes (2026-11-09)

Same title, host and settings as the next record. Executables built from
`b76f4a71e` plus `4cf83a445` and temporary diagnostics only, compiled in but
off unless stated.

- **A route with gameplay input.** The user recorded a route in pad-record
  mode (`docs/input-replay.md`). It walks from the crash site into the open
  dunes at about 94 s and plays there until about 140 s. A replay reached the
  dunes again at about 94 s. Over 103-140 s, with frames counted from
  presented flips:

  | 103-140 s | Recording, diagnostics off | Replay, GPU timestamps on |
  | --- | --- | --- |
  | Frame rate | 13.6 fps (73.3 ms) | 12.2 fps (82.0 ms) |
  | Draws per frame | 628 | 693 |
  | Render engine busy | 89 % (about 65.5 ms) | 84 % (about 68.7 ms) |
  | Command processor running | 75.0 ms | 82.3 ms |
  | of which fence waits | 33.1 ms | 31.7 ms |

  The replay is not frame-identical to the recording (65 more draws per
  frame), so the two columns bound the instrument cost only loosely: at most
  about 3 ms of GPU time per frame. The frame is over budget on both sides.
  The GPU needs about 65 ms per frame. The command processor's time outside
  fence waits is 42-51 ms. 60 fps needs the GPU cut by about 4x and that
  time by 2.5-3x; removing either bound alone leaves the other above 40 ms.
- **GPU timestamps by kind, dunes only.** Replay, the 41 s of timestamp dumps
  inside the dunes, per frame at 12.2 fps. A mark follows every draw and
  dispatch, and each item includes the gap since the previous mark:

  | Per frame | Time | Count | Mean |
  | --- | --- | --- | --- |
  | Compute dispatches | 25.1 ms | 57 | 443 us |
  | of which BVH traversal `0x3666fa39` | 18.7 ms | 0.9 | 20.1 ms |
  | Indexed draws | 16.8 ms | 539 | 31 us |
  | Geometry passes | 12.8 ms | 63 | 203 us |
  | of which `0xf4cfb9a5` | 10.8 ms | 47 | 231 us |
  | Non-indexed draws | 4.8 ms | 98 | 49 us |
  | Draw tails | 2.7 ms | 52 | 52 us |

  `0x3666fa39` alone exceeds the 16.7 ms budget. The geometry program
  `0xf4cfb9a5` costs about 10-11 ms per frame in both scenes measured: 76
  passes of about 145 us in the scene of the next record, 47 of about 231 us
  here. The record above shows that a pass costs the same at 2 and at 63
  groups, so these totals do not mean the cost follows the work per pass.
- **Excluded: the compute-after-draw transition.** Hypothesis: a compute
  dispatch recorded after a draw pays a pipeline switch that a dispatch after
  another dispatch does not, so grouping geometry dispatches would save it.
  Prediction set before the run: the same compute program costs at least
  twice as much after a draw as after a dispatch. The timestamp instrument
  split each dispatch by what preceded it in the command buffer. No program
  reached the ratio. Mean per call by predecessor:

  | Program | Buffer start | After a dispatch | After a draw |
  | --- | --- | --- | --- |
  | `0x90c95e2d` | 694 us | 817 us | - |
  | `0x108c4737` | 578 us | 573 us | - |
  | `0x82990f59` | - | 288 us | 278 us |
  | `0x8723fe62` | 346 us | 163 us | - |
  | Geometry `0xf4cfb9a5` | 190 us | - | 139 us |
  | Geometry `0x8e51dd94` | 110 us | - | 97 us |

  `0xf4cfb9a5` never followed another dispatch, so its own case is untested.
  The command processor reads of indirect-draw records that dominate the
  fence waits were already excluded as a frame-rate lever on 2026-11-02
  ("real gameplay frame rate" record: the waits move).

## Robot platformer: per-frame CPU read of indirect-draw records (2026-11-09)

Same title, host, route and settings as the next record; executables built
from `b76f4a71e` plus `4cf83a445` and temporary diagnostics only.

- **The largest fence wait reads indirect-draw records.** A temporary
  instrument logged the call stack of every fence wait of 5 ms or more. In the
  GPU-bound scene (about 375 draws per frame), the top site was
  `DrawIndexIndirect` reading its argument record from guest memory on the CPU
  (`ReadCurrentGuestBytes`, then `BufferWait`), which drains the queue. It
  happens about once per frame, waits about 18 ms, and follows the
  BVH traversal kernel `0x3666fa39`.
- **Why the device path refuses those draws.** Counters over a whole run:
  7,125 indexed indirect draws. 1,235 were recorded with device-side arguments
  and 5,888 were read on the CPU, 5,887 of them because the draw takes the
  geometry pass. That pass sizes its output buffer and its dispatch from the
  primitive and instance counts, so it needs the record on the CPU.
- **Excluded in the GPU-bound scene: skipping the read.** A temporary build
  switched every 5 s of wall time between the normal read and skipping the
  draw. Skipping is valid only because every record read in the window was
  empty: index count 1 and instance count 0, in 235 of 235. In that scene,
  with the GPU busy 94-97 % of wall time, the skip arm ran at 16.29 fps
  against 15.86 (+2.7 %), with fence waits of 37.7 against 38.8 ms per frame.
  The wait moved to the command-buffer ring: the processor waits for a free
  buffer of its eight while the GPU is busy. Geometry passes in that run used
  at most 512 groups and 8.9 MB of output, with 4,353 dwords per group. That
  build switched arms from boot. Before the window the normal arm read 174
  records that were not empty, so the skip arm may have dropped real draws
  there.
- **The route does not control the scene after 150 s.** It has no gameplay
  input: its last five presses are pause-menu inputs at 146-186 s, with
  nothing after. The "dunes" figures in these records therefore come from an
  idle scene: the robot standing in the open dunes, about 510-780 draws per
  frame. Of seven runs, three were in that scene from 190 s or 212 s on. Four
  were from 190 s to 250 s in an unidentified scene of about 375 draws per
  frame, with the robot lying in the sand. Two of those four were sampled
  earlier:
  - both spent 16-18 unpaused seconds above 500 draws per frame;
  - both spent about 10 s at 60 fps with under 100 draws per frame, between
    the `options` presses at 175.9 and 186.2 s, probably the pause menu;
  - in one, the 375-draw scene appeared just before that pause; in the
    other, just after it. So the pause does not trigger it.

  That scene is not the frame of the user's pause-menu freeze, which shows
  the sky during a cinematic.
- **Dunes: the same wait, the skip not measured there.** The read is also
  the top wait site in the dunes. A run in the dunes (190-250 s) had 689 of its
  691 waits of 5 ms or more with the same queue, the same preceding kernel and
  the same lag after `0x3666fa39` (median 17.7 ms). The 1 s samples of the
  three runs in the dunes bound the gain of removing every wait at 23-34 %.
  A second skip run started skipping 155 s into the run. It reached only the
  16 s span above 500 draws per frame, which a run without the skip also
  shows; that span is not the idle dunes scene of the three runs. In that span:
  - the read waited little (median 5.4 ms);
  - draws fell and the frame rate rose throughout;
  - adjacent windows, from five skip samples, gave about 0 % (skip at
    164-166 s, 17.7-18.7 fps, against 18.4 fps normal at 168-171 s) and about
    +8 % (skip at 173-174 s, 19.8-20.2 fps);
  - fence waits per frame did not fall (about 22 ms normal, 26-34 ms
    skipping).

  The dunes scene remains unmeasured. Without that measurement the device
  path for geometry-pass draws is not implemented. Measuring it needs a
  route with gameplay input that walks into the open dunes and stays there
  for 60 s or more without pausing, recorded on the executable and shader
  caches of the measurement (`docs/input-replay.md`).

## Robot platformer: paired kernel measurement, arena refusal fixed (2026-11-09)

Applies to `PPSA21567 - ASTRO BOT` 01.018, Intel A770, Linux, Mesa 26.2.6,
strict 60 fps-limited runs on the user route. Executables built from
`b76f4a71e` plus temporary diagnostics only; the arena fix (`4cf83a445`) was
added for the last run.

- **Method: two translations in one run.** A temporary instrument (not
  committed) translates one compute kernel twice, with a marker added to the
  module key, and switches between the two each second of wall time. GPU
  timestamps attribute each dispatch to its translation, and every 5 s dump
  gives a pair measured in the same scene under the same host load. The
  ratios of the compared dumps stayed within 1.026-1.062 and 0.911-0.947 in
  the two runs below, while the kernel took 18.5-23.3 ms per call across separate runs, so
  a few percent per kernel is measurable without repeated whole runs. Both arms compile fresh; the rest
  of the shader store was warm from a run that held the uncommitted exchange
  edit, so absolute times are not quoted as the tip's.
- **Rendezvous changes, per kernel.** For the tile-lighting kernel
  `0x3666fa39` (the largest BVH-family module), the translation of
  `77dd4ce0c` against the same translator with part-local skips, compare
  chains across side exits and conflict-ordered LDS turned off. In two
  windows of the route (5 s dumps 38-42 and 43-52) it took 23.1 against
  25.0 ms and 18.8 against 20.2 ms per call; median ratio 0.930 over 20 paired
  dumps, every dump of both windows between 0.911 and 0.947. At about one call per frame this is
  about 1.4 ms of a 65 ms frame, consistent with the frame rate staying within
  the noise of separate runs (previous record).
- **Excluded: loading a part's own word without a select.** Letting
  `SplitWavePartWords` load every word, its own included, instead of
  selecting its own value: 19.2 against 18.6 ms per call over the same part of
  the route (dumps 37-51); median ratio 1.031 over 19 paired dumps, each of
  those dumps slower (1.026-1.062).
- **What bounds the frame, by window** (the arena run: GPU timestamps and
  1 s performance samples, on a build with diagnostics):

  | Per frame | 188-207 s | 215-262 s (dunes) |
  | --- | --- | --- |
  | Frame rate | 15.0 fps | 13.2 fps |
  | GPU busy | 92 % of wall time | 46-50 ms, 15-32 ms idle |
  | Command processor active (run minus waits) | 9.7 ms | 50.7 ms |
  | Fence waits | 42.6 ms | 27.1 ms |
  | Draws / dispatches | 360 / 86 | 639 / 83 |
  | `0x3666fa39` / `0x874bd13f` | 24.1 / 12.4 ms | 19.6 ms / absent |

  The first window is GPU-bound. In the dunes the processor's active time
  alone (about 79 us per draw; state setup 13.0 and resource binding 12.75 ms
  per frame lead) is as long as the GPU's busy time, and fence waits add
  27 ms on top: the two run in series. A kernel cut there shortens the frame
  only through shorter waits; overlapping them would bring the frame to about
  50 ms. In dumps 47-50 the geometry kernel `0x360038d0f4cfb9a5` grew from 3.0
  to 20.4 ms per frame; not traced.
- **Where the kernel spends its time.** A temporary build that forces a miss
  at every BVH node ran the kernel at 23.6 against 24.7 ms per call in the same
  scene, so the node visits after the first cost about 1-2 ms (the trace
  region as a whole is priced in the 2026-11-14 region record: 11.4 of
  15.6 ms). The rest was read then as straight-line
  shading (about 1,550 VALU instructions), split-wave exchanges and register
  spills (8 KB of scratch per thread); the kernel is latency-bound. At
  960x540 internal resolution (game patch, not strict) it takes 6.45 ms.
- **Arena refusal fixed (`4cf83a445`).** A run stopped at 150 s, in the pause
  cinematic, with `!create_all_the_same`: a 3840x2160 RGBA16F (mode 27)
  storage view of 0x3fc0000 bytes at 0x53de40000 over 40 parents. 39 were
  render targets and storage views crossing or inside it, which the arena
  rule links. The 40th was a 640-wide sampled texture inside it, uploaded by
  the CPU and with no device write. The arena rule accepted only storage
  buffers as guest-byte caches. It now also accepts the CPU-texture link the
  single-parent and alias paths use: guest memory holds the content, no
  device write followed the upload, and no write-back is pending. With the fix
  the same route ran to its planned 265 s close. A temporary marker showed
  the predicate true for CPU textures in 8 multi-parent creations (the cap),
  one with 35 parents. It does not show that no other clause admitted them.
  The `GpuMemory` and `GraphicsState` unit suites pass (332 tests).
- **Open.** The user's pause-menu freeze was in the same scene. This refusal
  is a candidate cause, not a proven one. The same route without the fix had
  passed 150 s once, so the stop is intermittent.

## Robot platformer: fewer split-wave rendezvous, two refusals fixed (2026-11-09)

Applies to `PPSA21567 - ASTRO BOT` 01.018, Intel A770, Linux, Mesa 26.2.6,
strict 60 fps-limited runs on the user route. Commits `3d0b733a2`
(lookup memo), `91be7a38a` (depth link) and `77dd4ce0c` (split-wave
rendezvous, translator 210). The measured executables also held unrelated
uncommitted GPU-memory and split-wave exchange edits of the worktree.

- **Part-local skips.** A forward `S_CBRANCH_EXECZ` needed a whole-wave EXEC
  test, so every part met at each one. A part with no active lane now skips
  the range alone when the range is entered only through the branch, holds no
  rendezvous and defines no scalar word that is live at the target. Covered by
  four `EmulatorShaderMaskAnalysis.PartLocalSkip*` tests.
- **Conflict-ordered LDS.** Every LDS access had a rendezvous before and
  after it. Accesses now order across parts only where two conflict: one
  writes and their bytes may meet in different lanes, with addresses taken as
  affine forms of the thread coordinates. Reads, disjoint ranges and each
  lane's own slot keep no rendezvous; `S_BARRIER` orders everything across it.
  Covered by the `EmulatorShaderSplitLdsOrder` tests.
- **Compare chains across side exits.** The BVH traversal kernel tests up to
  four child pointers with `V_CMP_NE_U32 vcc, -1, vN` separated by
  `S_CMP_LG_U64 vcc, 0` and a branch that only leaves the program. Those
  compares now vote in one exchange. Static rendezvous sites in the largest
  BVH-family module went from 114 to 108 (single ballots 69 -> 61, two more
  grouped exchanges); the other family members from 113, 111, 107 and 107 to
  107, 105, 101 and 101. The device test
  `EmulatorPackedMipNative.SplitWaveGroupedLaneReadsAndComparesReturnWholeWaveWords`
  now places a side exit inside its compare run and checks that the four
  compares share one exchange and return whole-wave words.
- **Frame effect: small, single runs.** Part-local skips with LDS order
  (translator 209, warm store) ran the dunes at 15.15 fps against earlier
  baselines of 14.1-14.7 fps, within their spread. With compare chains and
  both fixes (translator 210), the dunes window from 212 to 250 s ran at
  15.50 fps in the same scene as the 15.15 fps run; per-frame CPU costs were
  equal within noise (descriptor and texture binding 1.81 against 1.73 ms).
  Fewer rendezvous barely move the frame, so their count is not what bounds
  the dunes.

**Lookup memo fix (`3d0b733a2`).** The intermittent `!create_all_the_same`
stop of the 2026-11-08 record follows this sequence on the recorded object
tables. The sampled-lookup miss memo
stored misses that came from the write order of storage images
("latest storage image has another format", "render target yields to the
newer chain") as if they were properties of the object graph. Recorded
sequence: a depth-format storage view B was created over the range; an
A-layout request with seed skipping saw B as the latest, missed and stored
the miss; its creation reused the equal storage image A, which became the
latest by stamp (146 and 147 ticks after B's creation in two runs); a later
A-layout request with the same key and a required seed hit the memo, skipped
the lookup that would now find A, and created an object with mixed parents.
A diagnostic counted lookups that the fix sends past the memo in a live run.
A miss is no longer stored when the latest storage image of the range was
consulted.

**Depth link (`91be7a38a`).** The `DepthStencilBuffer - Contains -
RenderTexture` refusal (a 960x540 colour target at the base of a 1216x684
depth target, recorded below) stopped one pause run at 151 s. The relation
now links like the `Crosses` form already did: both host images stay, the
depth target is not reclaimed and no bytes are converted. This removes a
fatal stop; it does not establish the content provenance the earlier record
asks for. If the guest reads through one image what it wrote through the
other, Kyty still does not convert it. The verification run did not reach
the relation, so the linked path has not run live.

**Still open.**

- A pause run stopped at 162 s with `GpuMemory write-back requested before
  exact resource dependencies completed` (storage buffer, pending queue 8).
  The stack diagnostic was not hit again before it was removed; no cause.
- A pause in the descent cinematic that froze the game for the user did not
  reproduce in four bounded runs: the pause menu drew and resumed at 128 s,
  a gameplay pause worked, and one press landed as the cinematic ended. The
  guest threads of the frozen run matched a healthy paused run. Live input
  over the time-based route is a hypothesis, not a cause.
- The verification run is one run; captures show the scene, not correctness
  of the BVH results.

## Robot platformer: remembered address-table entries excluded (2026-11-09)

Hypothesis: the BVH traversal kernel `0x3666fa39` waits on guest address
translation. Every node load first walks the device table (header, region
directory, binary search, entry): four or more dependent reads before the
node's own. Inside the traversal loop the only translated accesses are the
intersection's node loads and the runtime-descriptor buffer loads that pick
the next node, so two remembered entries per invocation should skip that
walk on almost every call.

- **Change tried.** `guest_device_address` kept the indices of the last two
  entries that held a whole access in two Private variables. A call first
  tested both remembered entries with the lookup's own fit test. An index of
  at least the table's count read the table's zero prefix and never matched.
  The header loads did not wait for that test. Entries are disjoint, so a hit
  returns what the search would. The GPU load probe repeated its load to
  exercise the hits; swapping the two remembered results failed four
  device-address tests, so the probe covered them. Translator 208, patch kept
  with the run evidence.
- **Offline, same recipe at SIMD8 against translator 207.** Instructions
  45,535 -> 47,355, loops 194 in both, spills:fills 626:2,668 -> 608:2,521.
  The module passes `spirv-val`.
- **Dunes, one run on a private store (the third after the version change).**
  The BVH kernel took 25.3-26.3 ms per call over the dune dumps (24.9 ms on
  translator 207's plateau). GPU time per frame was 61.8 ms against 62.3 ms,
  and the frame rate was 13.6 fps against 14.9 fps. That run was the first to
  compile the dune pipelines, and the GPU was 84 % busy against 91-93 %, so
  the frame rate is not comparable; the GPU figures do not move. The ship
  scene before the dunes ran 7.5-8.0 ms per call (8.3-8.7 ms before).
- **Excluded.** Removing the walk from nearly every node load leaves the
  kernel's time unchanged, so address translation is not what the BVH
  traversal waits on in the dunes. Table chunks are 64 MB, so the BVH sits in
  one or two entries and the remembered entries should hit. The hit rate was
  not measured.
- **Run notes.** On an empty private store, the first run stopped at the
  save-card menu at 340 s. The second was still in the ship cinematic at
  250 s and ended at 267 s on the known intermittent guest jump to address 0
  (`rip 0x9004a0c3e`, seen since 2026-10-26). The third reached the dunes.

## Robot platformer: BVH intersection runs only the node's layout (2026-11-09)

Applies to `PPSA21567 - ASTRO BOT` 01.018, Intel A770, Linux, Mesa 26.2.6,
strict 60 fps-limited runs on the user route to the dunes. Both arms are
`e82878229` with the same unrelated uncommitted GPU-memory edits and
diagnostic per-dispatch GPU timestamps (5 s dumps); the change arm adds this
lowering (translator 207 against 205). One run per arm.

- **Observed.** The BVH traversal kernel `0x3666fa39` keeps the node pointer
  in a scalar register and copies it to the address VGPR before each of its
  three `image_bvh_intersect_ray`, so every lane of a wave tests the same node
  and the node type is wave-uniform. The lowering computed the fp16 box, fp32
  box, child sort and triangle test (three divides) on every call, loaded all
  32 node words, and did so for lanes outside EXEC as well; only the result
  store was EXEC-gated.
- **Change.** Lanes outside EXEC skip the call. A switch on the node type
  (out-of-range nodes folded into their own cases) runs one layout: triangles
  load 16 words, boxes 28, out-of-range triangles return the miss constants,
  and user or out-of-range box nodes return four invalid pointers, as before.
- **Offline, same recipe at SIMD8.** Instructions 45,665 -> 45,535, loops
  188 -> 194 in both SPIR-V and ANV (one more address lookup per layout),
  spills:fills 596:3,055 -> 626:2,668, scratch messages inside the traversal
  loops 2,012 -> 1,827. The 19 stored modules with the lowering pass
  `spirv-val`.
- **Dunes, one run per arm.** Sampled frame rate over the whole dune
  window 13.95 -> 14.92 fps (+7 %), over its last 25 s 14.40 -> 14.96 fps
  (+4 %); GPU time per frame 65.0 -> 62.3 ms with the GPU 91-93 % busy. The
  BVH kernel's elapsed time per call, dump by dump from dune entry, fell from
  27.4 to 24.9 ms on the plateau (-9 %); that figure includes overlap with
  other queued work, and the 2026-10-30 records measured a run-to-run spread
  of 18.5-23.3 ms per call, so it is a direction, not an isolated cost.
- **Correctness.** Kept by construction (same arithmetic per layout, same
  out-of-range results), `spirv-val`, and the loop gate: all 47 split
  recipes of the run have as many ANV loops as inlined SPIR-V loops. The
  stop-time captures of both arms score healthy and show the same frame, but
  whether that frame depends on BVH results was not established (a build
  forcing every intersection to miss left earlier captures unchanged).
- **Run notes.** The first run of translator 207 lagged the time-based route
  (still in the opening cinematic at 340 s); the measured run is the second,
  on a warm store. A first build placed the result `OpPhi` after a store and
  failed pipeline creation at start; `spirv-val` reports it.
- **Not covered.** No test executes the lowering; the out-of-range and user
  node cases are kept by construction and are not exercised by the route.
  Each arm is one run.

## Robot platformer: relaxed split-wave exchange spins excluded (2026-11-09)

Hypothesis: the rendezvous behind ballots, lane reads and their groups could
spin on relaxed sequence loads, since the acquire fence after the loop already
orders the bank reads; only the plain synchronization would keep its acquiring
last load. ANV emits one SLM fence per acquire load, so this removes one fence
per spin iteration. Offline on `0x3666fa39` the loops stayed (188), SLM fences
went from 520 to 406 and the block loads were unchanged.

**Excluded.** Over the 39 split compute recipes the change produced in a
strict run, ANV reported fewer loops than the inlined SPIR-V holds in 27 (by
one in 26, by four in one). On one of them, making only the exchange spin's
last load acquire again restored the loop (10 -> 11). A spin of relaxed loads
whose returned sequence the caller never reads (a dead guest result) has no
effect the compiler must keep, so it is removed and that part does not wait.
With two banks, its next publish can then overwrite a bank that a slower part
still reads. A spin needs an effect the compiler keeps even when its result
is dead; translator 205 already has exactly one acquire per spin, so this
fence axis is closed.

**Run notes.** The strict run (diagnostic per-dispatch GPU timestamps) had no
device loss. It was the first run of a new translator version, so every
shader recompiled and the time-based route lagged by about 45 s; the BVH
kernel ran only 33 times and no per-call figure was comparable. The reference
band for that kernel, from an earlier timestamp run over the dunes, is a
median of 23.8 ms per call (10th-90th percentile 23.2-24.0 ms, about one call
per frame). Count the first run after a translator change as warm-up, and
gate loops over every split recipe offline before a game run when the change
can be simulated.

## Robot platformer: split-wave lane reads and compares share exchanges (2026-11-09)

`ddc3664b6` (translator 205). The BVH traversal compute shader `0x3666fa39`
runs as a guest wave64 split into 8-lane host subgroups on the Intel Arc
A770. Each wave-wide value cost one cross-subgroup rendezvous. After each ray
intersection it reads 2-4 lanes at one selector and evaluates runs of
compares whose masks feed scalar arithmetic. Runs of `V_READLANE` at one
selector now share one exchange: 44 reads form 32 groups, saving 12
exchanges per pass through the two traversal loops. Runs of adjacent
compares whose masks are not lane-local pack their votes and publish them
together. No such compare run forms in `0x3666fa39`; in the translator 205
store, 7 other split compute recipes of this title hold one or two.

**Same-translator ANV statistics** (offline, SIMD8, not a frame effect). The
unfused arm was translated by a diagnostic build into a private copy of the
store with only this kernel's entries removed:

| `0x3666fa39` | Instructions | SENDs | Spin loops | Spills:fills | Max live |
| --- | --- | --- | --- | --- | --- |
| Without grouping | 44,573 | 1,695 | 200 | 593:2,684 | 365 |
| With grouping | 45,665 | 1,623 | 188 | 596:3,055 | 347 |

The 12 fewer spin loops are exactly the lane-read groups. The rise in fills
is within this kernel's register allocation sensitivity: a probe-only change
moved them from 2,225 to 2,864 (2026-10-30 record).

**Verification.** The device test
`EmulatorPackedMipNative.SplitWaveGroupedLaneReadsAndComparesReturnWholeWaveWords`
runs both groupings with a required subgroup size of 8 and checks every
whole-wave word against its scalar value, with no validation errors. Forcing
the ballot field shift to 0 makes it fail. It skips on devices whose minimum
subgroup size is above 8.

**Frame effect not isolated.** Two strict runs stopped with the intermittent
`!create_all_the_same` refusal at 181 s and 137 s. The two that reached the
dunes desynced from the time-based pad route: one stood idle at the start
(13.29 fps, 54.6 GPU ms/f over 205-255 s), while the translator 203 baseline
was moving among the rocks (10.88 fps, 79.4 GPU ms/f). The scenes differ, so
no speedup is claimed. The route lands in different game states when load
times vary; compare windows only when their captures show the same camera
state.

## Robot platformer: intermittent storage-view refusal (2026-11-08)

On the Intel Arc A770, an isolated LTO executable with the same source as
`fc39895dd` and no game patch stopped at 179.7 s with
`!create_all_the_same` during the recorded route. The new 960x540,
mode-27 storage image required its initial content. Of its six overlapping
objects, an equal-range storage image had the same image parameters except
that its creation was allowed to skip the guest seed; the other parents
included a 3840x2160 render target with a recorded device write and storage
images in other layouts. This reproduces the refusal without the six pending
graphics changes in the worktree. It does not establish which parent owns all
bytes needed by the incoming read.

**Do not reuse on the timestamp alone.** A storage object's `write_time` is
updated at binding, before the dispatch's mutation-conflict check. A binding
can therefore advance it even if that attempt does not record the dispatch.
Also, `skip_seed` currently affects both equality and the update callback;
ignoring it in equality alone can leave a later guest update unuploaded.
Establishing a recorded whole-image write, the intervening aliases and the
required queue ordering is necessary before reusing that backing.

An isolated, diagnostic-only binder trace filtered to the reported guest
address reached 360 s without the refusal. It recorded repeated bindings of
two different storage formats over the range and saw the exact mode-27 view
once at startup; it did not capture the failing transition. The 245 s
capture reached the dunes, with the previously documented visible haze; it
is not evidence of correct graphics or successful movement. The instrumented
run is not a performance comparison. The executable was stopped by its
harness at the time limit.

A second diagnostic traced the actual
`vkCmdDispatch`/`vkCmdDispatchIndirect` recording after the mutation-conflict
check. It saw 4,951 recordings with shader checksum `0x05472e62` and a storage
descriptor at that address, but only one entry through the traced creation
branch for that checksum. Descriptor address and recorded dispatch alone do
not identify which host image was bound or prove completion. This retry was
stopped manually after 240 s without reproducing the refusal.

A third diagnostic traced the selected host image and materialization route.
The same compute shader and storage descriptor first bound a render target,
then a different storage image over the same guest address. The run stopped
at 149 s on another mixed-parent `!create_all_the_same`, this time at
`0x53de40000` for a 3840x2160 storage view. The address-filtered trace did
not identify that view's contents or the unaccepted parent, and cannot prove
a safe alias policy.

Replaying the captured parent metadata through the alias predicates in a
diagnostic test found six copyable render targets. The oldest target's GPU
timestamp was 33646, while a sampled parent uploaded from CPU memory at 44757;
the current admission rule rejects that combination. The replay is a
reconstruction, not a second live reproduction of the failure. A separate
test checked each of the 204800 eight-byte texels in that parent's range:
the plan for a later render target maps every texel to the same guest byte.

That coverage does not establish that the later target contains newer data
for every texel: a draw can update only part of an attachment, and a tiled
GPU-owned target can start undefined. `device_write_time` is marked for a
draw, not proof of a complete overwrite. A prototype that admitted the case
on copy coverage and that timestamp was therefore discarded. Three further
baseline diagnostics, an isolated prototype run and a clear-timing diagnostic
did not reproduce this alias refusal; the prototype path did not activate.
No runtime fix or performance gain is established. A safe admission needs
proven per-range content provenance and ordering, including later CPU writes.

## Robot platformer: real gameplay frame rate, internal resolution and FSR 3.1 (2026-11-02)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5, Ryzen 5
7600, 1280x720 window, 60 fps limit, clean link-time optimized build, user route.
Runs with a game patch are not strict runs and support no compatibility claim.

**Correction.** The records below that quote a "hub" at 30-39 fps measured the
space cinematic (about 128 s into the route) and the RESUME/SKIP pause menu (about
148 s). Play after the route ends, in the crash-site dunes from about 200 s,
measures 12-13 fps at the guest's own 1920x1080. Quote this title only from there.

**Why it holds there.** GPU timestamps sum to 60.7 ms per frame. The largest items
are the BVH kernel `0x3666fa39` 22.6 ms, `0x874bd13f` 12.75 ms, indexed draws
6.9 ms, draw tail 4.5 ms and geometry 3.3 ms. In the same state the command processor runs at about 49 %
of a CPU core and waits about 45 % on fences; no other host thread passes 5 %, so
the guest CPU is not the limit.

**Excluded.**
- Removing command processor reads of one-index indirect arguments: 13.7-14 fps
  with fence waits unchanged. The waits move.
- `RenderResolutionScope = RenderTargets` as a test of resolution. At 640x360 the
  main passes stay 1920x1080, since only targets of the 3840x2160 display extent
  are planned, and the presented image stays 3840x2160. The earlier reading that
  resolution does not help came from this test and is withdrawn.

**Internal resolution is the lever.** `0x3666fa39` runs 2,087,936 invocations per
call, close to the pixels of 1920x1080. With a game patch that lowers the guest's
grid to 960x540, measured on a build with diagnostics:

| Per frame | 1920x1080 | 960x540 |
| --- | --- | --- |
| GPU timestamps, sum | 60.7 ms | 36.0 ms |
| `0x3666fa39` | 22.64 ms | 6.45 ms |

The clean build measures 16.6-18.1 fps in the dunes against 12-13. `0x874bd13f`
does not appear under its hash at 960x540; where its work went was not traced.
Command processor work and fence waits now each take about 30 ms of a 55-60 ms
frame, so 60 fps also needs the command processor cut.

**Wait sites at 960x540.** Fence waits of 0.2 ms or longer, 40 s of dunes. Three
storage publications per frame, all smaller than 64 bytes, are refused by the partial
line rule (`docs/bringup/cross-title.md`, 2026-11-02), so `DRAW_INDEX_AUTO` submits and
waits for the CPU copy: 16.7 s in 3,700 waits, nearly all of the wait time. A one-run
ceiling with the rule forced off, not a fix:

| Rule | fps | Fence waits per 10 s | Largest sites in 40 s |
| --- | --- | --- | --- |
| On | 16.9-18.5 | 4.6-5.1 s | `DRAW_INDEX_AUTO` 16.7 s |
| Off | 18.5-19.5 | 4.1-4.7 s | `WAIT_REG_MEM` 13.2 s, `DRAW_INDEX_INDIRECT` reads 6.8 s |

The waits move again: the guest's own label waits and argument reads take over, so
removing this one wait is worth about 5-8 %, which does not justify exposing the
crash the rule prevents. Command processor time is flat, led by descriptor binding
(39 % of its samples) and shader input parsing (19 %); no single item stands out.

**Wait sites at 1920x1080.** Same 40 s dune window, on a build with fence stacks
(9.4-11.5 fps with that overhead): indexed draws 11.2 s, of which 10.0 s in 803 waits
overlap `0x3666fa39` in flight, `WAIT_REG_MEM` 5.1 s, `DRAW_INDEX_AUTO` 0.8 s. Removing
every wait still leaves the GPU sum of 52-60 ms per frame, about 18 fps at best.

**Excluded: removing EXEC guards by register liveness in compute.** Offline, on the
translated modules, a guard was dropped when no later read could observe the old value
in an inactive lane. That removed 1,649 of 2,047 guarded stores in `0x3666fa39`, 2,370
of 3,857 in `0x874bd13f` and 829 of 1,012 in `0xbf8ee9fc`. `0x3666fa39` fell from 40,981
to 35,475 instructions and from 2,594 to 1,867 fills; `0x874bd13f` from 79,488 to 65,587
instructions and from 10,024 to 7,653 fills. Loaded into a dune run in place of the
translated modules, the static gain did not reach the frame:

| Last 35 s of the route | Translated | Guards removed |
| --- | --- | --- |
| `0x3666fa39`, mean per call | 21.85 ms (369 calls) | 19.77 ms (470 calls) |
| fps | 11.9-12.7 | 12.2-13.5 |

The scene mix differs between the runs, since `0x874bd13f` ran every frame only in the
second, so about 2 ms is a ceiling on the gain, not a clean A/B, and `0x874bd13f` has no
comparable baseline. The bound also ignored cross-lane reads and the image was not
compared, so it does not justify a whole-program analysis that touches every compute
shader.

**Where this leaves 60 fps.** Native 60 fps in the dunes needs GPU work to fall from
52-60 ms to under 16 ms on the Arc A770, and no measured lever approaches that. Occlusion
queries are no lever here: their results are published on the command processor without
a wait.

**Excluded as a playable option: lighter lighting patches.** The non-tiled renderer
plus GI off at 960x540 gives 20-22 fps, and frames over 50 ms nearly disappear. The one
capture at 225 s is a washed-out image with no texture detail, against a full
scene at 240 s on the same route without them. Not compared at the same frame.

**FSR 3.1.** Guides follow the general rule in
[render-resolution.md](../render-resolution.md).
- Instrument: the first two runs looked fine but never ran the upscaler, because
  the presented image was larger than the window. A trace showed `upscales=0`.
  Only the guide grid decides now, and 5,160 dispatches were counted in one route.
- Motion guide: raw dumps of consecutive frames. Warping the newer frame by
  `-motion` lowers the frame difference (0.00035 against 0.00067 unwarped and
  0.00070 with `+motion`; 0.00277, 0.0031 and 0.0032 in a second pair), so the
  target is normalized motion from the previous position to the current one. About
  55-64 pixels carry a marker value of 128 and are passed unchanged. Static frames
  show a uniform offset of 0.04-0.08 pixels, consistent with jitter left in the target.
- Depth: not inverted, read from dumps (0.988 at the bottom of the frame, 0.9986 at the top).
- Cost: 16.6-18 fps with and without the filter at 960x540, so no measurable loss
  in these single runs. No crash on a full route. Two captures look correct;
  no side-by-side against linear was made and motion quality is not verified.
- Validation layer: the two upscaler issues found (usage flags, depth aspect) are
  fixed, and the layer has not been run again since. It also reported `rw_luma_history`, declared `rgba8` by the SDK's own shader
  and bound to a half-float image; not investigated.

**Other titles and AMD.** Strict matrix with `Fsr3` selected, 33 folders, clean build:
27 interactive, 1 controlled timeout, 5 invalid, 0 crashes. The timeout is the title at 2
presents that has moved between presenting and timeout in earlier matrices too. Grids
at or above the window never engage the upscaler, so this mostly checks the fallback.
On the Raphael iGPU with RADV, the 960x540 route stops after about 90 s with
`native-wave admission stage=0x1 guest=32 proof=1 host_default=64`
(`GraphicsRenderPipeline.cpp`, `CreatePipelineInternal`): a vertex program needs exactly 32
lanes and RADV has no exact width for that stage. This is the fail-closed limit already
recorded for `docs/titles/role-playing.md`, reached before any confirmed upscaler
frame, so `Fsr3` has not been verified on AMD. Whether the unpatched route stops at
the same point was not measured. Suggested direction: a width-neutral proof for that
vertex program. NVIDIA has no host here.

**Open.** In the dunes a white haze grows toward the camera and covers Astro at every
resolution, with and without the filter, while the distant scene stays clear. It looks
like a defect, not dust, but the cause has not been investigated. No other title has run with `Fsr3`. AMD and NVIDIA have not run it.

## Robot platformer: green blocks in the crowd scene were MIMG D16 stores read as 32-bit (2026-10-31, fixed)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5, 60
fps-limited runs. All runs used the undeclared `CB_SCALE=2` debugger probe to
pass the hub-start assert below, so none of them is a strict run.

Observed: from about frame 3500, dark green rectangular blocks with stripes of
period 8 rows covered the ship and the bots. Dispatch dumps placed the depth of
field chain before them. Skipping one compute program of the chain, the
composite `bf8ee9fc` (groups 152x86, writes the 2432x1368 colour target), removed
the green at frames 3661 and 4172 (run gw251; the same arm also hit the GPU job
timeout in gw250).

Cause: the guest program holds eight `ImageStore` instructions with overlapping
data registers such as `v[5:8]` and `v[7:10]`, which only make sense with the D16
bit of the MIMG encoding (word 1 bit 31): two 16-bit components per VGPR, low half
first, with the format deciding float or integer (RDNA2 ISA, MIMG D16). The parser
ignored the bit, so packed dwords were stored as one 32-bit float per channel.

Fix: the parser records D16 on `image_store` and sizes the data operand by VGPR.
The emitter unpacks half floats for float views and zero-extends the half for
integer views, and every other MIMG opcode with D16 set now stops instead of
reading the wrong registers. Storage store proofs refuse D16 stores. Translator
version 189 (186 and 188 were used only by excluded private experiments), resource
fold capture schema 4.

Verified: run gw255 (330 s bound, no fatal, no job timeout) reached the crowd scene
with the composite running: captures at frames 3675 and 4068 show clean bots and
depth of field. The first version refused integer D16 stores and stopped at the
hub (gw253, gw254), so the path is exercised by that title as well. The D16 float
and integer paths have no dedicated test; `EmulatorShaderMimg` and resource fold
replay pass. The integer path was only proven not to stop at its store (compute,
pc 0x468, dmask 0x1); its output was not inspected.

No regression: strict matrix on this change gives the same outcomes per case as the
previous one (26 interactive; `case_429dd9d08d87` controlled timeout after 2
presents; `case_2daaacae7295` exit 139; 4 invalid fixtures). Its bounded windows do
not prove that no title reaches a D16 load or sample later, which would now stop.

Hub-start assert: without `CB_SCALE=2` runs gw244 and gw246 still end in the guest
assert of the record below; with it, more than 13 hub passes survived. This
contradicts the older "no evidence that more space helps" note in this file (one
passing run); the scale is a probe and not a fix, and the strict cause remains open.

Open: the A16 bit (word 1 bit 30) is still ignored by
`source/emulator/src/Graphics/ShaderParseMIMG.cpp`; a program using it would read
packed 16-bit coordinates as 32-bit ones. Direction: honour it, or stop on it as
D16 does now. The scene itself runs at 6-7 fps in this window.

## Robot platformer: where the render thread blocks, GPU-paced list start excluded (2026-10-31)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5, 60
fps-limited runs gw256 and gw257 with the `CB_SCALE=2` probe, sampled at about 1 kHz
from `/proc` task syscall state and guest return addresses on the stack (not committed).

- **Excluded: the render thread waits for the GPU before it starts a frame.** From its
  wake to its first submit it takes a median of 1.2-2.0 ms in every frame period bucket
  from under 20 ms to over 400 ms (3523 frames), and it runs without blocking in that
  time. This does not reproduce the first-list growth of the older debugger timeline
  below, and it does not time the list build itself, which runs on job workers.
- **The frame is bound by submit.** In the crowd window (90 s, 736 frames) the render
  thread spent about 87 s inside four submit sites (`0x2aaca9` 33 s, `0x2aadb9` 29 s,
  `0x2aad99` 19 s, `0x2aacf9` 6 s), blocked on the per-batch decode completion in
  `GraphicsRing::Submit`, while the game thread waited for it (79 s at `0x2a4ac7`). The
  frame period is therefore the command processor's decode time, including what that
  thread waits for.
- **Excluded: job workers poll for work.** The eleven workers wake about 500 times per
  second with almost no run time, but they sleep in a blocking guest wait (`0xaea6c8`)
  and the timeline above does not show a poll-sized delay. The 500 Hz is consistent
  with a periodic host wake of an interruptible wait; that source was not read.
- **Open.** What decides that the clear runs before all lists of a crowd frame is still
  unknown. Host CPU is not saturated, which makes guest priority or affinity unlikely
  but does not exclude them.

## Robot platformer: hub-start guest assert, frame timeline (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5, 60
fps-limited run gw190 under a debugger probe (not committed), which ended in the
assert after 328 s, and strict run gw191. Addresses are guest image offsets.

- **Thread order.** The game thread loop (`0x2a37f0`) waits for the render thread's
  state at `0x2a4ac7`, publishes the skeleton palettes (`0x736e960` at `0x2a4ec9`),
  sets the state and signals at `0x2a50bc`, and starts the next frame's update at
  once. That update clears the published palette (`0x73fd090`, called from a job
  reached through `0x105c400` and `0x10ae200`). The render thread (`0x2a9fd0`) builds
  its lists through `0x7097de0`, on itself and on job workers. The job system uses
  atomics; no guest primitive orders the clear against the list build, so the two
  overlap by design.
- **Timeline, milliseconds after the signal.** The render thread wakes after about
  0.2. Across 1542 frames with skinned items:

  | Frames | Count | First list | Last list | Clear | Items without a palette |
  | --- | --- | --- | --- | --- | --- |
  | All items have a palette | 964 | 1.7 | 4.2 | 6.8 | 0 of 130084 |
  | Some items miss it | 578 | 3.6 | 6.2 | 2.6 | 33492 of 84351 |

  An item that misses the palette takes the path that writes its skinning data into
  the command buffer. Misses by frame period: 33-50 ms 13 %, 50-80 ms 9 %, 80-120 ms
  14 %, 120-200 ms 22 %, 200-400 ms 25 %; the first list moves from 1.1 to 4.9 ms over
  the same range while the clear stays at 4-6.5 ms.
- **The fatal frame.** Partial misses are survived in all 578 frames. The crowd
  arrives at frame 4107 (lists 10 to 25, items about 40 to 520). The first three
  crowd frames have every palette, with periods of 1651, 338 and 187 ms. Frame 4110
  has 7 items with a palette and 513 without, first list at 3.9 ms, and the assert
  follows. So one whole crowd frame on the larger path is enough.
- **Not the cause: guest-visible end-of-pipe labels.** A secondary account blames
  labels shown to the guest at record time. Every `WriteAtEndOfPipe32/64` and
  `FlipWithInterrupt` branch here records the store for the GPU to perform; none
  stores at record time.
- **Excluded: a long stall before the assert is required.** gw190 had periods of 1651,
  338 and 187 ms just before it, but gw191 (strict, no debugger, one-second counter
  windows and a 250 ms per-thread CPU sampler) held the crowd at 1169 draws per frame
  and 7 fps through the last 10 s before the same assert, with a largest command
  processor run of 59-120 ms. The largest run in the crowd arrival window of the
  passing runs gw180 and gw181 was 531 and 562 ms, so a stall does not separate them
  either.
- **Host CPU in those 10 s.** All threads together used about 0.8 of one core: the
  command processor thread 0.54, every other thread at most 0.04. Guest threads are not
  starved of total CPU in the steady crowd; run-queue delay of a single wake was not
  measured.
- **Open.** gw180 and gw181 held the crowd for over 90 s at about 3 fps, and gw191
  asserted after about 20 s at 7 fps. What decides that one crowd frame loses the race
  in nearly every list is not known.
- **Limits.** gw190 ran under a debugger, which may lengthen periods. Whether the
  guest paces on vertical blank between the signal and the next update was not read,
  and no frame with skinned items ran faster than 33 ms, so the race at the intended
  frame rate is unmeasured.

## Robot platformer: PrimitiveId in the interpolation geometry stage limited geometry distribution (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs gw181 (before) and gw182, gw184 (after).

- **Cause.** Every generated interpolation geometry stage read `PrimitiveId`
  and forwarded it, though only the fragment transport passes read it. The ANV
  source selects instance-level geometry distribution on DG2-G10 when a
  geometry stage reads `PrimitiveId` (workaround 14019166699,
  `update_vfg_distribution_mode`; read in the 26.0 source, the installed
  driver's behaviour is shown by the timing below). A draw with one instance
  then stays on one slice. Before the change, those draws used about 70-100 of
  512 execution units by estimated cycles; one slice has 64.
- **Change.** Native pipelines compile the stage without the builtin; the
  transport passes keep it under their own module key (geometry version 95).
- **Time per triangle** (GPU timestamps over pipeline statistics, crowd scene):

  | Pixel program | gw181 | gw182 | gw184 |
  | --- | --- | --- | --- |
  | `cbb6bb28` | 44.5 ns | 12.8 ns | 10.5 ns |
  | `9dacb90f` | 10.0 ns | 3.4 ns | 3.4 ns |
  | `d4bef057` (few triangles per draw) | 16.1 ns | 13.4 ns | 14.0 ns |

- **Whole-frame rate, crowd scene** (about 1175 draws per frame, same binary, an
  environment toggle selects the arm, no per-draw timestamps). gw186 and gw188
  ran with the guest's `CB` size table scaled by 2, a probe to pass the hub-start
  assert, so they are not strict runs. gw181 is the strict run from before the
  change, with timestamps:

  | Run | Arm | fps | Fence wait, ms/frame | CP run, ms/frame | Run-queue wait / run |
  | --- | --- | --- | --- | --- | --- |
  | gw186 | without the builtin | 6.34 | 63 | 161 | 41 % |
  | gw188 | builtin forwarded | 3.22, 3.40 | 202, 205 | 318, 320 | 91 % |
  | gw181 | before the change | 2.86-2.96 | 201-206 | 329-338 | 35 % |
  | gw189 | without the builtin, strict | 5.47 (one window) | 50 | 182 | 45 % |

  The host was busier in gw188 than in gw186, which inflates its CP time. gw181,
  on a quiet host, gives the same rate and fence time, so the host load does not
  explain the gain: the frame rate doubles in this scene and the fence wait falls
  to about a third. The frame is now about 151 ms, 63 ms of it in the fence, so
  CP cost is a principal limit there.
- **Later windows of gw186.** 4.88-5.6 fps at about 600 draws per frame, CP run
  185-236 ms/frame, and 4.06 fps under foreign load. These windows pass frame
  4246, a black and green wipe; no capture identifies them as the hub. The run
  ended at its 420 s bound.
- **Native readers of the builtin.** None: only the transport capture and resolve
  programs read `PrimitiveId`, so no native pixel program lost an input.
- **Limits.** The strict confirmation is one window (gw189, about 1.85x against
  gw181): strict runs gw182, gw184, gw185 and gw189 all hit the guest assert at
  hub start (below). Before the change about 3 of 17 strict runs passed it, so 0
  of 4 is expected about 45 % of the time with no effect at all. These runs neither
  show nor exclude a regression at hub start, and more runs would not settle it on
  this host; fixing the assert will. gw183 ran out of its 330 s bound during the
  hub load, with a foreign build on the host.
- **Other titles.** The strict games matrix on this binary ended with 25 interactive,
  1 controlled timeout, 2 host crashes and 4 launch failures, against 26 presenting,
  1, 1 and 4 before it. `case_429dd9d08d87` moved between presenting and controlled
  timeout, as it already did between earlier matrices. `case_e16011e8b594`
  (`PPSA02929`) crashed with exit 139 while stopping, after its stability point and
  605 presents; four reruns of that case ended interactive with exit 0, so this
  rare shutdown crash did not reproduce. No regression is shown. The matrix checks
  neither pictures nor gameplay, and `playable-regression` was not run.

## Robot platformer: heavy character draws are real content, and same-queue guest-address waits (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs gw180 and gw181, `504361d49` plus temporary probes
(not committed).

- **Hub start outcomes on the same route.** Guest assert: gw146, gw149,
  gw153-155, gw175, gw176, gw178, gw179, and with the geometry change above
  gw182, gw184, gw185, gw189. GPU job timeout: gw144, gw148, gw150, gw152, gw168
  (gw168 with the size-table probe). Passed: gw147, gw180, gw181, and gw169 with
  the probe.
- **Hub start outcome against host contention** (run-queue wait over run time of
  the emulator's threads, last sample of each run). Passed: gw169 36 %, gw180
  31 %, gw181 35 %, gw147 54.5 %. Guest assert: gw154 38 %, gw189 45 %, gw155
  81 %, gw178 100 %, gw182 69 %, gw184 69 %, gw185 98 %. The correlation is weak:
  the ranges overlap between 38 % and 54.5 %. `CPU_WEIGHT=1000` did not lower the
  wait (gw185).
- **Excluded: a replay defect behind the repeated large draws.** The pixel
  program `cbb6bb28` draws the same vertex program, 32-bit index buffer
  (608,112 indices, 202,704 triangles) and vertex buffer (107,967 records,
  stride 76, 11 attributes) several times per frame, but each draw has its own
  textures, world matrix and bone palette (183 records). A capture at frame
  3800 shows a crowd of about 60 costumed characters in the intro.
  - The figure "125,056 triangles per draw" was an average of 9 large and 6
    small draws per frame: (9 x 202,704 + 6 x 8,584) / 15 = 125,056.
- **GPU time by program, last 7 windows of gw181** (17 frames in 5 s, about
  276 ms of GPU work per frame, queue 8 busy about 91 % of the wall clock; the
  scene was probably the hub, no capture confirms it):

  | Program | ms/frame | Notes |
  | --- | --- | --- |
  | pixel `cbb6bb28` | 80 | about 9 ms/draw, 45 ns/triangle |
  | pixel `9dacb90f` | 66 | 34 draws/frame, 2 ms/draw, 10 ns/triangle, about 68k fragments/draw |
  | compute `874bd13f` | 35 | |
  | pixel `28d8852c` | 14 | |
  | pixel `d4bef057` | 14 | 23.9k triangles/draw |
  | pixel `bf8ee9fc` | 11 | |

- **Shader statistics, real vertex, geometry and pixel stages compiled
  together at SIMD8 on ANV.**
  - Both heavy pixel programs share a byte-identical 594-word interpolation
    geometry program (154 instructions, 1,598 cycles, no spills), so it does
    not explain the difference between them.
  - The vertex stage differs:

    | Vertex program | Instructions | Sends | Cycles | Spills:fills | Live registers |
    | --- | --- | --- | --- | --- | --- |
    | `976809a8` | 12,388 | 340 | 115,621 | 195:482 | 264 |
    | `57572d59` | 4,412 | 133 | 32,559 | 42:72 | 180 |
    | `a2bb8f2d` | 1,330 | 38 | 9,108 | 0:0 | 83 |

  - Replacing the guest-address lookup of `976809a8` by an identity compiles
    to 12,005 instructions and 100,985 cycles, spills unchanged, so the lookups
    are not the lever. This agrees with the record of `0x0a0014400c2ae56d`
    below.
  - In the final assembly of `976809a8`, 677 of 1,017 messages are spills and
    fills. A guest buffer load repeats per component the SIMD absolute value,
    sign select, shift and multiply of the signed word index.
- **Open defect: same-queue host waits for guest-address publication.** At the
  end of gw181 reason `gda-storage`, host wait, same queue: 10,175 waits, 95.8 s;
  other queue: 2,248 waits, 4.2 s. After frame 3614 about 2,500 waits took about
  60 s at about 24 ms each, a drain per wait.
  - State: a draw that reads storage written earlier on its own queue, in
    `RetryGuestDeviceAddressDraw` (`GraphicsRun.cpp`) through
    `GpuMemory::PendingStorageWriteBack` (`GpuMemoryWriteback.cpp`, host wait
    when no publishable entry exists, or the consumer is not ordered, or
    `StorageIsWholeGuestContent` is false).
  - Same-queue consumers already count as ordered, so these waits come from a
    false clause of `StorageIsWholeGuestContent` or from a failed record or
    mark. Which clause is not yet measured.
  - Direction: count the failing clause per wait, then satisfy that case with
    a device-side copy in queue order.
  - Measured and addressed on 2026-11-06: the failing step is the record
    (`DeviceMayWriteGuestRange`). See "Device-address overlays for refused
    storage publications" below.
- CP cost alone is about 75 us/draw in the hub, near 11 fps at 1170 draws with
  no GPU cost at all, so the GPU work above is not the only limit.

## Robot platformer: EXEC guard folding in lane-local pixel programs excluded at runtime (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs, `4305c2080` plus a temporary translator change
(version 188, not committed).

- **Excluded: folding the per-lane EXEC guards where the pixel mask-flow
  proof shows that EXEC holds every launched lane.** The change replaced the
  load of the lane's own EXEC bit with the constant 1 at those instructions.
  - Coverage (gw177): of 91 pixel programs, 75 were lane-local and 59 had
    foldable instructions; 14644 of 59067 instructions folded and 29042 kept
    their guard. In `8ca0b9ff` the remaining guards sit inside lane-divergent
    if-regions (save EXEC, narrow, restore), so 780 of its 1482 instructions
    folded.
  - Offline, same SPIR-V on ANV: `8ca0b9ff` still compiles at SIMD8 only,
    4359 -> 2879 instructions, 109 -> 83 sends, estimated cycles
    39913 -> 30872, live registers 100 -> 81.
  - Runtime, GPU time per call: on the title `8ca0b9ff` took 50-58 us with
    translator 185 (gw147) and 44-54 us with the change (gw177). In the ship
    scene translator 185 took 72-77 us; the change was not measured there
    cleanly (gw177 stopped before it, gw178 ran with per-draw markers and
    took 81-87 us). The intro character programs
    `cbb6bb28` and `9dacb90f` took 6.0 ms and 1.3 ms per call in the one full
    intro dump of gw178, against 5.6-6.0 ms and 1.2 ms in gw147.
  - Upper bound (gw179): every guard of `8ca0b9ff`, `cbb6bb28` and `9dacb90f`
    was patched to the constant 1 in a private shader store, which is wrong
    for masked lanes and serves only for timing. Offline this also compiles
    `8ca0b9ff` at SIMD16 (1942 instructions, 25012 cycles for 16 fragments
    against 39913 for 8). At runtime `8ca0b9ff` took 40-48 us per call on the
    title and 69-73 us in the ship scene once it settled. The run stopped at the start of the
    intro on the guest assert recorded below, so the character programs were
    not measured.
  - So the heavy pixel program is not limited by ALU work or SIMD width,
    as the offline record of the dune GPU cost predicted from its latency.
    Branching per lane on EXEC, to fold the guards that remain inside
    divergent regions, is not pursued for it.

## Robot platformer: constant loads as uniform buffers excluded, and a guest fault at hub start (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs, `ad7028e68` plus temporary probes.

- **Excluded on paper: binding scalar constant loads as uniform buffers**
  (the open lever in the dune GPU cost record below; not built).
  - Kyty emits every `S_BUFFER_LOAD` as a load from the storage-buffer
    array, indexed by the V# slot. In the final NIR of the `8ca0b9ff` pixel
    program (gw135) the driver already turns 54 of them into wave-uniform
    block loads with constant offsets (`load_ssbo_uniform_block_intel`) over
    10 buffers; 18 other storage loads are divergent.
  - The array index of those loads is dynamic (a pushed V# slot plus a
    base), and ANV pushes only uniform buffers with a constant binding and
    index. Pushing them would need one fixed uniform-buffer binding per
    scalar V#, a descriptor layout redesign, for loads the driver already
    batches.
  - The program waits on memory latency, not on load count (3.7 ns per
    fragment against about 0.56 ns of ALU time), so fewer constant loads
    would not shorten it measurably.
- **Guest assert at hub start (open; gw146, gw149, gw153, gw154, gw155).**
  All five runs (default route `scripts/input/reach_first_gameplay.pad`)
  stopped right after `LoadLevelResources: hub_crashsite_tutorial` and
  `SubLevelLocator sublevel_locator3 ... restore to 6`, before
  `ui_pause_cinematics` starts.
  - Cause: the title's own assert. The fault (`int 0x42` at guest
    `rip 0x9004a0c3e`) is in the out-of-space callback of a guest command
    buffer (`0x9004a0c10`). The callback never chains a new block; it
    reports `command_buffer_writer.ppr.cpp` line 31 with the message
    "There are too many objects/batches in this scene." and traps.
    `rcx = 0xdeadbeef5533ccaa` is Kyty's stack-protector guard value
    (`LibKernel.cpp`), not a debug fill pattern.
  - The command buffer grows packets up from its base and embedded constant
    data down from its top. In gw149 the failing request was a 24 KB
    embedded constant block (allocator `0x900496f90`), and an earlier block
    of the same size sat about 53 KB above the base of the 4 MB buffer:
    embedded constants, not packets, filled it.
  - Buffer layout (eboot 01.018, static): the title's GPU device object
    creates five groups of three buffers from a size table of 7, 7, 4, 4
    and 6 MB (creator `0x9004a38c0`, one end-of-pipe event queue per group).
    gw150 (gdb on the buffer init function `0x9004a0930`) lists the same 15
    buffers. Both failing buffers of gw149 and gw150 are 4 MB ones.
  - Embedding (static): per draw, the embed stage walks the constant slots
    the bound shader uses and copies each slot not yet uploaded into the
    buffer through the allocator above; a slot that does not fit calls the
    callback. The reset routine that rewinds the cursors (`0x9004a05d0`) has
    no static caller, so the per-frame rewind is reached indirectly.
  - Content (gw153, gw154): a temporary sampler read the 15 buffer cursors
    every 20 ms through `/proc/<pid>/mem` and dumped buffers past 90 % use.
    - The three buffers of a group are used in turn. Over 250 s the buffers
      of the five groups were rewound about 840, 315, 325, 220 and 2 times
      each: the 6 MB group is essentially unused.
    - In crowded frames the embedded data are mostly bone matrices: blocks of
      1024 matrices of 48 bytes in which only slots 0-70 and 512-582 are
      non-zero. The 7 MB buffers of the second group held 98-103 such blocks
      at 6.0-6.3 MB of use, and the 4 MB buffers of the fourth group 74-82 at
      3.7-4.1 MB (90-98 % full). The intro level already reached 97 % in the
      fourth group.
    - In the hub-start frame of gw154 the two fullest buffers held 79 and 82
      blocks for the draws of four meshes (20, 20, 11 and 10 blocks in one
      buffer), with no block repeated between the two buffers.
  - Excluded:
    - Save state: the title's save memory slot is all zeros and no savedata
      directory exists, so every run starts a new game.
    - Render resolution: gw153 at 960x540 failed the same way.
    - The guest particle heap: `JsParticle heap memory is full!` is printed
      after the particle heap fails a request, which the title then retries
      on its engine allocator. With the mspace fix in `docs/bringup/cross-title.md`
      (2026-10-30), gw155 printed it 0 times and hit the assert at the same
      point.
  - Correlation, not a mechanism: the failing runs overlapped a foreign
    process at 100 % CPU (run-queue wait 11-16 % of run time against 1-2 %
    in passing runs, CPU pressure avg10 1.3-5.8 against about 0.1).
  - About 55 earlier runs on this branch loaded the same hub and continued
    into its cinematic. Of the ten latest runs (gw146-gw155), only gw147
    passed and gw151 did not reach the hub; gw146, gw149 and gw153-gw155 hit
    this assert, gw148, gw150 and gw152 the GPU job timeout recorded below.
  - Direction: find what bounds the number of skinned draws per buffer on
    hardware: which engine pass records into each group, and whether a
    visibility or LOD decision, a GPU write-back or thread timing changes
    that count.
  - **Why a draw embeds its bone matrices (static, eboot 01.018, and a gdb
    probe in gw167).** The skinning binder (`0x9004ad1f0`) binds a V# to the
    skeleton's GPU palette when the render item's palette flag is set, and
    otherwise copies the whole declared constant buffer (2 x 24 KB) into the
    command buffer. The render-list builder (`0x907097de0`) sets that flag
    when the skeleton node holds a GPU palette pointer (node `+0x50`).
    - The pointer is cleared by the bone-transform setter (`0x9073fd090`),
      which game jobs call per bone, and set again by the skinning publish
      (`0x90700a50c`), which allocates the palette from a per-frame GPU ring
      and stores it at `+0x50`.
    - In gw167 nodes with and without a palette both alternated every frame.
      A draw embeds its matrices when the builder reads the node between the
      clear and the publish, so the number of embedding draws per frame
      depends on thread timing, not on scene content.
  - **Larger buffers tested and not adopted (gw168, gw169).** A temporary
    tool rewrote the size table to 14, 14, 8, 8 and 12 MB through
    `/proc/<pid>/mem` 0.37 s after launch, before the buffers are created. It
    was used only as a probe to get past the assert; it is not a fix.
    - gw168 stopped at 220 s on the GPU job timeout below, right after the
      hub load.
    - gw169 passed the hub start and stayed in the hub at 3.2-3.5 fps until
      the 330 s bound, without reaching `ui_pause_cinematics`.
    - In both runs the 20 ms sampler saw every buffer peak below 25 % of the
      doubled size, under 2 MB in the buffers that are 4 MB by default. The
      default sizes would have held these runs too, so the runs give no
      evidence that more space helps.

## Robot platformer: merged loads in the heavy vertex program excluded, and the BVH node fetch (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw144, gw145), `ad7028e68`
plus temporary probes, and offline compiles of stored modules at SIMD8
(`VK_KHR_pipeline_executable_properties`).

- **Vertex program `0x0a0014400c2ae56d`** (pixel key `6bcfb82d`, about 6.6 ms
  per dune frame). Translator 185 compiles it to 6432 instructions, 139
  sends, spills:fills 179:264 and 216 live registers.
  - Its 27 guest-address lookups are dead after compilation: the final NIR
    holds no global loads, and a copy whose lookup body is replaced by one
    load compiles to the same statistics. The record below counts them in
    the SPIR-V only.
  - Its data comes from 250 one-component storage-buffer loads and 5
    uniform-block loads. The 125 one-dword loads that carry the guest's
    multi-dword buffer loads stay separate because each component computes
    its word index as `(offset + 4k) / 4` with a signed division, which hides
    from the driver that the words are adjacent.
- **Excluded: one shared word index per multi-dword raw buffer load.** A
  translator change (version 186, not committed) emitted `offset >> 2` once
  and added the component number.
  - The module the change stored compiles to 5479 instructions and 48 sends
    (-65%); estimated cycles fell from 50,571 to 45,486 and spills stayed at
    179.
  - Dune GPU time per draw of `6bcfb82d` (Vulkan timestamps, 5 s dumps with
    at least 800 draws): median 307.3 us in gw145 (23 dumps) against
    314.1 us in gw137 (31 dumps), inside the run-to-run spread (316 and
    320 us in gw137 and gw138). GPU time per frame stayed at about 76 ms.
    The arms are different binaries (gw137 is `34c370da8` plus probes).
  - gw145's dune fps (8.3-9.4) is not comparable: gw144 lost the device
    before the dunes (2026-10-29 timeout record), so gw145 translated the
    dune programs during those windows.
  - Not adopted: no measured gain, and a translator version change makes
    every shader store recompile (gw144 spent two 30 s windows near 1 fps on
    a cold store).
  - Why the draw time did not move was not measured. Lead: the program reads
    its vertex data from storage buffers in cached system memory, so it may
    be bound by read latency rather than message count (see the transient
    pool placement bound in the next record).
  - The BVH kernel `0x3666fa39` has no multi-dword raw buffer loads (36
    one-dword sites), so the change does nothing there.
- **The BVH node fetch is already uniform and vectorised (null result).**
  The kernel loads each node as eight 16-byte vector loads. Wrapping the
  node pointer and T# words in a first-lane broadcast compiles to 39,352
  instructions, and marking the node block pointer non-writable to 39,335,
  against 39,335 for the stored module: the driver already treats these
  loads as uniform and read-only.
- **Excluded by static analysis: dropping the DS ordering between
  consecutive LDS reads of the BVH kernel.** Of its 29 DS orderings (one
  part rendezvous after each DS instruction), 8 follow a read whose next LDS
  access is another read with no write between; dropping those is sound,
  because the rendezvous after the last read still orders every read of the
  run before a later write. All 8 sit outside the kernel's loops (only one
  ordering, after a read, is inside the traversal loop), so each runs once
  per invocation: about 0.5 ms per call at the microbenchmark rate, below
  the kernel's run-to-run spread of 18.5-23.3 ms per call, for the cost of a
  translator version change. Not built.

## Robot platformer: shape, inputs and split exchanges of the BVH kernel (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw141, gw142), translator
version 185, `fb7b6f307` plus temporary probes (not committed).

- **Shape (corrects earlier records).** `0x3666fa39` is a direct dispatch of
  8156x1x1 workgroups of 256 invocations, 2,087,936 invocations per call,
  once per frame on the graphics queue. Two instruments agree: a print of the
  dispatch's group counts on the command processor and an invocation counter
  in the translated program. The count is close to the 2,073,600 pixels of a
  1920x1080 target; the mapping of invocations to pixels was not checked.
  The 2026-10-28 record below and `docs/bringup/cp-gpu-sync-chain.md` said
  one 16x16 workgroup per frame. That was wrong: the kernel is a throughput
  pass that fills the GPU, not one latency chain.
- **Inputs** (load counters added by the probe, every dune window alike).
  - Five storage buffers from the transient pool, in cached system memory
    (memory type 1), of 16 bytes to 17.9 KB. Each invocation reads about 201
    dwords from them, all from under 20 KB, which the GPU caches hold (16 KB
    column of the latency table in the next record). Placement is excluded
    as a lever for this kernel.
  - Guest-address loads: about 3 data dwords per invocation, each preceded by
    a lookup that reads about 17 table dwords (49.6 per invocation, 1.34
    binary-search iterations per lookup in gw141).
  - Sampled 2D, 2D-array and 3D images and two storage images (the output)
    were not counted.
- **Split exchanges.** Each invocation takes part in 55.6 split-wave
  exchanges per call; the guest wave64 runs as eight 8-lane parts.
- **The early returns are wave-uniform.** Of the three returns, the two early
  ones test a scalar load and the whole-wave EXEC words, so the eight parts of
  a wave leave together and no exchange waits on a part that has returned.
- **Not usable: clock attribution inside the kernel.** The probe read the
  subgroup clock (`VK_KHR_shader_clock`) around the address lookup and the
  split exchange. No time share is recorded: the split share came out above
  the whole subgroup lifetime in many windows (up to 1.9), and the compiler
  may reorder clock reads, which carry no dependency between them. The probe
  also changed the kernel: spills:fills 538:2225 without it, 654:2864 in
  gw141 and 955:3487 with 32 KB of scratch in gw142, where dune windows fell
  from about 10.8 to 7.4 fps.
- **The shared store holds the current translation of this kernel.** The
  module the probe build produced differs from the stored one only by probe
  instructions (opcode histograms). The kernel took 18.5-19.4 ms per call in
  gw141 against 22.3-23.3 ms (gw118) and about 22.5 ms (gw137); that spread
  is unexplained run-to-run variation, not a module difference.
- **Bound.** Removing the kernel entirely would take the dune frame from
  about 93 ms to about 71 ms (about 14 fps). The other frame costs are the
  indexed draws (about 22 ms) and about 23 ms of command processing that does
  not overlap GPU work.
- **Native-width census** (gw143, 300 s to the dunes, a temporary print of
  the native-width proof's verdict once per compute program). Of 61 distinct
  compute programs, 31 pass the proof and run at their native width. 27 are
  refused because an instruction reads another lane or a wave-wide count,
  and 3 because a scalar written inside a lane-conditional region is live at
  its join. Both refusals are correct: the split run is what keeps those
  programs exact. The BVH kernel is refused at `v_readfirstlane_b32` of an
  LDS-loaded value (pc `0x1df0`), and its body holds 44 `v_readlane`,
  36 `v_writelane`, 30 `s_ff1_i32_b64` and 9 `v_readfirstlane`: per-lane
  arrays walked with a lane mask, so no width-neutrality proof applies to
  the whole program. Proving single readfirstlane operands uniform would
  only remove some exchanges inside the split run; it would not make the
  kernel native.
- **Where the exchanges come from.** The translated kernel has 159 exchange
  call sites: 73 wave-wide ballots, 53 lane reads (44 `v_readlane` at a lane
  chosen by `s_ff1` over a mask, plus the 9 `v_readfirstlane`) and 29
  orderings after DS instructions. The guest traverses its BVH
  wave-coherently: one node pointer per wave, children merged across lanes,
  and a wave stack held in the lanes of `v59`. How the 55.6 exchanges per
  invocation split between the traversal loop and straight-line code was
  not measured: 28 of the 29 DS orderings sit outside the kernel's loops
  (record above), so a per-node-step count cannot be derived from it.
- **Static cost by part** (offline compiles of the stored module with
  helper bodies replaced, never run; the baseline is 39,335 instructions,
  spills:fills 538:2225 and 362 live registers at SIMD8). Replacing the part
  rendezvous with a part-local subgroup operation gives 26,776 instructions,
  323:1353 and 258. Replacing the guest-address lookup with one load gives
  35,420 instructions, 459:1904 and 350.
- **Exchange cost bound** (microbenchmark, 8156 workgroups of 256
  invocations at subgroup size 8, eight parts per 64-lane wave). 56
  rendezvous per invocation cost about 3.2 ms over the same loop without
  them, at full occupancy and with almost no work between exchanges. The
  real kernel runs at lower occupancy, so this is a lower bound.
- **Excluded: one shared arrival counter per wave** instead of one sequence
  word per part (one atomic add per arrival, one atomic load per spin). It
  was about 6% faster in the same microbenchmark (3.52 against 3.73 ms) and
  was not built into the translator.

## Robot platformer: dune GPU cost by stage, translated program size, and concurrent sharing excluded (2026-10-30)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw130-gw139), translator
version 185, `34c370da8` plus temporary probes (removed). Dune windows run at
about 10 fps.

- **GPU time per frame** (gw137, Vulkan timestamps over 12 dune windows of
  5 s): about 76 ms of GPU work per frame.
  - Compute dispatches recorded on the graphics queue (Kyty queue 8,
    `QUEUE_GFX`, which runs on the render engine): about 26 ms, of which the
    BVH kernel `0x3666fa39` takes about 22.5 ms (record below). They
    serialize with the draws. Dispatches on Kyty compute queues 0-7 (the
    compute-only family) take about 6 ms per frame, consistent with the
    compute engine's ~7% busy time below.
  - Indexed draws: about 22 ms. The three heaviest pixel keys take about
    7 ms (`0x210025508ca0b9ff`, 116 us per draw), 6.6 ms
    (`0x210022506bcfb82d`, 316 us per draw) and 4.2 ms
    (`0x210011e0d4bef057`, 59 us per draw).
  - Geometry-stage draws about 5.6 ms, the rest under 4 ms each.
- **Stage split of the heavy draws** (pipeline statistics queries, gw130):
  - `8ca0b9ff` is fragment-bound: 1.26G fragment invocations (about one
    screen per frame) against 314M vertex invocations, 3.7 ns per fragment.
    The program declares early fragment tests, so its kill does not defeat
    early depth. At the A770's ALU rate the same instruction count would
    take about 0.56 ns per fragment; the program waits on memory and
    latency, not arithmetic.
  - `6bcfb82d` is vertex-bound: 206M vertex and 5M fragment invocations,
    18 ns per vertex. Its vertex program `0x0a0014400c2ae56d` (a 483600-index
    draw) compiles to 6430 instructions with 178 spills, 263 fills and 4 KB
    of scratch at SIMD8; the other dune vertex programs are 1329-1678
    instructions without spills.
  - `d4bef057` is mixed (206M vertex, 69M fragment invocations).
  - Depth-only passes cost about 2.2 ns per vertex.
  - The interpolation geometry stage compiles to 151-216 instructions and is
    needed: the pixel programs read per-vertex attributes through it. It is
    not the multiplier.
- **Driver statistics** (`VK_KHR_pipeline_executable_properties`, gw134,
  gw135). Every dune colour pixel program compiles only at SIMD8, with
  2.5k-5.7k instructions, 132 live registers and 100-121 sends. In the final
  NIR of one of them, 54 loads are uniform-block loads from storage buffers:
  guest `S_BUFFER_LOAD` constants whose V# is bound as a storage buffer. The
  push range covers only 32 bytes of push constants and 96 bytes of one
  uniform buffer.
- **Translated program size.**
  - Pixel program of `8ca0b9ff`: about 1564 guest instructions (about 1000
    VALU, 43 samples) become 99k SPIR-V words. About 1.9k are per-instruction
    EXEC selects, and each of 31 vector compares builds a full-mask ballot
    (11 quad broadcasts and 2 subgroup ORs). The lane-local mask plan that
    avoids the ballot is used only by split compute waves.
  - Vertex program `0x0a0014400c2ae56d`: 929 guest instructions (about 440 FP
    VALU, 39 buffer loads) become 65k words. Each four-dword buffer load is
    emitted as four one-dword loads with their own index arithmetic (125
    one-dword loads). The SPIR-V calls the guest address translation 27
    times, but those calls are dead after compilation (record above).
- **Bound: the transient pool in device-local host-visible memory** (gw133).
  Placing the whole transient pool there with a host shadow moved indexed
  draws from 53.3 to 45.6 us per call with flat fps, about 3 ms of 76. The
  shadow misses device writes from storage-to-vertex copies, so this is a
  bound, not a change; a same-binary arm is still needed before quoting it.
- **Excluded: concurrent sharing disabling Intel compression as the dune
  bottleneck** (gw137 against gw138).
  - Mechanism: on Intel before Xe2 with more than one device queue, ANV
    disables auxiliary surfaces (CCS compression, HiZ, fast clears) for
    every image created with `VK_SHARING_MODE_CONCURRENT`. Since compute
    moved to a compute-only family, every Kyty image is concurrent on the
    A770 (`docs/bringup/cross-title.md`, compute-only family record).
  - Experiment: the same binary with a temporary switch that keeps every
    queue in the graphics family. gw139 (`INTEL_DEBUG=perf`) confirmed the
    driver's "concurrent sharing mode" warning disappears in that arm.
  - Result: dune windows 9.9, 10.1, 9.9 fps (two families) against 9.9,
    9.9, 10.1 fps (one family). Per-draw GPU time is unchanged: `8ca0b9ff`
    116.2 against 116.3 us, `6bcfb82d` 316 against 320 us, graphics-queue
    compute about 4.4 ms per call in both. The exclusion covers this scene on this
    GPU, not other titles.
  - One of the two single-family runs (gw136) exited with code 0 about 80 s
    in, while the worldmap loaded, with no error in its log; the other
    (gw138) reached the dunes. Not investigated, since the arm does not ship.
- **Descriptor pool pressure.** Mesa reported `VK_ERROR_FRAGMENTED_POOL` 351
  times and `VK_ERROR_OUT_OF_POOL_MEMORY` 22 times in gw135; Kyty recovers by
  allocating new pools. Not measured as a GPU cost.
- **GPU busy time without timestamps** (gw140, same route, no timestamp
  queries, engine cycles sampled from the DRM client's `fdinfo` every 5 s):
  dune windows 10.6, 10.8, 10.8 fps; the render engine is busy about 75% of
  wall time (about 70 ms per 93 ms frame) and the compute engine about 7%.
  Timestamps barely distort the stage split above. The remaining ~23 ms per
  frame is command processing that does not overlap GPU work, so both the
  GPU program cost and the CP cost bound the frame rate.
- **Memory latency by placement** (stand-alone Vulkan microbenchmark on the
  A770: one invocation follows a random index cycle with a 64-byte stride,
  every load dependent on the previous one):

  | Placement | 16 KB set | 1 MB set | 8 MB set and larger |
  | --- | --- | --- | --- |
  | Device-local | ~260 ns | 389 ns | ~420 ns |
  | Device-local host-visible (BAR) | 88 ns | 372 ns | 421 ns |
  | System memory (host allocation or imported host pointer) | 95 ns | ~845 ns | ~1000 ns |

  System memory is cached by the GPU for small working sets, but a miss
  costs about 2.4 times a device-local miss. Guest-address loads that resolve
  to flexible-memory snapshots or direct-memory windows read system memory.
  The BVH kernel's inputs are cache-resident (record above); what the heavy
  pixel programs read has not been measured. ANV exposes only cached (snooped) system memory on the A770, so
  an uncached host-visible placement cannot be tested there.
- **Excluded: EXEC-select folding or lane-local compare masks as a way to
  compile `8ca0b9ff` wider than SIMD8** (offline, on the final NIR of the
  gw135 pipeline).
  - The driver compiles the pixel program at SIMD8 only: 4811 instructions,
    111 sends, no spills, 132 live registers.
  - A liveness pass over that NIR peaks at about 171 SIMD8 register units
    (110 divergent, 61 uniform). Of these, 51 uniform units hold
    uniform-block loads, 38 are NIR registers and about 24 are EXEC selects.
  - Rewriting all 934 EXEC-bit selects as plain moves lowers the peak only
    to about 156 (-9%). SIMD16 doubles every divergent value, so it would
    need roughly half the peak.
  - The lane-local plan instead of the per-compare ballot removes ALU work
    only, and the program waits on latency (3.7 ns per fragment against
    about 0.56 ns of ALU time, above). Neither change is pursued for this
    program.
- **Open levers, by measured share:** the BVH kernel; the pixel programs'
  constant loads (binding scalar-only, read-only V#s as uniform buffers so
  the driver can push them; excluded on paper in the record above); vector
  buffer loads in vertex programs.

## Robot platformer: stale modules in the shared shader store, and the BVH kernel cost (2026-10-29)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw111-gw119).

- **Observed.** The shared module store held `0x3666fa39`, the BVH traversal
  kernel, under translator version 184 written on 2026-10-28 at 19:49. The
  commits that define version 183 (`67ae9e1d9`, 21:25 that day) and 184
  (`c5a580803`, 2026-10-29 14:55) did not exist yet: a pre-release build
  with the version already raised wrote it.
- **Why it kept loading.** A module's identity is the shader id, stage,
  options and `kShaderTranslatorVersion`, not the generator text
  (`ModuleIdentity`, `SpirvBinaryCacheStore.cpp`). Every later run of the
  committed version-184 translator loaded those modules.
- **How it was found.** Runs with an empty private store (`KYTY_SPIRV_CACHE`,
  gw114, gw115) produced a different `0x3666fa39` module. The committed
  translator publishes each split-wave exchange once; the stored module
  routed every exchange through a wrapper that publishes twice (0, then the
  value), so it waits on twice as many rendezvous.
- **Scope.** Of the 333 modules the committed translator produced in gw115:
  - 233 are byte-identical to the shared store's entries of the same name
    (the translator is deterministic);
  - 84 differ: 76 written on 2026-10-28 around 19:00 and 8 on 2026-10-29
    around 14:00, by the `c5a580803` A/B arms;
  - 16 are missing from the shared store.
- **Measurements affected.** Every shared-store run at version 184 loaded
  modules without `67ae9e1d9` and `c5a580803` for those programs: the
  `c5a580803` A/B (gw99, gw100; `docs/bringup/cross-title.md`) and
  gw101-gw113. The Fix 4 figure of the dune compute cost record below
  (26.2 to 22.7 ms) loaded version-183 modules that a pre-release build of
  that commit wrote, so it is unverified too.
- **Cost of the committed module.** The kernel's time depends on the scene,
  so only 5 s windows of the same scene compare:
  - dune windows of gw118 (strict 480 s, a private store warmed by the cold
    run gw117, every module from the committed translator): 22.3-23.3 ms per
    call;
  - dune windows of gw111 and gw112 (the stored double-publish module):
    24.6-26.4 and 25.3-29.3 ms per call;
  - whole runs mix scenes: gw116 (late boot, hub and intro) averaged
    25.99 ms over 1345 calls, gw117 (cold boot) 20.28 ms with windows from
    24.7 to 33.9 ms.
- **Narrowed: fewer rendezvous is a small lever for this kernel.** Halving the
  rendezvous gained at most about 10% in dune windows, measured on different
  days, so most of the cost is elsewhere. Offline ANV statistics of the two
  recorded pipelines at SIMD8 (stored module, then committed module):

  | Statistic | Stored | Committed |
  | --- | --- | --- |
  | Instructions | 47838 | 39335 |
  | Sends | 2399 | 1486 |
  | Loops | 336 | 183 |
  | Spills:fills | 541:2354 | 538:2225 |
  | Max live registers | 360 | 362 |

  Merging the same-lane `v_readlane` exchanges of one stack push into fewer
  rendezvous would gain less than that halving did.
- **Excluded: the guest address lookup as this kernel's lever** (gw118 against
  gw119). The traversal is wave-coherent: the node pointer is an SGPR, each
  iteration loads one node through `image_bvh_intersect_ray` and the BLAS
  words through `s_buffer_load`, and each such load translates its guest
  address through the in-shader lookup (directory, binary search, entry: a
  chain of dependent loads). A temporary probe gave only this kernel a
  one-entry cache of the last translated table entry, checked with the same
  bounds as the lookup. Physical imports are entries of up to 64 MB, so the
  traversal's loads should hit it; the hit rate was not counted. The module
  with the cache (recipe changed, 140 more words) took 21.6-23.2 ms per call
  in the same dune windows against 22.3-23.3 ms without it, under 2%. The
  remaining candidates are the spills (538:2225 at SIMD8) and the rendezvous
  of the wave-wide mask and stack steps. This record also listed the latency
  of one workgroup per frame; the dispatch has 8156 workgroups (2026-10-30
  record), so that candidate does not apply.
- **Narrowed: the spills follow from the guest's register file** (offline,
  2026-10-29, the gw118 recipe compiled by ANV with its shader cache disabled).
  - The guest program uses v0-v73 and s0-s54. At SIMD8, ANV gives every
    32-bit value a whole register, uniform or not, and a thread has 128.
    The guest's own registers already fill them before any temporary.
  - A liveness pass over the final NIR puts the peak at about 265 registers
    (constants excluded, since they fold into instructions), against ANV's
    362. At that point 166 are per-lane values and 99 are uniform ones. Most
    are guest values that live across the traversal loop.
  - Rewriting the module with `spirv-opt -O` left it unchanged: 539:2216
    spills:fills and 369 max live.
  - Fills are spread over the whole program rather than gathered in one
    region. The outer loop holds 1072 fills and 305 spills. 86 of the 176
    rendezvous spin loops fill and spill one register on every iteration.
    This record assumed one workgroup on one Xe core, where each fill
    stalls a thread with little other work to switch to; with 8156
    workgroups per dispatch (2026-10-30 record) other threads can hide fill
    latency, and the fills cost issue slots and scratch traffic instead.
  - Since the peak is mostly guest values, reshaping the module cannot cut
    it by the 30% that would remove most spills; only `spirv-opt` was tried.
    Keeping cold guest registers in shared memory, which has lower latency
    than scratch, was worse when tried (2026-11-10 record above).
  - Excluded (2026-11-10, from the gw115 probe log of the guest program):
    running one guest wave per host workgroup, so that its parts could
    rendezvous with workgroup barriers. The program holds two `S_BARRIER`
    and one `DS_ADD_RTN_U32` on a 1920-dword LDS array, so the four guest
    waves of a workgroup share LDS and must stay in one host workgroup.
  - The guest's scalar state stays uniform after translation. In the final
    NIR, 175 of 183 loops and 400 of 712 registers are convergent. So
    scalar values made per-lane by the translation are not the source of
    the per-lane peak, and no uniformity lever is open there.
- **Fix.** Translator version 185 retires the stale modules. The next boot
  translates and compiles every program again.
- **Rule.** A build with an uncommitted or experimental translator runs only
  with a private `KYTY_SPIRV_CACHE` (`docs/BRINGUP.md`, debug and execution
  tactics).
- **Transient-pool stop on a cold boot** (gw114, private empty store,
  400 s; the cold boot gw117 on version 185 passed the same point). The run stopped while loading the worldmap on
  `!create_all_the_same` (`GpuMemoryCreate.cpp`): a new 1920x1080
  RenderTexture at `0x53ad00000` (size `0xff0000`) crossed or was contained by
  14 objects of a transient pool, among them StorageTextures, a 2432x1368 and
  a 1216x684 render target with depth, and 1920x1080 render targets. gw115
  and gw116, on warm private stores, passed the same point.
  - Cause (2026-10-29, from the gw114 parent table): every parent but one was
    accepted by the multi-surface render-target alias. The one blocker was a
    DepthStencilBuffer lying wholly inside the new colour target
    (IsContainedWithin). The depth reclaim accepted only a depth target of
    exactly the same range.
  - Change: a colour target that covers a depth target's whole range now
    reclaims it, as the exact-range case already did. A depth target that
    extends past the colour target stays live. Unit test
    `AllowsRenderTargetMultiSurfaceParents` covers both relations.
  - Not yet exercised live: the cold boot gw124 (300 s, empty store) reached
    the dunes at 240 s with clean captures, and a temporary probe on the new
    branch never fired, so the race did not recur. The AMD case in
    `docs/bringup/cross-title.md` (an incoming StorageTexture) stays open.

## Robot platformer: the clock-driven resolution controller is stereo-only, and the GPU clock rate (2026-10-29)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw84-gw88). Evidence came from
an uncommitted probe, a static read of the title's eboot and two
gdb runs.

- **Question.** Is the title rendering at a high internal resolution because
  its dynamic resolution sees end-of-pipe clock writes that Kyty stamps at
  host completion, so the GPU looks nearly free?
- **Observed: the controller is dormant.**
  - The controller pointer at eboot `+0xee29f68` points to a live object.
  - Its resolution index (`+0x0c`) is 3 from frame 69 to frame 4145.
  - Its target fps, GPU-ms and level fields stay 0 for the whole run.
  - Its update never runs, so its input timestamps cannot change the
    resolution.
- **Gate, from the eboot.** The frame-statistics function (eboot `0x2a57e0`)
  calls the controller update (`0x73e5060`) at `0x2a7ca7` only under three
  conditions:
  - the controller's static guard is set;
  - bit 2 (`& 4`) of the current display mode is set;
  - bit 0 of the statistics field `+0x5c` is clear.
- **The display mode is the title's `MainDisplay` setting.**
  - Its options are built at eboot `0x2b2887`: `Auto` = 0, `Mono` = 2,
    `HmdStereo` = 0xc, `SocialScreen` = 0x22, `SideBySide` = 4.
  - A value of 0 (`Auto`) falls back to 2 (`Mono`). This is the mode logged
    as `ChangeDisplayMode from %d to %d`.
  - Only `HmdStereo` and `SideBySide` have bit 2, so this controller updates
    only for head-mounted stereo or side-by-side output.
- **Live check** (gw87-gw88, gdb as parent, a hardware breakpoint on the
  gate and a watchpoint on the setting value):
  - the setting is written once, with 0, when it is created;
  - the gate then reads mode 2 on every frame sampled (39 of 39).
- **Excluded: this controller as an fps lever on a flat display.**
  - In `Mono` the title never runs it, so device timestamps for the clock
    writes cannot change the resolution.
  - A secondary implementation reports resolution changes on the same build.
    They cannot come from this update in `Mono`, so that report needs another
    mechanism or another display mode.
- **Found, then fixed the same day: the GPU clock rate.**
  - The ring reader converts a timer pair with `(end - begin) * 1e6 / 1e8`
    (multiply by `0xabcc77118461cefd`, shift by 90), so it assumes a
    100 MHz GPU clock.
  - `GraphicsRenderWriteAtEndOfPipeClockCounter`
    (`source/emulator/src/Graphics/GraphicsRenderEop.cpp`) wrote
    `Kernel::TimePort::GetCounter()`, which runs at 1 GHz on Linux. Every
    GPU interval the title computed was therefore 10 times too long.
  - Evidence for 100 MHz, from three independent sources:
    - the title's own conversion above;
    - the RDNA2 GPU timestamp clock: RADV reports `timestampPeriod = 10` ns
      for the RDNA2 iGPU of the test host (`vulkaninfo`);
    - one secondary implementation uses a 100 MHz domain for these writes.
  - Fix: on Gen5, the end-of-pipe clock write (`RELEASE_MEM` data_sel 3, the
    only GPU-clock source the command processor writes) converts the host
    counter to 100 MHz. Gen4 keeps the host counter, because Gen4 titles
    read its rate from `GetGpuCoreClockFrequency`, which returns the host
    counter's rate.
  - Gen5 exports no GPU clock frequency query, so no other guest-visible
    value has to change with it. `S_MEMTIME` and `S_MEMREALTIME` are still
    refused by the translator (`ShaderParseSMRD.cpp`).
  - Measured on the dunes (strict 340 s, gw103 against gw101 without the
    change): the first six 30 s windows ran at 39.2, 14.9, 17.8, 5.1, 11.7
    and 31.0 fps against 38.7, 14.2, 18.6, 5.9, 10.5 and 34.6. The frame rate
    is unchanged, as expected for a dormant controller. gw103 then lost the
    device at frame 3303; that is the GPU job timeout below, which the
    baseline gw100 also hit without this change.
- **Measured.** The gw85 dunes ran at 8.25 fps, with CP 226.2 us per draw
  including 66.7 us of fence wait, and 556 draws per frame. Without the waits
  the CP costs about 89 ms per frame, so a cheaper GPU alone cannot pass about
  11 fps.

## Robot platformer: GPU job timeout before the dunes, and dune wait sites (2026-10-29)

- **Open defect: GPU job timeout.**
  - Host: Intel A770, kernel `7.2.11-zen1`, Mesa 26.2.5, xe `job_timeout_ms` 5000.
  - One strict 340 s run on `8f3b5d6b6` (gw77) lost the device at frame 3167 (guest submit
    23165). The kernel reset the render engine for a timed-out `fc_script` job.
  - The faulting context was queue 7, slot 3, host sequence 13244. The recent-draw report holds
    no queue-7 record, only queue-8 work (dispatches of `0x4c97f40f`, 192x1x1, and `0x1404b233`,
    plus draws), so the hung job is unattributed.
  - Two other runs on the same kernel and binary (gw76, gw78) reached the dunes without a fault.
  - It recurred twice on 2026-10-29, at the same place in the route: gw100 (`12325b8be`, the
    baseline of an A/B pair) at frame 3364 and gw103 (`f0db2e1b5` plus the clock change above) at
    frame 3303. Five other runs of that day passed the same point (gw97-gw99, gw101, gw102). The
    kernel again reset the render engine for a timed-out `fc_script` job.
  - In both runs the first queue-8 command buffer that never completed holds the same work. In
    record order:
    - point-list draws of VS `0x360038d0f4cfb9a5` with PS `0x210045a05d93a5ea` (37 in gw100;
      11 in gw103, after 11 draws of VS `0x360043b08e51dd94` with PS `0x2100371083c06780` and
      `0x210046a060eca166`);
    - one draw with PS `0x210001602712f299`;
    - two fills by `0x4c97f40f` (4608 and 8640 groups; a 64-lane kernel that stores one 16-byte
      value per lane).
  - The recent-draw trail holds 64 records, so the work before these records is not known for
    gw100.
  - Lead, not verified: PS `0x210045a05d93a5ea` walks a per-cell light list. It reads
    `head[cell]` through the V# at table offset 256, then follows `{light, next}` nodes through
    the V# at offset 272 until `next == -1`. The loop has no other bound, so a node buffer that
    reads back 0 (a zero carrier or an out-of-range binding) or a list with a cycle never ends.
  - The program does not hang by itself: gw96 drew it about 1060 times over three 5 s windows
    without a fault.
  - The fills come after the walks, so they are not a list-head clear for these draws. The walks
    read heads and nodes written before this command buffer. A hang therefore needs valid heads
    whose nodes read back zero or stale, which is a publication-order question between earlier
    writers, possibly on another queue.
  - The PS is rare on this route: of the saved runs, only gw96, gw100 and gw103 drew it, about
    175-225 s into the route. Probe runs gw105-gw107 (a temporary probe at all three PS bind
    sites) logged nothing, and so did the clean gw109 (frame 4282). A Blender render of another session shared the GPU during all three,
    so they ran slower (frames 3101, 3235 and 3555, against 4322 for gw104), and the time-based
    input route then plays differently.
  - It recurred on 2026-10-30 in gw144 (`ad7028e68` plus an uncommitted
    translator 186 experiment on a cold private shader store) at the same
    route point, about frame 3408, during a level load. The kernel reset the
    render engine for a timed-out `fc_script` job and reported no page fault.
    The queue timeline wait (`vkWaitSemaphores`) saw the loss; the run kept
    no submit fault entries.
  - It recurred again in gw148 (`ad7028e68` plus probes, default route, RenderDoc layer loaded):
    `vkWaitForFences` returned -4 on queue 8, slot 4, host sequence 76005, and the kernel logged
    a timed-out `fc_script` job. Its hung queue-8 buffer draws VS `0x360038d0f4cfb9a5` with PS
    `0x21001960c4a8f3bb`, not with the light-list PS.
  - An offline comparison of the nine kept trails (gw23, gw25, gw42, gw44, gw54, gw77, gw100,
    gw103, gw148) finds the same tail in seven of them, in the first queue-8 buffer that never
    completed: one 3-vertex rect draw (VS `0x0a00027047a9c428`, PS `0x210001602712f299`), then
    the two `0x4c97f40f` fills of 4608 and 8640 groups. The 24-group fills of the same kernel
    complete in the same trails. gw23 (a queue-7 buffer) and gw25 do not have the tail.
  - The light-list PS `0x210045a05d93a5ea` is in only two of the four recent trails (gw100,
    gw103), so the light-list lead above does not explain the timeout on its own.
  - Neither the rect PS nor the fill kernel can spin: their stored modules have no loop
    (`OpLoopMerge`). The trail names the buffer that timed out, not the command inside it.
  - gw148 and gw150 (same binary; gw150 under gdb) timed out at the start of the hub, right
    after its level load, where gw146 and gw149 hit the guest assert above. In gw150
    `vkWaitForFences` returned -4 on queue 8, slot 1, host sequence 87154; its hung buffer holds
    16 point-list draws of VS `0x360038d0f4cfb9a5` with PS `0x21001960c4a8f3bb`, then the rect
    draw and the two large fills.
  - gw152 (same binary, default route) lost the device at the same point.
  - Eight of the ten kept trails (all but gw23 and gw25) hold point-list draws of a
    `0x3600...` vertex program (`0x360038d0f4cfb9a5` or `0x360043b08e51dd94`) in the hung
    buffer.
  - **Localized on 2026-10-30 (`1cd28c3d2` plus probes, Intel A770, xe, Mesa 26.2.5).**
    Per-command buffer markers name the command. Every timed-out run with markers stops
    inside the `vkCmdDispatch` of the geometry compute program of VS `0x360038d0f4cfb9a5`,
    between the marker after its descriptor bind and the marker after the dispatch
    (gw197-gw199, gw202, gw204, gw206; 2 or 15 groups). The same pipeline completed in the
    steps just before, the pass end, the fill and the bind all land, and the kernel reports
    no page fault. The stop lasts about 0.6-1.1 s before the device loss in five runs, near the
    640 ms engine preempt timeout, and 6.1 s in gw198, the 5 s job timeout.
  - Marker semantics, read from the Mesa source (main branch, 26.2.5 not compared): ANV adds
    an end-of-pipe sync before every buffer marker at any stage, so a landed marker means the
    earlier command finished, and the probe serialises the buffer.
  - **Excluded, each with the program patched in a private store and the file confirmed
    unchanged after the run:**
    - the 2^20 rendezvous spin limit as the holder: 4096 (gw198) and 64 (gw199) still stop at
      the same step;
    - the 6 workgroup-scope execution barriers: rescoped to subgroup (gw202) still stops;
    - the only other loop, the guest-address binary search, whose `(lo + hi) >> 1` wraps in
      32 bits if a table word reads above 2^31: an overflow-safe midpoint on top of the two
      changes above (gw206) still stops. With every loop bounded and no workgroup barrier
      left, the program text cannot spin, so what holds the engine is not known.
    - a part-divergent branch around a rendezvous, statically: 0 sites, and a hand-built
      positive was reported. Not checked: uniformity of the early-return condition before 11
      rendezvous.
    - overlap with the compute family as a requirement: with each submit first waiting for the
      other family's queues (200 ms bound), gw204 still stopped, and it hit that bound only
      after the stop began.
  - **An instrument failure to avoid:** gw205 looked like a pass of the patched program, but
    its header payload size was stale, so the store rejected the file and regenerated the
    original. It counts as an unpatched pass. Re-disassemble the store entry after each run.
  - **Compute family against one family (not separated).** Hub-start runs on this binary with
    the bone-palette probe or until the guest assert:

    | Layout | Stopped by timeout | Passed the hub start | Ended early by the guest assert |
    | --- | --- | --- | --- |
    | Compute family | 9 (gw192-gw194, gw197-gw199, gw202, gw204, gw206) | 2 (gw205, gw208) | 1 (gw203) |
    | One family | 1 (gw214) | 2 (gw201, gw207) | 1 (gw200) |

    Two passes against a rate of about 80% is suggestive, not proof. An earlier count of 6/6
    against 0/2 overstated it: gw200 ended before the timeout point and the compute-family
    passes came later. Level load time does not separate the outcomes either (timeouts at
    3.8-9.4 s, passes at 4.2-8.9 s), so the earlier fast-load table no longer holds. One
    family does not lower the crowd rate: gw201 and gw207 read about 5-7 fps, gw191 about 7.
    gw214 (`8325dc821` plus diagnostics, one family) stopped the same way: the kernel reported
    `Engine reset: engine_class=rcs` and a timed-out job about 180 s into the run. So one family
    does not prevent the timeout, and no compute-queue job is needed for it. gw210 (one family,
    220 s bound) reached the hub with no timeout; gw211-gw213 ended at their bound soon after the
    hub load and are not counted. The order-9 page allocation failure in that kernel report comes
    from the core dump capture of the GuC log, after the reset, so it is not a cause.
  - **Next step:** the engine state of a timeout. The xe core dump
    (`/sys/class/drm/card0/device/devcoredump/data`, root only, kept 5 minutes) holds the ring
    head, batch position and engine status. Without it, only further blind variations remain.
- **Validation run at the hub, resolved 2026-10-30 (gw215 one family, gw216 compute family, gw217
  after the fixes, counted without deduplication).** gw217 reports neither of the first two; it ended at 200 s, too early to speak for the third. gw224
  (340 s, on the pull vertex stage fix) also reports neither of them, and its `viewType` messages prove it
  reached the scene of gw215 and gw216.
  - Depth-aspect copies on a compute-only queue: 924 pairs of `vkCmdCopyImageToBuffer` and
    `vkCmdCopyBufferToImage` in gw216, both directions illegal. Intel Arc A770 does not declare
    `DEPTH_COPY_ON_COMPUTE_QUEUE` for any depth format. Queue selection now uses a compute-only
    family only when the host declares it for every depth format it renders to (with
    `VK_KHR_maintenance10` enabled), so this host runs all queues on the graphics family.
  - A barrier to `COLOR_ATTACHMENT_OPTIMAL` on the transfer-only presentation stage image, about
    2,600 per run: `UtilBlitImageTo` now restores that layout only for images with attachment
    usage.
  - A built-in block mismatch between VS output {Position, PointSize, ClipDistance, CullDistance}
    and the interpolation geometry stage input {Position}: the input now declares the full block.
    Geometry translator 96.
  - Believed to be a static over-approximation of the layer, unconfirmed by GPU-assisted validation: `VUID-vkCmdDispatch-format-07753`,
    `viewType-07752` and `None-02691` on bindings 1, 2, 6 and 7. Those bindings hold aliased SPIR-V
    variables, one per typed view (`ShaderPlanStorageImages`, `mirror_unfilled_bank`), and every
    access goes through the variable matching its descriptor, while the layer checks each variable
    against every array element. The paired counts agree (RGBA16F "requires UINT" equals R32_UINT
    "requires FLOAT"). The atomic case is settled: an image atomic is refused unless its descriptor
    is a proven 2D `R32_UINT` (`ShaderImageAtomicResourceSupported`), so the atomic on
    `R16G16B16A16_SFLOAT` is never executed.
  - GPU timeout after the gate: gw218, gw221, gw222 and gw223, default mode, 300 s each through
    the hub with no engine reset, fps windows 3.7 to 16.8 under foreign CPU load. Before it the
    compute family hung 9 of 11 runs and one family 1 of 3, so one family is 1 of 7 so far. This is
    a correlation: the hang is a dispatch on the render engine and its cause is still open.
  - Open, found while fixing the block: the interpolation geometry stage forwards no `ClipDistance`,
    so a vertex shader exporting clip distance 0 through that stage loses it
    (`ShaderInterpolationGeometry.cpp`). No run shows a visible effect yet. Direction: forward it when
    `UsesVertexClipDistance0Export` holds.
- **In-flight write-back refusal: the stop became a host wait** (gw108 `453fd755a` plus
  diagnostics, frame about 2044; gw209 `fd6dcbf8b` plus diagnostics, about 107 s, worldmap load).
  - Both runs stopped with `GpuMemory cannot version an in-flight write-back backing:
    type=StorageBuffer size=0x1fa40 queue=7` (`GpuMemoryCreate.cpp`, `GpuMemoryChooseMutationAction`
    returns `RejectWriteBackConflict`). In gw209 the pending use was queue 7 sequence 6120, with 3
    device-indirect dispatches and no arguments inside the object.
  - Change: `GpuMemoryMutationConflictScope`. The indexed draw, auto draw, geometry pass and compute
    dispatch paths arm it after preparation. A conflict leaves the object as it was and reports the
    pending use; the operation abandons itself before its first draw or dispatch, and the processor
    waits for the use without recording locks, as for other pending dependencies. The use is
    written back first and the retry uploads the changed bytes. A parent invalidated by a write-back
    while it has its own pending writable uses now takes the bytes at its next use.
  - Verification: gw213 forced the conflict on one in 200 writable in-flight binds. It fired 62
    times (queue 7: 48, queue 8: 14), with no stop and no failed retry, and the run reached the hub
    bound of 150 s. gw210 and gw211 passed the same route, with no natural conflict.
  - What triggers the natural case is not shown: neither stop reproduced after the backtrace and
    alias listing were added (gw210-gw213). gw212 counted 95000 ordinary rehashes of write-back
    storage with no pending use, so ordinary rebinds are not the route. Lead, not verified: the
    write-back invalidation of an aliased object, which clears `submit_id` and the hash.
  - Open limits: the fragment transport path and uploads outside a draw or dispatch keep the strict
    stop, because they cannot abandon. Bytes the guest stored in a page that the pending use also
    wrote can still be overwritten by the write-back, as for any store during a use.
- **Dune wait sites** (gw78, same binary, whole run, fence waits of 0.2 ms or longer, by call site):

  | Call site | Wait time | Waits |
  | --- | --- | --- |
  | `DISPATCH_INDIRECT` argument reads | 31.4 s | 7596 |
  | 32-bit `WAIT_REG_MEM` | 21.3 s | 11432 |
  | `DRAW_INDEX_INDIRECT` argument reads | 12.8 s | 8728 |
  | Indexed draws over pending device-address storage | 6.6 s | 4338 |
  | `DMA_DATA` image fills | 3.2 s | 7829 |

  - Argument reads that hit the current recording submit it and wait for all of it, so each wait
    lasts as long as the queued GPU work.
  - The frame is serialized: in the dune windows the processor waits on the GPU, and the GPU then
    idles while the processor records.
- **Excluded: eliding `DRAW_INDEX_INDIRECT` record reads by register rule.** One reference skips
  the CPU read when `INDEX_BUFFER_SIZE` cannot hold one input primitive. In the dunes these
  draws are point lists (`prim=1`) with `INDEX_BUFFER_SIZE` 1-2, so one index already draws a
  point (gw79):

  | Records | Count | Argument-read wait |
  | --- | --- | --- |
  | Empty | 3339 | 12.85 s |
  | Carried work | 376 | about 0 s |
  | Triangle lists with a large index buffer | 285 | about 0.1 s |

  The rule would drop real points, so it does not apply. The sync-chain design is in
  `docs/bringup/cp-gpu-sync-chain.md`.
- **Parked: `0x3666fa39` latency** (about 22-27 ms per call, the largest single GPU item; 22.3-23.3 ms
  in dune windows for the committed translator's module, see the stale-module record above).
  - The guest builds a deduplicated list across lanes: `s_ff1` over a compare mask, `v_readlane`
    of the found lane, and `v_writelane` into one VGPR capped at 60 entries. It does this 30 times.
  - Every step needs the whole 64-lane wave, so each is a rendezvous of all eight split parts.
    Narrowed later the same day: a module with half the rendezvous gained at most about 10%, so
    the rendezvous count does not set most of this cost (stale-module record above).
  - The listing shows no translator change to test, and the offline loop-retention check used for
    the earlier spin work is not available. No change is attempted.
  - Running it on the compute family is excluded too (gw128): the very next compute dispatch reads
    its output, so there is no work to overlap it with (`docs/bringup/cp-gpu-sync-chain.md`).
- **Not measurable: scanning the NGG front once per program** (`5790919c2`). The GS-front
  vertex path now computes its scan of the immutable front program (GETPC, global loads,
  scalar-buffer base registers, native wave state) once per cached program instead of per draw.
  - One strict 340 s dune run with the change (gw104) against gw101: per draw, state setup fell
    from 32.5 to 30.1 us, resource binding from 43.2 to 40.6 us and storage descriptors from 12.3
    to 11.1 us. The totals rose: command processor from 236.8 to 250.4 us, draw processor from
    190.5 to 197.9 us.
  - gw101 ran another binary with a diagnostic probe, and the two runs drew 585 and 601 draws per
    frame. The comparison is not like-for-like, and the gain is within run-to-run variance.
  - gw104 reached frame 4322 in 340 s, interactive throughout, without a device loss.

## Robot platformer: host memory, descriptor churn and a recycled view at hub load (2026-10-28)

- **Observed** (Intel A770, strict 340 s route to the crash-site dunes,
  60 fps limit): the process peaked at 2759 MB of anonymous memory plus
  swap. A sampling heap profiler (scratch `LD_PRELOAD` tool, not in the
  repository) put 2080 MB live at 210 s. The largest items were the
  descriptor cache table (about 470 MB), the driver descriptor pools (about
  450-490 MB, growing with live and recycled sets), the write-back page
  snapshots (about 256 MB), the dirty-page tracker (112 MB) and the driver
  context (about 125 MB).
- **Localized: descriptor churn.** The dunes make about 1000 set lookups
  per frame and hit about 31 %. The cache sat at its 65536 live-set cap
  with 37 driver pools. Temporary counters classified the misses of one
  30 s window: 136305 of 138943 differ from a cached set only in the
  storage-buffer ids of buffers that already exist (rotating ring slots),
  424 bring a new buffer object, 351 differ only in ranges, 4 only in
  images. Holding more sets therefore buys almost no hits.
- **Fix 1** (`f14a6d996`): cache entries keep only their bound storage
  buffers, images and samplers instead of arrays sized for the largest
  layout. The live heap at 210 s fell from 2080 to 1635 MB, the table from
  about 470 to 35 MB, with the same hit rate.
- **Fix 2** (`68dc3bbfa`): the live-set cap is 16384, trimmed 2048 at a
  time (the recycle limit stays four trims). Hit rate about 31 % as
  before, 10 driver pools instead of 37. Against the tree before both
  fixes, the run peak fell from 2759 to 1743 MB and the anonymous plus
  swap footprint at 150 s from 2422 to 1666 MB. Dune windows ran at 7.57
  against 7.47 fps, the command processor at 231.5 against 241.8 us per
  draw, and the descriptor finalize stage at 9.2 against 8.6 us per draw
  (within run noise).
- **Fix 3** (`c8abdb479`): the attributes a pixel program reads through
  `V_INTERP_*` are summarized once per parsed program instead of on every
  draw. Its share of the per-draw shader analysis was not measured
  separately.
- **Fix 4** (`67ae9e1d9`, translator 183): pointwise `S_*_B64` algebra over
  in-block lane-local masks gathers only its root in a split wave.
  `0x3666fa39` went from 26.2 to 22.7 ms mean per call; the sample is small
  and noisy, and the runs loaded modules a pre-release build wrote (see the
  stale-module record), so the figure is unverified.
- **Fix 5: strict stop at hub load** (`5112aeec9`). One run stopped at about
  200 s, while loading the crash-site hub level, on `!create_all_the_same`:
  a 3840x2160 single-level storage view (tile 27, 8-byte texels) over 22
  objects of a recycled range. The render-alias rule seeds such a view from
  the 64 KiB blocks of the 2432x1368 targets with the same texel size, in
  write order, but required every other parent to be crossing or
  overwritten since by those targets. 4-byte 480x270, 240x135 and 120x67
  downsample targets and four-byte storage views had drawn there after
  them. The single-parent path and the storage-over-caches rule already link
  such parents as re-descriptions (another texel size, tiling or format), so
  the render-alias rule now links them too. The same load stopped in four
  earlier runs that day on other parent sets (a superseded storage buffer, a
  shifted copy of the same view); those were resolved by `467401517` and the
  crossing rule before this run. The fixed case is intermittent and did not
  recur in the runs below, so it is verified only against the recorded
  parent list.
- **Open: remaining host memory.** The write-back page snapshots (about
  256 MB) and the dirty-page tracker (112 MB) are the next items. A lazy or
  partial snapshot must keep the protection of CPU writes to GPU-flagged
  pages.
- **Verified.** Focused `kyty_unit_test` suites (1495 passed), the three
  repository CTest gates (4 tests passed), strict 340 s dune runs on Intel
  A770 (time limit reached), and the strict games matrix: 26 of 32 titles
  reach the interactive state, as before these fixes. The per-draw cost of
  the titles with the most draws per frame was not re-measured under the
  lower live-set cap.

## Robot platformer: dune waits on aliased storage, DMA stores, pipeline churn and indirect arguments (2026-10-28)

- **Observed** (Intel A770, strict 340 s route to the crash-site dunes,
  60 fps limit, warm pipeline cache): the dunes ran at 6.4 fps with
  `WAIT_REG_MEM` taking 6.5-6.7 s and fence waits 9.1-9.4 s of each 30 s
  window. Nearly all of the `WAIT_REG_MEM` time was 64-bit waits kept
  synchronous by the pending-alias check (1020 waits, 28.8 s per run).
- **Localized.** A diagnostic census of the aliases of each pending
  writable storage buffer at those waits found only skipped parents and
  textures created with `PARAM_SKIP_GUEST_UPLOAD`. Their write-back refresh
  writes transparent black or copies from the device and never reads the
  published guest bytes, so the write-back does not have to run before them.
- **Fix 1** (`e44b49060`): the pending-alias check counts only aliases whose
  refresh reads guest bytes. Those waits fell from 1020 (28.8 s) to 5
  (0.009 s) per run, and `WAIT_REG_MEM` to 0.65 s per dune window.
- **Fix 2** (`467401517`): consolidating those waits exposed a strict
  `create_all_the_same` stop at about 150 s. A storage texture over a
  recycled 1920x1080 range met a storage buffer whose last writable use
  (still in flight) came before render targets drew over that range. Its
  write-back publishes bytes the targets replaced, so the buffer is now
  redescribed like a superseded storage texture.
- **Fix 3** (`a8c76062a`): the wait then moved to one-dword immediate
  `DMA_DATA` stores, which retired the consolidated producer on the host
  (58 s per run). They are now deferred to end of pipe like a one-dword
  `WRITE_DATA` (0.003 s per run). Loading windows got faster: the first
  window went from 25.5 to 33.5 fps, and the dunes were reached about 400
  frames earlier.
- **Result after fix 3.** The dunes were still at 6.7-6.8 fps: the
  command processor was busy about 175 ms per frame and the GPU about 83 ms.
  Fence waits per 340 s run are now led by CPU reads of indirect arguments
  (`ReadCurrentGuestBytes` waiting on their producer): 31 s for
  `DISPATCH_INDIRECT` and 11 s for `DRAW_INDEX_INDIRECT`, then 10.5 s for
  32-bit `WAIT_REG_MEM` and 6 s for indexed draws over pending ranges.
- **Fix 4** (`5e4d4a964`): pipelines re-created with transient targets. A
  diagnostic count over one run found 2118 framebuffers created and 2969
  pipelines deleted with them: pipelines were keyed by a per-framebuffer
  sequence, so a target recreated every frame also recreated its pipelines
  (3313 misses per run, about 2.6 s of each 30 s dune window). Pipelines are
  now keyed by Vulkan render pass compatibility (attachment formats and
  sample counts), the extent, and whether the pass begins without a clear,
  which fragment transport requires; a freed framebuffer no longer deletes
  them. Misses fell to 293 per run and to none in the last dune window;
  the dunes went from 6.7-6.8 to 7.2-7.5 fps.
- **Verified.** Focused `kyty_unit_test` suites (1152 passed with fix 4),
  the three repository CTest gates (4 tests passed), five strict 340 s runs
  with no fatal stop; the dune captures at frames 3797 and 4207 match the
  previous build's captures of the same scene. The strict games matrix on
  Intel A770 with fix 4 reached 25 of 32 cases interactive (24 of 32 on
  2026-10-27). Its one new non-interactive case, `PPSA03969 - F.I.S.T.`
  (host crash after 30 presents), is the startup race recorded in
  `docs/titles/mechanical-fist-platformer.md` (guest mutex at offset 0x58 in
  `PthreadMutexLock`); it also crashed in three of six matrix runs on
  2026-10-27, before these fixes.
- **Excluded for now: indirect dispatch arguments read on the device.**
  A `DISPATCH_INDIRECT` recorded as `vkCmdDispatchIndirect` from the
  argument buffer (with a barrier to indirect command reads) removed the
  host reads of dispatch arguments, but the dune windows stayed at
  7.2-7.5 fps: the fence waits moved to the `DRAW_INDEX_INDIRECT` argument
  reads and the queue-wide device-address wait before them. Two of four
  strict runs lost the device on queue 8 (`KYTY_SUBMIT_FAULT stage=fence_wait
  result=-4`). The change was reverted; it needs indirect draws on the device
  as well and the cause of the device loss found first.
- **Fix 5** (`1fd051d50`): indirect draws waited for every pending
  device-address write-back on their queue before reading their arguments,
  although `ReadCurrentGuestBytes` already waits only for writers of the
  argument range and direct draws never used the queue-wide wait. It was
  removed. One strict 340 s run: dunes at 7.6-7.8 fps (7.2-7.5 before),
  fence waits still about 9.3 s per 30 s window. Fence waits per run, led by
  host reads of indirect arguments: `DISPATCH_INDIRECT` 40 s,
  32-bit `WAIT_REG_MEM` 12.8 s, `DRAW_INDEX_INDIRECT` 12.6 s, indexed draws
  over pending ranges 7.8 s, `DMA_DATA` image fills 2.8 s.

## Robot platformer: dune compute cost and split-wave rendezvous (2026-10-28)

- **Observed** (Intel A770, strict route to the crash-site dunes, 60 fps
  limit, warm pipeline cache): about 6 fps. A native GPU timestamp census
  put about 83 ms of GPU work in each frame with the GPU busy about half the
  time: compute 36 ms, of which one program (`0x3666fa39`) took 24 ms per
  call, indexed draws 20 ms, the geometry pass 7 ms. The processor waits
  about 46 ms per frame on fences and 36 ms on `WAIT_REG_MEM`.
- **Localized.** `0x3666fa39` runs 16x16 workgroups (four guest waves each;
  3316 instructions, 72 VGPRs, scalar find-first-set and `v_readlane` loops
  ending in an image store). This record first said a single workgroup per
  dispatch and concluded that the cost was latency, not occupancy; the
  dispatch has 8156 workgroups (2026-10-30 record), so the pass fills the
  GPU. As split wave64 on 16-lane host subgroups it spilled heavily:
  offline ANV compiles of the recorded pipeline gave 1574:9325
  spills:fills at SIMD16 and 470:2397 at SIMD8.
- **Fix 1** (`6242d13ac`): guest programs over 64 VGPRs split into eight
  8-lane parts where the host allows width 8; others keep 16-lane parts.
  `0x3666fa39` went from 36.9 to 24.4 ms per call.
- **Fix 2** (`acd44a7f8`, translator 181): a rendezvous waited for the other
  parts one after another; one spin now reads every part's sequence, so
  their latencies overlap. `0x3666fa39` went from 24.4 to 21.5 ms per call;
  dune windows from 6.0-6.1 to 6.4-7.0 fps. The frame-3280 capture shows
  clean dunes.
- **Excluded: relaxed loads in the spin.** With relaxed loads and one
  acquire fence after the spin, Mesa removes the loop wherever the caller
  ignores the returned sequence (`split_sync`): the final NIR of an 8-lane
  pipeline had 32 of 188 rendezvous with no loop between the release store
  and the acquire fence. The parts then ran unsynchronised and the device
  was lost on the worldmap at 43-46 s (translators 178 and 180). Acquire
  loads keep every loop (200 of 200 at translator 181).
- **Fix 3** (translator 182): ANV compiles each acquire load in the spin as
  a shared-memory load followed by a fence that waits for it, so one spin
  iteration of P parts paid about 2P serialized round trips. Only the last
  load of each iteration now acquires; the others are relaxed and issue
  together, and one acquire fence follows the loop. The acquire load keeps
  the loop, unlike the excluded fully relaxed spin: the final NIR of the
  same 8-lane pipeline keeps all 200 rendezvous loops, with shared-memory
  fences down from 1843 to 643 and instructions from 66105 to 50259.
  `0x3666fa39` went from 28.9 to 22.4 ms per call (GPU timestamps, strict
  340 s dune runs before and after, last six 5 s windows each).
  Focused `kyty_unit_test` suites (1492 passed); the strict games matrix on
  Intel A770 reached 26 of 32 cases interactive (25 of 32 before), with no
  case losing presents.
- **Open: 32-lane parts render wrong** (2026-10-29, translator 183, Intel
  A770). Forcing two 32-lane parts per guest wave instead of eight 8-lane
  parts for programs over 64 VGPRs (a temporary override, compute pipelines
  compiled in an earlier run) drew the dunes saturated yellow with the cliffs
  missing in every dune capture of a strict 400 s run (frames 2867-3407);
  8-lane runs on the same route are clean. `0x3666fa39` took 101 ms per call
  there against about 22 ms with 8-lane parts, but with wrong output that
  time says nothing about the rendezvous trade-off, so 32-lane parts are not
  excluded on cost. 32-lane parts are the default split on hosts whose only
  subgroup size is 32, so the defect matters beyond this experiment.
  Unknown: whether the wrong image comes from the split path at 32 lanes
  (exchange layout, lane masks above bit 31 of a part) or from ANV at
  SIMD32. No test executes a split program on a GPU (see the test gap
  below). Direction: force 32 lanes for one program hash at a time to find
  the program with the wrong output, compare its 8-lane and 32-lane results
  offline, then check a host with a fixed 32-lane subgroup.
- **Excluded: GDA address translation cache** (translator 174): some
  compute pipelines took 13-16 s each to compile on ANV and the worldmap
  stopped on a strict storage refusal at 72 s.
- **Test gap.** The 1147 focused unit tests passed with the broken spin; no
  test executes a split rendezvous on a GPU. Only a strict run or an offline
  NIR check of a recorded pipeline shows it.
- **Excluded: consolidating write-back waits over aliased storage.** A wait
  on a current-submission label whose `RELEASE_MEM` also writes back stays
  synchronous while a pending writable storage buffer shares bytes with
  another GPU object. In a 480 s dune run those waits took 98 s (32-bit
  32 s, 64-bit 66 s at about 39 ms each), against 0.03 s for the same waits
  without an alias. A diagnostic build that consolidated them as well
  removed the `WAIT_REG_MEM` time, but it stopped at about 90 s on a strict
  `create_all_the_same` refusal: a storage texture contained in and
  crossing objects still owed by the pending write-back. Until that write-back,
  59 % of the consolidations (5172 of 8789) looked up an alias that was not
  storage: mostly the depth target's exact HTILE range (221k lookups), then
  render textures (14k) and textures (2.6k). A lazy per-resource wait would
  therefore still wait in most cases. The lead is to propagate storage
  writes to their image aliases on the device in queue order, so they no
  longer depend on the host write-back.
  Narrowed by the dune waits record above: the aliases at the dune waits never read
  the guest bytes, so those waits now consolidate.
- **Narrowed** (2026-10-29, see the dynamic resolution record): the title's
  resolution controller reads these timestamps, but it only runs for stereo
  or side-by-side display modes.
- **Verified.** Focused `kyty_unit_test` suites (1147 passed), the three
  repository CTest gates (4 tests passed), strict 340 s runs on Intel A770
  (cold and warm) with no submit fault.

## Robot platformer: hub ground samples mips below the T# MIN_LOD (2026-10-28, fixed)

- **Observed** (Intel A770, strict route to the first crash-site hub, 60 fps
  limit): the sand dunes drew as power-of-two orange and yellow rectangles
  with noisy patches, a different pattern in each run.
- **Localized.** A native draw census (`KYTY_TRACE_DRAW_PS=all` with
  `KYTY_TRACE_DRAW_PS_MIN_PRESENT` and `KYTY_TRACE_DRAW_PS_LOG`) named the
  terrain draw: a G-buffer pass (five colour targets) sampling 2048x2048
  tile-9 textures with 12 mips in BC7 sRGB, BC5 and BC4. The shader log of
  that pixel program carried the bind warning that `T# MIN_LOD != 0` was
  ignored.
- **Cause.** The title streams terrain mips and raises T# `MIN_LOD`
  (word 1 bits 19:8, unsigned 4.8, in image levels; RDNA2 ISA image resource
  table) above the levels it has not loaded yet. Kyty bound views of every
  level, so the sampler read non-resident mips holding stale bytes, one
  rectangle per terrain page.
- **Fix.** `VK_EXT_image_view_min_lod` carries the same field: a view's
  `minLod` is absolute in image levels and Mesa's AMD driver writes it into
  that T# field unchanged. The device enables the extension and its `minLod`
  feature when present (Intel A770 and the AMD Raphael iGPU both expose it).
  Image view descriptors carry the u4.8 value; a guest texture or sampled
  alias bound with a `MIN_LOD` above the view's base level gets a cached view
  clamped to it, limited to the view's last level.
- **Limits.** Sampled depth and stencil views stay unclamped (descriptor
  layouts are chosen from their standard view id) and log a bounded warning,
  as does a device without the extension. Clamped views use the per-image
  view cache (256 views); a title that sweeps many fractional values per
  image would stop at its explicit refusal.
- **Verified.** Focused `kyty_unit_test` suites (1100 passed). Strict 270 s
  run on Intel A770 (60 fps limit, scripted intro skip): the hub capture at
  frame 3086 shows clean sand dunes where frame 3030 of the previous build
  showed the rectangles; no MIN_LOD warning, no fatal stop. Hub fps is
  unchanged (about 6).

## Robot platformer: hub vertex streams re-uploaded every frame (2026-10-28, fixed)

- **Observed** (Intel A770, crash-site hub): about 1.4 fps, with about
  860 MB per frame copied between storage and vertex objects of the same
  ranges.
- **Cause.** Vertex streams over ranges also held by read-only storage
  objects created their own vertex objects; each one reclaimed the other's
  range, so every frame re-uploaded both.
- **Fix** (`5ff4a6a3e`): vertex streams are fetched from a read-only storage
  object that contains them.
- **Verified.** Strict hub run: about 6 fps. Remaining cost per 30 s window:
  command processing 30.6 s, fence waits 14.4 s, draw 14.1 s, dispatch
  10.8 s, write-back 9.5 s; about 80 submits and 35 fence waits per frame.

## Robot platformer: whole-dword half selects and a recycled 4K storage allocation (2026-10-27)

- **Observed** (Intel A770, strict route through the intro into the first
  crash-site hub, 60 fps limit), each stop after the previous fix:
  - About 1670 presents: a compute program stopped at its emission,
    `v_add_f16` in SDWA form at pc `0x32c` with `DST_SEL = DWORD` and
    `SRC0_SEL = DWORD`; the half destination admitted only `WORD_0` and
    `WORD_1`.
  - About 1910 presents: a 3840x2160 tile-27 storage image (format `0x47`)
    was created over a range that crosses two 2432x1368 render targets and a
    2432-pitch storage image; the overlap set did not fit any admitted rule
    and the run stopped at the `create_all_the_same` refusal.
  - After both fixes the run passed 3200 presents into the hub.
- **Contract.** A 16-bit operand under a `DWORD` source select reads bits
  15:0 of the register, and `DST_SEL = DWORD` is the instruction's own
  unmodified store (RDNA2 ISA, SDWA fields). A storage view whose format,
  tile mode, texel size, extent or pitch differ from a view over an equal or
  crossing range cannot address the same texels: the guest has recycled the
  allocation, so the new view seeds from guest memory instead of a texel
  copy. A contained range may still be a sub-view of the same image, so it
  keeps the layout-insensitive comparison.
- **Fix.** The half SDWA destination admits the three word selects (4-6) for
  the destination and every source; byte selects, sign extension, `DST_U`
  above 2 and SDWA clamp or OMOD stay refused. The storage redescription
  predicates compare pitch as well, and compare the layout for equal and
  crossing ranges (before, only equal ones). A new storage image whose every
  parent is such a redescribed storage image or render target links all of
  them as one recycled allocation.
- **Packed min and max.** VOP3P opcodes 17 and 18 are `V_PK_MIN_F16` and
  `V_PK_MAX_F16`; each half follows the scalar `V_MIN_F16`/`V_MAX_F16` rule
  with `IEEE_MODE = 0` (a NaN operand yields the other input). They reuse the
  packed half emitter.
- **Verified.** `Fp16SdwaHalfDestinationReadsAndPreservesHalves` runs the
  whole-dword encoding for both lane states; the refusal test now uses SDWA
  clamp, still unmodeled. `PackedHalfArithmeticSelectsAndNegatesEachHalf`
  checks min, max, negation and the NaN rule. The arithmetic, half and
  storage suites pass (121 tests). The next strict run passed both stops
  and reached the hub.

## Robot platformer: packed half arithmetic in a hub compute program (2026-10-27)

- **Observed** (Intel A770, strict route runs past the first crash-site
  hub): a compute program (stage 4) stopped the run with
  `unknown vop3p opcode: 0xf` at pc `0x324` (about 300 s); with that opcode
  decoded, the next run stopped at `0x10`, pc `0x3c8` (about 240 s), and
  then at VOP2 opcode `0x3c`, pc `0x400` (about 450 s). With that decoded,
  the program stopped at its emission: `v_cndmask_b32` in SDWA form at pc
  `0x56c` (`DST_SEL = WORD_1`, `DST_U = 2`, both sources `WORD_0`) failed
  the lowering preconditions.
- **Contract.** VOP3P opcodes 14, 15 and 16 are `V_PK_FMA_F16`,
  `V_PK_ADD_F16` and `V_PK_MUL_F16`, and VOP2 opcode 60 is
  `V_PK_FMAC_F16`, the packed FMA accumulating into VDST (RDNA2 ISA, VOP2
  and VOP3P opcodes, packed math): two independent half operations in one
  VGPR. `OP_SEL` and `NEG`
  select the source halves and their negation for the low result,
  `OP_SEL_HI` and `NEG_HI` for the high one; packed math has no ABS or OMOD.
- **Fix.** The VOP3P decoder admits the three opcodes and keeps `NEG_HI` in
  the field the mixed FMA reads as ABS; the VOP2 decoder gives
  `V_PK_FMAC_F16` the packed FMA with VDST as its addend and both halves
  read in place. The SDWA partial destination (whole-dword result, then
  the merge into the kept bits) now admits `V_CNDMASK_B32`, VOP2 with VCC as
  its implicit third source, when it has no float source modifiers; before,
  only two-source VOP2 and VOP1 forms were admitted. The emitter loads each half through
  the engine's half contract, operates in binary32 and narrows once to half:
  binary32 holds at least 2 x 11 + 2 significand bits, so a sum or product
  rounded twice equals one half rounding, and the fused form recovers its
  addition residue as `V_FMA_F16` does. Inactive lanes keep the VGPR.
  Constant sources are refused, because the ISA does not state how packed
  math reads an inline constant; clamp is refused as well.
- **Verified.** `EmulatorShaderArithmetic.PackedHalfArithmeticSelectsAndNegatesEachHalf`
  (pixel and compute) checks add, multiply, FMA and FMAC values, `OP_SEL`/
  `OP_SEL_HI`, `NEG`/`NEG_HI` and inactive lanes;
  `Fp16SdwaHalfDestinationReadsAndPreservesHalves` runs the guest's
  `v_cndmask_b32` encoding for both VCC values. `kyty_unit_test` 2656 passed
  and the three CTest gates pass. The next strict run decoded the program
  and stopped later, at the SDWA destination of the record above.

## Robot platformer: hub OR reductions run natively (2026-10-27)

- **Observed** (Intel A770, strict route runs to the first crash-site hub):
  three hub pixel programs ran through the fragment wave transport (banded
  execution), the main cost at about 0.5-0.7 fps there. Each holds the
  whole-wave OR reduction that `FragmentPartialWave` already admits, but in
  forms the proof did not accept.
- **Causes**, found by replaying the dumped programs through the analysis
  offline:
  - The entry is `s_orn2_saveexec_b64 vcc, exec` with the condition taken
    as `exec & launch mask` just before it. The region proof accepted only
    an SGPR copy of EXEC as the entry source.
  - A `s_mov_b32 m0, s32` (restoring M0 after an ADD_TID spill) sits between
    the entry and the initializer. The region admitted only scalar loads
    there.
  - The network registers are first rewritten by `v_mul_f32` in SDWA form
    with `DST_SEL = DWORD`, and by `v_sub_f32` with a DPP `quad_perm` source
    and `BOUND_CTRL = 1` (a screen-space derivative). The liveness proof
    treated every SDWA and every DPP instruction as keeping old lanes.
- **Fix.** The region entry may save EXEC itself; its condition is then
  proven from the last EXEC write, branch or label before the entry. A
  scalar copy into M0 counts like a scalar load inside the region: it reads
  no lane and the emitter runs it as written. A write ends a network
  register when SDWA selects the whole destination dword, or when a DPP16
  source cannot skip a lane: full row and bank masks, and `BOUND_CTRL` set
  or a quad permutation, whose source lane is always in range (RDNA2 ISA,
  DPP16 fields: an inactive source lane reads zero, an out-of-range one
  skips the write only without `BOUND_CTRL`).
- **Second pair (2026-10-28).** Two more hub pixel programs stayed on the
  transport. Their tiles, recorded into one submission, ran past the 5 s
  GPU job timeout of the `xe` driver, which reset the render engine and
  stopped the run in the hub ("hub GPU hang"). Their reductions differ in
  two forms: the saved mask is written by the `s_orn2_saveexec` itself (an
  `S_*_SAVEEXEC` stores the EXEC it replaces into its destination, as a
  copy of EXEC does), and an `s_waitcnt` for the scalar loads sits between
  the entry and the initializer. The proof now accepts the saving
  instruction as the allocation of the saved mask and holds `S_WAITCNT`
  among the region's scalar steps, since it waits only for those loads.
- **Excluded for the hang.** A compute-queue fault: the reset named the
  render engine. Cross-queue device waits: a run with every such wait on the
  host hung the same way. The hang ended only when the programs left the
  transport.
- **Verified.** All five dumped programs now prove `FragmentPartialWave`
  with one reduction each. The `EmulatorNativeWaveAdmission` tests cover
  the guest entry form with the M0 copy, the self-saving entry, and whole
  versus partial SDWA and DPP writes; the full `kyty_unit_test` (2661
  passed, 11 skipped by the environment) and the three CTest gates pass.
  A strict 480 s run on Intel A770 (60 fps limit, scripted intro skip)
  reached controllable gameplay in the crash-site hub with no fatal stop;
  hub fps went from about 0.2 to 11-25.
- **Open.** The hub ground draws as blocky orange and red rectangles, and
  the frame rate decays to 1.3-1.6 fps after about 4 minutes in the hub.

## Robot platformer: dashed warp streaks on the studio logo (2026-10-26, fixed 2026-10-27)

- **Observed** (Intel A770, strict, branch `agent/astro-split-wave64`,
  presents 1020-1140): radial dashed streaks covered the screen behind the
  studio logo. Reference footage of the same moment shows the logo on a calm
  star field with no streaks. A full-resolution capture
  (`KYTY_NATIVE_CAPTURE_MAX_EDGE=0`) showed the same streaks, so the capture
  downscale was not involved.
- **Scene structure** (RenderDoc capture of present 1110): the streaks were
  geometry. One indexed draw of the title skybox mesh (72000 indices, star
  atlas, 1080p HDR target) already held them before any post pass.
- **Cause.** The mesh file is read whole by AMPR into guest memory and stores
  72000 32-bit indices (6484..54483, two triangles per quad). The guest draws
  them with `INDEX_BUFFER_SIZE = 72000` and `DRAW_INDEX_OFFSET_2`, after
  `sceAgcDcbSetIndexSize_0600(buf, 1, 0, 0)` (NID `-KRzWekV120`; the NID is the
  hash of that name). Kyty handled the NID as a fixed "default" packet that
  returned null for any non-zero argument, so the 32-bit request emitted
  nothing and the previous 16-bit `VGT_INDEX_TYPE = 0x400` stayed active. Read
  as 16-bit, every second index is 0, and half the triangles fan to vertex 0.
  The live run issued 519 size-1 calls and 57885 size-0 calls in 100 s.
- **Fix** (25467eb0): the NID emits `SET_UCONFIG_REG_INDEX VGT_INDEX_TYPE`
  with `0x400 | size | cache_policy << 6 | per_instance_object_id << 14`. The
  16-bit value matches the packet the guest stream already carried. The field
  layout agrees with two independent secondary implementations. Out-of-range
  arguments now stop the run.
- **Verified:** a strict 150 s run of the clean binary shows the logo at
  present 1195 on a clean star field, matching the reference footage. The
  72000-index draw now resolves to 32-bit indices. `EmulatorGraphicsPackets.*`
  (337) and the full `kyty_unit_test` (2652 passed, 3 skipped) pass, and so do
  the three CTest gates.
- **How it was found** (for similar bugs): a temporary trace dumped the PM4
  packets before the draw and the index memory at draw time. A CPU hardware
  watchpoint under gdb never fired, which pointed at a kernel or GPU writer,
  and the AMPR read hook found the file. `perf` hardware breakpoints need
  `perf_event_paranoid <= 1`.
- **Excluded before the cause was found** (each left the streaks unchanged):
  NGG primitive edge flags as index bits (8518fc7e), point size, the instanced
  additive draw, the 64x16 pattern texture pitch, and missing atlas mips (the
  guest T# declares one level).
- **Open:** `sceAgcDcbSetIndexSize` (NID `GIIW2J37e70`) still emits
  `IT_INDEX_TYPE`, which yields the same Kyty state. An index size of 2 (8-bit)
  reaches `GraphicsRenderDrawIndex`, which only maps 16- and 32-bit and warns
  otherwise.

## Robot platformer: Gen5 AGC HLE run reaches the tutorial level load (2026-10-26)

- **Setup.** Intel Arc A770, Mesa ANV 26.2.5, strict, `fc_script` with
  `scripts/run_guest.lua` and `scripts/input/reach_first_gameplay.pad`,
  `KYTY_RUN_MEMORY_HIGH=14G`, 1280x720. The first run had a cold SPIR-V cache;
  the 300 s run below reused the `~/.cache/kyty` cache the first one filled.
- **Startup fix.** The first strict run aborted at once in `GdsBuffer::Init`
  with `GDS queue-family migration is unsupported: owner=1 requested=0`. The
  Gen5 AGC/AgcDriver HLE set now issues the title's GDS work on both a compute
  and the graphics queue family, and the owner guard predated Kyty's concurrent
  resource sharing. The guard is gone; see
  [GDS backing shared across queue families](../bringup/cross-title.md).
- **Result (300 s window, exit 124 on the cap).** No fatal stop. The title
  runs `ps_logo` at 0:08 and `title` at 0:38 (guest clock), chooses the
  worldmap, runs `intro_next` (12.6 s load) and starts the crash-site tutorial
  load (`SubLevelLocator sublevel_locator_hub [hub_crashsite_tutorial]`, 4.5 s
  load) before the cap.
- **600 s run (exit 65).** With `KYTY_RUN_MEMORY_HIGH=16G`,
  `KYTY_RUN_MEMORY_MAX=18G` and `KYTY_DUMP_VIDEOOUT=1`, the title passes
  `hub_crashsite_tutorial` (4.6 s load), restores the sublevel locator and
  renders the crash-site scene: the present dumps at frames 1800 and 2600
  show the star field, ringed planet, asteroids and the blue crystals with
  the red/white aim markers. It then stops on the next draw at
  `GraphicsRenderPipeline.cpp:1680`: `fragment wave transport is required by
  the pixel stage ... native_admission{guest=64 proof=3 host=unavailable
  mapping=0 mask_ops=1 pc=0x568 reason=fragment row DPP source participation
  is unproven (neutral values do not prove helper recovery)} draw{extent=1920x1080
  samples=1 color_targets=3 write_mask=0x00000037 depth=1 depth_write=1
  blend0=1 input_num=5 system_inputs=0x1b02 subgroup=64}`. The program fails
  the native proof itself (row DPP), so the stop is not Intel-only; the
  fragment wave transport is the open item recorded in this file and in the
  graphics handoff.
- **Why the partial-wave fallback also refuses (shader dump).** With
  `KYTY_TRANSPORT_DUMP` the stop wrote the guest program to local scratch
  (1,524 instructions). At `0x55c`-`0x5a8` it runs a canonical whole-wave OR
  reduction: `S_ORN2_SAVEEXEC_B64`, `V_CNDMASK` seed, four row-shift `V_OR`
  steps, `V_PERMLANEX16`, `V_OR`, restore EXEC, `V_READLANE s8, v37, 31`,
  `V_READLANE s9, v37, 63` and `S_OR s72, s8, s9`; `s72` feeds predicates at
  `0xc20` and `0xec8`. Lane 63 belongs to the second 32-lane half, so a
  32-wide host wave cannot form the guest reduction; the partial-wave proof
  correctly refuses this program even though it accepts reductions that do
  not read across the two halves. This is the exact input the
  capture/shade/resolve contract must provide (raw lane banks, allocated
  lanes and coverage), not a missing import.
- **Observation.** After the worldmap load the title prints its own
  `GFX_: JsParticle heap memory is full!` 65 times with the heap at about
  36,800 KB of 36,864 KB. The same guest message was already recorded from the
  intro level in the 2026-10-13 TUNNEL_ENABLE entry (32 lines) and remains
  uninvestigated; it is a guest message, not an emulator refusal.
- **Limits.** The 300 s window was log-only (`PrintfDirection = Silent`). The
  600 s window's present dumps are raw local evidence, not sanitized
  captures, a frame score or input acceptance: rendering the crash-site scene
  is a runtime frontier, not a playability claim, and the run ends on the
  transport refusal rather than gameplay acceptance.

## Robot platformer: fragment wave transport executes (2026-10-24)

- **Runtime.** A pixel program refused by native fragment admission now runs as
  host passes: a capture draw over the covered lanes, scan and pack compute
  kernels that assemble guest 64-lane waves, the split-Wave64 shade compute
  module over the guest pixel resources, and a resolve draw that applies the
  draw's original fixed-function state. Capture, shade and resolve pipelines
  live with the draw state in the pipeline cache; the five transport regions
  share one device-local allocation, so every draw of a recording reuses it
  inside the 64 MiB scratch pool.
- **Verified.** With the transport selected, the flyby's transported draws
  recorded every tile (20-row bands planned to 39 MB, 35-word lane records
  after parameter packing) and the run presented frames past the previous
  gate, up to frames 2530-2660. Captures of those frames score healthy.
- **Module contract fixed.** The generated module listed `gl_HelperInvocation`
  in its entry-point interface without declaring it, and the fragment mask
  accessor loaded the packed EXEC alias that only the write adapter renames;
  both left undefined ids and the driver rejected the pipeline. Lane records
  now hold only the interpolation forms the program reads (4 words per
  interpolated parameter, 12 per `v_interp_mov` source, none for attributes the
  program never interpolates), and the shade prolog reads pack's active-wave
  count from the transport control buffer so dispatched groups past it exit
  immediately. Capture and resolve also need the transport descriptor set
  bound at index two; without it the host faults in the driver at the first
  draw.
- **Split compute pipeline admission fixed.** A transported pixel program failed
  split compute pipeline creation with `VK_ERROR_UNKNOWN` (-13). The generated
  module was invalid SPIR-V that the assembler accepts without validation:
  - a program with two `s_endpgm` sites reused one set of output-store ids;
  - the discard tail (`s_mov exec, 0`; null export; `s_endpgm`) emitted no
    terminator, leaving the last block unterminated;
  - an integer SDWA swizzle was written on the same line as its `OpLoad`, so the
    assembler dropped it as trailing text.

  Each epilog now carries its instruction index, an empty `s_endpgm` emits
  `OpReturn`, the epilog base reference uses the indexed id, and integer loads
  end with a newline. The dumped modules assemble and `spirv-val` clean after
  these changes. The row-DPP FI=0 guard also shuffled `%exec_lo`, a mask word in
  compute and split waves; it now shuffles the per-lane `%exec_lane_lo`.
- **On-device.** With those fixes the flyby's programs created their split
  compute pipelines and rendered 13,830 tiles: the 39,499-word module and the
  131,944-word row-DPP module (lane_words 33) both ran capture, scan, pack,
  shade and resolve per band.
- **Default-location capture.** A reused program drawn as a depth-only pass
  (`input_num=0`, `system_inputs=0`, `write_mask=0`) enables no interpolators,
  yet its code names attributes 1-3. The native pixel pipeline resolves the
  input count from those `v_interp` attributes, and resolves a default setting
  to attribute `i` at location `i`; the transport now does the same:
  - the capture receives the resolved input count, not the draw's register
    count;
  - input locations come from the decoded interpolator when custom
    interpolation is off;
  - a pair without an enabled barycentric field (and without custom
    interpolation) uses the perspective-center default instead of being
    refused;
  - a rejected capture names its exact admission clause.

  The transport also passes the guest pixel program's LDS size into the shade
  compute instead of zero dwords (`ExtraLdsDwords`).
- **On-device, depth-only draw admitted.** The depth-only program now creates a
  131,548-word shade module (17 lane words) and the run rendered past the
  previous 13,830-tile gate to 15,482 tiles with four transported programs.
- **Host SIGSEGV at label publication (fixed).** Those runs then exited with
  SIGSEGV. A gdb reproduction stopped in `LabelManager::FireCallbacks` at the
  EOP store `*dst_gpu_addr64 = value64`: the destination range was validated
  when the label was created, but the GPU allocation could be released before
  the fence fired. A destination that validated as a GPU allocation at creation
  now re-validates at publication; a released one is skipped with a bounded
  warning, while a plain host destination stores as before. Validating every
  destination instead stalled the guest at frame 27, so the label records at
  creation whether its destination is GPU-tracked. The check also runs outside
  the label manager's mutex at both creation and publication: taking the GPU
  memory lock under that mutex inverts the release path's lock order and
  stalls the guest (frame 6 with the first version). A run with this fix is the
  current check.
- **Offline suite.** `EmulatorFragmentTransport` now runs in the unit build
  (it was compiled but never force-linked from the runner): capture, shade and
  resolve modules assemble and `spirv-val`-validate, and the lane packing
  follows the draw's interpolator source (`v_interp_mov` costs four words as a
  parameter and twelve per-vertex). The suite also exposed a real gap for the
  depth-only draw: the shade generator registered a zero-length VGPR range when
  a program had no initial VGPRs and aborted; it now registers none. The
  runtime result of the depth-only program is the current frontier.
- **Transition crash: a non-indexed transport draw, not a GPU hang (fixed).**
  The earlier record attributed the exit at the flyby/hub transition to a
  render-engine hang. The kernel journal shows no engine reset at those
  crashes: the process died from a host fault inside the Intel driver (read at
  `0xf8`), captured as a userspace core. `GraphicsRenderDrawIndexAuto` had no
  fragment-transport branch, so the first non-indexed draw whose pixel program
  requires the wave64 transport bound the transport resolve pipeline and issued
  a plain `vkCmdDraw`; the driver dereferenced the resulting null pipeline
  state. The indexed path already recorded the transport; the recording is now
  shared and each path replays its own geometry. The RCS resets recorded
  earlier and below belong to other runs, not to this crash.
- **Parser gap seen from the pause menu.** Pressing options during the flyby
  reaches a shader with VOP3P opcode `0x21`, which the parser rejects
  (`ShaderParseVOP3P.cpp`). The plain route does not reach it; the opcode is
  still unimplemented.

## Robot platformer: gameplay level stops at the fragment transport gate (2026-10-24)

- **Setup.** Intel Arc A770, Mesa ANV 26.2.5, strict, SDL offscreen video,
  scripted route (cross every 3 s from the first pad poll), 600 s cap, HEAD
  e214bc69; one cold boot and one boot on the caches it left. Native PNG
  captures and agent samples only.
- **Warm boot removes the cold compiles.** The first present appeared inside
  the agent's ready window, presents 362-1034 ran at 23-30 fps, and the
  run wrote no Mesa shader-cache entries and two SPIR-V entries. The cold
  boot of the same tree held twice for 90 s and more than 120 s while it
  compiled pipelines and wrote more than 50 Mesa entries. Loading the same
  scenes therefore no longer pays a translation or pipeline compile; the
  remaining load time is the game's own resource work.
- **Guest-reported loads** (`LoadLevelResources`, warm boot): title level at
  0:45, worldmap 4.98 s, the intro flyby 14.87 s, and the first gameplay hub
  8.61 s. The guest's timer includes the GPU waits its loader performs, so
  these are not pure file reads.
- **First strict fail: the first gameplay draw.** The hub level starts and
  the renderer stops in `GraphicsRenderPipeline.cpp:1680`:
  `fragment wave transport is required by the pixel stage but the renderer
  does not select it yet`. Facts:
  `native_admission{guest=64 proof=3 host=unavailable mapping=0 mask_ops=1
  pc=0x568 reason=fragment row DPP source participation is unproven (neutral
  values do not prove helper recovery)}` and `draw{extent=1920x1080
  samples=1 color_targets=3 write_mask=0x00000037 depth=1 depth_write=1
  blend0=1 input_num=5 system_inputs=0x1b02 subgroup=64}`. The run exited
  with code 65. This is the first blocker after startup: the level loads,
  but its first pixel program needs a guest wave64 transport that the
  renderer computes but does not execute (`docs/fragment-wave-execution.md`).
- **Fragment-compute module validated (2026-10-24).** The refused pixel
  program now translates to a split Wave64 compute module of 94,932 SPIR-V
  words that assembles and validates. The module reads the captured per-lane
  record and writes the 34-word result lane. Three general emitter gaps
  surfaced: a VOP3 integer operation carries CLAMP, which integer results
  ignore; PERMLANE on the split layout needs the two halves' exchange (a split
  read); and a row DPP with BOUND_CTRL=1 is admitted when each host subgroup
  holds whole guest rows. The renderer still refuses the draw; the
  capture/scan/pack/shade/resolve runtime is the next step.
- **Visual state before the stop.** Captures show the publisher card, the
  studio logo, the tunnel with the lit ship, and the asteroid-field intro
  scene without visible corruption. The later gameplay scene is not rendered
  because the pipeline is refused.
- **Frame cost in the same warm run** (agent interval of 30 s during the
  load-to-interactive transition): 290 presents, 64,913 draws, 26,411
  dispatches, 16,729 submissions and 7,830 fence waits (13.6 s of the 30 s),
  with 14.9 GB hashed, 2.43 GB uploaded and frame-time p50 98 ms. The
  command processor and its fence waits, not GPU execution, set the rate.
- **Guest particle warning.** During the intro the guest logs
  `JsParticle heap memory is full!` with a 36 MiB heap while it renders at
  10-25 fps. It is a guest allocation failure and is not yet shown to be an
  emulator defect; a native-rate run must show whether it persists.

## Robot platformer: title ship and robots absent (2026-10-27, expected without a save)

- **Setup.** `PPSA21567 - Astro Bot`, Intel Arc A770, Mesa ANV, strict,
  `KYTY_PRESENT_FPS_LIMIT=60`, idle title route, branch tip `8e4c0799`
  plus temporary diagnostics (not committed). RenderDoc capture of
  present 1500; native captures every 60 s.
- **Observed.** In the reference video the controller-shaped ship with
  the robots flies in with the title letters and stays under the logo.
  Kyty draws the background, the letters (including their flight with
  warp streaks) and the logo, never the ship or the robots.
- **The title-ship level loads.** At `KYTY_PRINTF_LEVEL=Info` the guest
  reports `LoadLevelResources: title_controller_ship` (0.9 s),
  `Level has started: title_controller_ship` and `StartLevel title`,
  then loads `bg_layer` and `worldmap`. No guest error line follows.
- **The ship is not submitted.** In the present-1500 capture the
  G-buffer pass (five colour targets plus D32S8) holds only the logo
  letter meshes (17 draws, all projecting to the upper centre). Apart
  from the sky and background geometry, no draw of the frame projects onto
  the lower-left area where the ship sits in the video, except seven draws
  into a separate 1536x1536 colour target. The guest omits the ship's
  draws; the renderer does not lose or skip them (no skipped-draw report
  entry).
- **Zero-output effect draws.** 62 draws of the frame have an all-zero
  post-vertex output, among them the merged ES/GS draws that run as the
  geometry compute pass in the HDR forward pass and in the 1920x1080
  `R16G16` distortion pass (60 or 30 guest vertices expanded to 768 or
  384, and 576 to 24,576 vertex records elsewhere). Their GS user data names
  structured buffers (strides 128, 276, 704) that are zero in guest memory
  at record time. The same geometry pass renders the studio-logo particle
  dissolve, so the pass itself is not excluded but not implicated either:
  emitters with no live particles produce exactly this output, consistent
  with the ship's trail emitters having nothing to emit.
- **Excluded.**
  - A regression: captures from 2026-10-18 show no title ship either;
    the "lit ship" of those runs is the warp-tunnel ship.
  - GPU predication: `SET_PREDICATION` never removes a packet in these
    runs, and no draw is skipped.
  - Occlusion-query magnitude: the title issues about five synthetic
    occlusion `EVENT_WRITE` 0x39 queries per frame; stepping the counter
    by 2^20 per query instead of 1 leaves the title unchanged.
  - Asset loading: the level reports loaded and started.
- **Cause: no gameplay save.** The maintainer confirmed that the title
  screen shows the ship and its robots only after the first gameplay save
  exists; the reference video was recorded with one, and these runs start
  from empty save data. The missing draws are guest behavior, not a
  renderer defect. Re-check the title after a run has reached gameplay and
  written its first save.
- **Not a lead any more.** `IT_GET_LOD_STATS` is still consumed without
  writing its report (`GraphicsRunOpParsers.cpp`, `cp_op_get_lod_stats`),
  and the binder ignores `MipStatsCntEn`; neither explains the title, and
  a real report needs a producer for the counters, not invented counts.

## Robot platformer: crash-site hub stops and intro cost (2026-10-27)

- **Setup.** `PPSA21567 - Astro Bot`, Intel Arc A770, Mesa ANV, strict,
  `KYTY_PRESENT_FPS_LIMIT=60`, `scripts/input/reach_first_gameplay.pad`,
  branch `agent/astro-split-wave64` over `66bee914`, runs of 160-600 s.
- **Stop 1, fragment transport budget.** Loading `hub_crashsite_tutorial`
  ended in `fragment transport could not reserve host storage` for a
  1920x1080 draw of 4343 primitives with 45 lane words: one band of the
  whole draw needs more than the 40 MiB the transport took from the
  64 MiB per-recording scratch pool. Fixed by planning a draw as ordered
  primitive chunks replayed per band (`PlanDraw`; strip chunks start on an
  even primitive so the winding holds), and by moving the transport
  storage to one 128 MiB block per command pool (`TransportStorage`),
  which the tile barriers order across recordings of the same queue. With
  40 MiB, planned passes for that draw were 720 (543-primitive chunks,
  12-row bands); with 128 MiB they are 50. The pool block also replaces
  up to eight 40 MiB scratch entries per queue.
- **Stop 2, linear storage over tiled targets.** The next run exited at
  `!create_all_the_same` for a 960x540 linear six-level storage texture
  created over tiled render targets and storage textures at the same
  address. A view whose tile mode differs from the existing object's is a
  recycled allocation, not a view of the same texels: a tile-mode mismatch
  now counts as a redescription (`StorageTextureRedescribesRange`,
  `StorageTextureRedescribesRenderTarget`). The run passed the object
  without an exit.
- **Intro cost, vertex program cache.** The slow windows before the hub
  (1.3-1.7 fps) were CP bound: draw state setup took 1.2-1.6 ms per draw,
  almost all in `ShaderGetInputInfoVS`, and inside it 1.9 ms per call in
  the NGG front proof, which is memoized per cached program. The program
  cache emptied itself 78 times in 160 s on its 64 MiB byte budget (about
  45 fused fronts, up to 5.5 MiB of IR each), so every draw re-parsed its
  program and re-proved its front. With a 256 MiB budget the cache never
  empties on the route: the pre-hub window went from 1.7 to 6.5 fps, the
  ship cutscene from 1.3 to 5.6-7.2 fps, and frame 3169 is reached at
  270 s instead of 360 s. Peak host RSS was 10.8 GB at frame 3321, against
  10.2 GB at frame 3190 before the change (not the same point).
- **Verified.** No transport or alias exit through the hub load in three
  runs; `kyty_unit_test` 2653 tests pass; the boundary, archive and
  checked-in shader gates pass.
- **Open.**
  - The hub runs at 0.7 fps and is GPU bound: 18.6 s of each 30 s window
    are fence waits (791 waits, up to 395 ms each). Which passes cost GPU
    time needs per-event timings.
  - In the ship cutscene the ship's silhouette ends in 16x16-pixel blocks
    (at the 2432x1368 internal resolution) where the background should
    show. Not localized yet.
  - One earlier run (before the pool block) lost the device on an xe
    `Timedout job` while hundreds of transport passes were recorded per
    draw; it did not recur in three later runs, so the cause is open.
  - `EmulatorGraphicsState.StorageTextureArrayDescriptorsAreViewsOfTheLiveArray`
    (`source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`,
    line 6296) fails when run alone and passes in its suite:
    `StorageTextureObject::Equal` reads the process-wide guest generation
    (`GuestIsGen5`), which an earlier test sets. The test should set the
    generation itself.

## Robot platformer: logo particles through a split geometry pass (2026-10-22)

- **Setup.** Intel Arc A770, Mesa ANV 26.2.5, strict, `generic.pad`,
  `KYTY_RUN_MEMORY_HIGH=14G`, translator version 147; one boot with a
  cold SPIR-V cache and one warm boot.
- **Cost.** The studio-logo stretch (presents about 950-1100) dissolves
  the logo into particles through merged ES+GS draws that run as the
  geometry compute pass. In the paired layout the hot particle module
  compiled offline in 47 s (6,726 spills, 20,407 fills, 128 KiB scratch,
  2,144 live registers) and the stretch ran at about 2.8 fps.
- **Change.** The geometry pass runs in the split layout: one guest lane
  per invocation, each guest wave64 spanning two 32-wide host subgroups.
  The prologue writes `v0`-`v5` and `v8` per lane, exports and
  `GS_ALLOC_REQ` are per lane, and guest lane 0 of each wave leads the
  allocation. The first split run stopped at a `v_add_i32` whose source
  is a DPP `row_shr:1` with BOUND_CTRL=1; row DPP is now translated in
  native and split compute waves (cross-title record of the same date).
- **Result.** The two split modules of the stretch compile offline in
  15.0 s and 7.4 s (3,085 and 2,939 spills, 32 KiB scratch, 896 and 809
  live). The warm boot runs the stretch at 6.3-9.4 fps (agent samples)
  and ends clean at the 150 s cap; captures at presents 955, 1048, 1068
  and 1105 show the logo and its particle dissolve.
- **Recheck at version 148.** After the GS user-data base fix a cold
  boot holds at present 887 for the rest of a 180 s run while shaders
  compile; the next warm boot runs the stretch at 7.5-8.9 fps, shows the
  logo at presents 1055 and 1086, and ends clean at 150 s.
- **Holds attributed (2026-10-22).** Two loading holds in the warm boot,
  presents 850-905 (0.24 fps sample) and 1106-1112 (0.1-0.6 fps), came
  with both precompile workers compiling stale paired recipes from older
  translator versions (profiled at frame 872). With versioned recipe
  directories the warm boot's frame-time p99 is 0.33 s and its longest
  hold (5.8 s) is the title's own level load (cross-title record
  "Shader caches", 2026-10-22).
- **Open.** About 3,000 spills remain per module.

## Robot platformer: paired compute compile cost (2026-10-21, closed 2026-10-22)

- **Outcome.** The direction below landed as split Wave64 compute
  dispatch, and the paired layout was removed (cross-title record
  "Shader translator: paired wave64 layout removed", 2026-10-22). The
  split modules still carry about 3,000 spills each.

- **Setup.** Intel Arc A770, Mesa ANV 26.2.5, strict, HEAD 89e3b892,
  scripted route from logo to worldmap; a first boot with empty SPIR-V
  and Mesa caches, then a second boot with the caches it left.
- **Black picture near frame 850: the run memory guard again.** The run
  cgroup peaked at 11.1 GB (`journalctl --user` "Consumed" line) against
  the 7 GB `MemoryHigh` of those runs. Under reclaim throttling one paired
  lighting kernel took 98 s to compile in-game against 40 s offline. With
  `KYTY_RUN_MEMORY_HIGH=14G` and `KYTY_RUN_MEMORY_MAX=16G` the throttling
  disappears. Each heavy compile peaks at about 1.34 GB of resident memory,
  and the two background workers can run two at once.
- **Cold boot.** Frame 1 holds for about 4 minutes while 18 compute
  pipelines are created; the same lighting kernel took 103 s in-game
  without throttling (40 s offline, difference not explained). The title
  then runs at about 17 FPS to frame 842 and stalls again while about nine
  kernels of 7-14 s each compile.
- **Second boot with recipes.** Frame 559 at 45 s with 44 background
  compiles, a 10 s hitch at frame 846, frame 1404, then the wait on the
  stall kernel recorded on 2026-10-19 (436,742 SPIR-V words, a
  workgroup of four guest waves). Its recipe from this HEAD compiles
  offline in 270 s (338,833 instructions, 18,796 spills, 41,805 fills);
  the recipe recorded on 2026-10-19 took 496 s.
- **Cost model.** gdb samples of the offline compile sit in the Intel
  compiler's register coalescing, pre-allocation scheduler liveness,
  interference-graph rebuilds and spill selection: the time is the
  register allocator's spill iterations. The same SPIR-V compiled with a
  forced smaller subgroup (a cost bound, not a correct program) gives each
  value half the register footprint:

  | Kernel | Subgroup 32 | Subgroup 16 | Subgroup 8 |
  | --- | --- | --- | --- |
  | Stall kernel | 270 s, 18,796 spills | 6.8 s, 2,867 spills | 5.1 s, 1,842 spills |
  | Lighting kernel, 335,039 words | 28.5 s, 6,184 spills, 1,271 live | 5.2 s, 2,448 spills, 647 live | 4.0 s, 1,224 spills, 346 live |

- **Excluded compile reductions** (offline, temporary translator variants
  of the 28.5 s kernel):

  | Variant | Compile | Spills | Note |
  | --- | --- | --- | --- |
  | EXEC forced full | 25.5 s | 6,163 | 1,243 live registers |
  | Scalar register loads as subgroup broadcasts | 51.4 s | 6,372 | slower |
  | Scalar registers in workgroup memory | 34.7 s | 6,732 | slower |
  | Scalar registers and half of the VGPR halves in workgroup memory | 31.1 s | 6,072 | 34,144 bytes; the 171 halves would not fit in 64 KiB |
  | Driver spilling rate 5 / 20 (localization only) | 26.1 s / 33.0 s | | |

- **Native layout excluded again.** The three largest paired guest kernels
  dumped in these runs (2,378-2,707 instructions, BVH traversal) are
  refused at a `v_mov_b32` with a DPP16 control that is not a quad permute
  ("operand is not representable per lane"). Each also holds 44
  `v_readlane_b32` whose lane comes from `s_ff1_i32_b64` over a 64-bit
  mask, 36 `v_writelane_b32` and 9 `v_readfirstlane_b32`. Which host
  module each of them became was not recorded.
- **Direction.** Run each 32-lane half of a guest wave in its own 32-wide
  host subgroup, one guest lane per invocation with replicated scalar
  state, and exchange values between the halves through LDS where a mask
  becomes scalar data, at `v_readlane`/`v_readfirstlane`, and where one
  half must see the other's LDS writes. The expected compile cost is near
  the subgroup-16 bound. Risk: the exchange spins without a Vulkan
  forward-progress guarantee, so replicated scalar state that diverges
  would hang the GPU.

## Robot platformer: worldmap stall and dark top-left quadrant (2026-10-17)

- **Route (Intel Arc A770, Mesa ANV, strict).** Logo, title, "Continue",
  worldmap load. Through 486b7d0d presents stopped at about frame 823.
- **Stall 1: one cold compute compile.** `sudo gdb` on the command-processor
  thread showed it inside the driver's `vkCreateComputePipelines` for one
  paired-transport module (3 MB of SPIR-V). Offline, the module took 188 s to
  compile, with 11474:36425 SIMD32 spills. A 300 s run never finished it, so
  the driver never cached it. The guest program is a 5,458-instruction
  64x1x1 compute shader. Its only paired reasons were ADD_TID LDS spills and
  an M0 save/restore. (Correction: an earlier entry concluded the stall was
  not a compile.)
  - Fix: the native-equivalence proof
    (`ShaderComputeWaveNativeEquivalence.cpp`) now models M0 as an implicit
    input of the instructions that read it. ADD_TID spill slots are
    lane-private planes, not refusals. Scalar liveness is strong: a pure
    scalar op keeps its sources live only if its result is live, so the dead
    M0 copies no longer count as scalar state live at a join. The program
    runs natively; the run passes the old stop. Unit tests cover the three
    rules.
  - The classifier programs (`ds_append` with `mbcnt` slot order) stay
    paired, because their slot order is guest-visible.
- **Stop 2: scalar buffer load with an immediate offset.** The next stop was
  `s_buffer_load_dwordx4 s[4:7], s[0:3], vcc_hi` with an immediate offset of
  64. `EmitGuestScalarBufferLoad` refused any non-zero immediate. The RDNA2
  ISA defines the SMEM byte offset as SOFFSET + OFFSET, and the emitter now
  adds both. In wave32, VCC_HI is an ordinary SGPR source.
- **Stop 3: a freeze at frames 975-1010 is the harness memory guard, not the
  emulator.** Every worldmap run peaked at 7.2 GB against the 7 GB
  `MemoryHigh` of the run cgroup (`journalctl --user` "Consumed" lines). gdb
  showed the command processor throttled inside a large vector copy at about
  2 MB/s. With `KYTY_RUN_MEMORY_HIGH=16G` and `KYTY_RUN_MEMORY_MAX=18G` the
  title reaches the title menu and the next level load, with a cgroup peak of
  13.2 GB at about 420 s. This title needs a run cap of at least 600 s to
  reach gameplay.
- **Dark top-left quadrant on the logo and title scenes.** A composite draw
  blends a half-resolution layer over the scene as
  `dst = src.rgb + dst * src.a`, so the layer's alpha is transmittance. There
  were two defects:
  - **(1) Inert undersized depth clipped colour.** The draw binds a
    1920x1080 colour target with a 960x540 D32 depth target. It does not
    test, write or clear depth, and it enables stencil with no stencil
    plane. Vulkan sizes the framebuffer to the smallest attachment, so colour
    writes outside 960x540 were dropped. The guest DB does not bound
    rasterization when it leaves its surface untouched.
    `SanitizeRenderDepthAgainstColor` (`GraphicsRenderCore.cpp`) now detaches
    an undersized depth target when the draw leaves it inert, as it already
    did for 1x1 depth. A depth target the draw tests or writes is kept. Unit
    test `UndersizedDepthBoundsColorOnlyWhenUsed`.
  - **(2) The layer's DCC clear was dropped.** With fix (1) alone the whole
    scene went black, which isolated the second defect. The half-resolution
    layer is cleared by a compute fill of its DCC keys with 0x40404040,
    which encodes rgba (0, 0, 0, 1). `PublishComputeColorDccClear` returned
    without publishing because `BeginRead` reported `tracked=0`. The dirty
    page tracker had refused the layer's range: 131,068 of 131,072 pages
    were admitted by 32 s, held by 93 render-target and texture ranges. The
    layer then held its undefined alpha 0 (pixel readback, not thumbnails)
    and the composite darkened the scene.
  - **Fix for (2).** `GpuDirtyPageTracker::kPageTableSize` grows from 2^18 to
    2^20 slots (an admission budget of 524,288 pages, about 59 MB of fixed
    metadata). This title peaks at about 205,000 pages on the title scene.
  - **Verified with both fixes**, Intel, 16G guard, 420 s: the clear is
    published and consumed at the render-target bind; the layer's alpha
    mean is 1.0; the logo and title scenes render whole through frame 1181;
    the frame rate is unchanged (about 5 FPS there).
- **Defect found, not fixed.** A colour DCC clear refused by the tracker is
  still dropped silently: `PublishComputeColorDccClear`
  (`GraphicsRenderAttachments.cpp:480-481`) returns false when
  `observation.tracked` is false, and the dispatch path
  (`GraphicsRenderDraw.cpp:3231`) discards the result. Correctness therefore
  still depends on tracker capacity. Direction: register the range on demand,
  or report the refusal as a strict error instead of dropping it. Two more
  DCC fills were dropped in the same run before the fix; whether they cause
  the dark ship and garbled blocks of the title menu is not yet verified.
- **Trails behind moving geometry on the title flyby and menu (fixed
  2026-10-18).** Every frame the guest zeroes a 1920x1080 RGBA8 target
  (0x870000 bytes) with a uniform compute fill. `PropagateComputeUniformColorFill`
  refused it at `ResolveComputeColorFillIdentity`: besides the exact storage
  buffer and the exact render target, eleven render-target and storage-image
  objects of the transient pool partly share (cross or contain) the range,
  and any non-exact image alias refused the whole fill. The host image kept
  every earlier frame; its bound-sample dump showed stacked streaks. Seven
  other per-frame fills (R8G8, R16 and RGBA16F targets) were refused the same
  way or because their format had no decoded packing.
  - Fix: a uniform color fill now skips partial image aliases (render
    target, texture, display buffer, storage image, depth) and clears every
    exact alias. The dispatch has already written the storage buffer, so
    the skipped aliases did not hold the guest bytes before either. DCC
    metadata clears keep the exact-only rule. An all-zero fill decodes for
    every uncompressed color format. Unit test
    `UniformFillTexelsDecodeOnlyWhenTheQuadRepeatsOneTexel` covers the zero
    rule.
  - Verified (Intel, frames 1248-1252): the fill is applied (`color=1`), the
    target's dump is clean, and the flyby capture shows single silhouettes
    instead of smeared copies. Regression: Intel racing title (frame 55,378
    at 200 s, above every earlier run), fishing title 22-23 fps, sandbox
    title in its world, watercolour platformer; AMD fishing title 11.5-12 fps
    (one of three AMD runs hit the known boot
    stall before the first present).
  - Still refused: a 0x30000 fill whose range is also contained in a
    read-only storage buffer, a 0x10000 fill whose exact alias is a storage
    image (not in `FindComputeColorFillImages`), and a 0xffffffff fill of an
    R16 target (no evidenced packing). The albedo and normal G-buffers also
    hold stacked copies of the logo; no fill or DCC clear targets them in
    the observed frames, so whether the guest relies on depth or stencil to
    ignore them is not verified.
- **Cold compile of a paired light-list builder (open).** After the native
  M0 proof, the flyby froze again on one cold Intel compile of a 16x16
  paired compute module: about 300 s (299,725 ms measured). The program is a
  waterfall loop over `s_ff1`, `v_readlane` and `v_writelane` with quad_perm
  DPP operands, which the native proof refuses ("operand is not
  representable per lane"), so it must stay paired. A warm-up run that
  stopped after the compile finished stored it in the Mesa cache; later runs
  pass the flyby. Excluded knobs, offline with the same SPIR-V:
  `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` 288 s, no required subgroup
  size 297 s, `spirv-opt -O` more than 300 s. Open defect: the first boot on
  a new host or driver pays minutes per heavy paired module. Direction:
  compile known modules in the background before first use. The native
  proof now admits quad_perm DPP (entry below), but this module also uses
  `v_readlane`/`v_writelane` and stays paired.
- **Black ship and logo; skipped geometry-shader draws (open).** In the
  title scene the ship and the 3D logo reach the albedo and normal
  G-buffers intact but leave the lighting pass black, while forward and
  emissive background layers render. Separately, by frame 1340 about 10,000
  draws are skipped as `unsupported_ge_state`: NGG ES+GS programs with
  `max_vert_out` 4, 10 and 72 and triangle-strip output, which
  `ShouldSkipUnsupportedGeShader` (`GraphicsRenderDraw.cpp`) refuses. Merged
  ES+GS launch, shared LDS and primitive emission are not implemented.
  The black lit geometry had another cause, fixed in the next entry; the
  skipped draws remain open.
- **Black ship: exclusions and fix (2026-10-18, fixed).** The ship
  is a black silhouette in the tunnel shot and lit only at its base in the
  title menu. Intel A770, strict, HEAD 50775391, warm caches.
  - Not a regression: a 10-14 run on dff7b38b shows the same partial
    lighting in the menu and the same silhouette in the tunnel. The "lit
    ship" of the 10-14 entry below is that partial state.
  - BVH results excluded: a diagnostic build that forced every
    `image_bvh_intersect_ray` to return a miss, captured every 6 s, still
    shows the silhouette (tunnel, presents 1418 and 1451) and the black upper
    part (menu, presents 1284-1382). An unverified change to the triangle
    vertex order of node types 2 and 3 was withdrawn: it has one secondary
    source, and no capture differed.
  - DCC clear contract holds: a trace of presents 1270-1460 shows nine
    metadata clears published per frame and the 1920x1080 RGBA16F lighting
    target taking its clear at its first shader-image access. Its
    render-target bind does not erase it.
  - Skipped geometry-shader draws, per frame in presents 1280-1300 (about 79
    in total, all `CB_TARGET_MASK` 0xf or 0x7, so each writes only RT0):
    36 to target 0x53aa00000 with depth 0x53a500000 (24 with
    `max_vert_out` 4, 6 + 6 with 10); 29 + 1 + 1 to target 0x514080000 RGB
    with depth 0x513560000 (`max_vert_out` 4, 72 and 0); 12 to target
    0x5367f0000 with depth 0x513560000 (`max_vert_out` 4). These are
    single-target quad and strip expansions, which look like sprites or
    particles rather than ship geometry, and the entry above found the ship
    intact in the albedo and normal G-buffers. They are a real omission,
    not an explanation of the ship. Direction for the omission: run merged
    ES+GS programs as one workgroup with shared LDS and collect their
    exports; the renderer has no mesh-shader path today.
  - Tile classification excluded: a readback of the per-tile lists that the
    two lighting kernels consume (presents 1270-1346; each entry packs the
    16-pixel tile column in its high half and the row in its low half) holds
    about 1,430-1,570 of the 8,160 tiles per frame, and an overlay on the
    tunnel capture shows that they cover the black ship and logo. The kernels
    ran on those tiles and wrote black.
  - Cause and fix: both lighting kernels sample a two-layer RGBA16F array
    (10240x320). A dispatch earlier in each frame writes its layer 0 through
    a one-layer storage image at the same address. The sampled view needs
    two layers, so the alias picker rejects that image and the array is
    uploaded from guest memory, which holds stale bytes because storage
    images are not written back. A second defect hid the write:
    `storage_write_stamp` was set only when a storage image was created,
    not when a later bind found the existing image, so a newer write was
    never visible to the copy rules. Now every writable storage bind stamps
    its image, and a sampled 2D array upload copies its leading layers from
    the newest same-format storage image that starts at the array's address
    and was written after the upload (`CopyNewerStoragePrefixLayers`,
    `GraphicsRenderBind.cpp`). The block-compressed upload copy uses the same
    stamp.
  - Verified (Intel A770, strict, warm caches, no diagnostics): menu
    present 1212 and tunnel presents 1277-1371 show a lit, metallic ship
    and a lit logo where the earlier runs showed a silhouette; fps in the
    flyby is unchanged (about 5.6-6). The existing stall near present 1384
    is unchanged. Racing title, 200 s: same attract-mode screens and fps
    range as the run before the change.
  - Defect found, not fixed: a compute `S_BUFFER_LOAD` whose V# is built at
    run time reads the bound-buffer array with the V# base dword as its
    index. The guest-address path exists only for pixel shaders
    (`PixelScalarBufferUsesGuestAddress`, `ShaderSpirvBuffer.cpp:3207`), and
    the compute device-address predicate (`ShaderUsesGuestDeviceAddress`,
    `Shader.cpp:4401`) ignores these loads. Trigger: all nine kernels that
    contain `image_bvh_intersect_ray` build a V# over an instance's bottom
    level (`s[8:9] = s[28:29] << 3`, `s11 = 0x16204`) inside the instance
    walk and read it with three `S_BUFFER_LOAD`s. In one kernel's ISA the
    walk starts at node 37 (an fp32 box) and enters the instance branch only
    for a leaf popped from a box result, so the forced-miss run never
    reached these loads. Direction: use the scalar flow's entry sources to tell
    an entry-value copy of a bound V# (which holds the slot index) from a
    computed V#, and read only the computed one through the device address.
    A copy such as `s[16:19] <- s[10:13]` must keep the slot path.
  - Cold compile: the heaviest tiled-lighting kernel (466,347 SPIR-V words)
    took 356 s to compile offline on Intel: about 390,000 instructions,
    22,000 spills and 1,400 live registers at peak. Any change to the BVH
    emission makes every first boot pay this compile again. Three runs of
    HEAD plus read-only diagnostics stalled near present 1384 for more than
    76 s; the counters
    point to a compile in progress.
  - Stall confirmed as that compile (2026-10-19, HEAD f311b0ee, warm Mesa
    and SPIR-V caches): during the stall the command-processor thread spent
    100% of an 8 s user-space sample inside `libvulkan_intel`, under
    `PipelineCache::CreatePipeline` called from
    `GraphicsRenderDispatchDirect`; guest threads were blocked, and the
    submit, fence and pipeline counters did not move across two agent
    snapshots 23 s apart. The Mesa cache directory is far below its size
    limit, so the pipeline is not evicted: no bounded run has lasted long
    enough to finish and store it. The module is a wave64 kernel run as
    paired 32-lane halves: each half of every VALU instruction becomes about
    15 SPIR-V instructions (exec-bit test, load, store, select), so 3,282
    guest instructions become about 465,000 SPIR-V words. Direction:
    shrink the paired-wave emission or extend the native-width proof so the
    module stops pairing; a single run longer than about 9 minutes would
    store the pipeline in the Mesa cache but does not fix a cold first boot.
  - Native proof extended to quad DPP (2026-10-19, Intel A770, Mesa
    26.2.5, strict, warm caches). The native-equivalence proof now admits a
    full-mask `quad_perm` with FI=0 and BOUND_CTRL=1 as a per-quad read and
    takes the native layout only for a workgroup of whole quads. Two
    post-processing kernels of the logo and tunnel leave the paired layout.
    Their storage images lost tile coverage at first (the native analyzers
    do not prove them), and the logo stopped in `create_all_the_same`. The
    paired coverage proof models guest lanes of one 64-thread workgroup, so
    it now also covers native-equivalent wave64 layouts. Verified by A/B in
    one binary with the admission switched off, captures every 26 s: the
    logo is identical; the tunnel warp at presents 1268 and 1261 shows the
    same scene with no new artifact; the run with the change reached present
    1388 in 155 s against 1261 without it. Racing title, 200 s: gameplay
    reached, fps 25-78, as in earlier runs.
  - Native layout excluded for the stall kernel and the four heavy lighting
    kernels: all of them use indexed `v_readlane`/`v_writelane` (the lighting
    kernels first `v_readfirstlane` a value loaded from LDS), which a
    32-lane host subgroup cannot hold for a 64-lane guest wave. Offline
    Intel compile times of the four lighting kernels: 29, 36, 38 and 96 s;
    they compile during boot and come from the Mesa cache afterwards. The
    stall kernel (about 436,700 words) did not finish within 285 s offline.
  - Excluded compile reductions, offline with the same SPIR-V: `spirv-opt
    -O` made the 29 s kernel take more than 200 s; replacing the block
    dispatcher with structured control flow shrank a module to 113,000 words
    but it still took 36 s, with about 1,285 live registers at SIMD32. The
    cost follows register pressure, not module size: a 16x16 kernel with
    about 73 guest VGPRs holds two lanes per host invocation, far beyond
    the register file of a SIMD32 thread. Folding the register variables
    into one Function array with an index the compiler cannot fold also
    failed: 30 s became 43 s (966 live registers, more spills and scratch
    traffic), and more than 280 s once helper functions were inlined so
    that every register moved into the array. The shared stack (lanes 0-59
    of one VGPR, indexed by an SGPR) and the `s_ff1`-selected lane reads
    need all 64 lanes, and the 4-wave workgroup rules out workgroup
    barriers for a cross-subgroup exchange. Remaining direction: compile
    known modules in the background before first use.
  - Background compile of known compute pipelines (2026-10-19, Intel A770,
    Mesa 26.2.5, strict). Every compute pipeline creation records a recipe
    (SPIR-V, stage flags, required subgroup size, push range, descriptor
    layout plan, vendor and device) under
    `<spirv cache>/compute-pipelines/<title>-<version>-t<translator>-g<geometry>/`
    (translator versions added 2026-10-22; other versions are removed at
    start), written before the
    blocking compile so a run that stalls in a compile still records it;
    the store is bounded at 2,048 recipes and 256 MiB. At the first compute
    pipeline of the next boot two workers compile the recipes of this
    device, largest first, through the shared `VkPipelineCache`; the
    dispatching thread waits for a recipe a worker is already compiling and
    never skips a dispatch. Agent `perf-snapshot` reports
    `compute_precompile_*`; the counters count finished compiles and
    waits, while `compute_precompile_running` and
    `compute_precompile_waiting_ns` show a compile or a wait in progress
    (verified during the stall: running 1, waiting 28, 43 and 59 s). Measured with a cold Mesa cache and the same
    SPIR-V cache, 200-280 s runs: without recipes the title was still on
    its first frame at 195 s (18 pipelines, 121 s of compile on the CP
    thread); with recipes, 5 pipelines (182 s) compiled in the background,
    one CP wait of 30 s, frame 33 at 155 s and frame 631 at 196 s. Racing
    title, first boot (records one recipe) and second boot (compiles it in
    the background, cold Mesa): fps 41-90 and 25-81, as in earlier runs.
    Stall kernel with its recipe recorded, warm Mesa cache for every other
    module, two 275-360 s runs: the worker starts it at boot, the dispatch
    thread reaches it after 170-240 s and waits in the claim (95% of its
    samples in futex while the worker spends 214 s of CPU inside the Intel
    compiler); neither run reached the end of the compile. The same module
    and layout compile offline in 496 s (355,084 instructions, 1,368 live
    registers, 20,081 spills, 43,882 fills). The background compile
    therefore removes only the part of that time that elapses before the
    first dispatch; the module enters the Mesa cache only after a session
    that lasts until the compile finishes. Open: cut the register pressure
    of the paired layout for this kernel. Exit at the time cap (SIGTERM) with
    a worker inside the driver and the dispatch thread waiting: two runs
    exited normally, one was killed 2 s after SIGTERM (cause not
    determined).
  - Defect found, not fixed: `GpuMemoryCreate.cpp:2371` refuses a storage
    image (0x870000 bytes) whose range crosses two 1920x1080 render targets
    when the image is not proven write-only. Any kernel whose coverage
    proof fails reaches this refusal. Direction: split or alias the storage
    image per overlapped target instead of refusing.
  - Caveat for experiments: diagnostic environment switches that change
    code generation are not part of the SPIR-V cache key; use a separate
    `KYTY_SPIRV_CACHE` directory for each variant.
- **Unit test hang in CI (fixed in PR #20).**
  `EmulatorGraphicsDirtyTracking.SlotAndMappingChangesWaitForAFaultInFlight`
  (`UnitTestEmulatorGraphicsDirtyTracking.cpp:1728`) hung once in the
  DevTools ON job and passed on rerun; it passed 200 of 200 local runs.
  Cause: the test joined the slot-release thread before ending its fenced
  unmap. `BeginUnmap` holds the publisher fence, and the release waits for
  it in `AcquireIdentityWriter`, so the test deadlocked whenever the unmap
  thread was admitted first. The test now ends the unmap before joining.
- **Layered depth targets (fixed 2026-10-22: every slice is its own depth
  image, and a sampled array view is assembled from the slice images).** In
  the same frames a lighting kernel samples a 16-layer D32 array
  (1536x1536, shadow cascades)
  whose cascades the guest renders as single-slice depth views at one base
  (`DB_DEPTH_VIEW` slices 0..0 and 1..1). `DescribeRenderDepthInfo` reads
  `slice_start` and `slice_max`, but the depth backing is one layer, so both
  slices render into host layer 0, and the sampled array view over a
  one-layer image clamps every cascade to layer 0. A layered color target
  with a depth attachment still exits in `GraphicsRenderFramebuffer.cpp`.
  Not the black-ship cause: the kernel that does not sample this array
  also wrote black. Direction: give depth targets the color path's array
  model (`ResolveRenderTextureArrayView`, growing backing).
- **Ghost copies in the title menu (2026-10-19, fixed).** From present
  about 1205 the memory cards leave a trail of copies down the right side,
  the 3D logo letters repeat below themselves, and a black blob covers the
  bottom left. Intel A770, strict, warm caches, HEAD cd81ab00.
  - Cause: the guest clears the 1920x1080 main depth target each frame with
    a compute fill of its HTILE (word 0xfffffff0, depth 1.0). The first pass
    that binds that depth target in the frame renders into a 1024x1024
    color target (two draws). That pass consumed the clear as a
    render-pass load-op clear, which covers only its 1024x1024 render area.
    Depth at x >= 1024 and y >= 1024 kept earlier frames' contents, and the
    deferred lighting shaded them. Evidence: the ghosts end at exactly
    x = 683 of 1280 (1024 native) in presents 1207, 1209, 1215 and 1340;
    skipping only the two 1024x1024 draws moves the clear to a full-size
    pass and removes the ghosts; skipping every main-target draw does not,
    because the lighting still shades the stale depth.
  - Fix: a consumed HTILE meta clear now clears the depth plane of the whole
    image outside the render pass (`ClearDepthMetaWholeImage`,
    `GraphicsRenderAttachments.cpp`). A first use also clears stencil, as
    its CLEAR load op would. Register clears (`DB_RENDER_CONTROL`) stay
    bounded by the pass. Images with custom sample locations keep the
    in-pass clear, since their layout transitions would need the sample
    locations.
  - Verified (Intel A770, strict, warm caches, no diagnostics): presents
    1207-1240 show one copy of each card and no repeated letters, and
    present 1340-1344 has no blob; frames reached in 175 s and the fps
    samples are within the run-to-run range. Racing title, 200 s: gameplay
    with shadows, same fps range as the 10-17 runs.
    `check_emulator_boundaries.py`, `check_graphics_tables.py` and the 296
    `EmulatorGraphicsState` unit tests pass.
  - Defect found, not fixed: the same coverage hole remains for stencil. A
    zero HTILE word on a D32S8 target also marks stencil cleared over the
    whole surface, but after first use the helper clears only the depth
    aspect (`GraphicsRenderAttachments.cpp:1155`) and the pass's stencil
    load-op clear covers only its render area. Trigger: a zero-word HTILE
    fill consumed by a pass smaller than the D32S8 target, with a stencil
    register clear of 0. No title has been seen to hit it. Direction: pass
    the consumed event to the helper and add the stencil aspect for that
    pattern.
  - Not established: after present ~1227 the menu fades to one drifting
    card and the intro flyby follows, while reference imagery shows a
    settled menu with the logo and three cards. The runs use a scripted route
    that presses cross every 3 s from the first guest pad poll; whether the
    guest consumes it at this screen was not verified (the agent's pad
    counters count only agent input), so this may be guest flow.
  - Excluded first: the half-resolution layer at 0x53aa00000 (its DCC clear
    is published and consumed every frame); the target at 0x532830000 (a 1x1
    texture plus one fill); transient snapshots of the debris vertex data
    (an A/B with large transient uploads disabled shows the same ghosts);
    the vertex transforms of the debris program (the matrix dump holds
    legitimate tunnel debris); indirect draws (none in the menu). The
    earlier observation "the depth clear is consumed once per frame" was
    true but checked consumption, not the area it covered.
  - Open lead, unverified: `cp_op_draw_index_indirect` and
    `cp_op_draw_indirect` (`GraphicsRunOpParsers.cpp:639`, `:654`) pass
    only body dwords 0 and 3; dwords 1-2 are not consumed. No title has
    been seen to depend on them.
- **Pre-existing, unrelated failure.** Unit test
  `EmulatorGraphicsPackets.CompressedMrtExportReadsPackedHalfFromUintShadow`
  also fails on clean 486b7d0d.

## Robot platformer: intro frame cost on Intel (2026-10-18)

- **State (Intel Arc A770, Mesa ANV, strict, intro flyby, frames
  1150-1400).** About 5.3-6.6 fps between measurement windows of one build,
  with the render engine about 50% busy for the emulator process (DRM
  fdinfo engine cycles; whole-GPU residency read 84% and is not a
  per-process figure). Per frame: about 28 fence waits (57-66 ms),
  38 submissions and 120-140 ms of command-processor time. Fps moves
  within that noise, so changes are judged by GPU time per program in a same-frame A/B, using pipeline timestamps
  around frame 1150 (temporary diagnostic).
- **GPU time per program before this fix (frame 1150, 242 dispatches,
  116.8 ms).**

  | Program | ms | Notes |
  | --- | --- | --- |
  | Pixel shader A (2 draws) | 21.6 | 57 runtime-M0 `v_movrels` |
  | Pixel shader B (26 draws) | 18.3 | 51 runtime-M0 `v_movrels` |
  | Compute, 8160 groups | 16.2 | no device-address access |
  | Compute, 32400 groups, 269 instructions | 15.7 | paired wave64 |
  | Compute, 131072 groups, 249 instructions | 13.5 | paired wave64 |

- **Fix: runtime-M0 relative moves select over the reachable registers
  only.**
  - **Problem.** VGPRs are separate SPIR-V variables. `v_movrels`,
    `v_movreld` and `v_movrelsd` with a runtime M0 were lowered to a
    compare/select (or gated store) over every VGPR the program names at or
    above the base: about 50 candidates per move in shaders A and B. The
    chains doubled the Intel instruction count and kept every named VGPR
    live at every move.
  - **What M0 actually holds.** Both shaders read 5-float records from a
    VGPR table. M0 is either `s_bfe_u32` of a 3-bit field times 5, or
    `vcc_lo` times 5 on the path where `s_cmp_lt_u32 vcc_lo, 7` held.
  - **The analysis.** The emitter (`ShaderSpirvVector.cpp`) now derives the
    values M0 can hold by walking back along the unique-predecessor chain:
    fallthrough, or a label with exactly one forward branch and no
    fallthrough. Indirect labels only mark conditional fallthroughs, so
    they are not joins; `s_setpc` and `s_swappc` stop the walk.
  - It evaluates these definitions: `s_mov_b32`; `s_bfe_u32` up to 5 bits;
    `s_and_b32` with a mask below 32; and `s_mul_i32`, `s_add`, `s_lshl` or
    `s_lshr` by a literal. It also uses an SCC branch whose preceding
    `s_cmp_lt/le/gt/ge_u32` bounds the register by a literal on that edge.
  - Sets are capped at 32 values. The chain keeps only candidates at those
    offsets; other offsets still read zero, and an unknown definition or a
    join keeps the full chain. Translator version 137. Unit test
    `NarrowsDynamicMovrelsToBoundedM0Values` covers the bit-field set, the
    compare guard and a join.
  - **Offline Intel compile (`INTEL_DEBUG=fs`).** Both "after" figures also
    include fa13ff72's single guest load per `s_buffer_load`.

    | Shader | Before | After |
    | --- | --- | --- |
    | A | 11,027 instructions, 66:220 spills:fills, 109k estimated cycles | 4,842 instructions, no spills, 64k cycles |
    | B | 207k cycles, 82:213 spills | 123k cycles, no spills |

  - **Same-frame A/B (frame 1150, same 242 dispatches).**
    - Shader A: 21.6 to 11.0 ms (5.5 ms per draw).
    - Shader B: 18.3 to 9.0 ms.
    - Frame GPU time: 116.8 to 96.2 ms (-18%).
    - Fps windows of the fixed build: 6.1-6.6, still within the noise
      above. A 20 ms drop in GPU time did not move fps, so shader time no
      longer sets the frame rate here; command-processor time and fence
      waits are the larger measured costs.
    - The logo and flyby captures render as before.
  - **Regression.** Intel: racing title 25-89 fps across the route
    (within earlier runs), fishing title 23.5-24 fps, sandbox title in its
    world with textures, watercolour platformer 248-423 fps. AMD: fishing
    title 11.4-11.6 fps, with no ring timeout.
- **Defect fixed on the way.** A runtime-M0 `v_movrelsd` defined
  `%rel_eq_<index>_<reg>` twice when a register was a candidate of both its
  source chain and its gated stores, which is invalid SPIR-V. The store side
  now uses its own ids. Unit test `EmitsVop1MovrelsdWithDynamicM0`.
- **Paired compute programs: native-proof refusals (not fixed).** A
  temporary trace of `ShaderAnalyzeComputeWaveNativeEquivalence` maps the
  two hot paired programs:
  - **269 instructions:** pc 0x1b4 refuses a `v_mov_b32` with a quad_perm
    DPP source ("operand is not representable per lane").
  - **249 instructions:** pc 0x35c refuses an `s_cbranch_vccz` ("a scalar
    value written inside a lane-conditional region is live at its join").
  - Other refusals in the same run: quad_perm DPP moves in the 16x16
    light-list programs, `v_mbcnt_hi` (lane counts), and `v_readfirstlane`
    into `vcc_hi`.
  - Making these native needs a host-lane = guest-lane mapping (as in the
    native wave32 prolog) and converged quads for DPP. That relies on host
    reconvergence guarantees, which are not established for the wave64
    native path. Recorded as a lead, not attempted.
- **Excluded for the fence waits (measured, not committed).**
  - Allowing storage publication when every non-storage alias is an older
    render-target, texture or depth copy removed the draw-time
    device-address wait. The same wait then reappeared in `WaitRegMem32`
    (1,871 ms over 128 waits), so total fence time did not change. The
    remaining blockers are an HTILE alias and an exact texture alias.
  - Also excluded: memoising device-address lookups per program (compile
    blow-up), a device-local copy of the lookup table (no change), and a
    fixed 720p render-target scope (no gain, since the cost is not
    render-target pixel work).
  - Not usable here: `INTEL_MEASURE` crashes the run, and TOP_OF_PIPE
    timestamps were unreliable for per-pipeline cost.
- **Steady state after the intro (measured 10-18, not fixed).** Interval
  206-210 ms: command processor 160 ms, fence waits about 90 ms, GPU
  timestamp total 96 ms, of which the three paired compute programs take
  about 45 ms.
  - Draw-time device-address waits: about 15 ms each, about 6.4 s per 20 s.
    The storage written on the same queue is not published on the device
    because aliases block it: stale pool entries (old render targets,
    storage textures the device never wrote) and the newer render target
    or depth image.
  - Skipping the three paired programs (temporary, unsafe) cut the interval
    from 206 to 142 ms, but the skip also removed their publication waits
    and the base window held a streaming burst, so 64 ms is an upper bound,
    not the GPU share.
  - Publishing past those aliases (temporary, unsafe) cut the interval from
    211 to 175 ms, but `WAIT_REG_MEM` time rose from 4 to 47 ms: the guest
    then waits for GPU completion instead. Excluded as a standalone fix.
  - Publishing past only the aliases the write-back leaves untouched
    (storage textures, GPU-owned render targets) is safe but moves the wait
    as well: device-address draws stop waiting and `WAIT_REG_MEM` time rises
    from about 2 to 12 s per 26 s window; blocked time per draw stays near
    290 us and frame p50 stays at 100 ms. The label waits cannot join their
    batch because pending storage still has aliases the write-back refreshes:
    a texture of the same range the device wrote after the storage (about
    half) and depth images crossing it. Excluded. The render engine is busy
    only 22-31% of the time (DRM fdinfo), so the frame is serialised by these
    waits, not GPU-bound.
  - Frame time is CPU recording plus GPU execution serialised by the
    guest's own waits, so GPU time of the paired programs is on the
    critical path. A rigorous alias design (retire superseded pool
    aliases) is open.
  - Also excluded: removing the dispatch drain (the wait moved to
    `WAIT_REG_MEM`, interval 187 to 190 ms) and flushing every 8 draws or
    dispatches (206 to 226 ms).
- **Vertex program cache (fixed, 92ac44db).** Every user-data mapping
  dropped all cached vertex programs; about 600 recompiles per run. Programs
  now record the guest ranges they were built from and only overlapping
  mappings retire them: under 100 per run. Unit test
  `OnlyOverlappingMappingsRetireCachedProgram`. Intel sweep: 22 of 22
  titles reach their documented state (reruns below).
- **GDS on the native compute path (fixed, 116ca7a0).** An earlier change
  let a wave with `ds_write_b32` to GDS pass the native-equivalence proof;
  the native emitter has no GDS lowering and the void platformer stopped.
  The proof now refuses GDS access. Unit test `RejectsGdsWrite`.
- **Intermittent stops seen during the sweep (passed on rerun).**
  - Insect platformer sequel: `GpuMemoryCreate.cpp:1548`, "unknown
    relation StorageTexture Crosses StorageTexture" for two identical 64 KB
    tile-9 storage textures 4 KB apart. New; open. Direction: define the
    relation for same-shape storage textures that cross (evict or copy the
    older one) instead of refusing.
  - Racing title: "unsupported depth-reference image binding"
    (`GraphicsRenderBind.cpp:4193` then, `:4308` on c57e20a4).
    Seen before on 10-16.
  - Cartoon title: `GraphicsRun.cpp:1372` indirect register packet at
    address 0x100000001. Seen in the sandbox title on 10-16.
- **Standard 4 KB detile (fixed).** During streaming bursts the command
  processor spent about 13% of its self time detiling SW_4K_S textures: a
  full address computation and a library `memcpy` per 16-byte run. The
  offset of (x, y) is offset(x, 0) + offset(0, y), because the block index
  is linear in each axis and the in-block swizzle puts X and Y bits in
  disjoint positions. Column offsets are now computed once per copy and
  each run is a fixed 16-byte copy. Host benchmark (2048x1024): 1.9-2.5 to
  12-28 GB/s across element sizes. Unit test
  `Standard4KBEveryElementSizeMatchesReference` (1, 2, 8 and 16 bytes,
  partial tail run). In one profiled window after the change no detile
  frame appeared on the processor thread; that window may not have held a
  streaming burst, so the in-game share is not yet measured. The robot
  platformer and fishing title render as before.
- **Excluded: exec-merge elision in the paired emitter (measured offline).**
  The hot paired program keeps 32 guest VGPRs live at its peak, so 64
  values per invocation against about 32 that SIMD32 holds in 128
  registers; the spills are inherent to the paired layout. Removing every
  merge that a sound liveness rule allows (222 of 509 vector writes)
  changed the compiler estimate from 8.14M to 7.15M cycles and spills from
  2370 to 2173. Not pursued.
- **Excluded: per-program cache of the paired admission classification.**
  The ratio of time per dispatch to time per draw stayed at 1.69 with and
  without it, so the per-dispatch classification is not a measurable cost.
- **Excluded: dropping the queue-wide device-address wait before indirect
  draws.** The indirect arguments are already read through the range-exact
  current-bytes path, so the wait looked redundant. Without it, draw
  processor time rose from 86 to 179 ms per frame and command-processor
  time from 145 to 280 ms in the same frame window (one run; submissions per
  frame barely moved, so the cause is not isolated). Kept.
- **Case-insensitive path lookups (fixed).** A guest path that does not
  exist verbatim made `GetRealFilename` scan the parent host directory for
  an entry equal ignoring case. The robot platformer probes absent files in
  package directories of up to 24,500 entries, and a loader thread was the
  most sampled thread in four profiles (`directory_iterator` frames). Each
  directory's case-folded index is now reused while its write time is
  unchanged; a directory written within the last two seconds is not cached,
  and the cache holds at most 2^18 names. Exact-entry and ambiguity rules are
  unchanged. Unit test `ReusedDirectoryIndexFollowsDirectoryChanges`.
  - Offline, one lookup of an absent name in the 24,500-entry directory:
    15.6 ms to 0.74 ms (50 calls on the title's host copy).
  - Robot platformer run: no `directory_iterator` frame in the profile;
    frame 905 at 116 s and 1,235 at 177 s against 848 at 114 s and
    1,167-1,210 at 175 s in the two previous warm runs, which is within
    run-to-run spread. Steady state unchanged: 5.8-6.3 fps, 120 ms command
    processor per frame.
  - The apparent 20 s plateaus near frames 130 and 900 in these run logs
    are a sampling artifact: the agent status file repeats unchanged (same
    frame, fps and 70-90 ms since the last present), so presents continue.
  - Savedata path: the card title writes its four save files and holds
    227-245 fps (earlier runs 209-247).
- **Paired compute admission by mnemonic (fixed).** During the intro
  (frames 130-900) the command-processor thread spent about 70% of its
  samples in `ShaderGetInputInfoCS`: every dispatch re-runs the paired
  admission, and its generic scalar, vector and compare classifiers built an
  allocated `String8` mnemonic per instruction for prefix and substring
  tests. They now test the shared, allocation-free name table
  (`ShaderInstructionTypeName`), built from the same enum range, so results
  are unchanged; compute-wave and shader suites (332 tests) pass.
  - Robot platformer (one run): intro 12.7-13.5 fps against 11.3-11.7 in
    the three previous warm runs; frame 735 at 93 s against 631-687. Steady state
    unchanged (5.9-6.4 fps), where this analysis does not appear in the
    profile.
- **Pipeline cache persistence (fixed).** Every pipeline creation marks the
  driver cache dirty, including cache hits, and each 30 s checkpoint rewrote
  the whole blob. Each robot platformer run wrote the same 35,428,999-byte
  blob three times in its first minute (106 MB of the 128 MiB session
  budget); the fourth checkpoint hit the budget, so pipelines first created
  later in the run were never saved and every run compiled them again. The
  store now keeps the size and XXH3 digest of the blob the file holds (as
  loaded or written) and skips a checkpoint whose blob matches, without
  charging the budget.
  - First run: three writes of growing blobs, none refused; the file grew
    from 35.4 to 38.2 MB.
  - Next two runs, one write each and no refusal: frame 850-855 at 94-98 s
    and 1,167-1,196 at 155-159 s, against 631-687 at 93-96 s and about
    1,150 at 175 s before, so about 20 s less to the measured scene. Steady
    state unchanged (5.5-6.2 fps, 106-122 ms command processor per frame).
  - Command-processor samples taken right after the measured frame vary
    between runs from 4% to 83% inside the Intel driver; a focused sample
    of the same window found 9.5%, so single windows of that kind are not
    evidence for or against a change.
- **Open leads, by measured size.**
  - The 8160-group compute program (16 ms).
  - The two paired programs above (29 ms together).
  - Shaders A and B (20 ms together) still run SIMD8 with 19
    device-address lookup loops.
  - `WriteBack` → `BufferWait` full drains on the processor thread: 128 per
    32 frames, 573 ms.
  - Directory iteration in `Path::MatchCaseInsensitive` on the file thread
    during loads.

## Robot platformer: title screen save cards (2026-10-14, fixed 2026-10-27)

- Observed (Intel A770, strict, `generic` route, 390 s, no error): the title
  screen shows the warp background, the logo and the top save card with its
  "NEW GAME" pill. The two lower save cards stay black and a faint second
  logo remains. Reference imagery of the same screen shows three lit cards
  and no second logo.
- Localized: the tiled light classification dispatch (120x68 groups) writes
  an empty light list for every tile. Its output was read through the
  current-guest-bytes path and is a fact. An offline evaluation of its inputs
  passes 0 of 8,040 tiles; the angle term is the failing predicate because
  the half-resolution normal buffer read back as a constant flat normal.
  That readback is not reliable (next item), so constant normals are an
  unverified cause.
- Diagnostic caveat: the debug image dump (`UtilDumpVulkanImageRgba8Png`)
  reads back through its own utility command buffer and does not submit the
  recording in progress, and the per-frame bound-image dump is overwritten
  by the last bind of the frame. State read this way shows an earlier point
  of the frame, not the bind being studied. Use `docs/graphics-captures.md`
  or a readback at a submission boundary when in-frame order matters.
- Observed only through the debug image dump (not in-frame ordered, so
  not exclusions): the temporal antialiasing output and its history image
  are already black on the lower cards, and so is the HDR scene target
  except near the ring.
- Excluded:
  - The stencil plane read by the lighting passes, as cause of the black
    cards and the second logo. The guest clears the plane (2048x1280, 8-bit)
    with the uniform fill kernel each frame. Its stencil writes on that depth
    target use ALWAYS, REPLACE_TEST 128 and write mask 0x80; a capped probe
    of every depth state on the target over 200 s saw no other writing
    state. Four lighting dispatches sample it; the half-resolution normal
    encoder and the light classifier do not read it at all, so the empty
    light lists do not depend on it. With d72457e5 (fill clears
    the host stencil, confirmed on every frame) and 55cd634d (the 8_UINT
    sample binds the device stencil, confirmed by the sample bind catalog),
    the screen is unchanged on Intel.
  - Ordering between the dispatches: memory barriers separate them in the
    command stream.
  - Translation of the classifier's control flow switch, `ds_append`,
    `vcc_hi` and `v_mbcnt` (checked against the ISA).
  - A render-target/storage alias of the normal buffer: its writer and all
    four readers bind the same exact storage view, with no older alias.
  - The G-buffer pass: MRT export translation (compressed, MRT0-4), per-MRT
    write masks (0xf, blending off) and depth-stencil state (LEQUAL with
    write) are correct. Its stencil state is the ALWAYS / REPLACE_TEST 128 /
    mask 0x80 state above; an earlier note of EQUAL, REPLACE, reference 4 is
    not supported by the register probe.
  - An earlier exclusion that read a 1x1x1 indirect dispatch as "counter 0"
    was flawed: it used stale indirect arguments (fixed below).
- Changes made while localizing (none changes this screen visibly):
  - c8e3537c: an exact storage texture lookup returns the view with the
    newest storage write among all views of that range, instead of the first
    listed one; a recycled range can hold views in several formats.
  - 6d1b178d: dispatch-indirect arguments are read only after any GPU writer
    in the recording or on another queue has published them.
  - 55565820: a depth-only clear of a D32S8 target loads stencil instead of
    discarding it (depth and stencil load operations are independent).
  - 18d6e5be: HTILE clears done by the generic uniform fill kernel are
    published in recording order, and the clear word `0xfffffff0` (ZMask 0,
    maximum Z 1.0 in both HTILE layouts, SMem 3 with stencil tiling) is
    translated to a depth clear when DB_DEPTH_CLEAR is 1.0. On this screen
    the G-buffer depth clear (D32S8) is now consumed at the next bind; D16
    and D32 targets translate as well.
- Verified on the final tree: robot platformer (Intel, 390 s) same state, no
  error. Racing title (Intel, 125 s twice and 200 s): no error, in-race
  present rate 38-40, inside the 34-45 range of the 13-14 October runs; a
  temporary counter showed it never takes the dispatch-indirect path. 2D
  action title (AMD, 120 s): no error, gameplay without trails or white-out,
  29-30 presents/s. Fishing title (AMD, 200 s): no error, reaches frame 3,093
  against 2,008 for the earlier baseline in the same time, 5-8 presents/s
  against 4-6. Watercolour platformer (AMD, 90 s): same captures and rates
  (72.5 and 38.5). Unit tests: 2,419 pass. Table and boundary gates pass.
  `kyty_playable_regression.py` reports `honest_skip_no_guest_root` here (no
  local profile or guest root); it must run without `python3 -I` because it
  imports a sibling module.
- Robot platformer on AMD (390 s, no error) on d72457e5..5236078d: 3.3-4.2
  presents/s on the title screen, then about 0.4 once the next scene loads,
  as on 13 October; it reaches frame 888 at the seventh capture against 865
  before, and the last captures show the tunnel instead of black. The
  racing, 2D action, fishing and watercolour titles show no regression on
  that range (in-race 34-43 on Intel; 28-29, 5-7 and 68/43 presents/s on
  AMD).
- Open defects found here:
  - A guest compute fill of the stencil part of HTILE is not applied to the
    host stencil plane: `DepthMetaComputeFillTranslatable`
    (`GraphicsRenderAttachments.cpp:500`) only translates depth. This title
    clears the stencil plane itself and its HTILE word marks stencil
    expanded (SMem 3), so no SMem-cleared word has been observed yet.
    Direction: translate SMem-cleared words to a stencil clear when the
    stencil clear register agrees.
  - `FindDepthStencil` (`GraphicsRenderDescriptor.cpp:1535`) asks for the
    first object only, so a recycled range with depth views in two formats
    can bind the stale one, as storage textures did before c8e3537c.
    Direction: pick the most recently written view.
  - Fixed 2026-10-17: `CommandProcessor::DrawIndexIndirect` and `DrawIndirect`
    waited only for device-address write-backs before their CPU read of the
    arguments, so a draw whose arguments an earlier dispatch of the same
    recording wrote could use stale counts. They now read the arguments
    through the same current-bytes path as 6d1b178d.
  - Exact storage views are not separated by image type; a 2D/3D filter was
    tried and dropped because no failure needs it.

- Fixed 2026-10-27 (70a42245). Cause: a light-list walk compute kernel
  (wave32) reads its list head and its nodes through pc-keyed scalar loads
  (`s_buffer_load_dword` on a V# loaded with `s_load_dwordx4`). Excluding
  unused metadata storage compacted the storage buffer list after the
  dynamic scalar-load records had captured their indices, and the records
  were not renumbered. Every load behind an excluded entry read the next
  descriptor: the head came from the node buffer and the nodes from a
  16-byte buffer, node 0 read as `{1, 0}`, the walk never reached its `-1`
  terminator, and the hang ended in device loss with black cards. The
  guest head and node buffers, written back by their producer dispatch,
  hold well-formed lists; the writer trace (`KYTY_TRACE_GPU_WRITER_RANGE`)
  and an offline chain walk of both buffers confirmed it. The records are
  now remapped through the compaction's active-index table.
- Changes made in the same pass: 9938819b (a metadata-only storage V# keeps
  its guest words in its user SGPRs instead of being bound), da9a9bf9
  (`DB_SHADER_CONTROL` decides whether an output-free pixel shader runs;
  EXEC_ON_NOOP shaders with no attachment run in a scissor-sized
  attachment-less pass), 1253b21b (readlane and writelane select their
  lane modulo the wave size).
- Verified (Intel A770, strict, 160 s, cross pressed at 100 s on PRESS ANY
  BUTTON): no device loss and no fence timeout; the title renders and the
  three save cards are lit, matching the reference video. Unit tests: 2,652
  pass. The architecture-boundary, archive-DAG and checked-in host-shader
  gates pass; the graphics diagnostics suite passes 15 of 15 with the
  unrelated EVENT_WRITE check below skipped locally.
- Excluded while localizing: a zero head grid as cause. The 128x128 grid
  and its node pool filled by a pixel shader with `ds_append` are a
  different structure, not the one the looping kernel walks. Bounds of the
  scalar-load stride, and a missing high-word clear in wave32 vector
  compares (the native path already drops it).
- Resolved as expected behavior: the reference video shows the flying ship
  with robots on the title screen because it was recorded with a gameplay
  save; with empty save data the guest submits neither ("Robot platformer:
  title ship and robots absent", 2026-10-27). The graphics
  diagnostics `EVENT_WRITE` contract fails on the current tree: the test
  expects event 0x38 to keep the short packet form, while
  `GraphicsDcbEventWrite` and `GraphicsDcbEventWriteGetSize`
  (`Graphics.cpp`, since febdc198) treat 0x38 and 0x39 as addressed. One of
  the two needs evidence before either changes.

## Robot platformer: vertex attribute format words (2026-10-14)

- Observed: the flat blue streak shapes over the warp tunnel (open item of
  the entry below) come from additive particle draws whose pixel program
  receives NaN in every interpolant. A temporary NaN-count probe in the
  pixel program showed NaN in all 2.8 million fragments of one draw; the
  NaN came from the vertex program's exported parameters, which all derive
  from a normalized attribute.
- Cause: the vertex attribute control word carries a format override in
  bits 5..13, laid out as `(buffer data format << 2) | (components - 1)`.
  The guest's own fetch code decodes it that way (`s_bfe_u32` of bits 5..6
  for the component count, 7..13 for the format, then a DST_SEL table
  X001/XY01/XYZ1/XYZW). Kyty matched the whole 9-bit field against a
  partial table plus a raw-format-id fallback, and kept the V# format for
  anything else. The normal (`0x10b`, 16_16_16_16_SNORM, 4 components) was
  therefore fetched as 32_32_32_FLOAT. `0xcb` (2_10_10_10_UNORM) and
  `0x117` (16_16_16_16_UINT) were also silently replaced.
- Change (4ea75ee0): decode format and component count separately, take
  the DST_SEL from the component count, and refuse an override whose format
  has no vertex input mapping (the "keep the V# format" path and the raw-id
  fallback are removed). The Gen5 vertex format table now lists every float,
  normalized and unsigned format of the ISA format table that has a
  mandatory Vulkan vertex-buffer equivalent. Signed-integer and scaled
  formats stay unmapped until the input path models them.
- Verified (Intel A770, strict, `generic` route, 330 s, no error): the warp
  tunnel shows thin lit streaks with no flat shapes; the studio logo, title
  logo and the ship are visible. Distinct override fields seen in that run:
  `0x05d 0x0cb 0x0e3 0x101 0x10b 0x117 0x11f 0x12a 0x137`. Racing title
  (Intel, 125 s), fishing title (AMD, 200 s) and watercolour platformer
  (AMD, 90 s): no error, same states as the baselines; all three use only
  field 0 (the V# format), so the decode change does not touch them. Unit
  tests: 2,419 pass. Table and boundary gates pass.
- Open: an override with fewer components than its format (for example
  `0x10a`, four-component storage read as XYZ1) is refused by the vertex
  input layout when the shader reads four registers; not seen in a live run.

## Robot platformer: exposure fill and DCC metadata clears (2026-10-14)

- Observed: after `TUNNEL_ENABLE` the Intel intro showed only the UI over a
  black scene, with stale publisher text and a smeared logo. Bound-image
  readbacks at present 1400 showed the 3D scene rendered correctly into its
  2432x1368 RGBA16F targets (space backdrop, streaks, the console-shaped
  ship). So the black came from the final composite, not from skipped
  geometry.
- Cause 1, exposure: the composite pixel program multiplies the tonemapped
  scene by an exposure read from a 1x1 RGBA32F storage image. The guest
  resets the exposure state each frame with the uniform compute fill kernel,
  `(1, -1, 0.5, 0.18)`. The fill decoder refused RGBA32F, so the sampled
  texture stayed zero and the exposure pass wrote zero. Fixed in 582c6662: a
  16-byte texel is the fill quad itself.
- Cause 2, UI layer: the UI target (3840x2160 RGBA16F, DCC on) has its
  metadata filled with key `0x40` (color 0, alpha 1, per the Mesa
  `ac_descriptors.h` keys) before the UI draws. Kyty ignored metadata fills,
  so the UI accumulated earlier frames. The composite uses the UI alpha as
  scene transmittance, so the accumulated UI also hid the scene.
- Cause 3, lighting: the lighting target's metadata is filled with `0x00` and
  four lighting compute passes then write the target as a storage image
  before its first render-target bind. Consuming the clear at that bind
  erased the lighting and left the ship as a black silhouette.
- Change (fabd1ecb, contract in `docs/graphics-compute-uniform-color-fill.md`):
  - A metadata fill with a clear key is a pending clear.
  - The first access consumes it before the access happens: a render target
    with that DCC base, or a shader image, read or written, whose T# enables
    compression with that metadata base.
  - It is applied only to single-level, single-slice, single-sample surfaces
    whose fill holds one key per 256 bytes of texels.
- Verified (Intel A770, strict, `generic` route, 330 s, no error, 1,482
  frames): the captures show the publisher text, the studio logo over the warp
  tunnel, the title logo and the lit ship. A diagnostic run of the same
  tree applied seven metadata clears in present 1400. Racing title (Intel,
  125 s), fishing title and watercolour platformer (AMD, 200 s and 90 s): no
  error, and the same states as the 1d692f53 baselines. The fishing title
  keeps its in-game rate (about 150 frames per 26-30 s capture interval in
  both runs). Unit tests: 2,418 pass, 12 skipped, 0 failures. Table and
  boundary gates pass.
- Open, found while verifying:
  - Flat blue streak shapes cover the scene (fixed: see "Robot platformer:
    vertex attribute format words"). They are drawn by instanced
    particle draws (48 vertices x 150 and x 300 instances) into the 1920x1080
    effect layer and the 960x540 half-resolution layer, and composited by the
    pass that writes the scene before TAA. The shapes exist with and without
    the metadata clears; only their color changes.
  - Some frames show a black top-left quadrant in the half-resolution layer
    (960x540 written into a 1920x1080 surface).
  - The skip prompt's text is drawn as filled squares.
  - Not modelled: the pipe-dependent padding of the metadata plane, mip
    chains, arrays, MSAA, reversed component swaps, and register-key clears
    read through the texture unit.

## Robot platformer: Gen5 dispatch TUNNEL_ENABLE (2026-10-13)

- Observed: with the 64-entry table, a 600 s Intel run passed the title
  screen and loaded the intro level. It stopped at 461 s on
  `compute dispatch admission rejected: mode=0x0000a041
  reason=unsupported_dispatch_mode` (`GraphicsRenderDraw.cpp`). The
  initiator is COMPUTE_SHADER_EN, ORDER_MODE and CS_W32_EN plus bit 13.
- Contract:
  - The GFX10.3 register header (`gc_10_3_0_sh_mask.h`, Linux amdgpu) names
    bit 13 of COMPUTE_DISPATCH_INITIATOR `TUNNEL_ENABLE`.
  - A Mesa Vulkan driver sets it on every GFX10+ dispatch and says the
    vendor's own driver does too. The kernel driver honours it only on
    high-priority compute queues; elsewhere it is a no-op. It asks the
    scheduler to prioritise the dispatch's waves and does not change
    groups, lanes or results.
- Change: `ShaderComputeDispatchExecutionMode` clears `TUNNEL_ENABLE` on
  Gen5 before the dispatch plan, the lane order and the input info see the
  mode. Every other unknown bit stays refused. Gen4 is unchanged.
- Verified:
  - Intel robot platformer: 723 s with no error, 1,989 frames. The intro
    level starts (`Level has started: intro_next`). The captures show the
    title menu (logo and buttons) over a black scene, with the publisher
    logo text still drawn under it; the intro level is dark. That is the
    next problem.
  - The guest logs `JsParticle heap memory is full!` from the intro level
    on (32 lines). Not investigated.
  - Intel racing title: 14.9k / 19.4k / 24.9k / 25.7k frames, no error
    (previous run 25.6k).
  - Unit tests: the 154 compute-wave and dispatch tests pass.

## Robot platformer: 64-entry shader texture table (2026-10-13)

- Observed: after the storage-image alias fix, the Intel run stopped at
  253 s on `shader texture resource capacity exceeded`
  (`ShaderGetTextureBuffer`, `Shader.cpp:2181`). The table held 32 entries.
  The stopping pixel program reads 43 distinct non-empty 8-dword image
  descriptors. 40 of them are in the extended user data, each read by a
  constant-offset `s_load_dwordx8`; the program has no dynamic descriptor
  loads. So the guest really binds more than 32 images in one stage.
- Host limits: `maxPerStageDescriptorSampledImages` is 33,554,432 on the
  Intel A770 (ANV) and 8,388,606 on the AMD Raphael iGPU (RADV). The table
  size is a host bookkeeping cap, not a Vulkan limit.
- Change (`Shader.h`): `ShaderTextureResources::RES_MAX` is 64. The
  storage-image write-only and seed-skip masks, which hold one bit per table
  entry, become `uint64_t` (`Shader.cpp`, `GraphicsRenderDraw.cpp`,
  `GraphicsRenderBind.cpp`, `GraphicsRenderInternal.h`). The descriptor-cache
  arrays and the metadata push-constant bound follow `RES_MAX`.
- Verified:
  - Intel robot platformer: past the stop, 300 s with no error, 1,287
    frames at 2.9-7.2 fps. The captures still show the dark loading screen.
  - Intel racing title: 15.3k / 18.1k / 24.5k / 25.6k frames, at or above
    the previous run.
  - AMD (RADV): racing title 9.9k frames, unchanged; watercolour platformer
    3.4k and 3.9k frames.
  - AMD fishing title, same tree and route, 200 s. Steady rate is frames per
    30 s over the last three capture intervals:

    | Table size | Perf counters | Final frame | Steady rate |
    | --- | --- | --- | --- |
    | 32 | on | 2,860 | 188-207 |
    | 64 | on | 2,304 (slow load) | 192-196 |
    | 64 | off | 2,792 / 2,814 / 2,858 | 136-225 |

    The two sizes overlap. Runs on earlier binaries reached 3,058-3,106
    frames at 206-238 frames per 30 s. That gap is not attributed:
    - The storage-image alias change only replaces an EXIT that this title
      never reached.
    - Foreign processes loaded the host during some of the later runs
      (load average 4-12).
    - A repeat on HEAD under a load average of 12 gave 141-152 frames per
      30 s, so it does not separate the two causes.
    - The HEAD run before it stalled at frame 3 at boot. That is the known
      intermittent boot stall, which also reproduces on older binaries.
  - Unit tests: 2,413 pass. The one failure is the known intermittent
    `StandardSamplerRackDestroyWaitsForACallbackRunningOnAnotherThread`
    timeout (see the 2026-10-11 entry); it passes alone 3/3. Fixed since
    (ad8d4c76).
- Open: per-draw structures grow with the table (`ShaderPixelInputInfo`
  6,392 bytes, `ShaderVertexInputInfo` 8,984 bytes), and `BindDescriptors`
  zero-initializes and scans the full arrays rather than the bound counts.
  No cost was measured above noise.

## Robot platformer: storage image over a sampled and a buffer view (2026-10-13)

- Observed: after the pixel append fix, the Intel run stopped at 228 s on
  `!create_all_the_same`. The new object is a 128x128 R32 StorageTexture
  (64 KiB). Its range equals two live objects:
  - a sampled Texture with the same format, pitch, extent, tile and swizzle,
    created with its guest upload skipped;
  - a StorageBuffer of 4,096 16-byte records, uploaded from guest memory,
    with a write-back, not in use.
  The single-parent path already links each of these relations
  (`GpuMemoryAllowsTextureStorageAlias`). The multi-parent path had no rule
  for mixed parent types, so it reached the EXIT.
- Change (`GpuMemoryCreate.cpp`): `multi_storage_texture_alias` replaces only
  that EXIT. It links the new image when every parent independently passes
  `GpuMemoryAllowsTextureStorageAlias` and has published its content to
  guest memory: no texture materialized from a device image, and no buffer
  with an unfinished writable write-back. The image seeds from guest bytes.
  No earlier branch changes, so titles that did not reach the EXIT keep
  their path.
- Verified:
  - Intel robot platformer, twice: past the overlap, 1,277 frames at
    3.4-10.9 fps. It stops at 253 s on the 32-entry texture table: the
    shader declares more read-only texture slots. The 64-entry table entry
    above takes it past that stop.
  - Unit tests: 2,414 pass.
- Open (coherence, not verified):
  - Storage images have no write-back
    (`StorageTexture.h:53`).
  - A buffer write-back does not invalidate a StorageTexture parent
    (`GpuMemory.h:775`).
  - So the image's writes do not reach the sampled view (which holds no guest
    bytes) or the buffer view. The other direction works: a buffer
    write-back changes guest bytes, and the image's hash check reloads them.
  - Suggested direction: publish storage-image writes to linked views, by
    copy or write-back. Judge it with a scored capture of a scene that uses
    this range.

## Robot platformer: pixel GDS append under the launch EXEC (2026-10-13)

- Observed: the same linked-list pixel program allocates its node slot as
  `v_mbcnt_hi v7, exec_hi, 0` / `s_mov m0, s20` / `ds_append v6 gds` /
  `v_mbcnt_lo v0, exec_lo, v7`, before any EXEC write or branch. The
  program starts with `exp null vm done` and has no `s_wqm`. The native
  wave proof refused it twice: MBCNT observes EXEC as a number, and a
  memory write runs while helper lanes are live in the host invocation.
- Contract:
  - RDNA2 ISA: EXEC is the wave's valid mask until the program writes it,
    and `exp ... vm` sends EXEC as the valid mask.
  - DS_APPEND adds `count_bits(EXEC)` at `M0.base + offset` and returns the
    pre-op value to every valid lane.
  - V_MBCNT_HI is a plain source move in Wave32, so MBCNT over EXEC is the
    exclusive prefix of valid lanes at both widths.
  - Vulkan: helper invocations have no effect on stores and atomics, and
    their atomic results are undefined.
- Change:
  - `ShaderLaunchExecEnd` marks the first EXEC write or control-flow
    boundary. `ShaderCountsLaunchExec` accepts only MBCNT_LO/HI over
    EXEC_LO/EXEC_HI before it.
  - The partial-wave admission and the fragment mask flow accept those
    MBCNTs and GDS append/consume (and device memory writes) before that
    point. LDS and scratch writes stay refused.
  - Pixel emission builds a valid-pixel ballot (`exec_lane_lo` and not a
    helper). MBCNT counts it. One valid lane updates the counter by its bit
    count, and the pre-op value is shuffled to every valid lane.
  - The GDS pointer SGPR is the M0 source at the GDS instructions, not the
    last M0 write in the program (here M0 is later reloaded for
    interpolation).
- Verified:
  - The dumped program proves `FragmentPartialWave` at both widths.
  - Unit tests: admission at both widths; refusal after a branch and for a
    non-EXEC mask; SPIR-V with one counter atomic, no elect or broadcast,
    and validated by the toolchain.
  - Strict AMD (RADV) runs, no error:

    | Title | Frames |
    | --- | --- |
    | racing title | 5.6k / 7.5k / 9.3k / 9.9k |
    | watercolour platformer | 4.1k at 60 s |
    | fishing title | 2.0k, then 3.1k on a repeat, at 180 s (the first run loaded slower; same steady rate) |
  - Intel robot platformer: 1,160 frames at 1.8-7.9 fps, past the
    linked-list program. It stops at 228 s on `!create_all_the_same`: a
    64 KiB StorageTexture whose range equals a sampled Texture and a
    StorageBuffer. That is the next stop.

## Robot platformer: image atomic swap with a GLC return (2026-10-13)

- Observed: after the Wave32 OR reductions, the Intel run stopped at
  `ShaderParseMIMG.cpp:49`. The parser admitted GLC only on
  IMAGE_ATOMIC_ADD (0x11). The stopping program is a per-pixel linked list:
  `image_atomic_swap v9, v[0:1], s[0:7] glc` exchanges a new node index into
  a 2D R32_UINT head image, and the node is written through buffer stores.
- Contract:
  - RDNA2 ISA: IMAGE_ATOMIC_SWAP (opcode 15) is `tmp = MEM; MEM = DATA;
    RETURN = tmp`.
  - GLC on an atomic selects the pre-op return (ISA 8.1.10).
  - Load and store cache policy (GLC on non-atomics) is still refused.
- Change:
  - `ImageAtomicSwap` is a new instruction type. It shares the parse,
    admission (`ShaderImageAtomicSupported`), storage-usage and emission
    paths of IMAGE_ATOMIC_ADD through `ShaderIsImageAtomic`.
  - `Recompile_ImageAtomic` emits `OpAtomicExchange` or `OpAtomicIAdd` on
    one R32 texel, in compute and pixel stages.
- Verified: unit tests cover both opcodes with and without GLC, and the
  sampler-operand refusal for both.

## Robot platformer: Wave32 whole-wave OR reductions (2026-10-13)

- Observed: once the Wave32 lane masks were narrowed, the Intel pixel
  program stopped at pc 0x166c with `fragment row DPP source participation is
  unproven`. The construct is the Wave32 form of the wave64 worldmap
  reduction:
  - `s_mov_b32 s32, exec_lo` ... `s_orn2_saveexec_b32 vcc_lo, s32`;
  - `v_cndmask_b32 v23, 0, v23, s33`, with `s33 = s32 & s64`;
  - four `v_or_b32` with `row_shr:1/2/4/8`;
  - `s_mov_b32 exec_lo, vcc_lo`, an unrelated `v_rsq_f32`, then
    `v_readlane` of lanes 15 and 31 and `s_or_b32`.
- Contract:
  - RDNA2 ISA: V_READLANE_B32 takes `lane % 32` in a wave32 and ignores EXEC.
  - The upper halves of EXEC and VCC are ignored in a wave32.
  - Row DPP stays within a 16-lane row in both widths.
- Change:
  - The neutral-region proof (`ShaderFragmentNeutralRegionEnd`) takes the
    guest wave size. For Wave32 it accepts the B32 save, ORN2_SAVEEXEC,
    AND and restore forms on single SGPRs. It runs on the narrowed program,
    and the select is classified with its encoded pair condition.
  - Every other region user (tier, partial-wave reads, emitter) still asks
    for the Wave64 form only. In a Wave64 program a B32 entry widens only
    EXEC_LO, so it never forms a region.
  - The reduction proof uses the guest lane count, refuses a constant lane
    at or above it, and follows the B32 mask algebra for the residual
    (stale-lane) obligation.
  - The mask flow result records the guest width it was proven for.
  - `FragmentPartialWave` is no longer limited to Wave64. Host selection
    already limits it to host subgroups no wider than the guest wave, so a
    host invocation id is the guest lane.
  - At both widths, instructions that name no network register, keep EXEC,
    and are neither branches nor labels may now sit between the restore and
    the reads.
- Verified:
  - The dumped program now proves `FragmentPartialWave`, with one reduction
    whose reads cover lanes `0xffff` and `0xffff0000`.
  - Unit tests: 2,412 pass. New cases:
    - the Wave32 sequence and its SPIR-V emission;
    - refusal at Wave64;
    - refusal for lane 32;
    - stale lanes after a B32 WQM;
    - independent versus observing instructions before the reads (Wave64).
  - Strict AMD (RADV) runs, no error:

    | Title | Frames |
    | --- | --- |
    | racing title | 7.6k / 9.4k / 11.2k / 12.0k |
    | watercolour platformer | 4.1k at 60 s |
    | fishing title | 3.1k at 180 s |
- Robot platformer, cold SPIR-V caches:
  - AMD: 303 s without error, 941 frames at 3.3-5.3 fps, still on the
    publisher logo. The previous run loaded at about 0.4 fps.
  - Intel (Arc, no concurrent GPU job): 1,190 frames at 3.5-8.3 fps. It
    passes the logo into a dark loading transition, then stops at 251 s on
    `ShaderParseMIMG.cpp:49`: `glc == 1` on a MIMG opcode other than 0x11.
    That is the next stop.

## Robot platformer: wave32 lane masks are one dword (2026-10-13)

- Observed: after the SDWA fix, the same Intel pixel program (guest wave32)
  was refused at pc 0xf48 with `reason=mask observed as a number`:
  - 0xf30 writes a number into `vcc_hi` (`s_bfe_u32`).
  - 0xf40 runs `v_cmp_eq_u32 vcc, 3, vcc_hi`.
  - 0xf48 and 0xf5c read `vcc_hi` back as that number.
  The mask flow had marked both VCC words as lane bits at 0xf40.
- Contract (RDNA2 ISA 2.1: "the upper half of EXEC and VCC are ignored for
  wave32 waves"; 3.x: "when used by a wave32, the upper 32 bits of VCC are
  unused and only the lower 32 bits contribute to VCCZ"):
  - In a wave32 program, every VALU lane mask is one dword: compare and
    carry-out destinations, carry-in and the `v_cndmask` condition.
  - The upper SGPR of the encoded pair is neither written nor read, so it
    may hold a number across the instruction.
  - The guest relies on this, as shown above.
- Implementation:
  - `ShaderWave32LaneMasks` narrows those operands; `S_BFM_B64` shares the
    compare format and is excluded.
  - `ShaderAnalyzeFragmentMaskFlow` takes the guest wave size. At 32 it
    walks the narrowed program, branches on VCC_LO only, and accepts the B32
    mask algebra (AND, OR, ANDN2, XOR, ORN2, NOT, MOV, AND_SAVEEXEC) wherever
    the region rules accepted the B64 forms.
  - Wave64 behaviour is unchanged.
  - The emitter already kept the upper word in wave32: `ResolveMaskAccesses`
    stores only the low ballot word and drops the high-word clear.
- Next stop: the same program now clears the mask flow and stops at
  pc 0x166c with `fragment row DPP source participation is unproven`. This
  is a wave32 whole-wave OR reduction:
  - `s_orn2_saveexec_b32`, then a `v_cndmask` seed;
  - four row-DPP `v_or_b32`;
  - `v_readlane` 15 and 31, then `s_or_b32`.
  The partial-wave reduction proof (`ShaderFragmentLaneReduction.cpp`)
  covers only wave64: `S_ORN2_SAVEEXEC_B64`, 64 lanes and B64 mask algebra.
  Extending it to wave32 (lane sets 0-15 and 16-31) is the direction. The
  DPP precondition stays: a host shuffle from an inactive or absent lane is
  undefined.
- Verified:
  - The mask flow proof was checked offline on the dumped program.
  - Unit tests: 2,410 pass. A new case runs the same program at both widths:
    wave32 keeps the number in VCC_HI, wave64 refuses it. The
    whole-pair VCC branch case now states the wave32 result.
  - A temporary per-program check (env-gated, removed) showed that no pixel
    program of the racing title, the watercolour platformer or the fishing
    title runs as wave32. Their analysis is unchanged.
  - Strict regressions on AMD (RADV, warm), no error:
    - racing title: 5.6k / 7.4k / 9.3k / 10.2k;
    - watercolour platformer: 3.6k at 60 s;
    - fishing title: 3.1k at 180 s.
  - On Intel the three titles ran without new errors, under a concurrent
    host GPU job on the same device, so their frame rates are not
    comparable:
    - One racing title run hit the open intermittent depth-reference
      refusal recorded below.
    - One stopped in `String::Size` before the first frame, and did not
      reproduce on two reruns.

## Robot platformer: SDWA low destination selects (2026-10-13)

- Observed: the Intel stop at pc 0x157c of a 1920x1080 pixel program is
  `v_cvt_i32_f32` in SDWA form with control dword 0x0006140d
  (`dst_sel=WORD_0`, `dst_unused=PRESERVE`). The lowering preconditions
  refused every destination select other than DWORD.
- Contract (RDNA2 ISA, SDWA table 88; the LLVM AMDGPU modifier syntax gives
  the same ranges):
  - `DST_SEL` 0-3 select a byte, 4 bits [15:0], 5 bits [31:16], 6 the dword.
  - `DST_UNUSED` 0 pads the other bits with zeros, 1 sign-extends the
    field, 2 keeps the prior bits, 3 is reserved.
  - Neither source says where the result sits for `WORD_1`/`BYTE_1-3`
    (shifted into the field or taken from the matching bits). Only one
    secondary implementation shifts it, which is a lead only.
- Implementation:
  - `ShaderSdwaLowDestinationSupported` admits `BYTE_0` and `WORD_0` with
    `DST_UNUSED` 0-2.
  - It applies to VOP1 and VOP2 with a plain single-VGPR destination, no
    clamp or output modifier, and no MAC or dot forms.
  - The per-lane emitter (`EmitSdwaLowDestinationInstruction`) runs the
    ordinary instruction as a whole-dword write, then merges its low field
    into the prior value (pad, sign-extend or preserve). An inactive lane
    keeps the prior value.
  - Paired and native compute-wave banks still refuse these selects, and so
    do shifted selects, reserved `DST_UNUSED`, clamp and output modifiers.
- Verified: a unit test pins the admitted and refused control words. A
  strict Intel run of the robot platformer passed pc 0x157c and stopped at
  the wave32 VCC case above.
- Defect found, not fixed: the host-visible compute scratch pool
  (`TransientBufferPool::Scratch`, `GraphicsRenderCommandBuffer.cpp:1090`) frees
  entries only in `Free()`. A command buffer that detiles many large
  resources fills its 16 MiB, and the detile then falls back to the CPU with
  `ResourceUnavailable`. A per-submit reset or a size-aware reuse that keeps
  the two-buffer detile rule is the direction.

## Robot platformer: raw texels between aliased depth and colour views (2026-10-13)

- Observed: one 960x540 range (1024x640x4 bytes, 64 KiB blocks) is
  time-shared every frame by a B10G11R11 render target (mode 27), a
  10_11_11 storage view (mode 27), an R32F storage view (mode 24, one level)
  and a D32 depth target with no HTILE. A bounded diagnostic showed the depth
  bound read-only (`LEQUAL`, depth writes off, no clear) and the R32F
  storage view as its producer: image-store analysis marks it fully
  overwritten. The previous code had no relation for depth over storage, so
  the read-only depth started undefined and hit the read-only clear refusal.
- Contract:
  - The 4-byte depth (mode 24) and render-target (mode 27) layouts were
    taken to address texels alike, pinned by a unit test that used the
    production helpers. Disproved on 2026-10-16: the guest's mode 27 micro
    tile differs (see "Four-byte render-target micro tile"); the seed now
    admits mode 24 views only.
  - A depth target over an equal-range render target or matching storage
    view, and a storage view over such a depth target, copy the raw 32-bit
    words of the newest writer (`create_texel_bits_seed`), then retire it.
  - Matching means:
    - one sample, one layer, one level;
    - an equal extent;
    - a pitch equal to the depth's 64 KiB-aligned width;
    - R32F or B10G11R11 colour;
    - no HTILE;
    - host extent equal to guest.
  - Any later write to another overlapping view refuses the seed.
- Copy (`UtilCopyTexelBits`): image to buffer to image, with no conversion.
  Its staging is a device-local per-command-buffer pool
  (`AllocateTransientCopyBuffer`). The host-visible compute scratch pool did
  not work for it: on Intel, two detile entries (8.3 MB and 6.6 MB, other
  usage masks) already held 14.9 of its 16 MiB, so the 2 MB copy was
  refused at about 140-150 s.
- Limit: Intel Arc (DG2) does not expose `VK_EXT_depth_range_unrestricted`,
  so a depth upload outside [0,1] is outside VUID-07931 there. RADV exposes
  it.
- Excluded:
  - Clamping the words into a depth image destroys the bit pattern the
    colour views read back, so it was rejected.
  - The nondeterministic `!create_all_the_same` at another range in the same
    title (78 s, Intel) is the documented transient-pool class. This seed
    path did not run there.
- Next stop (Intel, reproduced twice at 178-189 s, 1920x1080 pixel stage):
  - `native_admission{... pc=0x157c reason=invalid instruction before native
    wave analysis}`.
  - The instruction is `v_cvt_i32_f32` in SDWA form (control dword
    0x0006140d: `dst_sel=WORD_0`, `dst_unused=PRESERVE`, `src0_sel=DWORD`),
    a 16-bit partial write that keeps the high half of the VGPR.
  - The parser kept the destination select and the lowering preconditions
    refused it (fail closed); the per-lane merge is in "SDWA low destination
    selects" above.
- Verified:
  - The robot platformer on AMD (RADV, `Fixed` + RenderTargets) ran 600 s
    and then 300 s, both status 1 with no error. It is still loading the
    worldmap at about 0.4 fps, with black captures.
  - On Intel it ran 178-189 s, until the SDWA stop above.
  - Regressions (strict, no errors):

    | GPU | Title | This change | Baseline |
    | --- | --- | --- | --- |
    | Intel A770 | racing title | 16.0k / 20.6k / 26.1k / 27.5k frames at 30 / 60 / 90 / 120 s | 15.4k / 20.1k / 25.7k / 27.0k |
    | Intel A770 | watercolour platformer | 10.7k at 60 s | about 10.5k |
    | Intel A770 | fishing title | 5.6k at 180 s | about 5.5k |
    | AMD (warm cache) | racing title | 6.3k / 8.4k / 10.3k / 11.2k | 5.7k / 7.5k / 9.4k / 10.3k |
    | AMD (warm cache) | watercolour platformer | 4.1k at 60 s | about 4.1k |
    | AMD (warm cache) | fishing title | 3.1k at 180 s | about 3.0k |

    The first AMD pass ran right after a host reboot. It was 25-35% lower
    from the first window on, and recovered on rerun.
  - Unit tests: 344 graphics-state and detile tests. Gate scripts:
    boundaries, graphics tables.

## Robot platformer: whole-wave OR reductions as partial waves (2026-10-13)

- Fix: the worldmap pixel shader's whole-wave OR reduction is admitted with
  a new proof, `FragmentPartialWave`. A Wave64 pixel program on a host
  subgroup of any width is a partially populated guest wave. When its only
  whole-wave observations are proven OR reductions and reads that stay
  inside launched quads, the proof holds at every host width. It therefore
  needs no `requiredSubgroupSize` for the fragment stage, which the Intel
  Arc host cannot provide.
  - Reduction contract (`ShaderFragmentLaneReduction.cpp`): a proven neutral
    region with a `v_cndmask` seed whose condition holds only lanes of the
    entry EXEC (no helper, no lane outside the launched quads). Its steps are
    row-shift DPP ORs with full row and bank masks, `v_permlanex16` and
    plain ORs. The reads are static `v_readlane` after the EXEC restore,
    before the first export or indirect jump.
  - Lowering: each read is one `OpGroupNonUniformBitwiseOr` over the seeds of
    its guest lane set (lane 31 reads lanes 0-31; lane 63 reads 32-63),
    followed by a quad-uniform broadcast. The network steps are not emitted.
  - Network liveness: after the reads, every path must overwrite the network
    registers before any other read. An overwrite under a non-full EXEC
    leaves the register "residual", stale only in launched lanes outside the
    restored mask. A residual register may be read only while EXEC stays
    inside that mask (tracked through mask algebra, joined monotonically
    across loops), and never through DPP, readlane/writelane/permlane, DS or
    a non-lane-local image read.
  - The fragment mask flow now records, per instruction, whether EXEC holds
    only lanes of the launched quads, all of them, and which `v_cndmask`
    conditions are live-only.
- Excluded, from a temporary diagnostic in the analysis: requiring a full
  EXEC for the overwrite. The worldmap shader kills the network registers
  under the discard mask (`s_andn2 ... vcc` then `s_wqm`), so that rule
  refused it on both of its paths.
- Excluded: exact-state deduplication in the liveness walk. A loop head in
  the same shader produced more than 16 distinct states; the walk now joins
  states monotonically.
- Current frontier on both hosts: a wave64 vertex program, refused with
  `ngg_front_refusal{... reason=complete program exceeds the 1024-instruction
  proof bound}`. Neither Intel nor RADV can pin the vertex stage to 64 lanes.
- Verified:
  - The robot platformer passes the worldmap pixel stop and stops at the
    vertex bound on Intel Arc A770 (172 s) and on AMD RADV (272 s, fixed
    render-target resolution), strict runs.
  - Regressions, one strict run each on both hosts (status 1, no error): the
    racing title is in a race, the watercolour platformer is in the intro
    and the fishing title is docked in gameplay. The capture frame counts
    match the previous runs.
    - The first Intel run of the racing title after a host reboot (cold disk
      cache and a fresh SPIR-V cache) was still loading at 90 s. A warm rerun
      matched the previous runs frame for frame.
  - Unit tests: 980 tests across the shader, fragment, compute, NGG, packet,
    state and detile suites. New admission cases cover the proof, refusal
    of observed network values and of row-masked steps, stale lanes under
    WQM, and SPIR-V emission validated with `spirv-val`. Gate script:
    boundaries (strict).

## Robot platformer: worldmap admission chain (2026-10-13)

- Four general fixes move the robot platformer from its early stops to the
  worldmap level load on both hosts:
  - **Tiled storage writers composed into linear views.** A linear storage
    view over memory last written through single-level 2D storage images in
    the 64 KB R_X layout is composed from those images, as it already was from
    tiled render targets. The newest complete writer of each range wins.
  - **Loop exits that break through an arm.** The CFG structurizer accepts a
    loop exit that enters an arm placed between the header and the latch when
    every path of the arm stays under the header, leaves only to the merge and
    shares no block with another exit or with what follows the merge
    (`ShaderControlFlowGraph.cpp:IsBreakArm`). Only graphs refused before are
    affected.
  - **Wave32 lane masks in scalar flow.** A wave32 `V_CMP`/carry-out writes only
    the low SGPR of its pair. The scalar flow read every program as wave64, so
    a compare into `s[13:14]` hid that `s14` still held its entry value. A
    compute shader's extended user data load through `s[14:15]` then lost its
    descriptor mapping (emitter refusal on `S_LOAD_DWORDX4` at 0x1740). The flow
    now keys on the guest wave size. Compute resource analysis passes the
    dispatch plan's size; the other stages keep the conservative wave64
    reading.
  - **Any-lane mask tests.** `S_CMP_LG_U64`/`S_CMP_EQ_U64 mask, 0` followed by a
    forward SCC branch asks what `S_CBRANCH_VCCZ` asks. The fragment mask flow
    opens a divergent region for it instead of refusing the compare as a
    numeric observation. The summary is cleared by any other SCC writer or a
    join. Every other SCC reader and back edges stay refused. Masks that may
    hold helper bits under WQM qualify, as they do for VCCZ.
- Current frontier on both hosts: a 1920x1080 worldmap pixel shader (wave64,
  3 colour targets, depth write, blend) is refused by native wave admission.
  It is now classified `FragmentNeutral32` (it was `ExactSubgroup` before the
  any-lane fix). The refusal is participation: "fragment row DPP source
  participation is unproven" at the first of four `V_OR_B32` row-shift steps
  (`row_shr` 1/2/4/8, `bound_ctrl` 0), in a whole-wave OR reduction.
  - The reduction is: `s_orn2_saveexec` region,
    `v_cndmask 0, x, live&exec`, the row scan, `v_permlanex16`, then
    `v_readlane` 31 and 63 after the restore and `s_or` of the two halves.
  - The same shader also has three `v_readfirstlane` waterfall loops, refused
    with "target quad is not proven to retain a participating real member".
  - Helpers relay partial ORs through the scan, so neutral initial values do
    not make a non-participating helper's shuffle source defined. This
    refusal is correct and must not be relaxed.
  - A whole-wave reduction proof is a viable direction: lower the readlanes
    as subgroup OR reductions of the initializer, valid at every host width.
    It needs the initializer's condition proven narrow, the scan registers
    dead after the region, and EXEC after the region proven within the
    region's entry EXEC.
- Intel-specific gate: the Arc A770 lists only COMPUTE, TASK and MESH in
  `requiredSubgroupSizeStages`, and fragment subgroups vary from 8 to 32.
  `FragmentNeutral32` therefore has no legal mapping on Intel
  (`host=unavailable mapping=0`) even with the participation proof. Intel
  needs a proof valid at every width, or the fragment wave transport (the
  compute stage can pin 32). AMD (RADV) can pin fragment 64 and reports
  `mapping=1`.
- Scoping experiment (temporary, uncommitted bypass of the participation
  refusals): AMD then stops at a vertex program with
  `ngg_front_refusal{... reason=complete program exceeds the 1024-instruction
  proof bound}`. RADV cannot pin the vertex stage to 64 either.
- Open gap: the pixel and vertex resource analyses still read scalar flow as
  wave64 (conservative, not a correctness defect).
- Verified:
  - Intel Arc A770, strict runs with a fresh SPIR-V cache:
    - the racing title (125 s) is in a race;
    - the watercolour platformer (90 s) is in the intro;
    - the fishing title (200 s) is docked in gameplay.
    - All three end with status 1 and no error.
    - The same three runs on AMD (RADV, fixed render-target resolution):
      racing, intro and docked gameplay, each with status 1 and no error.
  - The robot platformer reaches the worldmap level load and stops at the
    pixel shader above, on Intel (160 s) and on AMD (267 s). AMD stopped at
    about 61 s before.
  - Unit suites: 596 shader, fragment, compute and NGG tests, including new
    break-arm, wave32 mask and any-lane cases. Gate script: boundaries
    (strict).

## Reference workload runs d58-d96: stable 266 s, no fatal, still black (2026-10-04)

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

Superseded on 2026-10-14 (see "Robot platformer: exposure fill and DCC metadata clears"): the black screen was the
composite multiplying the scene by a zero exposure, under an accumulated UI layer; the skipped merged ES/GS
programs are particle emitters. Earlier reading, kept for the record: the skipped-draw report names a geometry
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

## Robot platformer: non-indexed transport and 64-bit DS writes (2026-10-24)

- **Evidence for the transition crash.** A gated in-process core capture (a
  temporary build that restores the default fault action so the system keeps
  the fault context; not committed) symbolized the driver frames, and the Kyty
  frame resolved to `GraphicsRenderDrawIndexAuto` at the plain `vkCmdDraw`. The
  kernel journal for the same timestamps contains no Xe reset: this was a host
  driver fault, not a render-engine hang.
- **Fix: non-indexed transport draws.** The capture/shade/resolve recording is
  extracted into one helper. Indexed draws replay the guest index snapshot;
  non-indexed draws replay `draw_count` vertices from the resolved first vertex.
  Both use the same band planning and fixed-function state.
- **Fix: `ds_write2[st64]_b64`.** The tutorial level's pixel program uses
  `ds_write2st64_b64` (opcode 0x4F), which the decoder refused. Both opcodes now
  decode with two-dword data pairs; the offsets stay packed like the b32 form
  and scale by 8 bytes (times 64 for st64). The SPIR-V LDS recompiler stores
  both pairs, the wave-LDS path re-emits the raw opcode, and the module declares
  the offset constants. Two packet tests cover the tuple and the qword scaling.
- **Fix: `ds_read2[st64]_b64`.** The same level also loads two qwords per
  instruction (opcode 0x77). Both read opcodes decode into a four-dword
  destination, reuse the qword-scaled offsets and are re-emitted on the
  wave-LDS path; a packet test checks that the two qwords land in four
  destination registers.
- **Verification.** Strict run, SDL offscreen, scripted route, 900 s cap, Intel
  Arc A770, Mesa ANV 26.2.5: the transition crash is gone and the tutorial
  level loads (5.3 s), which the title never reached before. The DS packet
  tests, `check_emulator_boundaries.py` and `check_graphics_tables.py` pass.
- **Current frontier: new guest-seeded StorageTexture over mixed views.** With
  the DS gaps closed, the level runs its gameplay resource setup. The guest
  reports its particle heap full (`JsParticle heap memory is full!`, 36813 KB of
  36864 KB) and a strict `!create_all_the_same` stop follows in the memory
  create path. Measured facts (temporary diagnostic, not committed):
  - The new object is a StorageTexture, guest-seeded
    (`PARAM_SKIP_SEED = 0`), `neo = 1`, `levels = 0x1_00000006`, resource type 9,
    depth 1, base array 0, one range.
  - Its nine parents are four StorageTextures (IsContainedWithin, Contains,
    Crosses), four RenderTextures (Crosses, Contains twice, IsContainedWithin)
    and one sampled Texture (IsContainedWithin, CPU upload). Every parent
    reports `in_use` and no pending write-back function, but the four
    RenderTexture parents and the sampled Texture have
    `gpu_update_time > cpu_update_time`, and only some parents' submission
    dependencies are complete; one StorageTexture parent was materialized from
    a device image.
  - `multi_storage_texture_alias` refuses this shape correctly: seeding the new
    image from guest bytes would miss the render targets' pending device writes.
    The overwritten-storage and single-format-redescription policies do not
    apply either.
  The intended mechanism is the composite raw-render path (newest complete
  covering writer as baseline, newer GPU writers replayed in write order), but
  its destination shape does not admit this view: it requires a single-level
  64-bit-uint image (`PARAM_LEVELS == 1`, format 65, `size == width * height *
  8`), while this one is a linear R32F mip tail (format 71, `base_level = 1`,
  six levels, 960x540 starting from a 1920x1080 chain, size 5.55 MB). Coverage
  itself is available: parent 80 (RenderTexture, Contains) describes and covers
  the whole range and parent 103 (StorageTexture, Contains) also covers it;
  the other parents describe but do not cover.
  The existing raw copy is specialized (B10G11R11 to R16F, tile 27, one level),
  so admitting this view needs a level-aware linear composite seed path
  (per-level newest-writer selection and ordered replay), not a predicate
  relaxation. The particle-heap warning stays an open guest-side observation;
  the RCS timeout seen in one earlier build was not reproduced and both remain
  recorded.
- **Visual state and rate after the DS fixes.** Native captures at present 1343
  (intro flyby) and 2985 (tutorial loading, ship scene) both score healthy
  (first: entropy 4.03, 739 colour bins, no stripe, white 0.33%, sparkle 1.1%)
  and show the logo, ship and asteroid field without visible corruption.
  Corrected on 2026-10-27: the present-1343 capture shows the flying title
  letters and warp streaks, not the ship; the title ship and its robots are
  absent there because no gameplay save exists yet (see "Robot platformer:
  title ship and robots absent"). The
  same agent sample reports 5.6 fps at present 1343; performance is a separate
  open item. The tutorial still stops at the mixed-view storage alias above.
- **Measured load-window cost.** An agent performance window over present
  1282 to 1571 (103.993 s, 2.78 presents per second while the tutorial loads)
  records per frame: 292 draws, 82 dispatches, 51 submits and command buffers,
  about 1,390 upload calls (6.6 MB) and 52 MB hashed. Attribution from the
  same window's timers: draw state setup 31 ms, resource binding 12.5 ms,
  descriptor finalize/storage/texture 11 ms, dispatch write-back 9.4 ms and
  fence waits 41 ms per frame. The many upload calls are individually cheap
  (0.3 s in total); the sampled frame-time p50 is 38 ms, so the multi-second
  stalls sit outside the recorded draw frame. CPU draw and submission work,
  not GPU execution, sets this rate.
- **Excluded: dropped label publications.** A lost EOP fence value could have
  stalled the guest's particle free list, so the label path was checked first:
  across five 900-second runs the "label destination released before
  publication" warning never fired, and no store was skipped. The particle-heap
  warning is not caused by a dropped fence store.
- **Implementation map for the mixed-view storage seed.** Measured call
  sites: `GraphicsRenderBind.cpp` builds `StorageTextureObject` (including
  `skip_seed` from the binder mask) and calls `GpuMemoryCreateObject`;
  `ShaderComputeStorageSeedSkipMask` (`GraphicsRenderDraw.cpp`) proves a
  write-only, fully covered base extent; `ValidateUploadedStorageSeedCoverage`
  (`GraphicsRenderBind.cpp`) cross-checks the guest coverage buffer against
  `Width5+1 x Height5+1`; `GpuMemoryCreate.cpp` classifies the overlap and
  refuses mixed parents whose device writes are unpublished.
  Two admissible directions, both general:
  1. Publish pending device writes for the aliased range before the guest seed
     (materialize through the existing write-back/materialization path with the
     recording queue as consumer), then link every view. The per-range
     `WriteBackStorageRanges` entry exists but only collects writable storage,
     so render-target publication would need the materialization path and a
     create-time ordering point.
  2. Prove per-level coverage for a linear mipmapped destination so the image
     can be created without a guest seed, mirroring the write-only proof for a
     single base extent.
  Neither may copy between differing formats. The current refusal is correct
  until one of these exists.
  Measured with a probe ahead of the write-only check: the view is bound in a
  compute dispatch (write-without-sampler, dispatch mode 0x41), the
  write-only proof bit is zero, and the guest supplies no tile coverage for
  the binding (`coverage = 0x0`, origin and bounds absent). The proof route is
  therefore unavailable for this shape.
  The materialization cache stores creation recipes, not byte copies, and the
  render-target parents remain device-only, so their content never exists in
  guest bytes to publish. Publishing writes to guest memory is therefore not a
  valid seed source. A linear view over that memory needs a GPU-side raw copy
  with format reinterpretation into the linear destination, which is what the
  composite raw path does for its supported destination shape. Extending that
  path is the correct direction; guest-byte seeding cannot represent this
  alias.
  Next gap behind the alias, measured with the temporary link: the run passes
  the resource create and stops in the fragment stage on a shader emitter
  rejection for `VMaxF32 v8, v8, v8` (format 0x20405, raw 0x201010fa) even
  though the min/max emitter exists, so the operand form is what the emitter
  declines. A probe at the emitter's failure points shows why: the first
  source is a VGPR with `dpp` set while the destination and second source are
  plain VGPRs of the same register. The unsupported part is the DPP control,
  not the min/max operation. The row-DPP tier admits an out-of-row source with
  `bound_ctrl=0` only for zero-identity ALU shapes that keep the destination
  (or/xor/add); a maximum does not preserve the destination there, so the
  refusal is deliberate. Closing it needs either a predicated DPP lowering for
  non-keep-dst operations or the split-wave transport path for this program,
  not a new emitter.
  Excluded: linking the mixed parents with the ordinary guest seed. A
  temporary policy let the create proceed with the guest upload for this shape:
  the level passed the resource create and ran without an engine reset (present
  3046, loading phase), but the captured frame shows bright streaked corruption
  above the ship where the earlier frame had none. Guest bytes do not represent
  the parents' device content; the strict refusal is correct and the seed must
  copy from that content.
  Excluded: skipping the seed for this shape. Temporarily forcing an unseeded
  create for tile-0 format-71 multi-level storage views let the level pass the
  create, then produced a real RCS job timeout (Xe engine reset, device
  coredump, `vkWaitForFences -4`, queue 8 slot 0 sequence 97393). The view is
  read before being fully written, so the seed and its ordering are required.
  Narrowing from the same measurement: one StorageTexture parent (id 103)
  describes and covers the whole range, and another (id 129) covers it as well.
  The seed can therefore come from a storage image of the same family, which is
  a same-format image copy, not a conversion from a render target. The missing
  piece is a copy path whose destination is linear and multi-level: the existing
  raw copy writes destination blocks, and the composite admission still demands
  a single-level 64-bit-uint destination. Extending the destination shape and
  writing rows instead of blocks is the concrete work.
  Layout note for that extension: a linear (tile 0) storage view with levels
  above one is created as one stacked image (`real_height = height + height/2`
  and per-level offsets from the same layout helpers), while the guest range
  measured for this view (5.55 MB for 960x540) is larger than the image bytes,
  so the size alone cannot validate the destination shape. The raw copy must
  derive per-level offsets from the layout, not from the range.

- **The mixed-view storage alias is resolved: the composite raw path admits a
  linear multi-level destination.** The blocker above was a guest-seeded
  StorageTexture over mixed views (four StorageTextures, four RenderTextures
  and one sampled Texture) whose guest range is `0x53aa00000` +
  `0x54b000` (5,550,080 bytes) and whose descriptor is format 71 (RGBA16F),
  960x540, pitch 960, base level 1, six levels, tile 0, resource type 9.
  The composite path already copies the newest covering writer into a
  destination by byte identity; only the destination shape was refused.
  Measured facts that shaped the fix:
  - The range size is exactly the GFX10 linear mip chain of that descriptor:
    levels are stored smallest first, each level's row pitch is aligned to
    256 bytes and the chain totals 5,528,880 bytes, 5,550,080 after the
    256-byte alignment of the whole chain. The planner derives the per-level
    offsets, pitches and sizes from that contract and refuses any descriptor
    whose reported layout is not exactly that chain.
  - The host image is a mip backing, not a stacked atlas: on Gen5 a linear
    (tile 0) view with several levels is one mipmapped image (ten host mips
    for a 960x540 base), so every level is uploaded into its own image mip
    (`dst_level = level`) and the image extent is the base extent. The earlier
    assumption of one stacked image (`real_height = height + height/2`) applies
    only to layouts that keep the single-level atlas, and the copy refused the
    destination because of it.
  - The copy itself is unchanged: the scatter kernel writes the source image's
    texels at their guest addresses into a scratch buffer that holds the
    destination's guest bytes, and the upload then reads that buffer per
    level. Byte identity holds because the guest range is shared by every
    view: the destination sees the writer's bytes at the same addresses.
  Changes: `StorageTexturePlanLinearMipUpload` and
  `StorageTextureCanCompositeLinearMipRawRenderDestination`
  (`Objects/StorageTexture.cpp`), the composite copy takes the destination
  parameters and uploads per level (`TileGpuDetile.cpp`), the create path
  admits the shape (`Objects/GpuMemoryCreate.cpp`). A unit test checks the
  planned regions of the measured descriptor for both the mip backing and the
  stacked atlas.
- **Verification of the alias fix.** Strict run, SDL offscreen, scripted
  route, 900 s cap, Intel Arc A770, Mesa ANV 26.2.5: the run no longer stops
  at the mixed-view create. It reaches the tutorial (`Level has started:
  intro_next`, `sublevel_locator_hub [hub_crashsite_tutorial]`) and keeps
  running past the level start into the hub loading phase, which the title
  never reached before. The focused unit tests (`EmulatorTileDetile.*`,
  `EmulatorGraphicsPackets.PlansLinearMipStorageUploadAtlas`) pass.
- **Open: the run ends with a device lost at the hub load.** The runs that
  clear the alias later fail with `vkWaitForFences failed: result=-4` on
  queue 8 (slot 3, sequence 99372 with the instrumented build; slot 6,
  sequence 95791 with the clean build), and the kernel journal records no Xe
  engine reset for that window. This is the recorded queue-8 device-lost class
  of the graphics handoff ("a real `VK_ERROR_DEVICE_LOST` fence wait on
  queue 8, slot 2" for this title), not the earlier RCS job timeout, and the
  title reached the hub only after the alias stopped blocking it. Whether the
  new copy contributes is not established: two runs agree on the failure, but
  both carry the new path, and the slot and sequence differ, so the evidence
  points at a load-dependent hang in the hub's own work rather than a fixed
  defect in the copy. Next step: capture the hub frames before the hang and
  compare a run with the new path disabled at the same scripted route.
- **Next test to write: the mip-backing destination at the Vulkan level.**
  The composite copy is proven for the single-level destination by
  `EmulatorTileDetile` (a source pair and a destination image, compared
  against the expected tiled bytes), but the new mip-backing destination is
  only proven by the layout planner and by the title no longer stopping. The
  fixture used there creates single-mip images, so the test needs a variant
  that creates a mip chain and reads every level back, and it must compare
  each level's rows against the guest bytes of that level of the GFX10 chain.
- **The row-DPP emitter gap is closed for the destination-source shape.**
  The hub's first gameplay draw (1920x1080, three colour targets, subgroup 64,
  pc 0x568) uses `VMaxF32 v8, v8, v8` whose first source carries a row-class
  DPP (control 0x111, the row bshift, bound_ctrl 0). The emitter refused it:
  the row-DPP tier admitted a bound_ctrl=0 shift that leaves the row only for
  the zero-identity operations that keep the destination (or/xor/add), and a
  maximum does not preserve the destination there.
  Measured with a temporary tier probe: the module is a compute module with
  whole rows and a native wave, so the refusal was exactly the
  keep-destination rule and nothing else.
  The tier now classifies the operation instead of admitting a fixed set. When
  the permuted source and the other source are the destination register and
  the operation is idempotent (min, max, or, xor, and), a lane that reads
  outside the row keeps the destination under both readings of the hardware
  contract, so the operation may run with the lane's own source value
  substituted for the missing one. The lowering selects that value instead of
  zero for this shape (`RowDppMode::Input`); the proven zero substitution of
  the keep-destination shapes is unchanged.
  Verified: the program translates and the run reaches the hub load; the full
  unit suite passes (2,484 tests, 2,473 passed, 11 skipped, 0 failed). The
  draw is still refused afterwards by the transport participation proof
  ("fragment row DPP source participation is unproven (neutral values do not
  prove helper recovery)"), which is the next gate.
- **The single-level composite destination is generalized.** The hub's own
  resources then needed a linear single-level view (format 71, 960x540, pitch
  960, tile 0, one level) over a range of `0x3f4800` bytes at
  `0x53af4b700`, with nine mixed parents. The composite admission refused it
  for three narrow reasons: it named one format id (65), it demanded the
  range be exactly the image bytes, and it demanded a 64 KB aligned start.
  The admission now names the contract instead: a linear single-level view
  whose pitch is its width, whose texel size the copy supports, whose swizzle
  is the identity, and whose range covers the image. The range may start
  inside a block (the copy addresses the guest bytes, not blocks) and may
  extend past the image. The copy checks the image against the view's extent
  and texel size instead of one format.
- **Visual evidence after the alias and DPP fixes.** A native capture of the
  world map scene (present 1742, 1280x720, A2R10G10B10) scores healthy:
  entropy 4.41, 412 colour bins, no stripes, no blowout, sparkle 0.35%. The
  frame shows the planet, the asteroid field and the nebula without the
  streaked corruption the excluded guest-seed experiment produced, so the
  copy path that seeds the mixed-view storage view does not corrupt the
  image.
- **Measured rate with the merged upstream range-cache work.** An agent
  performance snapshot over 127 s of the run reports 1,741 presents, a
  frame time of p50 41 ms, p95 116 ms and p99 190 ms, with 41.1 s in fence
  waits, 15.8 s in uploads, 3.3 s in hashing, 3.2 s in write-backs and
  0.9 s in detiles; the pipeline lookups cost 0.27 s over 203,605 hits. The
  rate is still set by host work per frame, not by the device.
- **The packed mixed FMA of the gfx10 family is decoded.** The hub's pixel
  program also reaches a VOP3P opcode the parser refused: `0x21`. The gfx10
  opcode table (the decoding chart LLVM publishes from the ISA) names `0x20`
  as `V_FMA_MIX_F32` (the mixed scalar FMA the engine already emits) and
  `0x21` as `V_FMA_MIXLO_F16`, the same mixed FMA with a half destination
  written to the low half of the destination register. The parser now decodes
  the opcode into its own instruction type and the emitter lowers it with the
  engine's half contract: the selected sources at their selected precision,
  binary32 intermediates with the addition residue recovered, one rounding to
  half, and the low half of the destination written while the high half is
  kept. The translation is not yet observed in a run.
- **The AGC commands the game imports are being filled in.** The game
  reserves the space of a command with the writer's GetSize before it calls
  the writer, and eighteen writers of the draw command buffer had no such
  export, so the guest reserved the wrong amount and the emulator's packet
  did not fit in it. The rewind and conditional execution commands of both
  command buffers were missing as well, so the guest could not run them at
  all. Each implementation follows the AGC module's own body: the packet
  words it writes, and the size its size function reports, are the evidence
  kept in `firmware/work/evidence/`.
  The inventory of missing exports is now measured against the tree itself
  (`firmware/abi/astro-imports-missing.csv`): 124 exports are really
  missing, 58 of them AGC commands, 25 libc, 21 AGC driver, 13 JSON and 3
  video out. The earlier count of 245 included ninety exports the tree does
  register under a second registration style.
- **The gfx10 lane permute and the mixed FMA variants are translated.** The
  hub's programs reached two instructions the emulator did not know:
  `ds_bpermute_b32` (opcode 0xb3) and `V_FMA_MIXHI_F16` (VOP3P 0x22). The
  official gfx10 ISA (the RDNA 1.0 instruction set architecture, sections
  10.4.4 and 12.10) gives both contracts, kept in
  `firmware/work/evidence/`:
  - `ds_bpermute_b32` is a gather: every lane takes the value of the lane its
    index names, the index is a byte address with the instruction offset added
    to it, only its bits [6:2] select a lane of the same half wave, and a
    disabled source lane reads zero. The host subgroup that holds the
    destination lane also holds every lane its half-wave index can name, so a
    native shuffle reaches the source; split compute with 16-lane parts puts
    one half in two host subgroups, and the full guest lane then goes through
    the wave exchange (`firmware/work/evidence/ds-bpermute-spirv-contract.txt`,
    `rdnai1-ds-bpermute-shade-compute-spirv`).
  - The first emitter named no execution scope: the hub's shade compute
    rejected its module at assembly (`Expected operand for
    OpGroupNonUniformShuffle`), and validation then rejected the destination
    merge because the float VGPR variable was loaded as `uint`. Translator
    160 names the subgroup scope, declares the shuffle capability, bitcasts
    the destination, and reads split compute lanes through `%split_read`.
    Unit tests
    `EmulatorShaderArithmetic.DsBpermuteShuffleNamesTheSubgroupScope` and
    `EmulatorShaderArithmetic.DsBpermuteSplitWaveReadsGuestLanesThroughTheWaveExchange`
    assemble and validate both strategies. The first translator-160 run does
    not reach the hub; it stops at the stall recorded below.
  - `V_FMA_MIXLO_F16` and `V_FMA_MIXHI_F16` are the mixed FMA of the
    selected sources with the result in the low or the high half of the
    destination. The emulator now decodes both and merges the half the opcode
    names.
  With those translated the run reaches the hub's own resources and then
  fails with the device lost described below, so the hub's first gameplay
  draw now executes.
- **A suspended submitter must not pass the command processor (fixed,
  2026-10-26).** The merged tree stopped at `frame 1`/`present 0`. The guest
  main thread waited on a guest mutex whose owner polled the title's
  `Gpu::GpuDevice::EopEq` event queue (`KernelEqueuePrivate::WaitForEvents`,
  eight events, 1 µs), and all three command processors were parked in
  `WaitForSuspendedRuns`. The `KYTY_WAIT_TIMEOUT_MS` deadline named the waits:
  8-byte `WAIT_REG_MEM` on `0x40b5faa40`, `0x40b5fc140` and `0x40b5fc1c0`,
  value 0, reference 1, mask `0xffffffff`, function 3 (equal), queues 6/7/8,
  with no pending GPU producer. The wait trace shows `0x40b5fc140` is a CPU
  store: 77 suspensions and no end-of-pipe label for it in the same window.
  The early release of the suspended batch's decode completion let its
  submitter take the lock ahead of the thread that writes that label, so the
  label never arrived, the end-of-pipe event could not fire and the ring
  stayed parked. Only queued submitters return now; the suspended batch waits
  until decode consumes its stream. Verified on the strict route: reverting the
  early release for every batch gives 586 presents at 45 s, one ~90 s stall
  that recovered, and 2157 presents at 360 s; the narrowed fix gives 3832
  presents at 360 s with no stall (HLE-only control `7dd05019`: 3374 presents
  at 420 s). Reverting the split-wave16 commit `2a198ab0` or the APR file-id
  commit `bc2a646d` alone did not change the stop, and a debug-level run
  (slower host timing) reached `ps_logo` before a later deadline, which
  confirms the stop is a race on submit/decode timing rather than a lost
  device. Demon's Souls, which the early release was added for, still returns
  the queued submitters; the suspended batch's own release is not exercised by
  that title's recorded deadlock (the value came from a thread blocked
  submitting the next batch) and remains unverified on that title here.
- **The merged tree stops presenting before the hub (resolved, 2026-10-26).** A
  bounded translator-160 run on the tree that merges the HLE runtime and the
  split-wave16 branches reaches three presented frames (`frame 4`,
  `present 3`, then `phase=stalled` with no present for 54 s) and writes no
  shader log past its first pixel programs. The kernel journal records no
  RCS reset for that process and the log has no `vkWaitForFences` failure.
  The last recorded error event before the stall is `gfx_storage_frontier`
  with `src=metadata r=metadata_only`. No shader that contains
  `ds_bpermute_b32` executes before the stall, so the lane-permute fix is not
  involved. The next run captured the process thread stacks (record below),
  which exclude host shader compilation as the block; the A/B of the HLE-only
  merge against the split-wave16 merge is recorded below and led to the
  submit/decode race fixed in the newest record.
- **The boot stop is a guest lock wait; no shader compiles during it
  (2026-10-26).** A bounded strict run of the same merged build (translator
  160, 1280x720, offscreen) reproduces `frame 1`, `present 0` and
  `phase=booting` for more than 100 s. A gdb all-stop of the emulator process
  at 180 s, taken by suspending the traced child with `SIGSTOP` and dumping
  every thread, shows all 91 threads parked: the window thread in
  `Kyty::Libs::VideoOut::FlipQueue::Flip` waiting for a present, the graphics
  ring batch thread in `WaitForSuspendedRuns`, the label manager, both
  async-job threads and the SPIR-V cache worker on their own condition
  variables, and the only Vulkan frames are idle `pthread_cond_wait`
  positions of the disk-pool threads inside
  `libvulkan_intel.so`/`libvulkan_radeon.so`. No thread is translating or
  compiling a shader, so the compile-storm explanation is excluded.
  Every guest thread waits on a kernel object: the guest main thread
  (entry 0) is in `PthreadMutexLock` on the guest mutex at `0x37d453360`,
  eight job threads wait on guest semaphore 6 from RVA `0x1081a9f`, one
  thread waits on semaphore 4 and the rest wait on engine condition
  variables. The blocking site is RVA `0x2a4ac2` of the guest image (a call
  through the pthread mutex wrapper at `0x640`, return address `0x2a4ac7`)
  inside a frame-timing function reached from `0x2a37c8` and `0x29fe11`. The
  guest stops before submitting the next frame because a contended guest
  lock is never released; this capture does not yet say whether the missing
  release depends on a GPU completion or on the guest's own job system.
  Next: read `PthreadMutexPrivate::owner` of the stalled mutex (the guest
  address is stable across runs) to name the holder, and A/B the HLE-only
  merge at `7dd05019` against this tree.
- **The HLE-only merge presents; the merged tree alone stops (A/B,
  2026-10-26).** The same strict route on the HLE-only tree (`7dd05019`,
  built from its own worktree) reaches `phase=interactive` and presents
  continuously for the whole 420 s window: frame 3375 and present 3374 at
  the last sample, with one `loading` sample in between. The merged tree
  stops at `frame 1`/`present 0` (record above). The 16-lane work itself is
  not the prime suspect: a 2026-10-24 run on this host presented frames 1343
  and 2985 of the crash-site scene with the 16-lane parts. Since the HLE-only
  tree predates the split-wave16 branch, the block is in the merge of the two
  trees, i.e. in the 25 commits of the second parent's branch (its tip is the
  split-wave16 shader commit `2a198ab0`; the kernel, libc and fault-delivery
  work is inside that range).
  Next: the mutex owner was read and the commit A/B ran; reverts of
  `2a198ab0` and `bc2a646d` did not change the stop, and the decode-release
  path in `6aa58ed2` did. The narrowed fix and its run are the newest record.
- **The narrowing SDWA is translated.** The hub's pixel program also uses
  `vcvt_f16_f32` with a sub-dword addressing modifier, which the half
  arithmetic refused. The ISA's SDWA fields give the contract: the source is
  read as a whole word and the result is written to one half of the
  destination, and `DST_U` says whether the other half is zeroed or kept
  (`firmware/work/evidence/sdwa-isa.txt`). The half conversion now accepts
  that form and merges the result into the half the fields name.
- **The hub's pixel program now translates end to end.** With the lane
  permute, the mixed FMA variants and the narrowing SDWA translated, the hub's
  first gameplay draw compiles and the run reaches the level's own resources
  (`hub_crashsite_tutorial`) and its sublevel locator. What remains at that
  draw is not a translation gap: the run dies with the queue-8 device lost,
  and the kernel journal records an RCS job timeout with an Xe device
  coredump (`Tile0: GT0: Engine reset: engine_class=rcs`, `Timedout job:
  seqno=136669`). The same class was recorded at 01:19, before the permute
  and the mixed FMA existed, so the hang is not a product of this session's
  shader work. The coredump of that job is the evidence for the next step:
  the hub's draw is the first one that runs the fragment transport's compute
  shade pass at scale, and the hang is in the compute engine.
- **The instruction dump accepts the modifiers of the mixed-precision
  operations.** The dump refused a negate or absolute modifier on an inline
  constant, which those operations carry; it now shows the modifier with the
  value.
- **The half sub-dword form is translated (fixed 2026-10-25).** The hub's
  half minimum carries the SDWA control `0x05051403` (ISA fields in
  `firmware/work/evidence/sdwa-f16-isa.txt`): WORD_1 of both sources,
  WORD_0 of the destination, DST_U=2 (preserve the other half), no output
  modifier and no clamp. The operand loader already applies the source word
  selects; the half emitter now admits that form and leaves the destination
  merge to the generator's per-lane SDWA pass, which implements DST_SEL and
  DST_U under the EXEC guard. A first attempt merged the destination in the
  half store as well and the per-lane pass merged it a second time, which
  corrupted the result (for WORD_1 it moved the new low half into the high
  half). Translator 153; unit test
  `EmulatorShaderArithmetic.Fp16SdwaHalfDestinationReadsAndPreservesHalves`
  covers both source word selects, preserved/zeroed/high-half destinations
  and the inactive lane. The 335 shader and packet tests pass. The hub run
  with this translator is pending.
  Translator 154 fixes the compare's mask destination: the hub's
  `v_cmp_neq_f16` at pc 0x998 names `vcc_hi` alone, and the half compare
  emitter wrote two mask words, so it read the second word of a one-word
  destination. Every VCC-writing compare emitter now stores only the words
  the destination names; the F32 and integer emitters already did. The
  not-equal dispatch entry itself existed since the earlier gfx10 gap fix,
  so the open item was the destination shape, not the entry. Evidence:
  `firmware/work/evidence/cmp-mask-destination-isa.txt`
  (`rdnai1-isa-vopc-sdwa-mask-destination`) and the strict-run logs that
  reported the missing emitter and then the out-of-range word. Unit test
  `EmulatorShaderArithmetic.HalfCompareWritesOnlyTheMaskWordsTheDestinationNames`
  covers the one-word and the pair destinations. The hub run with
  translator 154 is pending.
- **The device lost is intermittent: two of three runs.** A third run with
  the native capture directory enabled (same scripted route, same build)
  reaches the hub and keeps running until the 900 s cap with no fence
  failure, while the two earlier runs die with the queue-8 device lost. The
  failure is therefore load dependent, not a fixed defect of the new copy.
- **Capture evidence for the hub is still open.** That third run writes no
  capture before the cap: the capture milestones had not fired at that point,
  so the visual state of the tutorial scene after the alias fix is not
  scored. Next step: keep the capture-enabled route running until the
  milestones fire and score the hub frames with `scripts/kyty_capture.py`.
- **Fixed: the guest seed of a linear mip chain read the level rows from the
  wrong offsets.** The guest seed of a linear multi-level storage view
  uploaded every level with a halved pitch (`Objects/StorageTexture.cpp`, the
  `mip_pitch` loop in `update_func`), while the GFX10 chain stores the level
  rows with the 256-byte aligned pitch. Levels whose width is not a multiple
  of the alignment (120, 60, 30 texels for the measured descriptor) were read
  from the wrong offsets. The seed now takes the level row pitch from the
  layout itself (the padded sizes of `TileGetTextureSize2`), which is the
  single source of truth for the chain, and the unit test locks the aligned
  pitches of the measured descriptor (960, 480, 256, 128, 64, 32). Verified
  by the full unit suite (2,482 tests, 2,471 passed, 11 skipped, 0 failed)
  and the boundary gate; the title-level effect of the corrected seed is not
  yet observed.

## PPSA21567 - Astro Bot: space scene mosaic and bands (2026-10-31)

Observed on Intel Arc A770 (ANV Mesa 26.2.5), intro space scene. The sky was
replaced by dark horizontal bands, objects had 16 px block halos, and some cuts
left dark block-edged silhouettes (not re-checked, see Open).

**Cause.** The depth of field chain writes colour plus five mips over a
960x540 surface of a recycled range with one storage view per level, then
samples levels 0 to 5. In the same range and frame a one-level 960x540 colour
target receives half-resolution particle draws. The view of level 0 resolved
to that colour target (inexact range, one level is enough), levels 1 to 5 and
the sampled chain resolved to the storage surface, so the blur read a level 0
nothing had written: bands, alpha 10000. An in-run readback of every bound
image showed the correct downsample in the colour target and the bands in level
0 of the storage surface.

**Fix.** A write view of a chain binds the storage surface of the exact range
when a colour alias holds fewer levels. Other inexact views read the colour
alias only while it holds the newer writes; colour binds now stamp their image
so that comparison sees draws. Comparing stamps alone did not fix it: the
particle target is legitimately newer when the chain is written.

**Verified.** Binary `0ad5559b4` plus the fix and inactive temporary diagnostics,
with the diagnostic `CB_SCALE=2` setting, so not a strict run. 17 captures through
frames 3922 to 4551 show correct sky, stars, nebula and soft focus, with no bands
or block halos. Strict games matrix: 26 interactive, unchanged;
`case_429dd9d08d87` went from controlled timeout to presenting, and
`case_2daaacae7295` is still a host crash. The matrix does not inspect images, and
`playable-regression` was not run, so this is not a no-regression claim for
titles with compute-written mip chains.

**Excluded, with the observation.**

- Depth clear: the HTILE clear is consumed at the first bind of each frame and
  clears the whole 1920x1080 image to 1.0.
- HiZ: `INTEL_DEBUG=nohiz` keeps both defects.
- "Halos have depth below 1": invalid inference. The sky draw overwrites the
  probe colour and post-processing spreads it.
- Shader `73c39693`: skipping it keeps the defect; it matters only for cost.
- Depth of field composite as the faulty stage: skipping `bf8ee9fc` gives a
  clean frame only because it stops reading the bad blur.

**Earlier fixes in this scene.**

- A storage image at the base of a render target, over a smaller sampled texture
  and a smaller buffer, stopped on mixed parent types; the multi-parent rule
  now links those relations. A 600 s run then had no fatal.
- EXEC stays constant after a full mask write (translator 190): composite
  `bf8ee9fc` 16.9 to 13.1 ms.

**Open.**

- Dark block-edged silhouettes after later cuts (frames 4656 and up): not re-checked
  after the fix.
- Intermittent GPU hang at hub load, still inside the geometry compute module
  of vertex shader `0x360038d0f4cfb9a5`: 3 of 8 runs in this stretch, with pixel
  shaders `0x21001960c4a8f3bb` and `0x210016009aec9d6c`. The first pair is also
  the half-resolution particle draw above (point list, one vertex per draw in
  the dump), which narrows the workload. Cause unknown; the next evidence is a
  devcoredump. Excluded with translator 194, GPU reset at the same dispatch each time:
  - driver state re-emission (`INTEL_DEBUG=reemit`): 1 of 2 runs still reset;
  - the multi-group walker: one single-group `vkCmdDispatchBase` per group passed 3 runs and reset in the 4th, on a
    pass of one group;
  - half conversion is not required: 1 of 5 runs reset on the integer path (3 of 5 on the native path, not
    significant, and the program has no half conversion).
  The program declares up to 64 output vertices per point, so one point takes a whole 64-lane group; running these
  groups in one host subgroup would not fit.
  Also tried in this stretch, none of which removes the reset:
  - elected-lane-only spin in the exchange: same us/n, 2 of 2 runs reset;
  - idle submit interval 0.5 to 4 ms: submits fall only 25190 to 21329 per window, run reset;
  - 8 lanes instead of 16: reset at the same point (gw380);
  - spin limit 1024 instead of 2^20 on the 8-lane module passed 2 of 2 (gw381, gw382), which does not overturn the
    earlier exclusion (limits 4096 and 64 still stop, gw198, gw199): the base passes about half of its runs.
  True 32 lanes passed 2 of 2 but costs 396 us per dispatch against 222 at 16 lanes, so it is not a fix either.
- Geometry pass width. The pass was already 16 lanes on this host, not 32. vkstat cycles predict its real cost:
  32 lanes 3.52M cycles and 396 us/dispatch, 16 lanes 1.77M and 222 us; 8 lanes 1.29M with 1436 fills against 5187.
  The pass now follows the compute rule, 8 lanes above 64 VGPRs: first 60k dispatches 140 to 120 us each (gw380, gw381,
  gw382), whole window 45.0 to about 42 s, hub 350 to about 240 us each, GPU per dump 4.2 to 3.5 s. Hub frame rate
  moves little (7.4-7.7 to 7.9 fps), so the hub is not limited by the GPU alone.
- The cost of a geometry dispatch does not follow its group count: 1 group 222 us, 4 groups 148, 63 groups 146,
  512 groups 292 (gw369). The first 60k dispatches are a repeatable A/B window (139-146 us each over 8 runs).
- The command buffer split between a dispatch and a draw is not a frame-rate lever: dropping it under
  `INTEL_DEBUG=reemit` lowers the cost after the last mark from 111 to 48 us per buffer, but a gap of the same size
  appears between steps; first 60k geometry dispatches 45.1 s against 43.6 s, same frame rate (gw383 against gw381).
- Dynamic resolution is not a lever here: main passes are already 1920x1080 at the lowest tier (40-70 per frame),
  960x540 up to 37 per frame, 2432x1368 two, 3840x2160 one, with a 720p reported window (gw368).
- Offline half-conversion lead for `bf8ee9fc` (cycles, vkstat): base 802k, native
  half 686k, half plus EXEC fold 441k. The native half conversion landed, see "Native half conversion" below;
  the EXEC fold on this program is open.
- Crowd scene about 13 fps: far from the 60 fps goal.

## Whole-wave masks in hot pixel programs (2026-10-31)

Title: PPSA21567 - Astro Bot. Host: Intel Arc A770, ANV 26.2.5, xe, kernel 7.2.11.

Observed: the hot pixel programs rebuilt a whole-wave guest mask at every compare and EXEC write, two
subgroup reductions and seven or more quad broadcasts each. The collectives block NIR folding and SIMD16.

Changed (translator 193):

- An EXEC write the mask flow proves full no longer refreshes EXEC.
- A mask result read only as its owner's lane bit holds that bit alone in pixel programs, as split compute did.
- A LaneLocal or QuadLocal program gathers one quad's lanes and drops the wave reduction. The proof already
  accepts a lane that skips, and it refuses read-first-lane, lane exchanges, MBCNT and DS append or consume.

Measured with GPU timestamps, warm store, same frames and draw counts, 60 fps limit, diagnostic CB scale 2:

| Pixel program | Before | After |
| --- | --- | --- |
| `8ca0b9ff` (54 k draws) | 5662 ms | 3792 ms |
| `d4bef057` (78 k draws) | 5350 ms | 3155 ms |
| `550e721d` (31 k draws) | 6164 ms | 6595 ms |
| Compute `3666fa39`, `874bd13f`, `bf8ee9fc` | 21130 ms | 21262 ms |
| All GPU work in the 300 s run | 212.6 s | 185.6 s |

Compiler statistics: `550e721d` and `d4bef057` now offer SIMD16; `8ca0b9ff` still compiles SIMD8 only with 74
live registers, cycles 41820 to 25395. The captures of the space scene and its cutscene show no new artifacts.

Not claimed: a frame rate. Gameplay was not reached in these runs, and the CP remains the larger limit in
crowds. Entry-mask copies still refresh EXEC, because the guest flags them not full while the host launches
every invocation; that fold and half conversion remain open.

## Native half conversion (2026-10-31)

Observed: the f32 to f16 narrowing and the f16 to f32 widening ran as software bit manipulation in every packed half
program. On the hottest hub compute programs that cost a large share of the cycles offline.

Changed (translator 194): when the device reports `shaderFloat16`, `shaderInt16`, RTE rounding on 16 and 32 bits,
denorm and signed-zero/inf/NaN preservation on 16 bits and independent denorm behavior, programs that do half
arithmetic use `OpFConvert`. A NaN keeps its sign and top payload bits and is quieted, as the integer form does,
because a conversion only guarantees a NaN. Half multiply-add keeps exact rounding: a TwoSum residue says on which
side of the binary32 value the exact result lies, and rounding to odd carries that through the second rounding. The
choice is part of the shader key, so stores of both forms coexist. Other devices stay on the integer path.

Verified on Intel Arc A770 (ANV) with a compute harness running the emitted SPIR-V against exact results:

| Case | Inputs | Mismatches |
| --- | --- | --- |
| f32 to f16 | all 2^32 | 0 |
| f16 to f32, high bits set | all f16 | 0 |
| Half FMA, residue on | 268 M triples | 0 |
| Half FMA, residue off (negative control) | 16 M triples | 316 |

In game, with GPU timestamps and a warm store: compute `bf8ee9fc` 13.14 to 8.84 ms per frame (offline estimate -31 %),
`874bd13f` 5902 to 5099 us per dispatch. A capture of the space scene is clean. Interpretation limit: the runs reached
different scene mixes, so no frame rate is claimed. The frame stays at 85-130 ms in that scene.

Open: other GPUs untested; native half arithmetic (`OpFAdd` on `%half`) instead of widen, operate, narrow.

## EXEC fold outside pixel programs, GPU gaps (2026-10-31)

Title: PPSA21567 - Astro Bot. Host: Intel Arc A770, ANV 26.2.5, xe, kernel 7.2.11.

- The EXEC fold is not a lever for compute and vertex programs. A linear scan of the 113 hub modules that keep a lane
  variable, tracking which SGPRs hold the initial full mask: about 34000 compute selects sit under a mask that is not
  provably full, under 100 under a provable one, outside one module (1761 selects). In the largest modules the writes
  are `ballot(exec & compare)` and `exec & ~saved`, real divergent regions of up to 1049 selects. A write of all ones
  already folds in every stage. Limit: the scan ignores joins, so it estimates and is not a proof.
- Time between GPU steps is not a lever in a normal run. With marks around the pass begin (gw384, 81 s of timestamps):
  compute gaps 2.7 s, everything before a pass begins 1.3 s, barriers up to the begin 0.09 s, and the 4.8 s of gaps
  before non-indexed draws seen in gw383 is absent: it came with `INTEL_DEBUG=reemit`. The largest single pair is 393 us
  per frame step after a non-indexed draw before compute `1609d289`, about 1 % of the GPU.
- The 17 % after the last mark of each buffer (107 us each, 127k buffers) is mostly the completion of the draws before
  it, as the dispatch and draw split result above shows.
- Native half conversion reaches 2 of the last 150 modules in the store, so native half arithmetic is not a hub-wide
  lever either; it stays open for those programs.

## Title screen stop on mixed storage parents (2026-10-31)

`PPSA21567 - Astro Bot`, intermittent, one strict run in about a dozen on the
title screen: `!create_all_the_same` for a new 240x135 mode-27 storage view inside
11 parents of a recycled range. Ten were already linkable. The stop was a sampled
mode-24 chain whose last update was a device write, so it was neither a CPU-backed
texture nor a storage view. A texture in another format or tile mode now links as a
redescribed range. Verified only by the rule and by runs that did not stop; the
stop was not reproduced on demand, so its absence in about eight later runs is
weak evidence.

**Excluded: deferring texture uploads caused by `DMA_DATA` to their next use.** A
profile showed `UtilFillImage` under `DMA_DATA` at 5 % of the main ring thread, but
the flushes are bursts: 0 in most 5 s windows, about 150 in one. Deferring them on
one run gave 19.9 fps against a same-code spread of 18.0-20.5.
That spread came from the stall recorded under "Array uploads under guest stores", so a window mean could not judge
this trial. Deferral has not been retested since; the fix there batches the uploads instead, and
"Freezes from the pipeline cache and array uploads (2026-11-15)" re-uploads only the layers a store touched.

Main ring thread after these changes, 150 samples: 54 % busy and flat, 22 % fence
waits, 16 % register wait polls. No guest thread is busy. `DRAW_INDEX_INDIRECT`
argument reads are 5 %; removing them needs device draws, parked because indexed
draw recording consumes the index count on the CPU.

## Game patch trial and geometry cost (2026-11-01)

`PPSA21567 - Astro Bot` 01.018.000, A770, Mesa 26.2.5, `KYTY_PRESENT_FPS_LIMIT=60`,
private shader store, same route, same 14 s window at the hub level.
[Game patches](../game-patches.md) were user files; none of these are strict runs.

| Mods enabled | fps | Note |
| --- | --- | --- |
| None | 18.5 | Two earlier runs of this route reached the space scene by the window: 13.1, 13.6 |
| Deferred non-RT renderer, GI probes off | 20.6, 20.9 | |
| Same, fixed 1080p | 20.4 | |
| Same, plus frame time unlink, CPU limiter wait off, 144 fps limit | 18.3 | Reached the same hub state |
| Those plus the physics, Havok, video and intro mods | none | GPU job timeout and engine reset on the title level, one run |

The renderer mods remove most of the frame's `cs-q8` compute (12.5 s to 5.3 s of
GPU time in 120 s) and add `rp-pre` (about 1 s to 6 s). Geometry `f4cfb9a5` stays
at 120 us per dispatch. The fixed resolution mod did not move the rate. Frame
pacing mods remove no cost here: nothing suggests a guest limiter holds the frame.
Unmodified cost stays the limit, so these mods do not reach 60 fps on this host.
Not separated: which mod in the last row caused the reset. Not checked: whether
the non-RT image is correct; a capture showed the ship as a dark silhouette in
space, with no reference to compare it to.

The non-RT renderer stopped at `!create_all_the_same`: a 1216x684 storage view
over a transient arena with parents of every kind. Such a view now links as the
last case before that stop, so views handled earlier take the same paths as before.

Excluded: start-up cost as the source of the 120 us geometry dispatch. Issuing
vertex shader `f4cfb9a5`'s dispatch twice in a row, with a barrier, costs 122.4 and
114.5 us, so the cost is shader work and not a first-dispatch penalty.

## Label waits over aliased storage in the hub (2026-11-01)

Title: PPSA21567 - Astro Bot. Host: Intel Arc A770, ANV 26.2.5, 14 s windows of the gameplay route.

Fence stacks over a 110 s run: 14.7 s under 32-bit `WAIT_REG_MEM` on labels whose release also writes back
(2670 waits with a pending alias), 7.1 s reading `DRAW_INDEX_INDIRECT` arguments (3707 reads), 2.9 s in
`DMA_DATA` image fills.

- **Who blocks.** In 18400 label waits, 9222 had no pending alias and 3641 only aliases that never read guest
  bytes. The rest are depth buffers crossing the storage (about 1950 alone), storage buffers containing it
  (875, always with a depth buffer) and one 960x540 texture (199).
- **Still excluded: consolidating them all.** Doing so stopped early at `!create_all_the_same`: a 1216-wide
  storage view crossing a depth buffer and two storage views still linked because their submissions had not
  retired. Before that stop, still at boot and not in the hub, 1143 of 2285 consolidated waits met an alias lookup. If
  the hub is similar, a wait at the consumer would still run in about half of them, which bounds the gain near
  3 ms per frame. Not built further.
- **Geometry groups are almost all empty (2026-11-01).** The pass sizes each point's group from the declared
  `VGT_GS_MAX_VERT_OUT` of 64, so it runs 64 lanes as 8 host parts with rendezvous. A probe copied the header, the first
  two primitive words and the first record of every group of the first 40000 dispatches, boot to hub: 855,713 of about
  856,200 groups of `fc3d1580`, `f4cfb9a5` and `0e89d742` allocate 1 vertex and 1 primitive (`gs_alloc_req` payload
  `0x1001`) and export primitive word 0, the degenerate triangle 0,0,0. The rest, only in `f4cfb9a5`, are real, up to 64
  vertices and 32 primitives, so the probe does report live output.
- **Excluded: skipping the pull draw of an empty pass.** The pass raised the instance count of an indirect draw only
  when a lane exported a primitive that can rasterize, so an empty pass drew zero instances instead of 192 vertices per
  group. Paired by draw count with `f4cfb9a5`, `c4a8f3bb` stayed at 66-69 us per draw, and fps did not move beyond the
  spread (below). The vertex work was not the cost; the change was not kept. The pairing is inferred, not traced.
- **Excluded: cache invalidation after the pass.** With that draw, leaving the shader read out of the barrier's
  destination access gave 70 us per draw for `c4a8f3bb`. A draw that follows no geometry pass costs 24-32 us
  (`55ce09b9`, `adbba919`); what fills the difference is not known.
- **Open lead, parts with no live lane.** They could leave early, but the guest decides the allocation after its own
  test, so the work before that point stays. At best about 4 ms of the 34 ms per frame, and it changes the program where
  the hub GPU hang stops. Not built.
- **Resolved: the "two submit modes" are one stall inside the measured window.** Submits from the command processor
  stay at 31-40 per frame in 24 of the 26 kept 14 s windows (25 with the idle-GPU rule off, 79 in the profile training
  build); what varies is the utility queue, 1 to 94 submits per frame.
  It rises with how much of a stall at one route point falls in the window, which starts at a fixed wall-clock time
  while load time moves. The earlier rule to quote submits per frame is replaced by the fix and the measure below. The
  run with the idle-GPU rule off keeps its record: its 26 submits per frame at 17.2 fps had no utility submits, so
  it was slow for another reason, not because of this stall.
- **Pixel program `55db270b` (7 % of GPU time, about 900 us per draw).** Pipeline statistics over 992 draws: about 1,530
  primitives and 837,000 fragments per draw, so 1.1 ns per fragment and the cost is shading, not vertices. With vertex
  program `7abe409a` the driver compiles it at SIMD8 with no spills: 6,280 instructions, 129 SENDs, 19 loops, 71,485
  estimated cycles. Nothing here points at a translation defect, and no change was tried. The pairing with `4e1ef13e`
  printed no driver statistics.
- **Fixed:** the same stop also came up in a clean strict build, without this experiment, in one of six runs of
  the route to the hub: the 1216-wide view crossing a depth buffer in the transient arena. A depth buffer crossing
  such a view now links like the other crossing surfaces.

## Array uploads under guest stores (2026-11-01)

`PPSA21567 - Astro Bot`, A770, Mesa 26.2.5, same route, one run per arm on one diagnostic build, compared by 5 s
timestamp dumps rather than by a window mean.

- **Observed.** At one route point, in every kept run, buffers per 5 s rise from about 3,600 to 15,000 and frames per
  5 s fall from 107 to 49 (gp-ga3). The extra buffers hold no draws. They are utility-queue submits, each followed by
  a fence wait on the ring thread: 13,825 waits and 2.1 s in that one dump.
- **Cause.** One `DMA_DATA` store of 4, 16 or 64 KiB into a 17.3 MB texture array re-detiles and uploads the whole
  array, with one submit and one fence for each of its 192 layers. Per-object counts: 149 such flushes caused 28,608
  fills. Fence stacks symbolized against the matching binary point at the layer loop. An array this size touches one
  object per flush, so overlapping objects are not the cause.
- **Fix.** Layers are uploaded in batches of up to 32 MiB per submit. Larger arrays still stream, as before.

  | Burst, per 5 s dump | Before | After |
  | --- | --- | --- |
  | Buffers (worst dump) | 10,625 | 3,612 |
  | Frames (worst dump) | 71 | 87 |
  | Utility fence waits (burst dumps) | 13,835 | 216 |
  | Utility fence wait time (burst dumps) | 1.94 s | 0.34 s |

  Frames over the last nine dumps go from 923 to 959. Frames in the burst still dip to 87-104 against about 110, and
  the array was still re-detiled in full for each store until 2026-11-15 (see "Freezes from the pipeline cache and array
  uploads"). Validation over a 110 s run reports no copy error; it did report 46
  `vkCmdDispatch` messages, so the layer was active, and whether those predate this change was not checked. A capture after the burst shows no corruption. `PPSA09809 - OCTOPATH TRAVELER II`
  reached only its loading screen in an 85 s run, so its cube arrays were not checked.
- **The 16 % tail is a different thing.** It stays at 15-18 % of GPU time and about 5 ms per frame while submits per
  frame vary from 26 to 86, and it did not change with this fix (15.8 % and 15.0 %). That fits the earlier reading,
  completion of the draws before it, and no lever was found in it. The windows of those two counts do not line up
  exactly, so this is a comparison of ranges.
- **Hub GPU hang: not reproduced since 2026-10-31.** Runs that reached the hub ended in a device loss in 31 of 84 cases
  up to 11:40 on 2026-10-31, mostly 121-185 s into the run, and in 0 of 57 after it. Only 8 of those 57 are known to
  have lasted 180 s or more; the others ended earlier or have no usable start time. Four of the 8 are new 270 s runs on
  `197093a4e` plus diagnostics, all past the hub load. If the old rate still applied, 8 clean runs would have about a
  6 % chance. The kernel log also shows one engine reset at 00:36 on 2026-11-01 that matches no recorded run, so the
  hang is not proven gone, and no commit is proven to have removed it. A udev rule now makes `data` readable, but no
  dump has appeared since, so the rule is untested. Hunting a reset that no longer shows brings no frame rate, so this
  stays closed until a run hits it again.

## Vertex program cost: EXEC and relative moves (2026-11-01)

Measured with GPU timestamps per program over the hub dumps of `PPSA21567 - Astro Bot`, Arc A770, ANV 26.2.5, private
SPIR-V stores, one run per change. The per-frame map before the first change was 30.3 ms of GPU work per frame.

- **Merged-stage prolog.** 64 of the 70 vertex programs build EXEC with `s_sub_i32 vcc_lo, 0, s3` and
  `s_lshr_b64 exec, -1, vcc_lo`. On the host, s3 is the highest live lane span, so that mask is all ones for every live
  invocation, but the translator re-read it from a ballot and the driver could not fold it. Every guarded write stayed a
  select that kept the old value live. `b15968379` follows the prolog words and their spill slots through a forward
  dataflow and emits full EXEC where it is proven. Programs run through geometry compute keep the ballot. The sum of
  vertex instructions over the same programs goes from 170,110 to 147,462, and hub GPU from 30.29 to 25.13 ms per frame:
  `550e721d` 300 to 218 us per draw, `96d1762d` 90 to 35 us, `827d31eb` 297 to 237 us.
- **Relative moves.** Three large vertex programs read an array of vectors with `v_movrels` at offset `counter << 2`,
  22 sites in the largest, and each site was a chain of about 115 selects, one per VGPR named at or above its base. The
  low bits of the offset are known to be zero, so `44ba18678` keeps only the registers it can reach. The largest
  program goes from 36,439 to 18,961 instructions, fills from 4,480 to 1,371, spills from 841 to 342. Its compiler cycle
  estimate falls 56 %, but the draw falls less: `550e721d` 218 to 189 us (-13 %), `827d31eb` 223 to 180 us (-19 %), hub
  GPU 25.13 to 24.31 ms per frame. The two hub windows do not line up exactly. The same program is geometry-bound (a
  1-pixel scissor does not move it), so most of what is left is not instruction count.
- **Clean binary.** LTO build of `44ba18678` without diagnostics, warm store, console at warning level: 672 frames
  in 22 s over the first hub view, 30.5 fps. No frame-rate figure from the same window exists for the code before these
  changes, so this is a current level, not a measured gain. GPU work is 23.35 ms per frame there, so the next limit is
  still both GPU and CPU.
- **Instrument trap.** `KYTY_DIAG_SCLK` instruments compute programs when they are translated, and the result lands in
  the SPIR-V store. A later run on the same store without the variable still paid for it: `ff2adfc8` read 14.4 ms
  instead of 2.3 ms. Never reuse a store written with that variable for timing.

Excluded in this work, with what excluded each:

| Hypothesis | Observation |
| --- | --- |
| `robustBufferAccess` costs frame time | Turning it off moved `550e721d` by +3 %, `ff2adfc8` by +5 %, GPU 35.0 to 35.9 ms per frame |
| Direct guest-address lookup instead of the walk | Loops in one program 19 to 0, instructions 6,316 to 6,676, per-program time flat |
| Extending whole-wave masks in pixel programs | Upper bound about -4 % with the guard removed outright |
| Dropping partial-EXEC guards when no inactive lane reads the old value | Offline count with nested EXEC regions on the largest vertex program: 258 of 1,277 writes qualify, about 500 of 18,961 instructions, because each relative move reads every candidate register |
| Bounding the relative move offset from the guest | The loop limit comes from a rounded vertex attribute, so the guest gives no bound; the array looks like 12 registers but nothing proves it |

Still open: 640 selects from relative moves and 1,387 from partial EXEC stay in the largest vertex program, with 1,713
spill and fill messages per invocation. `ff2adfc8` is one dispatch of 2.2 ms. The frame tail is unchanged at 4 ms.

Also fixed: a storage texture over an uploaded texture that the device later wrote ended the run with
`!create_all_the_same` at about 145 s, depending on timing. `4f4a3df13` gives that parent the same arena crossing.

Regression: `PPSA06084 - Superliminal`, `PPSA28311 - Absolum` and `PPSA15552 - Dead Cells` ran 70 s each to their menus
on the EXEC change, with no stop and a correct capture. They have not been checked in gameplay, and not on the relative
move change. The unit suite passes on both.

## Frame partition and removed sync links in the hub (2026-11-01)

`PPSA21567 - Astro Bot`, first hub view, Intel Arc A770, ANV Mesa 26.2.5, xe, clean LTO build of
44ba18678: 3382 to 4088 presents in 20 s, 35.3 fps, GPU busy about 75 %, GPU about 21 ms per frame
(ceiling about 47 fps), command processor thread 15.4 ms per frame.

Per frame on the instrumented build: 5 guest submits, 35 graphics queue submissions, submitters
blocked 24-28 ms, guest between submits 2.3 ms. Blocking links on the graphics queue thread in a
20 s window of 704 frames:

| Link | Share | Cost |
| --- | --- | --- |
| Label wait with aliased pending storage, own queue, current submission | 1 per frame | 2.75 ms/frame |
| `DRAW_INDEX_INDIRECT` argument read, one zero-instance record | 1 per frame | 2.55 ms/frame |
| `DISPATCH_INDIRECT` argument read, producer on the compute queue | 1 per frame | 2.2 ms/frame |
| `WAIT_REG_MEM` 64-bit, other effects | | 0.35 ms/frame |

Each experiment ran one 20 s window against a baseline of 679-680 frames:

- Device waits for cross-queue labels only: suspensions on the graphics queue dropped from 4.6-8
  to 0.2 ms per frame and frames stayed at 679. The wait moved to the next link.
- Adding the cross-queue `DISPATCH_INDIRECT` read on the device: 701 frames, with the wait moved
  to the indirect draw and the aliased label (same-code spread is about 7 %).
- Also consolidating the aliased label with no guard, which is not a shippable behavior: 738
  frames (36.9 fps, +8 %), GPU busy 79 %, no fatal stop through the route, capture matches the
  baseline. It still leaves 2.98 s of indirect draw waits per 20 s.

The guard this needs is not free. In that run 5506 of 10815 consolidated waits (51 %) were followed
by a lookup of a non-storage alias before the write-back: 483185 exact `DepthStencilBuffer` lookups
(the HTILE range), 45668 `RenderTexture`, 3443 `Texture`. A correct guard turns those lookups back
into a wait, so roughly half of the 8 % remains. The indirect draw needs a device twin of the
geometry draw plan. The whole chain stays parked until the GPU frame is shorter: GPU work on the
critical path should shorten the frame directly, because the blocked links wait for that work.

Excluded here: vblank quantization (no present waits on the interval), spinning for 200 us before
sleeping on hand-offs (679 against 682 frames).

## Where the hub frame can still shrink (2026-11-01)

`PPSA21567 - Astro Bot`, first hub view, same host and route, one 20 s window per experiment. Clean LTO build of
44ba18678: 706 frames. The instrumented build: 679-680 frames.

| Experiment | Build | Frames | Reading |
| --- | --- | --- | --- |
| `INTEL_DEBUG=noccs,nohiz` | clean | 665 (-6 %) | Driver compression is active and memory bandwidth is not the limit |
| `INTEL_SIMD_DEBUG` without `fs8`, private driver cache, warm | clean | 651 (-8 %) | Excluded: forcing SIMD16 makes the heavy pixel programs spill |
| Compute-only queue family (forced) | instrumented | 688 (+1 %) | Within the spread; this family also hung the hub in earlier runs |
| 3 us of added CP time per indexed draw | instrumented | 680 (0 %) | |
| 12 us of added CP time per indexed draw | instrumented | 664 (-2 %) | |

**CP sensitivity is not resolved.** The added delay moved the frame count by 0 and by 16 frames. The same code spreads
by about 2.5 fps, about 50 frames in this window, so with one run per point these two runs put no usable slope on CP
time. About 100 indexed draws per frame is an estimate from the CPU difference between the two runs, so 12 us is about
1.2 ms per frame. Neither a saving from CP work nor its absence on the critical path follows from this.

**Compute programs are not wasteful.** Driver statistics joined with GPU cost: the width comes from the driver, and the
hot programs spill little or not at all. `ff2adfc8`, 2.26 ms per frame, runs SIMD16 with 2,754 instructions, 134 sends
and no spills. `90c95e2d`, 0.72 ms, has 78 spills and 343 fills. Only `a8eff16a` runs at SIMD32 with heavy spills, and it
costs 0.014 ms per frame.

**Vertex programs are bound by their own loops.** Vertex reuse is healthy: 3.4-4.8 indices per invocation for most programs
measured, 1.9 for `827d31eb` and 1.5 for `c4a8f3bb`. `550e721d` takes 184 us per draw with 846 vertices and `827d31eb` 174 us with 2,217,
so the time does not scale with vertices. It behaves like the latency of one thread. The guest program runs real
per-vertex math in its own loops (3,113 guest instructions, with 36 sines, 24 cosines and 35 square roots), which would also explain why cutting static instructions by 48 % moved the draw only 13 %. This reading is
inferred from these counts, with no hardware counters behind it.

**Register pressure gates pixel program width.** Compared in the same unit, the live registers of each program's SIMD8
compile: the 38 pixel programs that also get a wider compile have 11-57, and the 19 that stay at SIMD8 have 67-177, with
no overlap. The reported figure scales with width, so SIMD8 and SIMD16 compiles cannot be compared directly.

**The translator inflates that pressure.** Peak liveness computed from the final NIR agrees with the driver on `550e721d`
(84 against 85), and gives 208 against 230 on vertex program `8b21b61c`, whose peak sits in the guest's own loops.

| `8b21b61c` | Value |
| --- | --- |
| Guest VGPRs named by the program | 128 (v0-v127) |
| Host values live at the peak, per-lane | about 184 (114 of them registers from phi webs) |
| Host uniform values at the peak | about 24 GRF |
| NIR lines above 120 live | 48 % |
| Share of the pressure above 120: phi web registers, uniform blocks, vertex inputs | 52 %, 10 %, 10 % |

Guest liveness of the same program peaks at 125 VGPRs (mean 78, six bone loops of 265-770 instructions), so much of the
pressure is inherent. Per-lane values peak at 184, about 60 above the guest's own peak, so at least that excess comes from the
translation. One construct accounts for part of it: of 110 registers live at the peak, 34 are next read only as the
old-value arm of an EXEC select, in linear order and typically 600-800 NIR lines later. Across the program that arm is
1,286 of 3,470 register reads. The offline count above allows dropping only 258 of 1,277 such guards, so that proof
alone would free few of the 34, and the rest of the excess is not attributed. In the pixel program `550e721d`, 486 of 1,909
definitions are selects, and 267 of those share one condition: the lane bit of the WQM mask after the discard. The host
already demotes a quad with no live lane, so that bit should be true on every lane that still matters, but this has not
been built. An earlier run with such guards removed outright bounded the gain near -4 %, and its record does not say whether
compiled width changed. The time spills cost a vertex thread is not measured either.

**Reading.** GPU work on the frame is flat: the largest program is 2.56 ms of about 21 ms. 60 fps needs the GPU frame at
16 ms, and no measured lever gets there. The guarded label wait is estimated at about +4 %, half of one unguarded run and
inside the same-code spread. Host register pressure is the open shader lead.


## Full memory barriers in the hub (2026-11-02)

`PPSA21567 - Astro Bot`, first hub view, clean LTO build of translator 203, counters in an `LD_PRELOAD` shim. About 1,280 barriers per frame; 750 are
the guest-flush `ALL_COMMANDS` memory barrier. About 604 of those follow another one with no draw, dispatch or copy in between; 48 sit between two
dispatches and 12 between two draws.

| Arm (one run, windows at 130, 140, 150 s) | fps |
| --- | --- |
| Unchanged | 31.4, 37.4, 35.4 |
| Repeated barriers dropped and access masks narrowed to shader read and write | 32.0, 38.1, 33.5 |

Excluded on this host: the difference is inside the 2.5 fps same-code spread, and the driver defers these flushes to the next draw, so repeats cost
little. The experiment ignored layout transitions between barriers, so it is not a shippable change.

## Hub GPU idle time and early compute submits (2026-11-02)

`PPSA21567 - Astro Bot`, hub, Intel Arc A770, ANV Mesa 26.2.5, xe, clean LTO build. Windows of 10 s
read 31-39 fps; the same code spreads about 2.5 fps between windows.

- Timeline of one 3 s window on an instrumented build: GPU busy 73.5 %, idle about 10 ms per frame,
  of which 4.4 ms between submissions of the guest's second compute queue, 2.2 ms between
  submissions of its first. In those gaps the GPU has
  finished and the command processor is still recording.
- Per frame in the same scene: 174 draws, 87 dispatches, 38 submissions, draw processing 12.7 ms,
  dispatch processing 5.8 ms.
- Excluded: submitting recorded compute work as soon as the GPU is idle, as graphics work already
  does. 31.5, 39.3 and 32.6 fps against 31.4, 37.4 and 35.4 fps.

## Device-address overlays for refused storage publications (2026-11-06)

`PPSA21567 - Astro Bot`, dunes after the recorded user gameplay route, Intel Arc A770, ANV Mesa 26.2.5, xe, 60 fps limit.

- **Cause measured on main.** Draw-time host waits in `RetryGuestDeviceAddressDraw` held 26.0 ms of 31.7 ms fence
  time per frame (81.5 %, about 6 waits per frame, 585 draws per frame). 99 % of them came from
  `GuestDeviceAddressRecordPublication` refusing a same-queue result that had passed `StorageIsWholeGuestContent`.
  The refusal is `DeviceMayWriteGuestRange`: objects under 64 KiB that are not 64-byte aligned. That guard stays as
  it is, since it prevents partial-cache-line device writes from reverting CPU stores.
- **Change.** For such a refusal, `GuestDeviceAddressRecordOverlay` copies the object, in queue order, into a
  device-local slot. The device-address table maps that slot over the object's guest range, so later
  device-address reads on the same queue see the result without a host wait. Guest memory still gets the bytes
  from the object's write-back.
  - The same copy also goes to host-visible staging. Write-back completion publishes it if guest memory still
    matches the dirty tracker.
    - Why: `GpuMemoryCanWriteBackAtSubmission` defers the write-back of an object that a later submission uses.
      Without the old host wait, objects rewritten every frame reached guest memory rarely: about 1,300 deferred
      against 120 written per 2 s.
    - One run of that build crashed on a null call while another session loaded the disk. Its cause was not
      established.
  - A slot is dropped when its guest range changes, when a device write lands over it, or when a reader on another
    queue prepares. It is also dropped two flips after guest memory last matched the object. The refusal then falls
    back to the old host wait.
  - Snapshot publication compares every page against the last published bytes, and leaves the GPU's written-page
    flags for the object's own write-back.
- **Comparison method.** After the route ends (186 s), the scene varies from run to run whatever the build, so runs
  are not aligned by time or frame number. Per-second samples are grouped by draws per frame, and captures confirm
  the scene.

| Content | Main | Overlays |
| --- | --- | --- |
| Cinematic, 350-410 draws/frame | 12.27 fps (120 s); fence 47.8 ms/frame; WAIT_REG_MEM 1.2 ms/frame | 13.67 fps (25 s); fence 42.6 ms/frame; WAIT_REG_MEM 19.6 ms/frame |
| Heavier scene, 735-790 draws/frame | 11.01 fps (24 s); fence 39.0 ms/frame | 11.55 fps (27 s); fence 34.1 ms/frame |

- **Gain.**
  - In the cinematic, four overlay builds measured +11 to +13 %, which is consistent.
  - The heavier scene shows +5 %, from samples under 30 s. That is not significant.
  - The draw-time device-address wait is gone.
- **Where the wait went.** Part of it returns as WAIT_REG_MEM producer waits, and as `CommandProcessor::WriteBack`
  waits on host reads of GPU-written values (about 55-64 per second).
  - Those reads come from `ReadCurrentGuestBytes` (indirect draw arguments read on the host) or from `WriteGds`.
    Which of the two dominates is not yet measured.
  - The other two callers are excluded. A tagged run counted no ACQUIRE_MEM L2 write-back in the window, so an
    asynchronous L2 write-back would not help here. The dispatch caller records no time
    (`dispatch_writeback` is zero in every run).
- **Other titles sharing the path.** Both runs used the generic route for 125 s and stayed interactive at 60, 90
  and 115 s. Neither had an error, and the captures render correctly.
  - `PPSA09076 - DREDGE` reaches gameplay. It created 12 overlays during the run, and at most 3 were live at once.
  - `PPSA28311 - Absolum` reaches gameplay (the campfire scene). It created 958 overlays in two bursts before
    gameplay, at most 5 live at once.
  - In Absolum gameplay, its device-address waits (about 40 per second) refuse earlier, in
    `GuestDeviceAddressPendingWriteBack`, before any publication is recorded. The overlay does not cover that
    path; it is a separate lever.
- **Tests.** `EmulatorGuestDeviceAddress` covers the slicing of table entries by overlays. The CI suites and the
  architecture-boundary and archive-DAG gates pass.
- **Open.**
  - Device-side indirect draw arguments, if `ReadCurrentGuestBytes` turns out to dominate the remaining waits.
  - The WAIT_REG_MEM producer waits.

## Device-side indexed indirect draws and lazy alias waits (2026-11-06)

`PPSA21567 - Astro Bot`, dunes after the recorded user gameplay route, Intel Arc A770, ANV Mesa 26.2.5, xe, 60 fps limit.
Every A/B below alternates the change on and off every 10 s inside one run and compares samples by draws per frame.

- **Device `DRAW_INDEX_INDIRECT`.** When the record lies in a storage buffer that an earlier command of the same queue
  writes, a one-invocation converter turns it into `vkCmdDrawIndexedIndirect` arguments in queue order: it clamps the
  count to the `INDEX_BUFFER_SIZE` window, adds the host vertex offset (a sum outside int32 draws nothing) and latches
  the instance count. A later direct draw waits for that recording and reads the latch. The host neither waits for
  nor reads the record.
  - Eligibility: no quad-list chunking, geometry pass or fragment transport, 16- or 32-bit indices, a known index
    window. About 75 % of the dune records are geometry-pass draws and keep the host path, so about 25 % move.
  - Alone it gains nothing: 14.11 fps off against 13.60 fps on (350-399 draws per frame). The argument wait moves
    into `WAIT_REG_MEM` current-producer waits, about 137 per frame both ways, which double from 0.12 to 0.24 ms
    each (18 to 35 ms per frame). Those are the waits the alias guard keeps synchronous.
- **Lazy alias wait.** `WAIT_REG_MEM` now always consolidates a same-queue producer whose effects are a store or a
  store with write-back. The writable storage buffers it leaves with pending uses and aliases whose refresh reads
  guest bytes are recorded. The next draw or dispatch that creates or links such an alias waits for that storage's
  submissions first (a mutation conflict, retried after the wait). Outside a draw or dispatch that use stops the
  emulator.
  - The unit test for the old guard lost its alias case, since the guard moved from the wait to the alias's use.

| Draws per frame | Off (host path, synchronous guard) | On (device draws, lazy alias wait) |
| --- | --- | --- |
| 550-599 | 13.73 fps (56 s) | 14.66 fps (44 s), +6.8 % |
| 600-649 | 13.09 fps | 13.61 fps, +4.0 % |

  `WAIT_REG_MEM` time fell from 3.4 to 1.7 ms per frame. The on phases deferred 9,867 aliases and waited at 4,736
  uses. The captures render correctly.
- **Other titles.** `PPSA09076 - DREDGE` and `PPSA28311 - Absolum` on the generic route for 125 s stay interactive at
  60, 90 and 115 s (27-31 and 40 fps), with no error, and the captures render correctly.
- **Known limit.** A negative base vertex with indices small enough that a vertex index falls below zero: the host
  path skips the whole draw, the device path draws it and lets robust buffer access bound the fetch. The host
  cannot apply its predicate, since the drawn index range comes from the GPU-written record.
- **Open: intermittent write-back stop.** 2 of 6 dune runs with waits consolidated over aliases stopped with
  "GpuMemory write-back requested before exact resource dependencies completed"; about 15 runs with the synchronous
  guard did not. A run on another branch hit it too, so it may predate this change. The static suspect is the
  device-address read preparation (`GuestDeviceAddressWriteBack` → `WriteBackStorageRanges`), which writes back any
  collected storage with incomplete uses that is neither in place nor held by an overlay. Not confirmed with a
  backtrace. Fixed on 2026-11-07, see "Pending-storage scans missed overlays that aged out".
- **Unrelated stop seen once.** "unknown relation: DepthStencilBuffer - Contains - RenderTexture" also occurs on
  builds without this change.

## Split part width in the dunes (2026-11-06, not committed)

`PPSA21567 - Astro Bot`, dunes after the recorded user gameplay route, Intel Arc A770, ANV Mesa 26.2.5, xe, 60 fps
limit, private module store. Question: does a narrower host part (more registers per guest lane) pay where a split or
native program spills?

- **Offline.** Recorded compute pipelines compiled on ANV report spills by width. At width 32 the native wave32
  `0x874bd13f` has 2,918 spills and 10,021 fills. As 16-lane parts it has 1,578 and 6,220. `0xe1b1315d` drops
  from 860:1,830 to 42:140. Wave64 programs split at 16 lanes spill when the module needs more than 128 registers
  (`0x8723fe62`, `0x90c95e2d`, `0x108c4737`). `0xff2adfc8` and `0xc99976f9` do not spill.
- **Exclusive GPU time per dispatch.** A diagnostic bracketed every guest dispatch with end-of-pipe timestamps. The
  width rotated every 10 s inside one run, so each program ran at every width.

| Program | Before | After | Change |
| --- | --- | --- | --- |
| `0x874bd13f` wave32 | native 9.71 ms | 16-lane parts 8.89 ms | -8 % |
| `0xe1b1315d` wave32 | native 0.76 ms | 16-lane parts 0.31 ms | -60 % |
| `0xfd1ee59c` wave32 | native 56 us | 16-lane parts 81 us | +45 % |
| `0x2bcba9a2` wave32 | native 76 us | 16-lane parts 74 us | 0 |
| `0x8723fe62` wave64 | 16 lanes 314 us | 8 lanes 221 us | -30 % |
| `0x90c95e2d`, `0x108c4737` wave64 | 16 lanes | 8 lanes | -5 % |
| `0xff2adfc8` wave64 | 16 lanes 1.37 ms | 8 lanes 2.07 ms | +51 % |
| `0xc99976f9` wave64 | 16 lanes 85 us | 8 lanes 162 us | +90 % |

  Narrower parts pay where the wider module spills, and cost rendezvous where it does not. The guest VGPR count does
  not predict which case applies.
- **Frame rate.** In the main bin (350-399 draws per frame) `0x3666fa39` runs twice and `0x874bd13f` once per frame.
  The table compares means of 10 s samples, with standard errors.

| A/B, alternated inside one run | Off | On | Change |
| --- | --- | --- | --- |
| Wave64 always 8 lanes and wave-sensitive wave32 in 16-lane parts | 14.15 ± 0.12 fps | 14.70 ± 0.19 fps | +3.9 % |
| Wave-sensitive wave32 always in 16-lane parts | 14.52 ± 0.26 fps | 14.27 ± 0.15 fps | -1.7 % |
| Three-way rotation: wave64 at 8 lanes / both rules | 14.27 ± 0.37 fps | 13.82 ± 0.25 / 14.48 ± 0.13 fps | -3 % / +1.5 % |

  Only the first result reaches two standard errors, and the parts do not add up. The lever is at most 2-3 % of the
  frame, below what 10 s alternation resolves. The exclusive savings do not reach the frame: the GPU is about 88 %
  busy, these programs overlap the BVH dispatches, and frame time follows `0x3666fa39`. End-of-pipe timestamps also
  serialize dispatches, so they overstate frame gains under overlap.
- **Not committed.** No width rule changed. Removing the wave32 guard reaches every wave-sensitive wave32 program on
  Intel. `PPSA03524 - GTA San Andreas Definitive Edition` could not be smoke-tested: this branch stops at boot
  without the storage-image format support in the PR #65 fixes.
  `PPSA09076 - DREDGE` and `PPSA28311 - Absolum` dispatch no wave32 programs.
- **Open defect: wave32 in 8-lane parts.** With four 8-lane parts per wave32 the device was lost about 2 s after the
  first dispatch of five dune programs: `0x874bd13f`, `0xe1b1315d`, `0x2bcba9a2`, `0xfd1ee59c`, `0xbf8ee9fc`.
  Ten other wave32 programs ran correctly in 8-lane parts before that. The admission checks accept wave32 only in
  16-lane parts, so the product never takes this layout.
- **Re-read 2026-11-12, parked.** Nothing on HEAD changes the record: `0x874bd13f` still runs native at width 32
  because a wave32 program splits only above 64 VGPRs. No new rule was built or measured.
- **Amended 2026-11-19.** A width rule is now committed (`11f783e44`, crash-site record below): wave32 splits above 32
  VGPRs on hosts that go down to width 8. For the five dune wave32 programs in the table above it agrees with the
  measured sign (`0x874bd13f`, `0xe1b1315d`, `0x2bcba9a2` split; `0xfd1ee59c` stays native), so the remark that the
  VGPR count does not predict the case no longer holds for them; it is untested beyond them. `0x874bd13f` measured -8
  % here (end-of-pipe brackets, widths rotated inside one run) and -57 % on t246 (per-dispatch stamps, matched runs);
  the gap is unexplained.
- **Priced 2026-11-19, parked.** At the crash site with the rule, per-dispatch stamps give `0x874bd13f` 5.14 ms,
  `0xbf8ee9fc` 1.88 ms and `0xe1b1315d` 0.17 ms a frame in 16-lane parts (one stamped run; `0x2bcba9a2` is below the
  per-frame list). The one measured 16-to-8-lane change in this tree, `0x3666fa39`, fell 34 % (36.9 to 24.4 ms), so
  8-lane wave32 parts would save at most about 2.4 ms there, more than the 1.2 ms the crash site needs for 30 fps,
  and nothing at the second rock, where these programs are not the cost. It is an upper bound: `0x3666fa39` makes
  no cross-part lane reads, while `0x874bd13f` in 8-lane parts adds eight. An earlier version of this bullet used the
  second-rock figures and is superseded.
- **No localized cause for the device loss (2026-11-19).** Translate-only dumps of `0x874bd13f` and `0x2bcba9a2` in
  8-lane parts (four parts a guest wave, exit before dispatch) were read offline: local size 64 (two whole guest
  waves), exchange area 304 dwords (two waves of 152), lane index `part * 8 + lane` (0 to 31), the rendezvous bank
  toggled once per call and read with the same parity by the lane exchange, ballots merged as four bytes, and the one
  lane-exchange call site checked sits under a wave-wide EXEC test. Subgroup-scope control barriers also appear in
  the production 8-lane wave64 modules. `0x874bd13f` reads DPP rows through the guest-lane exchange eight times;
  `0x2bcba9a2` never does and no production 8-lane module does, so that path is not shared by the programs in the
  record and the record does not say which of the five lost the device. Neither module has a GDS append or consume.
  The admission checks stay at 16 lanes for wave32 and no 8-lane dispatch was repeated. Unchecked: the seven other
  lane-exchange call sites of `0x874bd13f`, and whether one part of a wave can skip a rendezvous (the rendezvous
  spins up to 2^20 iterations, which fits a device lost about 2 s after the first dispatch).

## Depth targets no longer wait for deferred storage aliases (2026-11-07)

`PPSA21567 - Astro Bot`, dunes after the recorded user gameplay route, Intel Arc A770, ANV Mesa 26.2.5, xe, 60 fps
limit, 170-300 s after launch.

- **Observed.** 16.7 fps, 319 draws per frame. The command processor took 60.5 ms per frame (151 us per draw) and
  waited on fences 11.8 times per frame for 32.1 ms. The device-address draw retry (`RetryGuestDeviceAddressDraw`)
  waited on the host 1.65 times per frame, 23.8 ms per frame, about 14 ms each.
- **Cause** (temporary counters, removed). 1.52 of the 1.65 waits were the lazy alias wait of the record above. A
  depth target linked to writable storage that a consolidated `WAIT_REG_MEM` left pending waited for that storage
  at its next draw: 0.96 per frame in auto draws, 0.55 in geometry passes and 0.01 in indexed draws. The guard
  covers aliases whose refresh reads guest bytes, but a depth target reads none. It keeps no guest hash
  (`check_hash` is false) and its update does nothing, so the write-back changes only its metadata. A new depth
  target over pending storage already relies on queue order (`GpuMemoryShouldLinkPendingDepthStencilStorage`).
- **Fix** (`GpuMemoryWriteBackRefreshReadsGuestBytes`). A depth target no longer counts as an alias whose refresh
  reads guest bytes, so storage linked only to depth targets is not deferred. The write-back walk still
  invalidates the depth target's metadata at completion.
- **Result.**

| Per frame, 170-300 s | Before (counter build) | After (counter build) | After (product build) |
| --- | --- | --- | --- |
| Retry host waits | 1.65, 23.8 ms | 0.07, 0.09 ms | - |
| Command processor | 60.5 ms, 151 us per draw | 49.9 ms, 110 us per draw | 52.2 ms, 114 us per draw |
| Fence waits | 11.8, 32.1 ms | 10.7, 30.4 ms | 10.9, 30.1 ms |
| Frame rate | 16.7 fps | 17.2 fps | 17.0 fps |

  The fence time barely moves because the frame here follows the GPU: about 60 ms of GPU timestamps per frame
  (record of 2026-11-02). The waits moved to other fences. The captures at 170 and 300 s match the earlier ones.
  The capture at 190 s is the cinematic's fade to black, which earlier runs also caught.
  - `PPSA09076 - DREDGE`, generic route, 40-120 s: 21.58 to 20.08 us per draw, 35.1 to 35.9 fps, picture correct.
  - `PPSA28311 - Absolum`, generic route, 80-120 s: 58.4 to 59.4 fps at the 60 fps limit, fence 2.2 to 2.3 ms per
    frame, picture correct.
  - All 108 tests pass, one skipped as before.
- **Open.**
  - In the dunes the frame is bound by the GPU programs `0x3666fa39` and `0x874bd13f`.
  - 0.95 `DRAW_INDEX_INDIRECT` records per frame are declined on the device and read on the host after a full
    drain.

## Pending-storage scans missed overlays that aged out (2026-11-07, fixed)

`PPSA21567 - Astro Bot`, boot and the recorded user gameplay route, Intel Arc A770, ANV Mesa 26.2.5, xe.

- **Observed.** 5 of 13 runs on this branch stopped 6-12 s after launch with "GpuMemory write-back requested before
  exact resource dependencies completed". The rate rose after the depth-target record above, because its host waits
  had been completing pending storage before the stop could occur.
- **Cause** (temporary backtrace and object dump, removed). The stack was `DrawIndexIndirect` →
  `RecordIndexedDraw` → `PrepareGuestDeviceAddressForRead` → `GuestDeviceAddressWriteBack` →
  `WriteBackStorageRanges` → `WriteBackObjectLocked`. The object was an 8-byte storage buffer with no alias and one
  incomplete use on the consumer's own queue. Its overlay was for the same write but had been held too long
  (`OverlayHeldTooLong`).
  - An earlier pending scan (`PendingStorageWriteBack`) had skipped the object while its overlay still held, and
    memoized "nothing pending" for the scan generation.
  - A frame flip then aged the overlay, but nothing changed the scan generation. The next pending scan took the
    memo path, which only revisits objects recorded as changed, and never looked at the object.
  - `WriteBackStorageRanges` keeps its own memo, which also ends at a completion, so it scanned everything. It found
    the object no longer held by its overlay and wrote it back before its use completed.
- **Fix** (`GuestDeviceAddressFrameDone`). A frame flip invalidates the storage scans. An overlay's hold only expires
  at a flip, so the next pending scan sees the object again. It then waits for the host, as designed for a result
  that cannot be published. The cost is one full scan per frame per consumer queue.
- **Result.**
  - 0 of 7 counter-build runs stopped: one full route of 330 s and six boots of 45 s.
  - Product build, 170-300 s: 17.2 fps, 116.6 us per draw, GPU 85.5 % busy, against 17.0 fps, 114.0 us and 85.9 %
    before. The captures at 170 and 300 s render correctly.
  - `PPSA09076 - DREDGE`, generic route, 40-120 s: 20.08 to 19.96 us per draw, 35.9 to 36.0 fps, picture correct.
  - `PPSA28311 - Absolum`, generic route, 80-120 s: flips per second stay at 58-61 in every sample, before and after,
    in two runs after. Per draw: draw processing 25-26 us, device-address preparation 0.68 against 0.67 us,
    write-back 2.92 against 2.83 us. GPU work is 15.1 ms per frame. An earlier reading of 56.2 against 59.4 fps
    divided by the window length when a 1 s sample fell outside the window. Picture correct.
  - All 108 tests pass, one skipped as before.
- **Excluded: a device path for geometry-pass indirect draws.** In the dunes 3052 `DRAW_INDEX_INDIRECT` records
  declined the device path from 150 to 322 s, against 2344 that took it. 99.9 % of the declines need the geometry
  compute pass, whose counts sit in push constants. A one-run ceiling read their arguments from guest memory without
  the drain (temporary switch, removed). Over 158-178 s, with 554 draws per frame in both runs:

| Per frame | Drain | No drain (ceiling) |
| --- | --- | --- |
| Frame rate | 15.41 fps | 15.62 fps |
| GPU busy | 77.2 % | 78.3 % |
| GPU work | 50.1 ms | 50.2 ms |
| Fence waits | 10.7 | 7.9 |

  The waits move to other fences and the frame follows GPU work, so the device path would gain about 1 %. The ceiling
  run stopped at 180 s on "!create_all_the_same" for a storage texture, which is expected when draws use stale counts.

## Display signal versus render work (2026-11-08)

`PPSA21567 - Astro Bot` 01.018.000, clean LTO build of `f4823fbe5`, Intel
Arc A770, Mesa 26.2.6, 60 fps presentation cap. The executable used no game
patch. HD and 4K runs started from separate copies of the same save, shader
store and pipeline cache and used the same recorded input. The configured
render target was Fixed 1280x720, Display scope, with linear presentation.
The window and the guest-reported screen were 1920x1080 for HD and 3840x2160
for 4K; they cannot be varied independently in this build.

- A separate diagnostic run broke inside `VideoOutGetOutputStatus` after the
  status fields were written: Astro called it and received resolution `1`
  under HD and `2` under 4K. The debugger and dispatch logging were enabled
  for those observations, not for performance measurement.
- In both configurations, the first three logged compute dispatches selected
  by a 1920x1080 texture had the same shader addresses and group counts:
  `0x500793000` (120x68), `0x500732a00` (30x68), and `0x5007c3f00`
  (120x68). Their recorded descriptors include 1920x1080, 960x540, and
  480x270 textures. This is evidence for these startup dispatches, not a
  census of all passes or of the dunes.
- The normal runs reported a registered guest display and actual present
  source of 3840x2160 in both configurations. The present destination was
  1920x1080 in HD and 3840x2160 in 4K. The coordinator reported a 1280x720
  candidate and `scaling_applied=false`; the candidate is not evidence that
  the game's rendering was reduced to 720p. Both captures made at 100 seconds
  showed a space cinematics sequence, not matched gameplay frames.
- Neither uninstrumented case reached the dunes: HD stopped after 159 s at
  `!create_all_the_same` for overlapping storage textures, and 4K after 151 s
  at `DepthStencilBuffer - Contains - RenderTexture`. A second HD run stopped
  at 141 s on `!create_all_the_same`. These are existing categories of strict
  memory-relationship stops; the trials do not establish that either screen
  mode caused them. There is no matched gameplay FPS comparison from these
  runs.

Conclusion: changing the reported output is effective at the VideoOut API
boundary, but it did not change the observed startup dispatches or final
guest surface. It is not evidence that Astro will choose a lower internal
render resolution. Presentation filtering cannot remove work already done by
the game's render and compute passes. A gameplay comparison still requires
reaching the same interactive scene without memory-relationship stops and
measuring its internal pass extents and GPU/CPU timings under both signals.

### Dune-view retries

One further run per signal used the same settings and seed copies. Both used
the same `f4823fbe5`-based LTO executable with one diagnostic-only local change:
the memory failure report prints all 13 object parameters instead of ten.
This changes neither resource decisions nor rendering. Both runs were stopped
by the harness at 330 s; neither reproduced a memory stop. Captures at about
190, 235, 280 and 325 s show the same first-dune view, with the character
remaining in approximately the same place and playing idle animations. They do
not establish successful recorded movement or broader gameplay stability.

For the 26 performance samples with endpoints between 195 and 325 s, the
agent counted 1866 presents over 129.903 s in HD (14.36 fps) and 1983 over
129.886 s in 4K (15.27 fps). Capture requests occur in the interval and can
disturb timing. These are single, sequential runs with different window and
swapchain sizes; their small difference is not an isolated or statistically
established effect of the reported output. Both continued to report a
3840x2160 present source, and the HD run presented it to 1920x1080.

The retry confirms that this initial dune view can be reached in each mode;
it does not measure internal dune pass extents, GPU timestamps or input
responsiveness. It also does not explain or fix the intermittent stops.

Two later HD attempts to check pad input stopped before the dune view. One
reported a mixed-parent storage-texture overlap; the other repeated the depth
`Contains` color relationship: a 1216x684 depth image over a 960x540 color
target at the same base. A preceding run stopped on that depth/color relation
as well. The additional object parameters distinguish a fully overwritten
storage image from one that needs its earlier contents. The correct content
source and synchronization for these overlaps remain unproven. Since
2026-11-09 the depth/color form links without reclaiming or converting
(`91be7a38a`, top record); content provenance is still unproven.

### Depth/color overlap investigation

Three further HD diagnostic attempts based on `f4823fbe5` reproduced the
`DepthStencilBuffer - Contains - RenderTexture` refusal at 103, 106 and
150.6 seconds. The 150.6-second run recorded the immediately preceding
color materialization: a 960x540, pitch-1024, single-sample tiled color image
at the same base as a 1216x684 D32 depth image. The color allocation spans
0x280000 bytes, while the depth allocation spans 0x3c0000 bytes. Color fast
clear was disabled, but the color attachment had a nonzero DCC address. That
fact alone does not establish that a pending DCC clear is absent.

A fourth attempt instrumented the DCC clear tracker at color materialization
and ran for 360 seconds without hitting this refusal; its earlier observations
found no tracked event at the queried address. This is not evidence that the
guest never wrote DCC metadata, nor evidence of the tracker state at the
previous failures. Captures from this run show the introductory sequence;
they do not demonstrate interactive gameplay or a 60 fps result.

A diagnostic model of Kyty's current 64 KiB depth and color tile-address
formulas, using slice zero without an XOR swizzle, maps the 518,400 visible
color texels to 493,824 represented depth texels and 24,576 depth-padding
positions; only 30,720 map to the same coordinate. The model uses the
color's pitch of 1024 and the depth allocation's aligned pitch of 1280. These
are software-model results, not independently verified hardware addresses or
proof that those texels were written. They do demonstrate why copying equal
coordinates between the host depth and color images is not a valid general
conversion under these assumptions. Reclaiming the larger depth allocation
also risks discarding bytes outside the color allocation. A correct solution
needs the actual content provenance, ordering, padding handling and any DCC
semantics; the current refusal remains unresolved.

A fifth diagnostic attempt, with depth-use logging filtered to writes and
clears, stopped at 161.5 seconds on a different error: `GpuMemory write-back
requested before exact resource dependencies completed`, for a storage
buffer. Its depth trace recorded an arm event for the target, but no later
matching write or clear; the sought depth/color failure did not occur in this
attempt. The executable includes `9b0abb808`, the earlier aged-overlay fix.
The message alone does not establish that the previously fixed cause has
recurred; this new occurrence needs its own call stack and dependency state.

A sixth attempt reproduced the depth/color
refusal at 94 seconds. The DCC tracker query immediately before the refused
color creation found no tracked clear at that address. The filtered depth
trace recorded two arms for host depth images at the same guest address and
no subsequent matching draw write or clear. The trace starts after depth
materialization and covers the traced draw path, so it neither proves initial
contents nor excludes writes through other paths. The address and the absence
of a tracker event are observations, not a general aliasing rule.

The ordinary depth-image creation path starts with an undefined host image;
its update callback does not upload guest bytes, and the first-use attachment
logic can clear it. A separate creation path can seed depth from a storage
image. The filtered trace does not establish which initialization produced
the failing parent's contents. Consequently, read-only draws after the trace
arms do not by themselves establish that guest memory is an authoritative
copy of that depth image.

To distinguish a recurrent write-back failure from the previously fixed
aged-overlay issue, a diagnostic build was prepared to log the failing
write-back's host stack and outstanding submission dependencies. The first
attempt was the 94-second depth/color failure above; the second ran to 360
seconds without either failure, so neither captured the requested stack.
`GuestDeviceAddressFrameDone` advances overlay age on the flip path, while
`PublishPendingStorageToGuestAddress` and the subsequent
`GuestDeviceAddressWriteBack` are separate operations. An age transition
between them is a concurrency hypothesis, not an established cause of the
161.5-second failure. A fix must retain the required queue ordering and be
tested against a reproduced pending-use transition.

A third stack-instrumented attempt also reached
the 360-second harness limit without either failure. Its recorded input left
the game at the new-game menu; a diagnostic controller tap advanced it to
the opening sequence near the end of the run. It did not cover the same route
as the failure and is not evidence that the error is fixed.

A later attempt with the new write-back error diagnostic compiled into the
isolated executable reproduced the depth/color refusal at 122.6 seconds.
The immediately preceding DCC
tracker query again found no tracked clear. The write-back error did not
occur, so its expanded message was not exercised by this run. The isolated
build was based on `f4823fbe5` with the diagnostic source file matching
`5c660ef7` and additional temporary tracing; it is not a clean build of the
branch tip.

A follow-up probe instrumented the first
materialization of the selected depth address, but stopped at 149.3 seconds
on `!create_all_the_same` for a 3840x2160 storage texture with mixed parents
at `0x53de40000`. The selected depth address was not materialized before the
stop. This repeats the storage-view frontier described at the start of this
record and supplies no evidence of the failing depth image's initialization.

A further depth-initialization probe hit
`DepthStencilBuffer - Contains - RenderTexture` at 125.8 seconds. The selected
address had two distinct depth image materializations during the run. At the
failing submission, the depth image was created with an undefined Vulkan layout
and no requested depth write or clear; color materialization at the same guest
address was attempted in that same submission. The failure occurred before
framebuffer creation, so this does not prove that the new depth image was
subsequently cleared or used. The earlier image also began undefined, but the
filtered lifetime trace does not establish its first-use initialization. The
color DCC tracker again had no matching pending event at the refusal. These
observations do not establish the guest depth contents or exclude depth tests,
stencil operations, other writers, or guest metadata writes.

The normal draw path materializes depth and color before it sanitizes and
creates the framebuffer. Simply deleting the depth object when the overlapping
color object is created would risk invalidating the depth pointer already held
by that draw, as well as discarding the depth allocation's uncovered bytes.
Any safe fix must account for simultaneous attachment use and content ownership;
this probe does not yet provide a correctness basis for such a change.

A follow-up run logged the depth state at
materialization and reproduced the same refusal at 154.9 seconds. In the
failing submission (`26369`), depth testing was enabled with Vulkan compare
operation 3 (`LESS_OR_EQUAL`), depth writes and depth clear were disabled, and
stencil testing was enabled. The depth image format at this frontier is D32,
so the logged stencil enable alone does not establish an active stencil plane.
The color materialization that failed had the same submission and guest base.
Consequently, a read-only depth image cannot be dismissed as inert in this
draw: its comparison can affect color coverage. These are instrumented
observations, not a verified fix or a gameplay performance measurement.

The [Vulkan render-pass specification](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)
prohibits using multiple attachments that alias the same device memory in one
subpass. Thus binding two host images to overlapping Vulkan memory is not, by
itself, a correct implementation of this guest draw. Separate host images need
an explicit, correctly ordered way to construct their contents and reconcile
later accesses; whether a snapshot is sufficient for the guest draw requires
further evidence about its guest memory ordering and overlapping texel writes.

A further isolated provenance probe ran for its
360-second limit without either reported refusal. At an earlier creation of
this same depth view, Kyty found 15 overlapping tracked objects, including a
960x540 color render target at the same base and larger render targets, and
selected `delete_all` followed by ordinary depth creation rather than
creation from those objects. Detachment checks still apply to objects with
write-back callbacks. The usage timestamps do not establish which guest bytes
were written or whether their contents remained needed, so this observation
alone cannot justify changing reclamation or choosing a seed image.

The diagnostic run reached the desert scene. Nine reset performance snapshots
ending between 255 and 295 seconds counted 646 presents over 45.079 seconds
(14.33 fps); this instrumented run and its route are not an A/B performance
comparison. A later diagnostic left-stick input was followed by a changed
character pose and camera framing; the axis was explicitly returned to its
neutral value. That is a limited input observation, not sustained gameplay or
visual-equivalence validation. The manual capture's automated color heuristic
reported `hot_corruption`; its verdict alone cannot establish corruption in a
sand-colored scene without a reference comparison.

## Freezes from the pipeline cache and array uploads (2026-11-15)

`PPSA21567 - Astro Bot`, Arc A770, Mesa 26.3.0-devel (git 34ad0a45aab), kernel 7.2.15, dunes route with the 60 fps
present cap, diagnostic builds, one run per arm. The game is on an NTFS volume; caches are on a btrfs volume mounted
with `compress=zstd:3,ssd,discard=async`.

- **Observed.** Freezes of 1-6 s in the first two minutes, with the ring thread blocked in `read()` of SPIR-V store
  files of 5-380 KB. Each read took 1.2-2.0 s with no major fault. The thread was in uninterruptible sleep in btrfs
  metadata reads (`read_extent_buffer_pages`).
- **Cause 1: the pipeline cache was rewritten on every launch.** The 136 MB driver blob was saved about 25 s after
  boot even when every pipeline hit it. The driver serializes an unchanged cache in a different byte order, so the
  blob had the same size and a new digest. A host sampler showed a burst of about 22,000 writes to the disk, a second
  in which none of 4 requests completed, then reads that waited 5.0 s in total and discards that took 2.0 s. The frame
  rate reached 0 fps in that second.
- **Fix 1.** Pipeline creation chains creation feedback. Only a pipeline the driver did not find in the application
  cache marks the cache dirty.

  | Run (one each) | Disk writes | Read wait |
  | --- | --- | --- |
  | Before | 77,500 | 260 s |
  | After | 6,934 | 18 s |

  The cache file was not rewritten after the fix. With an empty cache file it was still saved (43.9 MB), so new
  pipelines are kept. The fix removes one producer of the stall, not the stall: two later runs with no cache save
  still had SPIR-V reads of 0.5-2.0 s (4.6 s and 5.3 s in total).
- **The disk side.** The volume is a DRAM-less NVMe drive that takes 1.6-2.0 s per discard. btrfs held 4.7-5.1 GB of
  freed extents waiting for discard during these runs. In one run the backlog grew from 3.3 to 4.8 GB while the title
  ran, and the driver's own shader cache was written in that run; which writes refilled it was not separated. The
  run with the most discards between 40 and 60 s (95, 18.8 s of discard time, reads waiting 18.2 s) fell to 0 fps for
  several seconds in that window. The run with none there stayed at 19.8 fps or above. Mount options and trim policy
  are host settings; the emulator side is that the ring thread reads the SPIR-V store synchronously.
- **Fix 3: the store reads a title's working set ahead of use.** In a run with a large discard backlog, store loads
  on the ring thread summed to 20.4 s, with 260 ring-thread stalls inside `read()` and no shader compile. Reading
  the files into the page cache first did not hold: their pages were evicted during the run, and the stalls moved to
  metadata reads. A diagnostic build that read the run's 358 entries (64.1 MB, 147 ms) into process memory before
  use cut the loads to 36 ms in total (3 ms at most) with no ring-thread stall; that run also had fewer discards
  (8 s in total), so the host did not match. The shipped version records the entries a title loads in a manifest
  inside the title's compute-recipe directory (it follows the translator version): this session's loads in
  first-use order, then the names carried from earlier sessions. It is rewritten only when the set changed; a
  third run of the same route left it untouched. The next
  session reads the listed entries into memory on a background thread, bounded to 4,096 entries and 256 MiB. Each
  entry is freed when it is loaded and passes the same checks as a file read. Same route, the shipped store in a build
  with diagnostic lookup timers, store pages dropped from the page cache before the second run:

  | Run | Manifest | Store loads | Total | Longest |
  | --- | --- | --- | --- | --- |
  | Records the set | none | 413 | 374 ms | 33 ms |
  | Reads the set ahead | 451 entries | 429 | 73 ms | 2 ms |

  The second run had the busier disk: 970 discards taking 11.4 s and 103 s of read wait on the device, against 5.7 s
  and 10.8 s in the first (the 20.4 s run had 28.9 s and 86 s). Device counters include every process on the host,
  and the second run's 40,000 writes were not attributed. The drops at 80, 92 and 95 s appear in both runs, so they
  are not store reads.
- **Cause 2: every `DMA_DATA` store re-detiled the whole 192-layer array.** Each store of 4-64 KiB into the 17.3 MB
  array cost 6-9 ms. 192 such flushes per run summed to 1.8 s, in a burst at 65-76 s of the route.
- **Fix 2.** Texture updates use the page-generation runs that storage buffers already use. The layers those runs
  cover are detiled and uploaded. An empty baseline, label updates, backing versions and other texture kinds keep the
  full upload. One binary with a diagnostic switch for the full path gave both arms:

  | Arm | Flushes of 5 ms or more |
  | --- | --- |
  | Full upload | 192 |
  | Touched layers | 12 |

  Neither arm had the disk sampler, so their frame rates are not compared here.

  The 190 partial uploads sent 1 layer (144 times), 2 layers (45) or 8 layers (1). A diagnostic hash of each layer's
  guest bytes found no untouched layer that had changed in those 190 uploads; on the first one it reported a touched
  layer as changed, so the check engaged. The two arms reached different shots at the capture times (the faster arm
  is about 10 s ahead in the cinematic), so pictures were not compared frame for frame. Neither capture shows
  corruption.
- **Still open.** In one run, entering the dunes level dropped to 3 fps for about 3 s, with the ring thread reading
  SPIR-V store files, translating shaders, creating pipelines and creating textures at that point. How much of it was
  disk wait was not measured. Drops to 6-14 fps around 80, 92 and 95 s repeat with the working set read ahead, so
  they are not store reads; the 79-80 s drop also stays (7.9 fps) with the native capture moved from 75 to 86 s, so
  it is not the capture. Their cause is not identified. Whether the driver's own shader cache, written by the
  driver rather than the emulator, also waits on the disk was not checked.
- **Excluded: file status calls.** In one profiled run the title's main thread spent about 85 % of its sampled time
  in the stat call between 89 and 92 s, while the game volume's filesystem driver used 30-50 % of a core with no
  disk reads and the command-processor thread was blocked in a futex wait. A test build resolved an exactly spelled
  package file with one host lookup (the resolver the APR path already uses) and counted the calls: up to 81 s after
  launch the title made 27,648 stat calls, 7,382 on the one-lookup path, 752 on existing files that need the full
  resolve and 19,456 on absent files (it probes numbered variants of its UI files). In that run the calls ran at
  about 600-700 per second, nearly all absent files, from 66 to 76 s while the title held 29-34 fps, so the calls
  themselves do not slow frames. In that run and in the profiled one the drop came in the second after a burst of
  these probes ended (77 s and 92 s), which points at the load step that follows them. The test change was not kept:
  it reached only 27 % of the calls and showed no measured gain.
- **Drops from texture creation (2026-11-15).** Of 14 seconds under 15 fps in four runs of the dunes route (two of
  them on the stat test build), 9 spent 363-599 ms of the second creating GPU objects while 247 MB to 1,011 MB was
  uploaded, 3 spent 460-492 ms in draw state setup and 2 stayed under both thresholds (250 ms). In the three create
  and setup seconds of the one run sampled per thread, the command-processor thread was on the CPU; in the profiled
  run's setup second at 92 s it sat about 75 % in a futex wait. The large textures
  (22,413,312 bytes each) cost a median 9.4-13.8 ms to create, of which 4.3-6.0 ms was the upload and 1.2-2.5 ms
  Vulkan allocation: a fill above the 4 MiB pooled size created, mapped and freed its own staging buffer, paying an
  allocation and a fault on every page each time. Completed fills now hand such buffers to a free list bounded at
  128 MiB (db6d7c132). In two runs the same textures took a median 6.9 and 7.9 ms, with the upload at 1.5-1.6 ms and
  allocation at 0.2-0.3 ms. A first version also kept pooled staging, and Minecraft's dense view then cost 8.80 us per
  draw against 5.82 before (the list length was not measured), so only dedicated buffers are kept, and the next run measured
  5.33 us per draw in that view (one run each; the view reached differs between runs). The route desyncs after a
  hitch, so no frame-rate change is claimed for the dunes. In the run on the final build the five seconds under
  15 fps were two of draw state setup (439 and 514 ms) and three in the heavier later scene (668-733 draws per frame)
  with neither counter high; none was texture creation, and 8 of its 25 large textures still made two allocations.
  The free list is not released when the graphics context is replaced, which only the test fixture does today;
  in-flight fills had the same limit before.

## Dunes baseline re-measured against 2026-11-14 (2026-11-18, measured)

Intel Arc A770 (`8086:56a0`, Mesa ANV from git, 26.3.0), strict, 60 fps present limit, warm private SPIR-V and
pipeline caches per build. `fe853a27b` (the build behind the 2026-11-14 causal test) against `575cb70f8`.

**Causal runs.** One run per build with the 2026-11-14 probe (temporary, not committed; ported by hand to the
dispatch call on HEAD) and the same 340 s route and settings: 3 s blocks rotating the dispatch of `0x3666fa39`
issued or left out, crossed with a 10 us busy-wait after every draw. On both builds the probe left out every call
in its arms. Blocks are matched to the per-second samples by the midpoint of each sample interval; this method
gives back the 2026-11-14 table from its own run within 1.6 ms (frames exact).

Neither run reached the 2026-11-14 scene (the controller in the sand, about 752 draws a frame): both walked on to
the rock with the objective marker. From block 16 to the end (seven or eight blocks per arm) HEAD stood next to
the rock looking up the dune and the base stood back facing the rock:

| Build | Arm | Draws | Frame | Engine busy | Engine work a frame | Command runs a frame | Fence waits a frame |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `575cb70f8` | Program runs | 348 | 41.7 ms | 94 % | 36.8 ms | 30.9 ms | 13.0 ms |
| `575cb70f8` | Program left out | 347 | 25.9 ms | 92 % | 24.8 ms | 20.2 ms | 1.9 ms |
| `fe853a27b` | Program runs | 608 | 47.6 ms | 94 % | 44.1 ms | 31.2 ms | 8.2 ms |
| `fe853a27b` | Program left out | 615 | 36.1 ms | 85 % | 30.9 ms | 30.1 ms | 0.6 ms |

Command runs are thread time summed over the block, divided by flips.

- **With the program the GPU sets the frame on both builds.** The added 3.5 ms (HEAD) and 6.2 ms (base) of
  command work change the frame by at most 0.4 ms (k about 0).
- **Without it HEAD is no longer purely command-bound.** The base still pays 0.82 ms of frame per ms added to the
  command thread (36.1 -> 41.1 ms for 6.1 ms), close to the 2026-11-14 k = 1.02. On HEAD the frame moves 1.8 ms
  for 3.5 ms (k about 0.5) with the engine busy 92 % of the frame. The views differ, so this is not attributed to
  a commit.
- **Same spot, both builds.** In its first four blocks (341-358 draws, one block per arm, right after the first
  dispatch, which the 2026-11-14 protocol leaves out) the base stood where HEAD stood; the 197 s captures show the
  same rock and slope with the base's camera slightly closer. Probe frame times: base 40.2 and 39.5 ms with the
  program, 27.2 and 30.0 ms without; HEAD's first four blocks 42.3 and 41.3 ms, 26.8 and 27.9 ms, and its medians
  above. No GPU regression shows at this work; the base sample is one block per arm. Per-block engine work is
  noisier than the probe's frame times (HEAD reads 30-37 ms in program blocks at the same 42 ms frame).
- **Near the 2026-11-14 draw count.** While walking, the base crossed 766-780 draws a frame (blocks 6-8): blocks 6 and 8,
  with the program, ran 56 and 58 ms frames with 47-48 ms engine work, against 52.6 and 46.6 ms on 2026-11-14 at 752 draws.
  HEAD was not measured at that count.
- **Budget for 30 fps in HEAD's view.** Frame 41.7 ms, engine work 36.8 ms: the GPU is 3.5 ms over 33.3 ms with the
  lighting program and 8.5 ms under it without. The command path (25.9 ms frames without the program) is not the
  limit there.

**Unprobed runs.** One warm run per build of the 225 s route earlier the same day, medians of 165-215 s grouped by
draws a frame (the scenes differed; the HEAD binary carried the stamp `1d35b0a31` plus the then-uncommitted
null export fix, with the same runtime source as `253d981a9`): in the shared 400-560 band the base ran 43.9 ms frames with 38.6 ms engine
work and 67.7 us of command time a draw (436 draws), HEAD 46.0 ms, 44.5 ms and 58.5 us (539 draws). In an earlier
HEAD run that left the route, one window of 1,150-1,230 draws took 67-73 ms of engine work against 38-40 ms on the
base at a similar time; that scene was not compared and stays an open lead.

Route outcomes on the 225 s route: both base runs reached the dunes; of the HEAD runs, the cold warm-up reached
only the world map, one left the route for the ship crowd cinematic, one stopped at 140 s and one reached the
dunes. The stop was a pixel program on the per-lane fragment path refused at its null export; fixed in
`253d981a9`, where a null export emits nothing after the valid-mask bit has set which lanes stay alive.

## Open: depth range reused as a storage view at about 87 s (2026-11-18)

`PPSA21567 - Astro Bot` exits with code 65 at about 87 s in some dunes-route runs on `4bf5efeb9`
(two of six runs that day, one of them with temporary diagnostics, plus one on `3268e4c21`); the sample is too
small for a rate. The stop is
`unknown relation: DepthStencilBuffer - Crosses - StorageTexture` in `GpuMemoryCreate.cpp` (the relation switch's
`default`, line 1898 at `4bf5efeb9`): a 778,752-byte storage texture is created 0x1ba0000 bytes into the depth
plane of a live 3840x2160 D32S8 target that has three ranges (depth, stencil, HTILE). The new range lies inside the depth
plane; the classifier reports `Crosses` against the three-range parent. The depth target is still `in_use`.
The same run family once stopped at `!create_all_the_same` at the same point, and three
diagnostic-build runs on 2026-11-18 stopped there again at about 86 s (`vaddr 0x53d8b0000`, size `0x658800`,
`StorageTexture`, one parent `Crosses` and one `Equals`). The other runs pass this point, so
it depends on timing (which object still holds the range). The existing storage-over-depth rules handle `Equals` (a
storage view of the plane) and the recycled-range cases (`Contains`), not a view inside one plane of a live
multi-range target. Suggested direction: classify the relation per range for multi-range parents, then decide
whether the guest reuses a released depth range (seed from guest memory, as for sampled textures over depth
ranges) or views live depth texels.

## Open: dunes haze and zero-filled single-texel samples (2026-11-18)

`PPSA21567 - Astro Bot` 01.018, Intel Arc A770, Mesa ANV 26.3.0 (git), `4bf5efeb9` plus a local translator change,
dunes route, 60 fps present limit, warm private caches, 175 s bounds, temporary diagnostics (not committed). It
continues the open haze item above (a white haze over Astro in the dunes).

**Excluded for the haze.**

- **Fog volume aliasing.** The volumetric fog chain (inject `5d90f514`, temporal filter `e308b341` writing a
  240x135x64 RGBA16F volume in place, integration, then the apply draw) binds its sampled and storage views to the
  same live images. The two volumes ping-pong as the guest intends.
- **Depth input of the apply draw.** It resolves depth-exact to the live depth target.
- **RenderDoc.** A capture of this scene runs the A770 out of device memory, so no frame was inspected that way.

**Found, not fixed: single texels on `kRenderTarget` (tile 27) are zero-filled.** `Gen5SampleMayGuestUploadTiled`
lets tile-27 samples upload guest bytes only for BC1. Every other format without a live surface over its range
gets a zero-filled Texture object. In the dunes, five 1x1 RGBA32F samples (format 77; one is read by a compute
program that writes a 1x1 result a later pixel shader reads) hold `(1.0, -1.0, 0.5, 0.18)`. One 1x1 R8 (format 1)
holds `0xff` and one 1x1 RG8 (format 14) holds `0x8080`. The R8 and RG8 are sampled by draws into a three-target
1920-wide pass. The bytes stayed the same for the whole run. Each RGBA32F texel lies exactly under a writable 64 KiB
storage buffer. The provenance query reports that buffer as GPU written back, and the texture as uploaded
again afterwards. Whether that write-back covers the 16 bytes of the texel was not checked, so it is open
whether the guest bytes are the content these samples should read. The R8 and RG8 ranges have no other GPU object over them. Element (0,0) of slice 0 sits at offset 0 in `SW_64KB_R_X` for every element
size, so these samples can be read as linear.

- **Uploading them turns the frame black.** A trial uploaded uncovered one-element 2D tile-27 samples as linear.
  Every capture at 140, 150 and 160 s was exactly `(0,0,0)`, HUD included, while the GPU stayed about 92 % busy.
  The frame was black with formats 1 and 14 only, and black with format 77 only. Earlier runs on the same base without the trial show the dunes. A frame of exact zeros with the HUD gone fits a NaN reaching the final blend.
  The zero fill seems to hide a fault further down. The trial is parked and the zero fill is unchanged.
- **Suggested direction.** Follow the format-77 chain to the first target that turns non-finite: the compute
  program that writes the 1x1, then the pixel shader that reads it. Do this before allowing the upload again.
- One further sample, a 1x1 RGBA16F at the start of a live 3840x2160 target, also holds non-zero guest bytes. It
  stays zero-filled because nothing shows those bytes are the content.
- **Excluded (2026-11-19): the buffer content of the covered texels as the haze.** A temporary build copied each
  zero-filled one-element 2D sample that lies inside a single storage buffer from that buffer on the GPU, in
  submission order, instead of zero-filling it. It engaged on three texels, all at offset 0 of a 64 KiB storage
  buffer inside an in-place guest window: two RGBA32F (format 77, guest bytes `(1, -1, 0.5, 0.18)` at record time)
  and one RGBA16F (format 71) at `0x532830000`. The frame stayed normal, not black as with the guest-byte upload, and
  the second-rock captures at 120, 140 and 160 s matched the zero-fill baseline (grey mean and deviation 212/29,
  204/23, 203/25 against 212/28, 204/23, 203/25; one run, plus one crash-site run whose captures look as washed as
  before). Frame rate was unchanged (27.7 to 28.7 fps). The change is parked outside the tree; what value the GPU
  holds there at the read was not read back.

**Comment fixed.** `ResolveGen5SampleBacking` said `Unsupported` stops the run. It only logs, and the caller still
creates a zero-filled Texture object.

**Black flicker (user report), not reproduced.** With warm caches the per-second samples show a single graphics
pipeline miss in the window (279 ms at 135 s, of which 146 ms in pipeline creation) and no cache checkpoint. An
earlier run with a less warm cache spent 0.6-1.06 s in single misses and 399 ms in one 101 MB driver-cache
checkpoint. Both run on the thread that records draws, so they freeze frames rather than blacken them. The link to
the flicker is not shown.

**Other exits seen that day.** One run with temporary diagnostics stopped on a guest call to address 0 (`rcx`
`0xdeadbeef5533ccaa`). One stopped at 124 s with exit code 0, no error and no captures; its cause was not found.

**Excluded: translator version 245.** It rewrote partially out-of-range scalar loads as straight-line code. In
`vs_0x57572d59` spills rose from 93 to 103 and instructions from 4485 to 4578, and in `vs_0x4e1ef13e` spills rose
from 52 to 57. It was not kept.

**Translator version 246.** Scalar buffer loads by device address are now sized by `num_records` times the stride,
or by bytes when the stride is 0, as the other scalar buffer path already does. This follows the RDNA2 ISA, where the
stride of a scalar buffer load serves only the bounds check. No run showed it engaging, and the haze is unchanged with
it. On 2026-11-19 `PPSA17221 - Minecraft` reached its world with it (the save ends on the death screen) with no
recorded error, at 60 fps with the present limit.


## Open: a transient of extra draws in the dunes (2026-11-19, measured)

Setup: `PPSA21567 - Astro Bot`, the dunes route, A770 with ANV, 60 fps present limit, translator version 246, warm
private caches, two runs of about 200 and 235 s. After the route ends (about 140 s), the agent pad command sends three
jumps, and native captures are taken right after them and 5, 10 and 20 s later.

- **The route does not leave the robot in one place.** With identical input it ended near the first rock in one run
  and at the second rock in the next, and earlier runs left it at the crash site. Boot speed shifts every timed input.
  Per-window fps from different runs therefore compares different scenes; compare draws per frame and the capture.
- **A transient of extra draws.** Near the first rock one run held 1000 to 2300 draws per frame at 7 to 12 fps for
  about 40 s. The count then fell smoothly to about 540, and the run held 29 fps. Two earlier runs show the same decay
  from about 1250 to about 300 draws per frame, ending at 33 to 34 fps. During the slow phase every per-draw stage cost
  about three times more: about 64 against 24 us per draw on the command processor, with the texture descriptor at 16
  against 5 us and guest address population at 0.68 against 0.13 calls per draw. The command processor was busy about
  91 % of each window. No pipeline misses were counted. The extra draws were not identified.
- **Jumps on settled ground add no lasting draws.** At the second rock the run held 27 to 28 fps at 520 to 700 draws
  per frame for 95 s. Three jumps left no extra draws beyond the windows that took a capture, and the dust was gone by
  the capture 10 s later. Sand left in the air was not reproduced in either run.

- **The extra draws are one particle program.** A temporary per-frame histogram by shader pair (not committed)
  reproduced the transient in one of the four later runs that sampled from 135 s; the others may have reached it
  earlier, since they began at 520 to 700 draws per frame and fell from there. At its peak one pair, VS checksum `360038d0f4cfb9a5` with PS
  `2100361059f866a6`, issued 1167 of 1838 draws in a frame, each a point list of 2 vertices. It fell to 912 and 591
  draws 100 and 200 presents later, and was absent at about 530 draws per frame once settled. The other top pairs
  stayed the same in both phases. Hypothesis, not checked: the particles expire after a number of presented frames
  rather than seconds, so a slower frame rate keeps them longer and they in turn keep the frame rate down.
- **Profile of the settled phase.** User-space samples of the busiest thread (about 60 to 66 % of one core at 28 fps)
  are flat: the AVX2 index scan 7 %, `memcmp` 6 %, `GetQueryPoolResults` 4 %, and texture preparation 2 %. At 28 fps
  that thread is not saturated, so the settled scene is limited elsewhere. Two runs meant to profile the transient
  did not show it: the robot reached the second rock before sampling began.

- **The particle program is the geometry program `0xf4cfb9a5`.** Its vertex checksum ends in `f4cfb9a5`, the
  geometry pass of the 2026-11-09 records above: one guest group per point draw, run on this host as SIMD8 with
  44,571 instructions and 862 spills. A profile from 2026-11-18 timed its passes at about 54 us each, 73 calls in a
  settled frame. A run sampled from 92 s caught the transient as the route's walking and jumps began: 1150 to 1250 draws
  per frame at 15 fps from 118 to 142 s, with the render engine 93 % busy (deltas of the engine's busy cycles), and
  then 600 draws per frame at 24 fps with it 95 % busy. In that run the transient was limited by the GPU; in the
  first run the command processor was also 91 % busy. Both sides pay per pass.

- **Settled passes are batched.** Temporary counters (not committed) on segment placement, one 171 s run that left the
  robot at the crash site at 23 fps and about 380 draws per frame, without the transient. From 104 to 164 s, 46,900
  geometry passes (about 34 per frame) went into 5,942 segments, about 8 passes per segment; 87 % of placements joined
  an open segment. Of the new segments, 4,474 followed a recording end and 1,468 had a command recorded outside a
  render pass as the last hazard before them (the counter kept only the last one, so a packet before that command is
  not seen); no pass ran inline. Passes in
  a segment overlap (2026-11-09 records), so at that rate the passes are not what holds the settled frame. How the
  transient's passes segment was not measured.
- **Excluded at the second rock: the particle program as cost or veil.** A temporary build skipped every draw of VS
  `360038d0f4cfb9a5` in alternate 5 s windows; captures every 3 s. Settled, that VS draws only 8 to 11 times a frame
  (with PS `21001960c4a8f3bb` in the histogram run), so the test covers those draws only. At the second rock, skip windows ran 28.0
  and 27.4 fps against 28.2, 27.0 and 27.8 with the draws, and mean hue, saturation and lightness of the captures
  matched within 0.004 between arms. Not tested with the skip: the close shots where the robot looks washed white on the
  ground and blue in the air (crash site and first rock, 2026-11-19 captures), nor the transient.
- **The dunes white haze shows on close shots of the robot.** At the crash site at 23 fps the robot's face is washed
  white; near the first rock it is washed on the ground and looks normal at the top of a jump. Cause not identified;
  the single-texel lead above is still open.
- **The transient's passes are batched too.** In the run sampled from 92 s, render pass begins stayed at 42 to 45 a
  frame through the transient (1150 to 1240 draws a frame), as in the settled phase (40 to 42), and dispatches rose
  from about 89 to about 205 a frame. A segment per particle draw would have added about one begin per draw. The
  transient's cost is therefore GPU work inside the segments and pull draws, not segment breaks.

- **Crash site: the sky and cloud program `0x874bd13f` is about 10 ms of GPU time a frame (2026-11-19, t246).**
  Temporary per-dispatch GPU stamps (not committed), one 171 s run that settled at the crash site at 23.5 fps and
  about 370 draws per frame, render engine 97 % busy: compute 25 ms a frame, of which `0x874bd13f` (one call of
  `120x68x1` groups of 64 invocations) 12.3 ms and `0x3666fa39` 5.6 ms. A second build skipped that one dispatch: in
  the same scene (capture checked) at about 375 draws per frame the frame held 31 to 34 fps, engine 93 to 96 %
  busy, and the stamped command-buffer time fell from 41.0 to 30.6 ms a frame. One pair of runs. The skip capture
  shows what the program draws: the sky and clouds, replaced by stale blue bands. Its cost follows how much sky the
  view shows (same grid and SPIR-V: 1.5 to 2.9 ms in the hub, 5.5 to 8 ms during the transient, 12 ms at the crash
  site). This amends the 2026-11-14 record above that it freed no engine time (t216, close-up dunes view).
- **Its width is set by the native wave32 rule, and SIMD32 spills on the A770.** Before `11f783e44` wave32 programs stayed native,
  pinned to 32 lanes, up to 64 guest VGPRs. Offline on ANV the live SPIR-V at 32 lanes is 77,775 instructions with 2,819:9,569
  spills:fills; the same SPIR-V unpinned compiles at 8 lanes to 11,309 with 14:30. A temporary arm that split every
  wave32 program into 16-lane parts produced 42,718 instructions and 1,171:4,213 at 16 lanes for this program; an
  8-lane wave32 split is refused by the native-wave validator (only 16 is accepted). Both runs with the 16-lane arm
  left the robot at the second rock (27 to 28 fps at about 600 draws per frame, as without it), so its effect at
  the crash site is not measured.
- **Bound only: the EXEC guards.** Replacing the program's 3,701 EXEC-guard selects with their new value (not a valid
  translation) gives 51,711 instructions and 2,388 spills at 32 lanes, 32 % fewer estimated cycles, and still spills.
  The guards sit across 142 EXEC segments with almost none before the first EXEC change, so a sound "EXEC is full"
  subset would recover little; parked.
- **Excluded: 16-lane parts for wave64 programs above 64 VGPRs as a lever on this program.** Its SPIR-V is identical with
  and without that arm (it is native wave32); the arm's run reached the hub, so nothing else about it was measured.
- **The cloud program is not the haze.** With it skipped the crashed ship looks as washed as with it. In both crash-site
  runs the capture at 120 s is a dark frame without a frame-rate dip; whether that is a game fade or the reported
  black flicker is not decided.
- **Wave32 above 32 VGPRs now splits on hosts that go down to width 8 (2026-11-19, `11f783e44`).** A 32-lane hardware
  thread there holds 32 dwords of registers per lane, so the native wave32 budget is 32 guest VGPRs on such hosts and
  stays 64 elsewhere. In the dunes this splits `0x874bd13f`, `0xe1b1315d`, `0x2bcba9a2` and `0xbf8ee9fc` into 16-lane
  parts and keeps `0xfd1ee59c` native (SPIR-V compared against the arm that split every wave32 program). Stamped per
  dispatch, `0x874bd13f` took 2.5 to 2.6 ms split against 5.8 to 5.9 ms native at the second rock (two split runs, one
  native run, same scene) and 2.5 to 3.3 ms against 5.6 to 6.8 ms in the dunes intro of the idle route (one pair).
  Frame rate did not move where measured: the idle route's settled view holds 34 fps in both arms (the program is not
  in that view's per-frame list), and a clean build of the commit at the second rock held 28 fps (171 s, render engine
  95 to 96 % busy) against 27 to 28. At the crash site a clean build of the commit held 28.8 fps (27 to 31, 46
  samples, 376 draws per frame, engine 96 % busy, capture: the crashed ship close-up) against 23.4 fps (21 to 24, 377
  draws per frame) in the stamped native run of the same scene; stamps do not move the frame rate at the second rock
  (27 to 28 fps with and without). That run entered the dunes about 7 s later than the others (60 fps at about 45
  draws per frame until 99 s, after a stall at 90 to 91 s), which shifted the wall-clock route. Its cache copy lacked
  the split variants, which were translated at 84 to 85 s, and the stall second at 90 s shows 6 compute pipeline
  misses, so building them probably caused the delay (not proven). A native run on the same build entered on time and
  reached the second rock, so the pair is not from one build. `0xbf8ee9fc` has no per-dispatch number. `PPSA17221 -
  Minecraft` reached the world with exit 0 and a clean capture; that run compiled only two fragment modules, so the
  rule changed none of the compute programs it dispatched.

- **Still pinned at 32 in the dunes, and cheap.** The rule keeps `0x939e8346`, `0xeb964a28` and `0xfd1ee59c` native;
  they cost under 0.1 ms a frame (below the per-frame list, or 0.07 ms), so their width is not a lever. `0xfd1ee59c`
  spills 32:155 at 32 lanes offline.
- **Bound only: EXEC guards in the five-target pass.** Replacing the 361 EXEC-guard selects of `ps 0x6bcfb82d` with
  their new value (not a valid translation) lowers ANV's cycle estimate of its two SIMD16 executables by 27 % (25,415
  to 18,631) and 18 % (31,607 to 26,022). An earlier record found per-draw time falling far less than such estimates,
  and the skip test caps this pass at about 3 ms, so a sound subset would recover well under the 2.4 ms needed;
  parked. The BVH kernel `0x3666fa39` already compiles at 8 lanes (272:928 spills), so its width was not pursued.
- **The transient is one geometry program.** In the one-frame windows of a stamped run that crossed the transient
  (1,064 to 1,572 draws a frame), `0xf4cfb9a5` ran 436 to 1,155 one-group passes a frame and its stamps summed 10.6 to
  31.7 ms, against 12 to 13 ms for all render passes. The stamps are end-of-pipe timestamps around each pass, which
  serialize the passes, so the sum overstates their cost when they overlap.

- **Second rock: no single program holds the frame.** Stamped there with the 16-lane wave32 arm (its 27 to 28 fps
  matches the native runs): render passes 22 ms, compute 11 ms, geometry passes 4 ms a frame; the largest items are
  the BVH kernel `0x3666fa39` (5.5 ms) and a five-target pass keyed by `ps 0x6bcfb82d` (8.7 ms over its 144 draws,
  other programs included). Skipping every draw of `ps 0x6bcfb82d` (temporary build at native wave32 width; capture:
  same rock, props apparently missing, not verified) gave 29 to 30 fps against 27 to 28 and render passes 18.7 ms
  against 21.8: about 3 ms. That run had windows of low engine busy, so its frame rate is indicative only.

- **Second rock at 960x540 (game patch, diagnostic only).** 32 to 33.6 fps at about 556 draws a frame and 90 % engine
  busy, against 27 to 28 fps at 530 draws and 96 % at native resolution: pixel-bound work is about 5 to 6 ms of the
  frame and about 28 ms does not scale with resolution.
- **Guest compute queues 0 to 7, priced at the crash site.** With the rule, compute dispatches from those queues sum
  about 2.2 ms a frame in end-of-pipe stamps (their command buffers about 2.6 ms) of 18 ms of compute; the BVH kernel
  `0x3666fa39` (5.8 ms) comes from the graphics queue. Running them on a separate host compute queue could overlap at
  most that 2.2 ms and needs depth copies on a compute queue, a recorded exclusion on this host; parked.
- **The particle transient is not the haze.** In `hist1` the robot looks washed white both in a capture inside the
  transient and in a settled one. At the crash site the wash varies between captures at the same frame rate, with
  different framing, so it is not tied to one camera view. The 1x1 exposure chain (format 77, zero-filled) remains
  the unproven lead.
- **Per-lane proof for the split wave32 programs: none qualifies; parked (2026-11-19).** A temporary print ran the
  wave-width independence proof (the one that lets split wave64 programs run one guest lane per invocation without
  exchanges) on the 15 wave32 programs that use wave state on the dunes route. None passed: ten stop at the same
  early instruction with an EXEC write by a plain scalar instruction, `0x874bd13f` (clouds) and `0x2bcba9a2` at an
  operand the proof cannot keep per lane, `0xbf8ee9fc` and `0xe1b1315d` at further EXEC writes. The proof is written
  for wave64: it refuses 32-bit SAVEEXEC and every EXEC-conditional back edge. The clouds program runs 31 loops with
  its ballots inside them; whether they are EXEC-narrowed is unchecked (it stops at 0xa8, before any back-edge check,
  and no guest disassembly was taken). Admitting it would need a wave32 form of the proof, plus a route from a proven
  wave32 program to the per-lane mask emitter (the per-lane layout is built for guest wave64 only). A ceiling probe that let the 8x8 split wave32 programs skip the rendezvous wait (ballots kept
  only their own part's bits) landed at the second rock, where `0x874bd13f` costs 0.17 to 0.20 ms in every build;
  in its only frames before the rock (frames 3060 and 3100: 36 to 41 passes, 79 dispatches, the same view as frames
  3100 to 3140 of the rule runs `rule32a` and `idle-rule`) it took 4.8 ms against 2.56 ms with the rendezvous. The
  probe changed the program's work (each part saw only its own lanes in every ballot), so no ceiling was obtained. For the BVH kernel the fixed mechanics of its rendezvous were
  priced at about 4 % of its time (record of 2026-11-14; the waits between parts were not priced), so this route is
  not expected to give the missing 2.4 ms there either.
- **Second rock, per-program stamps (2026-11-19, frames from 4,600, 744 to 857 draws a frame).** The five-target pass
  (1920x1080, 121 draws; the stamp names a pass by its first draw's programs, here `ps 0x6bcfb82d`, so the time
  covers every program in the pass) 7.2 to 7.3 ms a pass, the BVH kernel 5.7 ms, the 2048x2048 shadow passes of
  `ps 0x0ff8a6ab` (6 passes, 280 draws) 4.7 ms, a one-target pass of `ps 0x860593d6` (120 draws) 2.4 ms, compute
  `0xff2adfc8` 1.2 ms. The five-target pass was recorded vertex-bound (2026-10-30 record above).
- **Bound only: guest-address lookups in the largest five-target vertex program.** The largest dumped vertex module
  with five colour targets (195k SPIR-V words) compiles at width 8 to 21,430 instructions, 234 sends, 30 loops and
  376:1,559 spills:fills. 24 of its loops are the in-shader guest-address lookup, inlined once per `S_LOAD_DWORDX8`
  (12 loads, each with a second lookup when it crosses a page). Their bases are SGPR pairs copied straight from user
  data, not the EUD pointer, so they get no dynamic scalar-load record: those records cover only EUD loads, and the
  pointer-table collector is not run for vertex programs. With the lookup body replaced by a plain return
  (offline, unsound), the module compiles to 17,143 instructions, 165 sends and 266:1,120 spills:fills, with
  estimated cycles 9 % lower. The one-entry lookup cache measured on the BVH kernel gained under 2 % (record of
  2026-10-29), and which vertex module the pass spends its time in is not matched, so this lever is not expected
  to reach 2.4 ms; parked. Correction (2026-11-19): that module (`vs 0x8b21b61c`) is not among the vertex programs
  that carry the pass's work (bullet below), and its figures are static compiler estimates, not measured time.
- **The five-target pass is mostly pre-raster work (2026-11-19, `vtx5`).** A temporary build set a 1x1 scissor on
  every draw with five colour targets (engaged: the captured frame is visibly broken). The pass still took 5.6 to
  6.8 ms against 6.4 to 7.7 ms at matched draw counts, so about 1 ms of it is rasterization and pixel work and the
  rest is vertex shading, primitive setup or per-draw work. Counted over the last print window of that run (20,000 five-target draws;
  index count times instance count, not vertex invocations, raw window totals), five vertex programs carry 96 %:

  | Vertex program | Pixel program | Draws | Index x instance | Share | Width-8 instructions | Cycles (static) | Spills:fills |
  | --- | --- | --- | --- | --- | --- | --- | --- |
  | `0xa2bb8f2d` | `0xd4bef057` | 6,653 | 480 M | 39 % | 1,226 | 38,976 | 0:0 |
  | `0x4ea3083b` | `0x8ca0b9ff` | 6,368 | 231 M | 19 % | 1,880 | 49,993 | 0:0 |
  | `0x0c2ae56d` | `0x6bcfb82d` | 404 | 222 M | 18 % | 6,951 | 179,955 | 332:232 |
  | `0x67d7f692` | `0x6bcfb82d` | 2,425 | 135 M | 11 % | 1,811 | 49,365 | 0:0 |
  | `0x57572d59` | `0xd4bef057` | 671 | 130 M | 10 % | 4,104 | 92,846 | 66:61 |

  `0x0c2ae56d` averages about 550,000 index-instances a draw; weighted by static cycles it is the largest item,
  but static cycles have tracked measured time poorly on this host, so no program is priced yet.
- **Second rock, two vertex programs priced by skip windows (2026-11-19, `skarm1`).** A temporary build skipped the
  draws of one vertex program in alternating 80-frame windows of one run (none, `0xa2bb8f2d`, none, `0x0c2ae56d`;
  each window printed its skipped-draw count: about 3,950 and 240) with no other change. Comparing stamped frames
  from frame 4,740 (rock 2 in the captures) whose five-target draw counts match the arm, the five-target pass stamps
  summed 8.3 to 8.7 ms a frame unskipped (143 to 152 draws), 5.7 to 6.3 ms with `0xa2bb8f2d` skipped (96 to 98
  draws; about 49 draws and 3.5 M indexes a frame removed) and 6.3 to 6.6 ms with `0x0c2ae56d` skipped (140 to 147
  draws; 3 draws a frame removed, each about 550,000 indexes of one instance). Frames with fewer draws are another
  view and were left out. So `0xa2bb8f2d` holds about 2.2 ms (0.6 ns an index) and `0x0c2ae56d` about 1.8 ms
  (1.0 ns an index): a ratio of 1.6 an index against 4.6 in static cycles. Indexes are not vertex invocations and the
  two meshes' vertex reuse is unknown, so two programs cannot separate a per-vertex floor from shader weight. The
  stamps serialize the passes and the per-frame print keeps only the 12 largest pass groups (dropped groups are
  under 0.25 ms); these are pass times, not frame-rate gains. A capture inside a `0xa2bb8f2d` skip window (frame
  5,235) shows the rocks, sand and props of the unskipped capture at frame 4,711 (the camera moved, so a pixel diff
  is not conclusive); what those 49 draws a frame put on screen in this view is not identified.
- **Second rock, three more vertex programs by skip windows (2026-11-19, `skarm2`, same design: none, `0x4ea3083b`,
  `0x67d7f692`, `0x57572d59`).** At matched draw counts the pass summed about 8.3 ms unskipped (142 to 150 draws),
  6.2 ms with `0x4ea3083b` skipped (about 48 draws a frame removed, 36,000 index-instances a draw), 7.5 ms with
  `0x67d7f692` skipped (18 draws, 56,000 indexes a draw) and 7.7 ms with `0x57572d59` skipped (two frames at 140 draws;
  6 draws, 193,000 indexes a draw). With `skarm1`, the five programs cost about 44, 44 and 47 us a draw for the three
  light ones (`0xa2bb8f2d`, `0x4ea3083b`, `0x67d7f692`, whose indexes a draw differ by 2x), 100 us for `0x57572d59`
  (two frames only) and 600 us for `0x0c2ae56d`; the five savings sum to about 7.5 of the pass's 8.3 ms. The equal
  cost a draw of the three light programs suggests a per-draw term, but it is not established: the one-target pass of
  `ps 0x860593d6` (2.4 ms, 120 draws) and the shadow passes (4.7 ms, 280 draws) cost 17 to 20 us a draw. The captures
  (frames 4,722 and 5,298) are at rock 2; in the `0x57572d59` window the robot looks missing.
- **Second rock: the five-target pass cost is not per draw (2026-11-19, `half1`).** Same window design, halving the
  index count (rounded down to a multiple of 12) of every draw of one vertex program: none, `0xa2bb8f2d`, none,
  `0x4ea3083b` (about 3,900 halved draws a window each; engaged: index-instances a draw averaged 0.88 and 0.91 of
  their `skarm2` values over the run, where one halved window in four predicts 0.875). At 141 to 150 five-target
  draws the pass summed 8.5 ms unhalved (four frames), 7.3 ms with `0xa2bb8f2d` halved (one frame) and 7.4 to 7.7 ms
  with `0x4ea3083b` halved (four frames): about half of the 2.2 and 2.1 ms their skips removed, so their cost scales
  with the work of each draw (vertices, primitives and pixels together). The one-target pass of `ps 0x860593d6` held
  111 draws and 2.33 to 2.37 ms in the same run, and its draws counted 6.4 G index-instances against 6.0 G for the
  five-target draws over frames 4,741 to 5,364 (the latter about 0.4 G low, since it includes the halved windows).
  Its two vertex programs, compiled alone (no pixel module was dumped for it), are `0x5c9f3e0a` (755 instructions,
  6 loops, 16 sends at width 8; 4.5 G of the index-instances) and `0x51485353` (1,906, 19 and 42; 1.8 G), against
  1,226, 11 and 29 for `0xa2bb8f2d`. Same index volume in 2.35 ms puts a ceiling of about 6 ms, less about 1 ms of
  raster (`vtx5`), on what a cheaper vertex stage could recover; that ceiling includes the guest's own attribute work.
  Vertex reuse of the meshes is unknown.
- **Where the vertex programs' guest-address lookups come from (2026-11-19, offline).** ANV's final intermediate code
  keeps uniform and divergent lookup loops of: `0xa2bb8f2d` 3 and 8, `0x4ea3083b` 4 and 9, `0x67d7f692` 4 and 9,
  `0x57572d59` 3 and 20, `0x0c2ae56d` 5 and 20, and for the one-target pass `0x5c9f3e0a` 2 and 4, `0x51485353` 3 and
  16 (each module compiled with the pixel module of its pass, or alone where none was dumped). Correction, same day:
  most divergent loops are not lookups but the partial-range branch of by-address row loads of three or more words,
  a non-unrolled word loop (rewriting it is excluded below); replacing the lookups with stubs offline left
  the loop count unchanged. In `0xa2bb8f2d` the
  divergent ones are buffer loads whose index is built from packed register bits (an 8-bit field, a bit-field
  extract), not the vertex index, all on one buffer descriptor loaded once; such loads cannot become native vertex
  attributes, which need a format load indexed by the vertex or instance id from the vertex-buffer table. `0x51485353`
  keeps 16 divergent lookups and still fits, with `0x5c9f3e0a`, in the 2.35 ms one-target pass at about 3 M
  index-instances a frame, so the divergent lookup count alone does not explain the five-target programs' cost.
- **Pipeline statistics per render pass (2026-11-19, `PPSA21567 - Astro Bot`, A770 ANV, diagnostic build).** Vertex-shader,
  clipping and fragment-shader invocation counts were read per pass beside the timestamps. Over 67 profiled frames of
  one run the five-target pass fits `ms = 2.27 x M vertex invocations + 0.60 x M fragment invocations + 0.155 x passes`
  (rms 0.26 ms). Scenes: at rock 2 the pass runs about 260 draws in 8.3-9.3 ms; the 130-140 draw blocks are the
  rock-1 area. With the five targets scissored to one pixel the fragment count reads 0, and 2.19 M vertex invocations
  still take 5.1-5.6 ms (fit: 5.3 ms), so that pass is bound by vertex work; in a lighter scene (0.75 M) it leaves about
  0.6 ms the fit does not explain. The one-target pass `ps 0x860593d6` runs 1.6-2.1 M invocations at 1.05-1.35 ms per
  million with `vs 0x51485353`, a statically larger program than `0xa2bb8f2d` (1906 against 1226 instructions, 42
  against 29 memory messages). Correction, same day: the pass label is its first draw only, `0x5c9f3e0a` carries
  about 71 % of that pass's index volume, and the per-program halving prices below show cost follows the program. Vertex
  invocations per index are 0.32 in the five-target pass and 0.23 in the one-target pass. The "halving saves about
  half" result above also halved fragments for `0x4ea3083b` (-0.17 M vertex, -1.0 M fragment invocations); for
  `0xa2bb8f2d` the fragment count did not move.
- **The five-target draws are not an equal-depth pass over a prepass (2026-11-19).** All 120 sampled draws of the five
  programs test depth less-or-equal with depth writes and stencil on, and none shares an index range and vertex offset
  with a one-target draw. Re-keyed on the index-buffer address alone (296 one-target entries recorded, engaged), no
  five-target draw reuses a one-target index buffer either. The one-target pass still acts as a depth prepass for
  the scene: halving `0x5c9f3e0a` raised the five-target fragment invocations from 2.7 to 5.15 M at rock 2.
- **Excluded: doubling the vertex programs' guest-address lookups as a price probe (2026-11-19).** A second lookup
  chained on the first through a runtime zero reached the SPIR-V (13 calls into the wrapper) but ANV compiled
  `0xa2bb8f2d` to the identical ISA, so this probe cannot price the lookups on this driver.
- **Per-program vertex prices at rock 2 (2026-11-19, halving arms with pipeline statistics, diagnostic build).** Halving
  one program's index count per 80-frame window and dividing the pass-time change by the vertex-invocation change,
  with the fragment term of the fit removed, gives in ms per million vertex invocations: `0x0c2ae56d` about 4.5 (about
  6 in a lighter scene), `0xa2bb8f2d` 1.5-1.9, `0x51485353` 1.45-1.6, `0x5c9f3e0a` 0.5-0.8. Cost follows the program.
- **Excluded: straight-line partial-range loads in the vertex programs (2026-11-19).** The partial branch of by-address
  row loads of three or more words (taken only when the row's last word is past the backed bytes) was rewritten from
  a non-unrolled word loop into one branch on word 0 plus a select per word, which is equivalent. Offline, ANV's code
  for the five programs keeps no loops (`0x0c2ae56d` 25 to 0, spills:fills 337:234 to 308:203; `0x57572d59` fills 76 to
  44) but grows 2-20 % in instructions with about twice the memory messages. Three runs with a private translation
  cache: the five-target pass sits +0.59 ms (median of 12 frames) above the pipeline-statistics fit, against +0.57 ms
  for the old code at rock 2; at 2.50 M vertex invocations both take 6.7 ms. The hot whole-range path is unchanged
  and reshaping the cold branch does not move it. A probe that read zeros for partial rows instead also left the
  route; a run whose native code was identical to the baseline but used the same translation-cache directory left
  it too, so that desync and the probe's one exit 139 are unattributed, not evidence of a guest-visible change.
- **Excluded: merging the per-word raw loads of the vertex programs into four-word loads (2026-11-19).** Offline
  gate only: with the whole-range path emitting one four-word typed load, ANV's code for `vs 0x0c2ae56d` still
  issued 52 single-dword bound-buffer loads, so the driver does not vectorize them and the change was not run.
  Stripping the EXEC selects from the same program (unsound, ceiling only) cuts its native instructions by 3 %
  (7,026 to 6,793), too little to reach the 2.4 ms rock 2 needs.
- **Excluded: the vertex stage is not inherently slower than compute on the A770 (2026-11-19).** A standalone
  Vulkan test (no emulator) ran one body as a vertex shader with rasterizer discard and as a 64-wide dispatch,
  2 M invocations each, timestamps around each stage, every invocation's store checked. A chain of dependent loads
  takes the same time in both stages (ratio 1.0-1.2 for 4 and 64 MiB tables, 8 and 64 steps; separate submits
  agree), and a pure-ALU body runs 2.1 times slower as a vertex shader, scaling linearly with its length. Moving
  vertex work to a compute pre-pass would therefore gain at most that factor on ALU and nothing on memory-bound
  work, before paying to write and re-read the outputs. Output slots cost nothing up to 4 vec4 and about 0.08-0.1
  ns per slot per vertex above that, but they do not explain the per-program prices: the dumped programs emit 8
  slots (`0x0c2ae56d`, about 4.5 ms per M), 4 (`0xa2bb8f2d`, 1.5-1.9), 1 (`0x51485353`, 1.5) and 1 (`0x5c9f3e0a`,
  0.5-0.8). The five-target pass at rock 2 runs 0.91-0.98 vertex invocations per primitive; how far that exceeds
  the unique vertices of these meshes is not measured.
- **Excluded: host primitive restart as a vertex-reuse lever (2026-11-19, `rst1`).** Turning restart on raises
  ANV's reuse window and lowered the title's invocations per primitive by 8-12 %, but makes indexed draws several
  times slower on the A770. The guest enables primitive reset and Kyty ignores it; see `docs/bringup/cross-title.md`,
  "Guest primitive reset is ignored; host restart is slow on ANV". That run ended in the known guest null-call exit
  139 while loading `hub_crashsite_tutorial`, after reaching it later than the baseline runs.
- **Excluded: a per-invocation cache of the last guest-address entry (2026-11-19).** The lookup kept the last
  matched entry in private variables and returned early on a hit (translator 247, temporary). The dumped
  `vs 0xa2bb8f2d` carries the cache (13 calls to the lookup, a different source hash) yet compiles to the same 1,226
  instructions with identical native code; `spirv-opt -O` also drops the four cache variables while keeping all 13
  lookups, so the cache never ran in that vertex program and its vertex-time effect is untested. The BVH kernel
  `0x3666fa39` grew from 29,954 to 35,938 instructions and 272 to 487 spills at width 8, and its crash-site stamp
  rose from 5.77 to 10.76 ms (`lkc2`). Reverted. One run of this series (`vtx2`) ended on the known intermittent
  `DepthStencilBuffer - Crosses - StorageTexture` relation exit (cross-title record).

Next: with the rule the crash site holds about 29 fps and the second rock 28, both GPU-bound at 95 to 96 % busy; 30
fps needs about 1.2 ms more at the crash site and 2.4 ms at the second rock. The largest shared item is `0x3666fa39`
(5.5 ms); at the second rock the draws of `ps 0x6bcfb82d` cost about 3 ms in the skip test, and the whole five-target
pass is mostly vertex work spread over five vertex programs, none priced by measurement yet.
