# Command processor and GPU sync chain (design, 2026-10-29)

Status: link 1 (`DISPATCH_INDIRECT`) is implemented (2026-10-29, see "Link 1 result"). Link 2
(`DRAW_INDEX_INDIRECT`) is deprioritized (see its entry); the other links are design and
measurements only.

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
     frame rate at roughly 9-12 fps. Link 1 showed that removing one link moves its waits to the next one
     (`DISPATCH_INDIRECT` 28 s to 0.27 s, `DRAW_INDEX_INDIRECT` 12.5 s to 34.4 s, frame rate
     unchanged). A device-side `DRAW_INDEX_INDIRECT` would move them to the `WAIT_REG_MEM` link the
     same way, and it costs the GPU twin of the geometry and NGG plan above. It is parked until GPU
     frame time and CP work per draw are lower, where removing the waits can change the frame
     rate.
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
