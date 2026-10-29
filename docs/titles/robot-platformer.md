# Robot platformer

Title: PPSA21567 - ASTRO BOT.

Records moved verbatim from `docs/BRINGUP.md`, newest first. The bring-up
manual keeps the invariants, methodology, frontier and an index of every
record. Titles are named by PS5 content ID and public name (`AGENTS.md`).

## Robot platformer: the clock-driven resolution controller is stereo-only, and the GPU clock rate (2026-10-29)

Applies to `PPSA21567 - Astro Bot` 01.018, Intel A770, Linux, Mesa 26.2.5,
strict 60 fps-limited runs on the dune route (gw84-gw88). Evidence came from
an uncommitted `DIAG-DRS` probe, a static read of the title's eboot and two
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
  - Next step if it recurs: correlate the faulting host sequence with a recent-draw trail that
    covers queue 7.
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
  point (gw79, `DIAG-IDXIND`):

  | Records | Count | Argument-read wait |
  | --- | --- | --- |
  | Empty | 3339 | 12.85 s |
  | Carried work | 376 | about 0 s |
  | Triangle lists with a large index buffer | 285 | about 0.1 s |

  The rule would drop real points, so it does not apply. The sync-chain design is in
  `docs/bringup/cp-gpu-sync-chain.md`.
- **Parked: `0x3666fa39` latency** (about 22 ms per call, the largest single GPU item).
  - The guest builds a deduplicated list across lanes: `s_ff1` over a compare mask, `v_readlane`
    of the found lane, and `v_writelane` into one VGPR capped at 60 entries. It does this 30 times.
  - Every step needs the whole 64-lane wave, so each is a rendezvous of all eight split parts. The
    cost follows from the guest algorithm.
  - The listing shows no translator change to test, and the offline loop-retention check used for
    the earlier spin work is not available. No change is attempted.

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
  and noisy.
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
- **Localized.** `0x3666fa39` is dispatched as a single 16x16 workgroup
  (four guest waves; 3316 instructions, 72 VGPRs, scalar find-first-set and
  `v_readlane` loops ending in an image store), so its cost is latency, not
  occupancy. As split wave64 on 16-lane host subgroups it spilled heavily:
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
