# Parallel draw recording (design, 2026-11-10)

Status: design, not built. The measured ceiling is below the build gate (see "Decision").

## Why

`PPSA17221 - Minecraft` in a world stays at 6-10 fps on the Intel A770 host because the
command-processor (CP) thread records several thousand draws per frame at about 21 us each
(`docs/titles/sandbox.md`, "Parallel recording ceiling"). Every draw is recorded on that one
thread, under the render mutex, into one primary command buffer. This page describes how the
recording could be split across threads, what bounds the gain and what must hold before any code.

## Current path (code facts)

- `CommandProcessor::DrawIndex` reads the submission id and index state under the CP mutex and
  releases it. `RetryGuestDeviceAddressDraw` then runs the recording callback, which retakes the CP
  mutex for each attempt; the host wait between attempts holds neither the CP nor the render mutex
  (`GraphicsRun.cpp:2475-2530`). `RecordIndexedDraw` (`GraphicsRenderDraw.cpp:1768`) holds the
  render mutex for the whole draw (`GraphicsRenderDraw.cpp:1800`). Lock order: CP mutex, then
  render mutex.
- Inside, in order: geometry and primitive plan, index snapshot
  (`TryUploadTransientReadOnlyBuffer`), vertex-stream and device-address preparation, target
  materialization and framebuffer lookup, register state (`CommandBuffer::RecordState`), pipeline
  lookup (`PipelineCache::CreatePipeline`), render pass begin, vertex binding (creates and
  registers memory objects), descriptor binding (`DescriptorCache::GetDescriptor`, shared pools),
  then `vkCmdDraw*` (`GraphicsRenderDraw.cpp:2362-2388`).
- Shared structures, each behind its own mutex: the guest device-address registry, the transient
  snapshot pool, the descriptor cache, the pipeline cache, the framebuffer cache.
- Render passes are `VkRenderPass` begun with `VK_SUBPASS_CONTENTS_INLINE`
  (`GraphicsRenderCommandBuffer.cpp:3584`). A pass ends on a new framebuffer, on any command
  recorded outside a pass (`RecordOutsideRenderPass`: copies, barriers, publications), after the
  vertex-clip probe (it ends a query inside the pass, then closes the pass to read the result,
  `GraphicsRenderDraw.cpp:2396-2405`), on a geometry segment and at `End`. No secondary command
  buffers exist.
- A command buffer slot holds up to 8 chunks and is submitted early when the GPU is idle, after a
  dispatch, or every 8 operations once 500 us have passed (`GraphicsRun.cpp:544-623`).
- Measured in the world scene (agent performance samples of two runs without temporary code,
  samples with at least 3 flips and 10,000 draws): 3,170-3,480 draws, 133-141 render-pass begins and
  95-111 submissions per frame, so 22-26 draws per pass and 29-37 per submission on average. The
  command-processor counter reads 77-88 ms of 98-110 ms frames.
- Per-submission bookkeeping at draw time: memory-object use, descriptor-set use
  (`RecordSetUse`), pipeline leases, snapshot leases, publication gate.

## Proposed split

Two stages per draw, connected by a draw record.

- **Stage A, CP thread, guest order.** Everything that reads or changes guest memory state or the
  memory-object registry, or decides command-buffer structure: register snapshot, shader input
  information (it decides which buffers and textures the draw reads), index snapshot,
  vertex-stream and device-address preparation with its retry, memory-object creation and use
  recording, target materialization, framebuffer lookup, render-pass boundaries, every command
  outside a pass, query indices, submission ids and leases. Output: a draw record with resolved
  buffer and view handles, offsets, the pipeline key, the descriptor contents, the dynamic state
  and the draw arguments.
- **Stage B, worker threads.** Pipeline lookup, descriptor-set lookup or allocation, register state
  emission and the bind and draw commands, recorded into a secondary command buffer that inherits
  the current render pass and framebuffer. One secondary per run of draws inside one pass segment.
- **Stitching.** Stage A begins every pass with `VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`
  (core Vulkan, so the same path holds on every host) and, at the segment end, records
  `vkCmdExecuteCommands` with the segment's secondaries in draw order. A submission waits for its
  secondaries; stage A continues into the next command buffer meanwhile.
- **Locks.** Stage B never takes the CP or render mutex. The pipeline cache needs a shared read
  path with creation serialized. Descriptor allocation moves to stage B only with per-worker pools
  for misses; otherwise it stays in stage A. Set-use and lease recording become per-worker lists
  merged at submit.

## What bounds the gain

Counter classes per frame of 105 ms of CP time (world scene, one run with temporary stage timers,
which slow the CP):

| Class | ms/frame | Stage |
| --- | --- | --- |
| Order-dependent (snapshots, write-back check, device-address preparation, binding, targets, waits, dispatches) | 61, of which 12-16 are host waits | A |
| Shared caches (pipeline setup, descriptor finalization) | 13 | B only with per-worker descriptor pools |
| Independent (register state, command emission) | 22 | shader input information stays in A; the split of the 22 ms is not measured |
| Not attributed | 10 | unknown |

- The sandbox record bounds CP time at 1.2x (only the independent work leaves the CP thread) to
  1.7x (caches and unattributed work leave too). The draw record adds a per-draw copy, and the
  shader input information that stage A keeps lowers the 1.2x end further; neither is measured.
- In that run the frame is 131 ms, so about 26 ms per frame lie outside CP processing (21-22 ms in
  the runs without timers). At the bounds above the frame falls to about 88-113 ms: 9-11 fps.
- Most of that gap fits the submit contract: a graphics submit returns only after the CP has run its
  whole batch (`docs/bringup/cross-title.md`, "A graphics submit returns after its batch is decoded"),
  the title submits one batch per frame, and the CP then waits about 15.6 ms per frame for the next
  one (`docs/titles/sandbox.md`, "Command-processor idle between batches"). With the split, the
  submitter can return once stage A has passed the batch's last guest read, so stage B overlaps the
  guest's next frame; this overlap is not in the bounds above.
- Moving work to stage B cannot reduce stage A. Stage A shrinks only by doing less ordered work per
  draw: the 12-16 ms of waits and the memory-object creation in binding are the two largest parts,
  and the two measured attempts on them are excluded (`docs/titles/sandbox.md`: staging drain and
  retry; in-place vertex binds).
- 30 fps is a 33 ms frame. With 22-26 ms per frame outside CP processing unchanged, stage A would
  have to fit in 7-11 ms per frame, 2-3 us per draw over 3,500 draws. Stage A at 7 us per draw over
  3,500 draws, with the gap kept, gives about 20 fps.

## Risks

- Short segments: passes hold 22-26 draws on average, so a secondary per segment is recorded about
  130-140 times per frame. The per-call cost of `vkCmdExecuteCommands` and of a small secondary on
  each driver is unmeasured.
- A secondary inherits no pipeline, descriptor, vertex-buffer or dynamic state, so each one
  re-emits all of it, and the state deduplication in `RecordState` restarts at every secondary.
- The vertex-clip probe's query inside the pass must move into the secondary or end the segment.
- Diagnostics that sample the draw order (`RecordRecentGuestDraw`) stay in stage A.
- Windows and macOS drivers: secondary command buffers are core, but their cost differs per
  driver; none is measured.

## Decision

Not built. The ceiling (1.2-1.7x of CP time, about 9-11 fps in the measured scene) is below the
2x build gate. Build only after the time outside CP processing (largely the serial submit, see
above) is cut, and a change brings stage A low enough that stage A plus the remaining off-CP time fit in a 33 ms
frame on the same route.
