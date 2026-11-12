# Parallel draw recording (design, 2026-11-10)

Status: design, not built. The measured ceiling is at or below the build gate (see "Decision").

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
  submitter can return once stage A has passed the batch's last guest read, so stage B could overlap
  the guest's next frame. The frame is then at least stage A plus the guest's gap, about 61 + 16 of
  131 ms, so at most about 1.7x, still under the build gate; whether the guest starts earlier when
  released is not established (`docs/titles/anime-fighter.md`).
- Moving work to stage B cannot reduce stage A. Stage A shrinks only by doing less ordered work per
  draw: the 12-16 ms of waits (1.8-11.8 ms in four runs after `d2ff690bc`; in the one run tagged for it, three quarters were a
  shared-page tracker false positive, fixed in `443032ed0`; 2.09 ms in one run after it) and the memory-object creation in binding
  are the two largest parts,
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

Recomputed after `443032ed0` (one world run with a temporary tag, about 389 frames): 90 ms frames,
68.8 ms of CP processing over 3,653 draws (18.8 us per draw), 2.1 ms of it host waits, and about
21 ms outside CP processing. Scaling the non-wait classes of the table above by the same factor
(an estimate; the classes were not timed again) gives stage A about 37 ms, shared caches about 10,
independent work about 16 and unattributed about 7.

- Parallel recording alone: CP time falls to 37-53 ms, the frame to 58-74 ms: 1.22-1.56x.
- Submit overlap alone: the submit may return only after the memory the batch references is copied
  ("A graphics submit returns after its batch is decoded"); without the split all CP processing
  stays serial, so the frame stays at least 68.8 ms: at most 1.31x.
- Both: the submitter returns once stage A has passed the batch's last guest read, so the frame is
  at least stage A plus the gap, about 58 ms: at most 1.56x.
- Both, with the copy that record prescribes: the submit copies the memory the batch references
  and returns, so the frame is at least the larger of copy plus gap and stage A. That reaches 2x
  (45 ms) only if the copy costs at most about 24 ms per frame on the submitting thread, or about
  8 ms if it runs on the CP thread. Measured (one world run with temporary stage timers and a
  read-only census, 3,607 draws per frame; the census window held about 5,270 draws per frame, so
  its costs are taken per draw): the lookups the copy has to repeat cost 17.0 ms per frame (shader
  input information 7.5, index snapshot and vertex range 5.8, device-address preparation 3.8), the
  other read-only snapshots 1.7 us per draw or 6.1 ms, and copying the 29-32 MB per frame that the
  command processor uploads now takes at least 1.3 ms on this host (23 GB/s single-thread memcpy).
  That is about 24 ms per frame, or about 21 ms if the instruments' share of that run's command
  processing (80 against 68-72 ms) is taken out evenly. Not counted: vertex binds cost 3.65 us each
  there (about 13 ms per frame; 2.2 us in a run with one temporary tag) and 94% of them copy guest
  bytes, but the copy share was not split out; nor are texture bytes, descriptor tables and nested
  command buffers. Not built: the case sits at the gate, open on the copy share of vertex binds and
  on what the submitting thread waits for.
- The gap is mostly guest user time. In the run that counted submits per thread, the thread making
  them was on a CPU 14.3 ms per frame, 1.3 ms of it in the kernel, and off a CPU about 7 ms of the
  gap; a thread with the same profile (14.5 and 1.3 ms) ran on `443032ed0`. The CP thread spent
  3.9 ms per frame in the kernel there (7.3 ms on the older build with temporary counters). What
  the submitting thread waits on during those 7 ms was not recorded: the temporary trace spent its
  line budget during loading.

