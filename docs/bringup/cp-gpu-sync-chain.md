# Command processor and GPU sync chain (design, 2026-10-29)

Status: link 1 (`DISPATCH_INDIRECT`) is implemented (2026-10-29, see "Link 1 result"). Link 2
(`DRAW_INDEX_INDIRECT`) was deprioritized on 2026-10-29 and reopened on 2026-11-10. Its
geometry-pass form was deferred the same day and then built (see "Link 2 reopened"). The other
links are design and measurements only.

## Why

In `PPSA21567 - Astro Bot` crash-site dunes (Intel A770, Linux, Mesa 26.2.5, 60 fps cap) the command
processor (CP) and the GPU run one after the other, not in parallel:

- The CP thread blocks on fences for about 27% of its time.
- The GPU is busy about 85 ms per frame, at about 120-150 ms per frame overall.
- Each blocking point is a CPU read of bytes that earlier GPU work of the same frame writes. The
  read submits the open command buffer and waits for all queued GPU work. While the CP waits the
  GPU drains, and while the CP records afterwards the GPU idles.
- If the two overlapped perfectly, a frame would cost max(CP work, GPU work), about 85 ms.

## The links (fence waits per strict 340 s run, gw79, `KYTY_DIAG_FENCE_STACK`)

| Link | Wait per run | Waits | What the CPU reads |
| --- | --- | --- | --- |
| `DISPATCH_INDIRECT` arguments | 28.5 s | 7484 | 12-byte group counts written by an earlier dispatch |
| `DRAW_INDEX_INDIRECT` arguments | 12.5 s | 8506 | 20-byte draw record written by an earlier dispatch |
| 32-bit `WAIT_REG_MEM` | 10.6 s | 8439 | A label whose `RELEASE_MEM` also writes back storage that shares bytes with another GPU object |
| Indexed draws over pending storage | 6.3 s | 3538 | Device-address storage written on the same queue |
| `DMA_DATA` image fills | 3.2 s | 9427 | Image contents for a CPU-side fill |

Each link moves the wait to the next one:

- A device-side `vkCmdDispatchIndirect` was tried on 2026-10-28 (gw41-gw44). It gave no fps gain:
  the waits moved to the `DRAW_INDEX_INDIRECT` argument reads.
- It also lost the device in 2 of 4 runs (see `docs/titles/robot-platformer.md`).
- Only removing all of the large links together frees the CP.

## What each link needs on the device

1. **`DISPATCH_INDIRECT`.** Two pieces of the CPU path depend on the argument values, and both
   need a GPU twin:
   - the group-count rewrite of the dispatch plan (`BuildComputeDispatchInput`, `plan.group_count`);
   - the empty-dispatch gate.

   A small conversion dispatch turns the guest record into the host dispatch arguments with
   exactly the CPU rules, followed by `vkCmdDispatchIndirect`. The CPU path stays for every case
   the conversion cannot express.

   **Link 1 result (2026-10-29).** Implemented in `GraphicsRenderComputeIndirect.cpp` and
   `GraphicsRenderDispatchDirect`:
   - It applies when `GpuMemoryAcquireGdsSource` reports the 12-byte record as held by a storage
     buffer of the recording (`DeviceBuffer`), the dispatch mode is 0x01 or 0x41, and no thread
     limits, depth metadata fill or uniform fill proof applies. The storage seed skip mask is 0
     for these dispatches. Every other case reads the record on the host as before.
   - The kernel copies the counts; counts above the host limits dispatch nothing and set a
     sticky flag that ends the run at the recording's completion.
   - Before shipping, a verify-only twin (gw95) copied the 12 bytes at the dispatch's stream
     point and compared them with the host read: 6400 of 6400 matched.
   - Strict 340 s dune run gw96 (same route as gw93): `DISPATCH_INDIRECT` fence waits fell from
     about 28 s to 0.27 s (239 host fallbacks). As predicted, the wait moved to the next link:
     `DRAW_INDEX_INDIRECT` rose from 12.5 s (gw79) to 34.4 s (10938 waits).
   - Frame rate is unchanged (6.4 vs 6.9 fps over the gw93/gw96 windows); gw96 reached frame
     4442 against 4330 in the same time. Captures match gw93 and there was no device loss.
   - Games matrix (strict, 60 fps cap): 26 interactive, 4 launch_failed, 1 presenting,
     1 host_crash; no case regressed against the zen-kernel matrix of the same day.
