# Command processor and GPU sync chain (design, 2026-10-29)

Status: design and measurements only. Nothing here is implemented.

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
2. **`DRAW_INDEX_INDIRECT`.** In the dunes these are point lists (`prim=1`) with
   `INDEX_BUFFER_SIZE` 1-2 (gw79: 3715 records, of which 3339 were empty and 376 drew work).
   - The rule "the index buffer cannot hold one primitive, so the draw is empty" does not hold
     for points: one index is one point. Those records must really be read.
   - A device-side form needs the index range bounded by `INDEX_BASE` and `INDEX_BUFFER_SIZE`, not
     by the record's start index.
   - It also needs a GPU twin of the CPU geometry and NGG plan that today consumes `index_count`
     on the CPU.
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
- **Argument residency.** Which argument and label pages in the dunes are GPU-visible (dma-buf
  imports)? Device-side reads are only valid there.
- **Measuring each link.** Each link has to be removed with an exact twin and measured with
  `KYTY_DIAG_FENCE_STACK` on the dunes, on Intel first and then on AMD.