Submit overlap measured (2026-11-11, `docs/titles/sandbox.md`, "Measured, one batch ahead does not
shrink the gap"): with the guest allowed one batch ahead, the CP still waited 18.9 ms of each frame
for the next batch. That probe had not engaged. With the early return engaged (same record, "Rechecked
with the early return engaged"), the guest still took command processing plus its own ~11 ms between
submits, held by the emulator's own quiesce on the guest's per-frame direct-memory release (same
record, "What holds it"): an early return pays only with that invalidation ordered on the command processor.

Rechecked against an exclusive per-draw profile (2026-11-11, one full-stack profile, 5,017 draws per
frame, 74.7 ms of CP processing, every draw-path sample in one row; `docs/titles/sandbox.md`, "The
path behind the draw entry priced by unit of work"). Where the 66 ms under the draw entry sit,
mapped onto the classes above (the profile shows where time is spent, not what depends on draw
order): about 35 ms in the order-dependent classes (storage and index snapshots, vertex and storage
objects, textures, device-address preparation, vertex uploads, targets, render-pass begins), about
11-12 ms in the shared pipeline and descriptor-set caches, about 14-15 ms in per-draw work (shader
input information, command emission, recorder) and about 5 ms unattributed. With about 35 ms serial
plus the 8.4 ms outside the draw entry, no thread count reaches 2x of CP time (at most about 1.7x).
The run that skipped the path behind the draw entry shows that path is over 90 % of the submit hold;
it does not move the order-dependent share, so parallel recording alone stays under the gate.

Combined with that redesign on paper, with four recording threads and the 24-29 ms between submits
of this frame: with its countable rows at their ceilings the frame is about 51-62 ms (about 1.65-2x
of 102 ms), with every row at its ceiling about 47-55 ms, and with its firm rows only about 71-83
ms. Only the 16 ms between submits of the skip run, where the GPU work was removed too, would put
the countable case at 43-58 ms. Stage A plus the time between submits stays at about 37-58 ms, above
the 33 ms reopen condition at every ceiling, and no count of the redesign's rows measures that time:
both stay closed, alone and combined. A bound over the designs priced, not a floor (the 8.4 ms
outside the draw entry and the remaining order-dependent work were not redesigned): with every row
at its ceiling, CP processing alone is about 23-26 ms, its serial part about 17-18 ms, already above
16.7 ms before the time between submits and the GPU, so even complete submit overlap would not reach
60 fps at this load.

Three cases stay under the 2x gate and the fourth sits at it; neither change is built. 60 fps
(16.7 ms) needs about 5.4x on this frame: the gap alone exceeds it, so any such path needs the
overlap, and the CP would have to process a draw in about 4.6 us. No measured lever or design here
reaches that.

Re-priced 2026-11-12 on the sandbox world (`PPSA17221 - Minecraft`), after the per-draw cuts
`0be0c39a2` and `7b2b35fd4`: 11.36 us per draw, 2,805 draws a frame, 31.9 ms of command-processor
time in a 44.8 ms frame. A draw plan is worth about 1.5 us per draw (pixel inputs already use a
cached analysis; vertex inputs are mostly the descriptor gather). Submit still returns only after
the batch is decoded, because the title rewrites its descriptor tables after submit, so the
command processor cannot run behind the guest. Under that contract the plan plus a two-stage
command processor reach about 31-36 fps at this load (the frame becomes the command processor's
31.9 ms, or about 28 ms with the plan). The gates above stay closed.

Front/back pipeline inside one submit, priced from source on 2026-11-12 (stage counters above, not
built). A front thread would take only what needs registers and guest memory: vertex input 1.12,
pixel input 0.80 and shader ids 0.53 us, about 2.5 us per draw. Everything else stays on the
thread that owns the command buffer, because it feeds the next draw: guest-address read
preparation mutates the device-address registry and can retry the draw, index preparation reuses
the previous draw's transient snapshot, materialization creates or finds host images, the pipeline
key needs the framebuffer's compatibility, which needs the materialized targets and the live image
layouts that the previous `BeginRenderPass` changed, binding creates memory objects and descriptor
sets, and emission resets the hashes of drawn targets that the next draw's materialization reads.
The back stage stays at about 8.8 us, so the batch goes from 11.36 to about 9 us per draw before
the hand-off (a multi-KB input record per draw): about 6.7 ms of a 44.8 ms frame at 2,805 draws.
Dispatches, guest-memory waits and writes, end-of-pipe writes, barriers and flips would drain it.
Reaching 5-6 us needs binding and the ordered part of state setup rebuilt as functions of a
versioned snapshot, or recorded on workers as above; neither is designed.