2. **`DRAW_INDEX_INDIRECT`.** In the dunes these are point lists (`prim=1`) with
   `INDEX_BUFFER_SIZE` 1-2 (gw79: 3715 records, of which 3339 were empty and 376 drew work).
   - The rule "the index buffer cannot hold one primitive, so the draw is empty" does not hold
     for points: one index is one point. Those records must really be read.
   - A device-side form needs the index range bounded by `INDEX_BASE` and `INDEX_BUFFER_SIZE`, not
     by the record's start index.
   - Defect found 2026-10-29, fixed the same day: `CommandProcessor::DrawIndexIndirect`
     (`GraphicsRun.cpp`) capped `index_count` at `INDEX_BUFFER_SIZE` but offset the index address
     by `start_index_location` without a bound, so a record with a nonzero start index read
     indices past `INDEX_BASE + INDEX_BUFFER_SIZE`. The draw now keeps only the indices inside
     `INDEX_BUFFER_SIZE - start` and logs a bounded warning when it drops some. What the guest
     reads past the end is still not defined by a published contract; the device-side form must
     settle it before it uses those indices.
   - Classification of the dune records (gw101, strict 340 s, 4096 records): 3472 empty (85%),
     369 single-index point records with more than one instance, 255 triangle-list records. No
     record had a nonzero start index or base vertex, so the bound never fired there. Each
     blocking argument read cost about 5.1 ms of CP time; the draws themselves about 0.54 ms.
   - It also needs a GPU twin of the CPU geometry and NGG plan that today consumes `index_count`
     on the CPU.
   - **Deprioritized (2026-10-29).** In the same dune runs the GPU is busy about 81.5 ms per frame
     and the CP spends about 110 ms per frame outside fence waits. Each of them alone holds the
     frame rate at roughly 9-12 fps. Per queue (gw109, clean GPU, dune windows at 8.4 fps, span of
     each recording from its first to its last landed timestamp), the graphics queue is busy about
     72 ms per frame and the compute queues about 7 ms. On this GPU the two engines share the
     execution units, so the sum bounds the busy time from above.
     Link 1 showed that removing one link moves its waits to the next one
     (`DISPATCH_INDIRECT` 28 s to 0.27 s, `DRAW_INDEX_INDIRECT` 12.5 s to 34.4 s, frame rate
     unchanged). A device-side `DRAW_INDEX_INDIRECT` would move them to the `WAIT_REG_MEM` link the
     same way, and it costs the GPU twin of the geometry and NGG plan above. It is parked until GPU
     frame time and CP work per draw are lower, where removing the waits can change the frame
     rate.
   - **Link 2 reopened (2026-11-10).** The parking condition is met. On the idle dune scene below,
     a frame costs about 72 ms: the graphics CP thread spends about 38 ms on the CPU and the GPU
     about 44 ms. Run one after the other, the two can never fit a 33 ms frame, so overlap is a
     precondition for 30 fps.
     - **Scene.** The recorded dune route with its input cut at 90 s: the robot stands at the
       crash-site spawn point and the view holds still. Window 96-116 s, 540-560 draws per frame,
       strict runs with the 60 fps cap, Intel A770, Linux. Five baseline runs gave 13.1-14.1 fps.
     - **Refusal.** `CommandProcessor::DrawIndexIndirectOnDevice` refuses in
       `IndexedIndirectStateEligible` because the draw takes the geometry pass
       (`ResolveGeometryDraw` is true). Later refusal points of `RecordIndexedDraw` were not
       reached and are not measured.
     - **The record.** It is one read per frame at one address, about 21 ms of the frame: the wait
       under the long compute kernel described below. It is a point list with index window 1 and
       count 1. Its instance count was 0 in 46 of 51 sampled reads and 512 in the other 5. The
       "about 5.7 waits per frame" quoted in the working notes were fence waits inside that one
       write-back, not separate reads.
     - **Attribution moves.** In 3 of 4 runs without the probe below, about 91 % of summed fence
       time sat at this read. In the fourth, about the same total sat at a `publish-host` wait of
       ordinary draws: a storage buffer that a texture aliases fails `StorageIsWholeGuestContent`.
       Submits, write-backs and fence waits per draw matched within 10 %. Why the alias exists in
       one run only is not explained.
     - **Probe (temporary, not shipped).** For geometry-refused records, the host path reused the
       last record read at the same address instead of waiting. Every 64th read was real and
       compared: 50 checks, 1 mismatch. Because the record stayed empty in the window, the probe
       also skipped that draw, so its frame rates are an upper bound. The probe was combined with
       a deeper submission ring (`CommandProcessorSubmissionSlots::SlotCount` 8 to 32, with the
       transient snapshot pool raised to match). "Blocked" is frame time minus CPU time minus
       run-queue wait of the graphics CP thread.

       | Arm | fps | CP thread CPU, ms/frame | Blocked, ms/frame | Render engine busy |
       | --- | --- | --- | --- | --- |
       | Baseline (5 runs) | 13.1-14.1 | 38.6 | 35 | 58-62 % |
       | 32 slots only | 13.8 | 38.0 | 34 | 61 % |
       | Record reuse only | 15.0 | 37.8 | 28.5 | 66 % |
       | Record reuse and 32 slots | 17.6 | 35.5 | 21 | 75 % |

     - **Reading.** The ring depth alone changes nothing while the read blocks. Without the read,
       the 8-slot ring becomes the wait. A ring slot is reused only after its submission completes,
       and the graphics queue submits about 36 times per frame in those runs (45 to 57 in the
       99-109 s window of later runs, the spread being idle-GPU flushes; `docs/titles/robot-platformer.md`): about 20 of them from
       `SubmitRecordedDispatchesBeforeDraw` and about 9 from `SubmitRecordedWorkIfGpuIdle`. So 8
       slots hold about a quarter of a frame. With both changes, the largest remaining waits
       (summed over all processors, queue not separated) are the `WaitRegMem64` producer wait
       (56 %), the `CommandProcessor::Reset` drain (24 %) and untagged waits (16 %).
     - **Ceiling.** Perfect overlap at about 36 ms of CP work and 43 ms of GPU work gives about
       23 fps. This link and the ring depth alone do not reach 30 fps.
     - **Shipped (2026-11-10): 32 slots.** Once the device-side geometry draw removed this read,
       the full ring became the wait: 4333 of 4739 ms of fence wait in a 20 s still-view stretch.
       `b7af968cb` raises the ring to 32 slots: 14.9 to 18.6 fps, render engine 66 % to 79 %
       busy, one run per arm (`docs/titles/robot-platformer.md`).
     - **Facts run (temporary probe, not shipped).** A capped log of the host draw that follows
       the refusal gave the same values in all 35 samples:
       - 4 waves (256 lanes per group), 2 parameters and 1 primitive per instance, so 512 groups
         of 4353 dwords: 8.9 MB of output.
       - The front and pull programs take the geometry block as push constants (64 and 32 bytes),
         not as the uniform-buffer spill.
       - The targets were eligible. The draw pipeline had no fragment transport and did not write
         guest memory; that flag is the draw pipeline's, not the geometry pipeline's.
       - The probe also called `GpuMemoryAcquireGdsSource` at the refusal. That call publishes a
         writer whose submissions have completed before it reports, so its answer
         (`GuestBytesCurrent`) does not tell whether the argument bytes are device-sourced. It is
         not a result.
       - Resident memory at 8 slots was 7.8 GB at 60 s and 9.3-9.8 GB at 80-116 s, the same as
         at 32 slots (7.8 GB, then 9.0-9.7 GB), one run each.
     - **Deferred, then built (2026-11-10): device-side geometry draw.** The device-side
       `DRAW_INDEX_INDIRECT` for geometry-pass draws (a GPU twin of the geometry plan) was first
       deferred for two reasons. Both are now resolved:
       - **Block form.** The host wrote the geometry block into push constants (output address,
         primitives, groups per instance, base vertex, first instance and index address) and the
         group count into a host `vkCmdDispatch`. Now a one-invocation converter
         (`host_shaders/geometry_indirect_args.comp`) reads the guest record in queue order. It
         writes a 64-byte header: the block, the dispatch arguments and the pull draw. The pass
         dispatches indirectly over that header, and the pull draws from it. The front program
         has a variant (`ShaderGeometryComputeInfo::device_block`, with its own program key) that
         loads the block from the address held in its first two push dwords.
       - **Output bound.** Before the record is read, the only bound was the 65535-group limit of
         `ResolveGeometryDraw`. For this state that is 65535 x 4353 x 4 bytes, about 1.06 GiB,
         over the 1 GiB output cap. The device form reserves min(65535, 1 GiB / group bytes)
         groups. A record above that draws nothing and counts one refusal, which the host reads
         at fence completion and records as `UnsupportedGeState`. This is the host contract:
         `ResolveGeometryDraw` admits only a nonzero `VGT_GS_MAX_VERT_OUT`, and
         `ShouldSkipUnsupportedGeShader` skips every such draw once its geometry pass is refused.
         The device refusal carries no GE-state census. Only the dispatched groups are zeroed
         (`host_shaders/geometry_output_clear.comp`, with the same indirect group count).
       - The converter follows the host rules: the index window past the start index, the
         topology's primitive count, the int32 base-vertex sum and the `NUM_INSTANCES` latch.
         The path requires `maxStorageBufferRange` of at least 1 GiB, because the clear binds
         the whole output buffer. Records whose bytes are current on the host keep the CPU path.
       - **Verified** with a temporary comparison in which the CPU path stayed authoritative and
         each device header was compared with the host plan once its submission completed:
         2792 records in 112 s, 0 mismatches, and 2607 records up to 100 s of the measured run
         below, which covers its still view, 0 mismatches. A run that flipped one expected word
         by hand reported exactly that mismatch. Every compared record was empty (no index in the
         window or no instance), so every compared pass had zero groups. A 120 s run of the
         recorded route with movement gave the same: 2426 records, all empty, 0 mismatches.
       - **Records with work.** On the device path they appeared in no run. The host-current
         geometry records with work all fall in the first 10 s of boot: 305 records of one index
         and 512 instances, during the publisher logos. A temporary probe copied each such
         record into a device buffer and sent it down the device path:
         - With the comparison on, 191 of these records were compared over the whole header
           (primitives, groups per instance, base vertex, first instance, index address,
           dispatch and draw), with 0 mismatches.
         - With the comparison off, 307 forced passes of 512 groups ran the block-reading
           variant, the clear and the pull without a fault or device loss. Their 3, 6, 9 and
           12 s captures are pixel-identical to an unforced run once the logo timing is aligned.
           The logos do not show these draws, so the captures exclude only a broken frame, not a
           wrong pass output.
       - The classic kernel emits the same instructions in the same order as before the variant
         was added, and its program key is unchanged.
       - **Measured** in one strict run of the idle route that took the CPU path until 101 s and
         the device path after it. Separate runs do not compare here: the 96-116 s window held
         297 draws per frame in one run and 556 in another. The CPU arm still ran the comparison
         harness (one converter dispatch per record and a lagged completion check); its cost was
         not separated.

         | Path | Window | fps | CP counters, ms/frame (fence) | Render engine | Host argument reads |
         | --- | --- | --- | --- | --- | --- |
         | CPU | 92-101 s | 11.9 | 86.9 (20.7) | 52 %, 43.4 ms/frame | about 291 ms per second |
         | Device | 104-116 s | 15.1 | 68.5 (15.5) | 66 %, 43.4 ms/frame | none |

         The CP counters are summed thread time of the graphics and compute processors (see
         `docs/bringup/cross-title.md`). The firm result is that the host argument reads are
         gone and the render engine idles less at the same work per frame. The frame rates are an
         upper bound on the gain, because the CPU arm carried the harness.
       - **Without the harness** the frame-rate gain is not established. A run with the device
         path alone gave 16.1 fps at 96-116 s (557 draws per frame, CP 64.4 ms, render engine
         69 % busy), inside the range of the earlier harness-free runs of this view (13.1-14.1
         and 16.5-17.3 fps). The run noise is too large for 2-minute runs to settle it. With the
         same path throughout, splitting that run's still-view samples into alternating
         6-second halves gave 16.3 and 13.5 fps. Two other runs reached the still view about
         12 s late, so their windows did not match.
       - **Cross-title** (strict, 125 s each, generic first-gameplay route, captures at 60 and
         115 s): `PPSA28311 - Absolum`, `PPSA09076 - DREDGE`,
         `PPSA03969 - F.I.S.T.: Forged In Shadow Torch`, `PPSA09809 - OCTOPATH TRAVELER II` and
         `PPSA21402 - Balatro` all exited cleanly with no device loss. None of them issued a
         `DRAW_INDEX_INDIRECT` in those 125 s, so these runs did not exercise the device path; they
         only show that the changed draw path left their other draws intact. Three 60 s captures scored
         `low_entropy` where the earlier run of these titles scored `healthy`: a loading screen, a
         settings menu and a fade before a cinematic, reached at different frames. The
         `gfx_storage_frontier` events of F.I.S.T. and OCTOPATH TRAVELER II also appear in earlier
         runs.
       - A strict 120 s run of the committed build on the same idle route had no error and a
         healthy capture at 110 s, still in the crash-site sequence. In the device-path run
         above, the still view rendered correctly at 100 and 112 s. The capture heuristic flagged
         those frames `hot_corruption`, its known false positive on sand.
       - **Known imprecision.** When a descriptor mutation conflict retries the draw, the
         converter is recorded again, so a refusal can be counted twice.
       - Not identified: a thread with about 14 ms of kernel time per frame, seen only in the
         probe arm with both changes.
     - **Frame rate moved without an intended change (cause open).** The baselines and probe arms
       above ran with the guest audio thread at 43-45 ms of CPU per wall second. Two later runs of
       one rebuilt binary, with and without the facts probe, had it at 36-37 ms/s and gave 16.5 and
       17.3 fps. The graphics CP thread was at 31-32 ms per frame and the read still held 92 % of
       fence time. Per frame, the guest draw and worker threads fell only 5-9 %, so a uniformly
       faster CPU does not explain it. The rebuild, the time of day and host load are confounded.
       Render-engine time per frame stayed at 42-44 ms in every run. The arm table holds within
       itself, but its absolute rates are not a baseline. Comparisons need a fresh baseline, plus
       host CPU load outside the process and the CP thread's core and frequency as witnesses.
     - **Where the alias-mode wait sits.** `RecordIndexedDraw` clears its pending submission on
       entry and sets none before the state refusal. The `publish-host` waits at the retry in
       `DrawIndexIndirectOnDevice` therefore come from indexed-indirect draws that pass the
       refusals, not from the refused geometry draw. One reading, not tested: whichever wait
       comes first in the frame absorbs the GPU catch-up.
     - **Fixed (ba77895f9): geometry output cap.** `ResolveGeometryDraw` admitted up to 65535
       groups, but `RecordGeometryPass` exited when the output passed the 1 GiB limit of
       `CommandBuffer::GeometryOutput`. With 4 waves and 2 parameters (4353 dwords per group), a
       draw of 61,667 groups or more was admitted and then exited; more parameters lower the
       threshold. `ResolveGeometryDraw` now refuses a plan whose whole output passes
       `CommandBuffer::kGeometryOutputMaxBytes`, so the draw takes the counted refusal path above.
       No title run reached the cap, so the refusal itself was not observed.
     - **Fixed (6d7969aad): geometry pass without a whole input primitive.** A triangle-list
       draw of one or two indexes took the geometry pass with zero groups, and the zero-byte
       output request then stopped the emulator. Such a draw has no input primitive and now
       records nothing.
     - **Fixed (29a4a93c2): drain in `CommandProcessor::Reset`.** `Reset` (`GraphicsRun.cpp`)
       started with `BufferWait()`, a full drain of the processor's recordings. The `R_DRAW_RESET`
       and `R_DISPATCH_RESET` packets reach it; the `GraphicsDrawInitDefaultHardwareState*`,
       `GraphicsDispatchInitDefaultHardwareState` and `GraphicsAcbResetQueue` writers emit them.
       In the arm with both changes it cost about 0.8 waits per frame at 5.7 ms. No state it
       resets needs the drain: an index buffer reaches the delete list that
       `GraphicsRenderDeleteIndexBuffers` empties only through the delete callback that
       `GpuMemory` runs after every submission that used the buffer completed, index uploads
       copy synchronously on the utility queue, and the rest is processor state that no recorded
       command reads. The drain is removed. Measured with the per-site wait counter on the dunes
       idle route in runs without those two changes: before, the site appeared in the first 2-3 of 11 windows of 10 s (1-10 ms per
       window, 10-16 ms per run, during boot); on `aa282cbf4` it is absent from all windows of two
       211 s runs, and no other `GraphicsRun.cpp` wait site appeared in the boot windows. Its whole cost was at most 0.1% of those windows, smaller than the run-to-run spread
       of the other wait sites, so whether another wait absorbed it cannot be resolved and the frame
       rate does not show it. Verified with the CTest suite (108 passed, 1 skipped) and a strict games
       matrix on the installed titles (2026-11-10): 26 of 29 packages interactive. The other three
       match earlier matrices: `PPSA03969 - F.I.S.T.: Forged In Shadow Torch` crashed once (as on
       2026-11-02) and was interactive on a rerun, `PPSA01341 - Demon's Souls` stays at `presenting`, and
       `PPSA03524 - GTA San Andreas Definitive Edition` stops at boot on a storage image format
       this branch does not support (fixed by the PR #65 work).
     - **Latent race, not changed.** A compute processor's `Reset` can call
       `IndexBufferDeleteAll` while another thread holds an index staging lease; `DeleteAll` then
       fails and the emulator exits. The removed drain did not cover it either (it drained only
       the resetting processor). Not observed in a run.
   - **The wait under the long compute kernel (measured 2026-10-29, gw111-gw113).** A temporary
     probe wrote a host-visible word after `0x3666fa39`, a BVH traversal kernel. It runs once per
     frame on the graphics queue as 8156 workgroups of 256 invocations (corrected 2026-10-30;
     this record first said one 16x16 workgroup) and takes 22-29 ms per call as split wave64 on
     this GPU. These runs loaded a stale module for that kernel; the committed translator's module
     takes 22.3-23.3 ms in dune windows (gw118, `docs/titles/robot-platformer.md`, stale-module
     record). The probe
     classified each host fence wait by whether that kernel was still running.
     - In the dune windows, one graphics-queue wait per frame starts while the kernel runs. It
       costs 21-23 ms of the roughly 131 ms frame. The other graphics-queue waits add about
       8-10 ms per frame.
     - Fence stacks tagged the same way (gw112, 340 s): the running-kernel wait is the
       `DRAW_INDEX_INDIRECT` argument read in 942 of 944 cases, 21.3 s in total.
     - The kernel itself writes only two storage images. A second probe recorded every writable
       storage binding and every compute dispatch in host order (gw113). The 20-byte record is
       written by a later compute dispatch (`0xd3ef7a03`) on the same queue, in the submission just
       before the read. The kernel sits three submissions earlier, before the writer in every one
       of the 1247 classified reads. `SubmitRecordedDispatchesBeforeDraw` splits each draw from the
       dispatches before it, which is why these are separate submissions.
     - The backing is host-visible, so after its writer completes the host could read the record
       without a device copy. A queue-order dependency remains, though.
     - **Excluded by this classification: waiting only for the writer's submission** instead of
       writing back with the recording. Every completion point after the writer also includes the
       kernel, so that change would not shorten this wait. It was not built or run.
     - The levers are therefore the kernel's own GPU time and this link (a device-side read). The
       kernel's time also counts toward the GPU frame, where it is about 30% of the graphics queue.
     - **Excluded: moving the kernel to the compute family** with a wait at its first reader
       (gw128, 360 s, measured 2026-10-29). A temporary probe matched the kernel's output storage
       images and its five buffers against every later draw and dispatch. In all 1733 calls the
       very next compute dispatch (`0x8f3da857`) binds an output image as storage and four of the
       five buffers, with no draw in between. With the consumer adjacent, a second queue has no
       independent work to overlap the kernel with, so neither its GPU time nor this wait would
       shrink. Moving the whole compute chain does not help either: the graphics-side
       `DRAW_INDEX_INDIRECT` read of `0xd3ef7a03`'s record starts while the kernel is still
       running (gw112 above), so there is almost no independent graphics work before the chain's
       consumer. It was not built.
3. **`WAIT_REG_MEM`.** The 10.6 s are waits on labels whose release also writes back aliased
   storage.
   - Two attempts are recorded as excluded: consolidating those waits, and eliding the submit and
     fence boundary for a same-recording producer (it broke the presentation contract).
   - What is still open is a device-side propagation of the storage write-back to its aliases, in
     queue order.
4. **Indexed draws over pending storage** and **`DMA_DATA` fills** are smaller. They follow the
   same rule: the consumer should wait on the device for the writer, not on the host.

## Mechanisms in the reference implementations

Two other PS5 emulators interpret PM4 on the host like Kyty, and both reach playable frame rates in
this title on desktop GPUs. They keep the CPU out of this chain as follows (studied as behaviour
only, not code):

- **Indirect arguments stay on the device.** This applies only when the argument bytes are
  GPU-visible: a host import of guest memory, or a GPU-owned buffer. Anything else falls back to
  the CPU read.
- **A GPU conversion twin for the cases where the CPU path rewrites arguments.**
  - A helper dispatch reproduces the CPU derivation exactly and reports the cases it cannot
    express, which take the CPU path.
  - One implementation has a verify mode that compares every GPU conversion with the CPU rule on
    the bytes the GPU read.
- **`WAIT_REG_MEM` passes in stream order.** A wait on a label that a recorded and still pending
  release writes passes without a CPU fence, because queue order already guarantees it. This
  requires that nothing on the CPU reads the label's bytes before the GPU writes them.
- **Device-lost rule.** The GPU never waits for a signal that only the CPU gives later.
  - When the CPU must copy bytes the GPU will read, the CPU waits for the copy before it hands the
    GPU work over.
  - A device-side wait on a semaphore that the host signals only after a later CPU step can hang
    the queue.

## Open questions before building it

- **The gw41-gw44 device loss.** Did that patch skip the `plan.group_count` rewrite or the
  per-dispatch runtime blocks (`thread_limits`, `geometry_block`)? Either would dispatch a
  different grid than the CPU path, which fits a device loss. The patch files were in a temporary
  directory and are gone.
  - A missing barrier is not the explanation. The guest's `CS_PARTIAL_FLUSH`, `ACQUIRE_MEM` and
    `EVENT_WRITE` flushes record `GraphicsRenderMemoryBarrier`, which is ALL_COMMANDS to
    ALL_COMMANDS with MEMORY_WRITE to MEMORY_READ|WRITE. That already orders an earlier dispatch's
    writes before an indirect command read.
- **Argument residency (measured 2026-10-29).** In strict 340 s dune runs (gw93, gw94) every
  blocking argument read was classified by the source the CPU read path chose:
  - `DISPATCH_INDIRECT`: 5345 of 6138 blocking reads (87%) were held by one writable storage
    buffer of the current recording, so a device-side read can take them from that buffer. The
    other 793 waited for another queue's submission.
  - `DRAW_INDEX_INDIRECT`: all 4471 blocking reads were held by such a storage buffer.
  - Of 2560 device-held `DISPATCH_INDIRECT` records, none had a zero group count, and none took a
    count-dependent branch of the dispatch (storage seed skip, depth meta fill, uniform fill,
    thread limits, a mode other than 0x01/0x41).
  - The dispatch plan reads the group counts only for its zero and host-limit checks; the shader
    does not receive them.
  - `WAIT_REG_MEM` labels were not classified.
- **Measuring each link.** Each link has to be removed with an exact twin and measured with
  `KYTY_DIAG_FENCE_STACK` on the dunes, on Intel first and then on AMD.
- **Measured in the hub (2026-11-01).** With the frame at 28 ms and the GPU at 21 ms, removing
  links one at a time confirmed that each one moves the wait: 679 frames per 20 s with device label
  waits, 701 with the cross-queue dispatch read too, 738 with an unguarded aliased label
  consolidation. A guarded form keeps about half of that, because 51 % of those waits are followed
  by a lookup of a non-storage alias. See `docs/titles/robot-platformer.md`.
