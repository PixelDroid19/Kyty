# Command processor and GPU sync chain (design, 2026-10-29)

Status: link 1 (`DISPATCH_INDIRECT`) is implemented (2026-10-29, see "Link 1 result"). Link 2
(`DRAW_INDEX_INDIRECT`) was deprioritized on 2026-10-29 and reopened on 2026-11-10 (see "Link 2
reopened"); the other links are design and measurements only.

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
       and the graphics queue submits about 36 times per frame: about 20 of them from
       `SubmitRecordedDispatchesBeforeDraw` and about 9 from `SubmitRecordedWorkIfGpuIdle`. So 8
       slots hold about a quarter of a frame. With both changes, the largest remaining waits
       (summed over all processors, queue not separated) are the `WaitRegMem64` producer wait
       (56 %), the `CommandProcessor::Reset` drain (24 %) and untagged waits (16 %).
     - **Ceiling.** Perfect overlap at about 36 ms of CP work and 43 ms of GPU work gives about
       23 fps. This link and the ring depth alone do not reach 30 fps.
     - **Next.** A device-side `DRAW_INDEX_INDIRECT` for geometry-pass draws: a GPU twin of the
       geometry plan, built verify-only first as link 1 was. The ring depth comes in the same
       piece, chosen with measured resident memory. At 32 slots without the probe it was
       9.2-10.0 GB; there is no 8-slot figure yet. A thread with about 14 ms of kernel time per
       frame appeared only in the probe arms and is not identified.
     - **Defect found, not fixed.** `CommandProcessor::Reset` (`GraphicsRun.cpp`) starts with
       `BufferWait()`, a full drain of the processor's recordings. The `R_DRAW_RESET` and
       `R_DISPATCH_RESET` packets reach it; the `GraphicsDrawInitDefaultHardwareState*`,
       `GraphicsDispatchInitDefaultHardwareState` and `GraphicsAcbResetQueue` writers emit them.
       In the arm with both changes it cost about 0.8 waits per frame at 5.7 ms. Which host state
       needs the drain is not established; `GraphicsRenderDeleteIndexBuffers` is a candidate.
       Direction: find that state and defer only it.
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
