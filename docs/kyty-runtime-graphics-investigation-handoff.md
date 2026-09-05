# Kyty Gen5 runtime graphics investigation handoff

Updated: 2026-08-23

Status: the last accepted strict frontier advanced into controllable gameplay
without a process-killing error, and the opaque black sprite/prop rectangles
were absent after correcting pixel-kill depth ordering. The current worktree is
not accepted at that frontier: two bounded runs lost the Vulkan device after
the first diagnostic input edge. Full playability acceptance, correct world
geometry, and stable 60 FPS remain open.

This document intentionally excludes private workload names, identifiers,
paths, binaries, screenshots, shader hashes, and raw logs. Keep those only in
ignored scratch and address the workload through `$KYTY_GUEST_ROOT`.

## Verified advances

The current graphics path retains these foundational isolated, tested changes:

| Commit | Contract | Verification |
| --- | --- | --- |
| `990b9a40` | Decode and lower Gen5 `ds_read2_b32` with two dword-scaled offsets over byte-addressed Workgroup LDS | Focused parser/SPIR-V test plus strict runtime |
| `14633fe6` | Preserve the layout of GPU-owned RenderTextures across update re-entry | Focused graphics state test plus strict runtime |
| `9cc21524` | Preserve discard semantics for null MRT0–3 export tails | Focused shader/SPIR-V test plus strict runtime |
| `9b026e53` | Keep pixel-kill shaders on late Vulkan depth commit while retaining early fragment tests for opaque shaders | Red/green SPIR-V test plus gameplay-era native capture |

On the accepted historical branch state, Linux Release passed 193
GraphicsPackets tests plus its targeted GraphicsState contracts. Its recorded
strict Release+Silent route, without `KYTY_BRINGUP_*` or permissive fallbacks,
exceeded 24,000 presents and used bounded diagnostic controller input to
exercise sustained movement in gameplay. That route proves the historical
runtime and input frontier, not formal input acceptance or the current dirty
worktree, whose device-loss boundary is recorded below.

The historical horizontal stripes and the later opaque sprite/prop rectangles
are absent in the post-fix native capture. Background, props, character,
lighting, transparency, and frame progression remain recognizable.

## Quick reproduction and verification

Use a Release build with silent guest logging. Keep the private workload path
in an environment variable and keep captures under ignored scratch:

```bash
cmake -S source -B _build_linux -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux fc_script
_build_linux/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Do not set `KYTY_BRINGUP_*`, trap-skip, permissive GPU, or fabricated input
flags in an acceptance run. Automatic input may shorten a discovery capture,
but it cannot prove interactive playability.

After a graphics semantic change, run the focused regression suites:

```bash
_build_linux/fc_script '{kyty_run_tests()}' \
  --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
```

Every test selected by the focused filters must pass. A runtime change is
accepted only when those tests pass and a strict re-run either preserves the
gameplay-era checkpoint or advances the first failure.

## Problem-to-solution guide

| Symptom | Proven producer | Resolution | Regression evidence |
| --- | --- | --- | --- |
| Structured exit: unknown `ds_read2_b32` | Gen5 DS parser/SPIR-V generator lacked the two-result LDS read | Decode both 8-bit offsets, scale each by four bytes, retain byte-addressed `vaddr`, and load consecutive destination VGPRs from Workgroup memory | Focused packet-to-SPIR-V test and strict runtime advancement |
| White or horizontally corrupted world after a valid earlier frame | RenderTexture update re-entry reset a GPU-owned tiled image to `VK_IMAGE_LAYOUT_UNDEFINED` | Preserve the current image layout on Update; use `UNDEFINED` only for initial creation or an evidenced invalidation/discard | Focused GPU memory/render-target state test |
| Striped or missing output around multi-render-target shaders | Null MRT export tails lost their discard/no-write semantics during SPIR-V generation | Preserve null MRT0–3 tails as no-write exports instead of fabricating color output or truncating the export contract | Focused shader/SPIR-V export test |
| Opaque black rectangles in transparent sprite or prop bounds | Kill-enabled `EarlyZThenLateZ` pixel shaders were emitted with Vulkan `EarlyFragmentTests`, allowing depth commit before `OpKill` | Omit `EarlyFragmentTests` for pixel-kill shaders so discarded fragments cannot write depth; retain it for opaque early-Z shaders | Red/green SPIR-V test and native gameplay-era capture |
| Large first-run stalls recur after restarting Kyty | `VkPipelineCache` was always created empty and never persisted | Validate the standard cache header against vendor/device/UUID, load compatible bounded data, and save dirty cache data atomically at a rate limit | Header tests plus isolated cold/warm driver measurements |
| Pipeline-cache writes can exceed the session budget after an I/O failure | A failed temporary-file write, flush, or replace did not consume budget and was retried on every pipeline lookup | Charge every disk attempt conservatively, rate-limit retries, and stop attempting after 64 MiB per process | Budget saturation test plus strict runtime disk counters |
| First-use shader persistence pauses the render path | Every new SPIR-V entry scanned the cache directory and performed write, flush, and rename synchronously on the compiling thread | Queue immutable entries to one bounded writer, coalesce duplicate identities, and keep persistence best-effort without invalidating the in-memory shader | Queue saturation/drain tests, cold/warm restart, and strict visual regression |
| Frame histogram reports hundreds of multi-second frames while presents advance | Every sample used `1000 / averaged_fps`; one slow FPS window was therefore copied into many fast frames | Record the monotonic delta between consecutive frame-loop timestamps and retain FPS only as a rate metric | Red/green interval test plus strict cold/warm runtime pair |
| Reload exits while a sampled texture overlaps live color/depth aliases | The texture crosses the color RT/storage pair and the depth metadata plane, but the mixed-parent policy did not recognize the exact DepthStencil relation | Link only the captured `DepthStencilBuffer Crosses Texture` metadata alias; materialize the image from the existing color surface | Exact policy test plus input-driven strict runtime beyond the former exit |
| Structured `!create_all_the_same` exit for a tiny storage view | The incoming `StorageBuffer` was contained by both a live `Texture` and a read-only `IndexBuffer`; the mixed-parent classifier handled the texture but not the index role | Link only the evidenced `IndexBuffer Contains StorageBuffer` relation and require every mixed parent to pass the shared classifier independently | Red/green policy test plus strict runtime beyond the former exit and a healthy gameplay-era capture |
| Scene reached only with automatic Cross input | Input automation bypasses the real press/release acceptance contract | Do not change graphics or synthesize completion. Re-run with real keyboard/controller edges and treat inability to reach gameplay as a separate input/synchronization frontier | Pending real-input acceptance |

### Pipeline compilation hitches across restarts

Kyty previously passed an empty `VkPipelineCache` to every graphics and compute
pipeline creation. A process restart therefore discarded the opaque driver
cache even when guest shader and render state were unchanged.

The cache store now:

- uses `KYTY_VULKAN_PIPELINE_CACHE` when an explicit test location is needed,
  otherwise a per-user cache directory;
- includes the Vulkan vendor ID, device ID, and pipeline-cache UUID in the
  default filename;
- accepts at most 64 MiB and validates the standard version-one header before
  passing bytes to Vulkan;
- retries with an empty cache if a driver rejects otherwise compatible data;
- writes a sibling temporary file and replaces the destination;
- saves after the first new pipeline and consolidates later dirty data at most
  once every 30 seconds;
- charges attempted bytes before opening the temporary file, including failed
  write, flush, and replace operations;
- enforces a 64 MiB attempted-write budget per process, so I/O failures and a
  long session cannot repeatedly replace the bounded blob into gigabytes of
  cumulative writes.

With Mesa's independent shader cache disabled to isolate this path, a bounded
cold run spent 268 ms in 87 `vkCreate*Pipelines` calls (maximum 25 ms). The
equivalent warm run spent 6 ms in 84 calls (maximum 6 ms), a 97.8% reduction
in the measured driver-pipeline stage. Cache snapshots were approximately
0.6 MiB and took about 1–2 ms each.

The Vulkan driver blob is not stable enough for whole-file content
deduplication on the current Linux driver: two equivalent warm runs produced
the same 1.1 MiB size but differed in about 855,000 bytes. The hard per-process
write budget is therefore the disk-wear guarantee; texture/resource caches stay
in RAM/VRAM and are never serialized per frame.

This does **not** prove that every pipeline miss is cheap: guest shader parsing,
SPIR-V generation/optimization, and application pipeline lookup occur outside
`vkCreate*Pipelines`. It also does not improve the established steady-state
gameplay rate by itself.

If the cache is suspected after a driver update:

1. Point `KYTY_VULKAN_PIPELINE_CACHE` at a new empty path for one run.
2. Confirm the new file has a nonzero size and a second run still reaches the
   same strict frontier.
3. Compare Release+Silent runs with the same resolution and shader-cache state.
4. If a file is stale, remove only that cache file. Do not disable validation,
   invent a pipeline, or substitute a placeholder shader.

Malformed, foreign-device, oversized, and unreadable files are ignored; cache
I/O failure is a performance miss, not a guest-visible semantic fallback.

### SPIR-V persistence without render-thread write I/O

SPIR-V cache misses previously compiled the shader and then synchronously
scanned the cache directory, evicted old files when necessary, wrote a
temporary entry, flushed it, and replaced the destination. A module compiled
through the translation cache could exercise both the source and module cache
paths. The data volume was bounded, but filesystem latency remained part of
the first-use frame.

Persistence now uses one writer thread per cache store. Producers copy a
validated immutable entry into a queue bounded to 64 entries and 8 MiB. The
worker retains the existing file format, exact identity validation, atomic
replacement, 64 MiB total capacity, 4 MiB entry limit, and 16 MiB attempted
write budget per session. Duplicate queued, in-flight, or successfully written
identities are coalesced. If the queue is full or persistence fails, the
compiled shader remains valid in the in-memory translation cache; disk I/O is
only a performance aid.

The store drains and joins its worker before destruction. Tests that need to
inspect, corrupt, or reopen a just-enqueued entry call the explicit drain
barrier rather than racing the writer. Queue saturation is observable through
stats and never causes a second compilation inside the same in-memory cache.
Cache reads do not acquire the writer's filesystem lock: they consume either
the immutable queued entry or a completely published file, so a directory
scan, eviction, flush, or rename cannot stall an unrelated cache hit.

An isolated cold run persisted 69 SPIR-V entries (about 4.2 MiB) and a 0.54 MiB
driver cache. After the scene warmed, a 30-second gameplay window presented
1,185 frames at about 46 FPS with p95/p99 frame times of 27/28 ms and no frame
over 50 ms. A restart against those files reported 23 translation-cache hits,
zero misses, and zero SPIR-V compilations. The strict three-edge visual gate
also passed against the prior baseline. These measurements verify persistence
and absence of a steady-state regression; they do not claim that shader or
pipeline compilation itself is asynchronous.

A later isolated cold/warm pair disabled the host driver's shader cache and
used the same Kyty cache population. The warm restart changed 4 SPIR-V
compilations into 23 exact translation-cache hits and reduced inclusive
pipeline-miss time from about 425 ms to 0.49 ms. With real frame intervals,
startup samples above 50/100 ms fell from 14/9 to 11/7; the single multi-second
loading interval remained in both runs and is therefore not attributed to the
cache. The synchronous Vulkan-cache checkpoint was also measured directly:
0.081 ms cold and 0.341 ms warm in this pair, too small to justify another
writer solely for frame-time improvement.

### Redundant hashes on GPU-owned surfaces

`GpuMemory::Update` previously hashed every `RenderTexture` and
`VideoOutBuffer` once per submit even when the object could not upload CPU
guest memory. Two existing contracts make those scans redundant:

- a tiled `VideoOutBuffer` is GPU-owned and its update callback returns without
  a CPU upload;
- a tiled `RenderTexture` without write-back is GPU-owned and its update
  callback preserves the current Vulkan layout without reading guest memory.

The constructors now derive `check_hash` from the same CPU-upload eligibility
used by their update callbacks. Linear display buffers and render targets with
write-back retain hashing. Texture, storage, vertex, and index resources also
retain their previous policy, so this change does not hide native guest writes
to CPU-backed data.

A 75-second Release+Silent discovery run used temporary counters around
`calc_hash`, grouped by resource type. Across thirteen five-second reporting
windows, `RenderTexture` and `VideoOutBuffer` produced zero hash calls after
the change. `Texture` remained dominant: 53.831 GiB and 4043 ms total across
those windows (the interval included loading and gameplay-era work). Individual
active windows still approached roughly 1.0–1.6 GiB/s and 100–140 ms/s.

This is a correctness-preserving removal of impossible work, not a claim that
steady-state FPS is solved. It narrows the next memory optimization to
CPU-backed textures. Replacing XXH64 with a faster hash can reduce cost per
byte but still performs full scans under the global memory mutex. The
higher-ceiling design is page-level dirty tracking with:

1. fixed, async-signal-safe page metadata;
2. all overlapping resource owners marked dirty on the first write;
3. explicit invalidation for HLE/managed writes;
4. preservation and restoration of the original guest protection;
5. hash fallback whenever page watching cannot be armed.

Do not restore the historical watcher directly: it locked a mutex, allocated
containers, and invoked callbacks from the write-fault handler. Those
operations are not signal-safe and can deadlock the emulator.

### Unknown `ds_read2_b32`

Capture the instruction words and decoded fields before editing. The observed
instruction reads two LDS dwords. Its `vaddr` is a byte address while `offset0`
and `offset1` are dword-scaled:

```text
address0 = vaddr + offset0 * 4
address1 = vaddr + offset1 * 4
vdst     = Workgroup[address0]
vdst + 1 = Workgroup[address1]
```

Resolve this at the shared DS decode/SPIR-V seam. Do not special-case the
captured program counter and do not treat the offsets as raw bytes. Verify that
the generated module uses the same Workgroup storage and address convention as
`ds_write_b32`.

### RenderTexture becomes undefined during Update

First distinguish image creation from update re-entry:

1. Record the resource relation, ownership, old layout, and requested
   transition.
2. Confirm whether CPU write-back invalidated the exact resource or only an
   overlapping alias.
3. If the GPU still owns valid contents, preserve its current layout.
4. If an evidenced invalidation requires discard, transition from
   `VK_IMAGE_LAYOUT_UNDEFINED` and do not claim content preservation.

Do not make this decision from GPU vendor IDs. The resource state and Vulkan
layout contract determine the transition.

### Null MRT tails lose discard semantics

Inspect the complete export sequence rather than only MRT0. A shader can write
one or more MRTs and end with null exports that carry termination/no-write
semantics. Keep those tails in the normalized shader representation so the
SPIR-V generator does not invent output for an inactive target.

Validate both forms:

- active MRT exports still write their declared components;
- null MRT0–3 tails produce no color write and preserve control flow.

Do not substitute zero color for a null export. Zero is observable output;
no-write is a different contract.

### Transparent quads become black rectangles

Use the producer/consumer boundary to avoid misdiagnosing this as texture
tiling or blending:

1. Confirm the sampled texture contains meaningful alpha.
2. Confirm alpha reaches the pixel shader and the shader emits `OpKill`.
3. Confirm zero-alpha blending would preserve the destination.
4. Inspect execution modes. If a kill-enabled shader also declares
   `EarlyFragmentTests`, depth may commit before the discard.

The implemented policy is:

```text
pixel kill enabled  -> omit EarlyFragmentTests, commit depth after discard
opaque early-Z      -> retain EarlyFragmentTests
```

Do not add a second pattern-matched alpha test, disable depth globally, or add
a vendor-specific workaround. Those changes hide the ordering bug and can
break opaque geometry.

### Automatic input is not a graphics fix

`KYTY_AUTO_CROSS` is useful only to expose a later graphics frontier quickly.
It does not validate keyboard/controller routing, press/release edges, scene
control, or synchronization. Keep discovery captures labeled accordingly.

For acceptance, start from a strict environment, deliver real input edges,
move in both directions, perform one action, and confirm that frame
presentation continues without reintroducing the visual defects above.

## Current frontier

There is no known process failure or repeatable visual corruption before the
current gameplay-era checkpoint. Always re-capture: a new structured EXIT,
host fault, or earlier visual regression supersedes later work.

The first proven bad boundary had been native VideoOut. The writer sampled a
valid RGBA8 atlas and emitted coverage into a four-MRT G-buffer. A later pixel
shader performed an alpha comparison and reached `OpKill`, but its guest depth
mode requested early rejection followed by late depth commit. Kyty translated
that mode to Vulkan `EarlyFragmentTests`, which can commit depth before
`OpKill`. Transparent portions of a sprite quad therefore occluded later work
as opaque rectangular footprints.

The fix omits `EarlyFragmentTests` when `shader_kill_enable` is active, allowing
Vulkan depth commit after fragment discard. Opaque shaders keep the existing
early-fragment path.

The current performance frontier is separate from graphics correctness. The
latest Release+Silent reset window covered 601 gameplay frames and reported
41.688 FPS with p50/p95/p99 frame times of 27/35/40 ms, a 52.140 ms maximum,
and one frame above 50 ms. Command processing consumed 12.357 s, submit/fence
waits 8.167 s, and `WAIT_REG_MEM` 7.197 s in that window; these nested timings
must not be added together. The remaining gap to stable 60 FPS still requires
producer-level work in command submission, synchronization, GPU memory
tracking, and resource binding. Change one contract at a time and compare
against the same correct gameplay capture.

## Evidence and exclusions

- Source atlas dumps contain meaningful alpha; the sampled descriptor used
  RGBA8 UNORM with identity swizzle and guest upload.
- Blend factors, compressed MRT component order, and sampled alpha propagation
  matched the captured contracts. With source alpha zero, blending preserves
  the destination.
- Adding another pattern-matched alpha discard did not help because the
  original failure was the timing of depth commit relative to an existing
  discard.
- Pure CPU tile-27 detile and final VideoOut conversion were not the producer.
- A red test proved the old generator emitted `EarlyFragmentTests` for a
  kill-enabled shader. The same test now proves kill-enabled shaders omit it
  while opaque early-Z shaders retain it.
- Temporary MRT, descriptor, and frame-selection instrumentation was removed
  before the semantic commit.
- Retiring an idle `StorageTexture` by frame age can discard the only valid
  GPU-authored contents because that object has no GPU-to-guest write-back.
  Permanently excluding every storage image from retirement was also rejected:
  distinct images in a long-lived mapped heap would have no residency admission
  bound. The complete fix must treat writable storage images as live resources,
  reserve their actual Vulkan memory requirements against the device-reported
  memory budget, release that reservation only after deferred destruction, and
  keep reconstructible textures on the existing bounded retirement path.
- On the current Gen5 world-scene branch, mixed sampled-image operations now
  use the compatible regular host sampler path and participate in shader
  identity. A strict, validation-enabled run advanced beyond the former mixed
  sampling exit, but that advancement is not evidence of correct 3D output.
- Live Gen5 buffer descriptors now validate the complete access width, reject
  wraparound, retain one resolved storage slot and base offset for a vector
  access, and make invalid accesses zero/no-op instead of falling through to
  storage slot zero. Atomics use the same resolved identity. Two bounded
  strict, validation-enabled runs exceeded 12,000 presents without a Vulkan
  device loss; captures still showed only coherent 2D screens plus corrupted
  3D fragments, so geometry correctness and gameplay remain open.
- The SPIR-V live-address resolver now also accepts a valid 48-bit stream span
  that crosses one 4 GiB low-word boundary, while rejecting larger high-word
  jumps and the 48-bit wrap. A red/green source test and toolchain assembly
  cover the contract. One subsequent bounded strict, validation-enabled route
  exceeded 13,000 presents with no last error or device loss, but its captures
  progressed only through the prompt, logo, and coherent 2D menu. No world or
  vehicle geometry was visible, so the boundary fix is retained as a general
  correctness change but is not established as the current 3D producer.
- A recorded session experiment disabling append-stream handling did not
  restore the missing world geometry, excluding append behavior as the sole
  producer for that worktree. No tracked patch, manifest, or reproducible
  command preserves the experiment, so do not generalize it to interactions
  with later live-resolver changes and do not repeat it without new evidence.
- Extending an existing detected buffer to the union of overlapping byte spans
  was tested as a vertex-fetch hypothesis. The strict run reached the
  interactive screen, then lost the Vulkan device immediately after the first
  diagnostic input edge. The change and its tests were reverted. Record count
  cannot be treated as an exact byte-span identity without an independently
  evidenced contract.
- Removing the depth-attachment creation-time HTILE clear mark did not improve
  the strict visual result and was reverted. The experiment excludes that
  single stale-clear site as the demonstrated producer; it does not prove that
  all depth metadata identity and lifetime handling is correct.
- A bounded debugger stop on the first target world draw materialized identical
  nonzero depth read/write bases, identical stencil read/write bases, the
  programmed HTILE base, no explicit depth clear, and enabled depth test/write
  with the captured greater-or-equal comparison. That draw therefore excludes
  divergent read/write attachment selection, a missing HTILE identity, and a
  stale explicit clear as its immediate producer. The structural lifetime risk
  below remains general; it is not causal evidence for this draw.
- The remaining depth model can retain distinct nonzero read and write bases
  while materializing the host depth image from the read base, and its pending
  HTILE clear state is keyed only by an address rather than the full attachment
  range, aspect, slices, and generation. Either mismatch can in principle
  explain geometry that appears only when depth is disabled. Current captures
  do not establish that causal chain, so do not invert reverse-Z, fabricate a
  clear value, disable depth, or import a color-compression policy as an HTILE
  fix. The next semantic change needs a red test and capture evidence for the
  complete read/write/HTILE attachment identity.
- Tightening embedded-fetch provenance was tested after a red fixture proved
  that dynamic attribute offsets, overwritten index VGPRs, and wide scalar
  loads could be misclassified as a position fetch. The guarded implementation
  and four negative/positive fixtures passed focused fetch, vertex, and shader
  cache tests and independent review. A single strict validation-enabled route
  then reached 12,332 presents but its uniform low-entropy native capture did
  not show 3D geometry. The frame may have been transitional, so it is not
  evidence of a new regression; it simply failed the required restoration
  gate. The translator change, cache-version bump, and fixtures were reverted
  together. This excludes that conservative provenance rewrite as the current
  3D fix; do not stack it with the next hypothesis.
- The target material draw also binds a 24 KiB scalar-loaded storage table whose
  entire guest-visible range is zero. A follow-up exact ownership trace found
  one Equals `StorageBuffer` owner and classified every matching owner as
  read-only; there was no writable overlapping backing awaiting GPU-to-guest
  publication. The same run reached more than 13,000 presents but its native
  capture remained low-entropy with no 3D geometry. This excludes descriptor
  initialization, host materialization, and a missing writable-buffer writeback
  as producers of those zeros. Do not seed the table or add a synchronization
  exception; the zero bytes are the guest-visible source for this draw. The
  temporary ownership trace was removed after capture.
- A temporary one-shot probe recorded HDR and depth immediately before and
  after the target world draw in the same graphics command buffer, then read
  the four bounded staging buffers only after that submission's fence
  completed. The draw changed 12,878 HDR pixels from zero to nonzero and
  changed the same 12,878 depth pixels. This proves that the target draw
  rasterizes and writes both off-screen color and depth; wholesale depth
  rejection is not the producer of the missing visible geometry. The next
  causal boundary is downstream: the first consumer, overwrite, or omission of
  that HDR image before VideoOut.
- A preserved bounded draw trace from the same branch records the same material
  draw writing an HDR RenderTexture and the first downsample binding that exact
  host image as `rt-exact`. The later full-resolution compositor also binds the
  same host image as `rt-exact` before writing the registered presentation
  buffer. This excludes a generic B10G11R11-to-RGBA alias mismatch and a
  guest-upload fallback for that observed route. It does not prove that every
  later frame or every overlapping HDR surface retains the same identity.
- The exact material pixel and vertex module identities were matched offline
  to their persistent shader-cache entries and their generated instruction
  censuses were classified without another guest run. Neither module contains
  the still-unsupported U64 CMPX, MIMG D16 packing, or FREXP operations. Do not
  port those missing operations as the current material fix without evidence
  from another failing shader.
- The exact pixel module does contain `V_MIN_F32` and `V_MAX_F32`, including a
  numeric epsilon clamp immediately before `V_RSQ_F32`. Their former direct
  GLSL `FMin`/`FMax` lowering did not encode the RDNA2 default-mode NaN operand
  selection. The lowering now tests both operands explicitly: one NaN selects
  the numeric operand, both NaNs preserve the RHS payload, and the ordinary
  numeric result remains the existing `FMin`/`FMax`. A red/green source test
  covers both operations, bit-preserving NaN selection and signed zero; SPIR-V
  assembly succeeds. Translator identity advances to 29 rather than reusing 28,
  which was written by a removed experiment. This semantic fix has not had a
  new guest validation because of the repeated device loss below, so it is not
  yet evidence of restored 3D geometry or playability.
- The preserved probe artifact later records a Vulkan device loss in
  `vkQueuePresentKHR` before the second required input edge. It is diagnostic
  evidence only, not a strict route or playability result. The readback probe
  and its tests were removed after recording the evidence; do not repeat it.
  The retained depth-readback barrier correction transitions both depth and
  stencil aspects for combined D32S8 images while copying only the depth plane.
- A second, separately created census artifact records the same presentation
  failure after the first diagnostic input edge and contains no census output.
  Its guest log is byte-identical to the probe artifact and neither artifact
  preserves a manifest, environment snapshot, submit identity, validation
  output, or process-exit record. They therefore establish the repeated error
  signature but cannot independently prove two differently configured launches
  or exclude the removed probe as the sole condition. Do not launch another
  guest probe until one bounded host-only diagnostic can join the failure to a
  submit attempt.
- Static tracing then found that both command-buffer submit paths discarded a
  non-success `vkQueueSubmit` result, recorded a successful submit state, and
  allowed the semaphore path to continue to present. They now share one
  submit-then-publish seam: statistics and `m_execute` are updated only after
  `VK_SUCCESS`, and an error stops before present after releasing the queue
  mutex. The red/green test injects device loss and host OOM and proves that
  neither path publishes; success still publishes. This makes the next failure
  report earlier and safer but remains host-only validation, not the cause of
  the prior device loss or evidence of corrected 3D.
- The same audit found that a timed-out or failed fence was nevertheless
  published as completed: callbacks could drain, the command buffer could be
  reset or freed, and `m_execute` was cleared while GPU work might still be in
  flight. Completion and reuse now occur only after `VK_SUCCESS`; blocking
  waits and swapchain acquire waits are bounded, and hard errors stop before
  using an invalid image or command buffer. Pure red/green tests cover timeout,
  not-ready, device loss, and success.
- `KYTY_SUBMIT_FAULT_TRACE=1` now records a fixed CPU-only trail of up to eight
  attempts across the ordinary, semaphore-signalling, and existing GPU detile
  submits. It evicts only the oldest completed attempt and never an in-flight
  one; when all slots are active, the immediate failed submit context remains
  available in the one-shot header even though it is counted as untracked. It
  stores only host attempt/order fields, logical submission identities, frame,
  and the last decoded PM4 opcode/offset when available; it emits one header
  plus at most eight records on the first device loss. It adds no Vulkan
  commands, waits, markers, readbacks, captures, retries, handles, or guest
  addresses. If submit itself rejects the attempt, its immediate record is
  exact; if fence, acquire, or present observes an asynchronous loss, the trail
  is a predecessor window rather than proof of one raw GPU command. Build and
  pure tests pass. One later manifest-backed strict diagnostic enabled the
  trace, delivered one input edge, and advanced thousands of presents without
  reproducing device loss or emitting a fault record. That negative run proves
  only its own bounded route: the earlier failing artifacts preserve no
  comparable manifest, and the trace's loss path remains runtime-unexercised.
- The same run exposed a separate bounded-resource frontier. Its stable
  pre-input snapshot had 15 live GPU objects; after the input-triggered loading
  transition it had 5,038, including 4,659 live `StorageBuffer` objects and
  3,916 `NewLinked` creations, while instantaneous FPS was below 4. The process
  remained under the 2 GiB memory-high threshold at the sampled point and was
  terminated deliberately; there was no capture or visual acceptance. Static
  tracing shows that each distinct overlapping storage range can allocate a
  new backing and acquire bidirectional alias links, while `GpuMemory::FrameDone`
  excludes every linked object from periodic retirement. This establishes an
  unbounded-lifetime mechanism, but `NewLinked` does not encode parent type or
  overlap relation, so it does not prove that all 3,916 creations share one
  causal topology. Before changing retirement, prove which read-only view/peer
  relations can be detached without losing GPU-owned surface state or pending
  submission ownership.
- The current worktree now classifies each effective `NewLinked` creation into
  exactly one anonymous aggregate: buffer-only read-only, surface-connected,
  mutable/other, or traversal-truncated. The graph walk is bounded by 64 unique
  nodes and 128 examined parent/link edges under the registry mutex; reclaim and
  create-from-object outcomes do not run it. Host-only tests cover a transitive
  surface connection, a mutable component, more than 128 parents, and a reclaim
  path. These counters remain diagnostic only; the collection below does not
  select a safe retirement subset or demonstrate a 3D correction.
- A subsequent single strict run collected that window under a 150-second
  timeout, 2 GiB memory-high, 3 GiB memory-max, zero swap, and 200% CPU quota.
  After resetting the counters before one submitted diagnostic `cross`, the
  next 107.7 seconds recorded 7,445 linked storage creations: 1,767
  buffer-only read-only, one mutable/other, zero proven surface-connected, and
  5,677 traversal-truncated. Storage-buffer live count reached 8,145, total
  live objects 8,524, and instantaneous FPS 6.754; cgroup memory peaked at
  2,156,904,448 bytes before timeout exit 124. No capture or visual acceptance
  was requested. Because 76% of the created topologies exceeded the diagnostic
  budget, zero observed surface-connected components is not proof of absence;
  do not retire the truncated set or claim that lifetime growth caused the 3D
  failure.
- `GpuMemory::FrameDone` now retires only a complete, currently proven
  buffer-only read-only component containing a storage view. Preflight accepts
  at most 64 SB/VB/IB nodes and 128 links, requires `Common`, age at least 120
  frames, no bound depth metadata, and completed submission dependencies, then
  frees every member or none. Each retirement pass has a separate 2,048-unit
  scan budget. The depth exclusion is host metadata set at both existing HTILE
  association points, not a cast of the opaque backing. Tests cover a real RO
  storage write-back callback without invoking it, whole-component retirement,
  a pure 130-storage truncated component, and a surface-connected component.
  Before the later run, this change had independent host review only. It
  deliberately leaves the dominant truncated set untouched and is not evidence
  of bounded runtime growth or corrected 3D.
- One later bounded strict run falsified that limited retirement as sufficient.
  Its pre-input window showed one linked storage creation, one logical free,
  and five live storage objects. After a reset and one submitted diagnostic
  `cross`, 57.752 seconds recorded 8,135 new linked storage objects, only 102
  logical frees, 8,902 live storage objects, 9,249 total live objects, and FPS
  2.723. Classification was 1,815 buffer-only read-only, 6,320 truncated, and
  zero surface-connected/mutable. A 30-second no-capture watch still advanced
  frame/present by 207 with no last error. Cgroup memory peaked at
  2,185,240,576 bytes with zero swap; timeout ended the sole process with exit
  124. There was no capture or visual acceptance. Therefore the fixed
  component scan may reclaim small dead graphs but cannot catch an actively
  growing large component.
- The next host-only correction keeps those large read-only ranges out of the
  persistent graph when the existing transient-snapshot contract proves guest
  memory canonical: up to 512 KiB, allocated range, and a complete overlap
  snapshot containing only read-only SB/VB/IB. Command-buffer fence ownership
  bounds lifetime; a 16 MiB pool with a 1 MiB critical reserve bounds memory.
  Reused slabs clear their unused tail, and descriptor-cache identity includes
  logical range so a smaller reuse cannot inherit a larger `VK_WHOLE_SIZE`.
  Every unsafe or exhausted case falls back to the strict persistent path.
  A later 105-second strict diagnostic reset its counters before the first
  input edge. After 58.497 seconds and two bounded edges it reported only 18
  new linked storage objects, 30 live storage objects, and 116 total live GPU
  objects, versus 8,135, 8,902, and 9,249 respectively in the preceding
  comparable post-input window. The sampled cgroup peak after the first
  transition was 2,149,568,512 bytes with zero swap; timeout ended the only
  process with exit 124 and no last error. Native captures showed a coherent
  credits screen and main menu without directional stripes. Both official
  offline scores remained `gameplay_like=false`, so this validates containment
  only on the observed route, not correct PLAY geometry, long-session memory
  stability, or playability.
- A later attempt to reproduce the exact two-edge PLAY checkpoint did not send
  input: `wait-present --min 8000 --timeout-ms 45000` expired, while the
  immediately following status had already reached present 8,446. The run was
  stopped deliberately with no last error, a sampled 2,140,639,232-byte cgroup
  peak, zero swap, 19 linked storage creations, 21 live storage objects, and 75
  total live objects. This is additional pre-input containment evidence only.
  Keep the present target at 8,000 but allow the condition waiter 60 seconds;
  do not send an early edge, increase the target, or count this run as a visual
  comparison.
- One bounded follow-up used that 60-second waiter and reached present 8,002,
  then delivered exactly two `cross` edges separated by 40 presents. Native
  captures at presents 8,084 and 8,320 showed only the small game overlay on a
  black frame, not the earlier car or world. The official scores were
  `gameplay_like=false`, `scene_ok=false`, and `stripey=false` for both. At
  present 8,513 both edges were confirmed delivered, `last-error` remained
  null, 17 new linked storage objects and 29 live storage objects were
  reported, and cgroup memory had peaked at 2,149,277,696 bytes with zero swap.
  The sole process was stopped deliberately. This is a comparable missing-3D
  result: transient snapshot containment prevents the prior object explosion
  on this route but is not sufficient to restore PLAY geometry.
- Host analysis then found an independent Gen5 input-materialization defect.
  A storage stream with sentinel `start_register = -1` was incorrectly treated
  as occupying descriptor registers `[-1, 3)`, so direct SGPRs 0, 1, and 2
  were omitted while SGPR 3 was emitted. With the GS-prolog shift these are
  shader SGPRs 8 through 11 and can feed live vertex-resource resolution. A
  focused red test demonstrated exactly the missing stores; descriptor overlap
  now ignores a non-extended storage range whose start register is negative.
  The new test and three adjacent embedded-fetch/storage-sharing contracts pass,
  and `fc_script` builds under the bounded `-j2` profile. This is host-only
  evidence of the corrected input contract, not yet a visual 3D or playability
  result.
- The first bounded strict attempt after that host correction cannot validate
  or reject it. The 95-second, 2.5 GiB, zero-swap run delivered both intended
  input edges and reached an `interactive` phase, with no last error and a
  1,824,403,456-byte host peak. Its only native capture was taken earlier while
  the runtime still reported `loading`; it showed the same small overlay on a
  black frame and failed the official gameplay/scene gates. The process reached
  `interactive` immediately before its total timeout, so no comparable
  post-transition capture exists and the run is inconclusive.
- That attempt also exposed a validation defect: the SPIR-V translator identity
  had remained at version 29 after changing SGPR materialization. A persistent
  module hit returns before `ShaderRecompileVS`, so the semantic correction can
  be bypassed by an older version-29 entry. No module files were written during
  the attempt; the newest persistent entries predated it. A red test proved the
  current key was identical to a legacy version-29 key. Translator identity is
  now 30, and focused persistent-hit, version-invalidation, and SGPR tests pass.
  The next strict visual run must use this identity before drawing any conclusion
  about the SGPR correction.
- One bounded strict run then exercised translator identity 30. It recorded 83
  translation-cache misses and 40 SPIR-V compilations, and new persistent
  modules were written during the run, so the corrected SGPR path was not
  bypassed by a version-29 module. Both intended input edges were delivered.
  The first native capture, at present 8,773, showed the coherent `PLAY` overlay
  plus only a few small 3D fragments; after another 284 presents, the second
  showed the overlay on black with no car or world. Official entropy was 0.2712
  and 0.2513 respectively; both remained `gameplay_like=false`,
  `scene_ok=false`, and `stripey=false`. `last-error` stayed null, host peak RSS
  reached 1,971,658,752 bytes, storage stayed bounded at 27 live objects with
  15 linked creations, and the sole process was stopped deliberately. This
  falsifies restored direct SGPR materialization as a sufficient 3D correction
  for the observed route. Retain the general sentinel/cache contracts, but move
  the visual investigation back to the first live V# consumer or the later
  attribute-provenance/remap boundary; do not claim geometry restoration.
- Offline decoding of every new version-30 module for the target vertex stage
  showed that this draw does not use the live MUBUF resolver. All five module
  variants expose dense Vulkan vertex inputs at locations 0, 1, and 2. Exactly
  one identity matches the captured mesh contract: RGB32F position at offset
  0, RGBA16F normal at offset 12, RG16F UV at offset 28, and stride 32. The
  other identities carry distinct, internally coherent layouts for other
  streams. The matching module's input types and `VulkanBuildVertexInputLayout`
  format/component checks agree; no format, offset, or location mismatch is
  established. Module identity does not include stream address or record count,
  so it cannot prove that the matching draw bound the expected 10,310 records.
  The existing checksum-filtered, hard-capped draw trace already records the
  effective VS identity, stream stride, attributes, record count, required
  records, and bounded vertex samples. Use it with one matching draw and a
  present floor before changing vertex layout or fetch provenance.
- The first attempt to collect that bind record used only the 32-bit checksum
  suffix from an older note. The trace compares the complete 64-bit pixel-stage
  checksum, so no draw matched and no log file was created through the bounded
  two-edge route. The existing parser test confirms exact 64-bit matching and
  rejects a different full checksum carrying the same kind of short suffix.
  This attempt provides no evidence that the draw was omitted and makes no
  statement about its vertex bind.
- The corrected full-checksum `limit=1` trace captured a different mesh variant
  sharing the same pixel and vertex-stage identities. Its stream used stride 24
  with attributes at offsets 0, 12, and 20, the required and declared record
  counts both equalled 9,081, vertex-input layout validation succeeded, and the
  bounded position samples were finite and plausible. That bind is internally
  coherent and does not establish the missing stride-32 mesh as corrupt. Offline
  cache inspection also shows that all five layout variants share the same
  vertex-stage checksum, so another checksum filter cannot distinguish them.
  Keep the existing exact pixel-stage trace, collect at most 32 matching draws
  after the present floor, and select the stride-32 layout offline. Do not add a
  new trace mode or unit test for this diagnostic distinction.
- Two capped attempts at that 32-match integration did not reach their present
  floor. The first exited at present 6,089 before input or a matching record;
  observed memory stayed below the 2.5 GiB hard limit and peaked near 2.15 GB.
  The second stopped at present 5,268 for about 49 seconds with no last error,
  no ordinary submission in flight, no suspended `WAIT_REG_MEM`, and about
  1.69 GB resident. It was terminated before input and also emitted no draw
  record. Neither run says anything about vertex binding or the visual fault.
  They were also launched as detached transient services, unlike the earlier
  successful scope-and-PTY invocation recovered from the session transcript.
  Do not classify this as a repeated renderer stall yet. The next bounded cycle
  must reproduce that exact launcher contract, explicitly clear diagnostic
  variables, and change only the trace limit from one to 32.
- The recovered scope-and-PTY launcher then reached the trace floor and captured
  the stride-32 material draw. Required and declared records both equal 10,310;
  position, normal, and UV decode as RGB32F at offset 0, RGBA16F at offset 12,
  and RG16F at offset 28. Bounded position samples are finite, normals are
  plausible, and the UV samples are distinct from position, including repeated
  coordinates outside the unit interval. The four interpolator controls select
  ordinary perspective-center inputs rather than custom interpolation. The
  material's first sampled image is a constant four-by-four LUT at every
  inspected mip, so coordinate order, wrap, and bias cannot explain variation
  in that sample. This excludes the observed vertex layout, a position-as-UV
  decode, and custom interpolation as immediate producers for this draw; it
  does not prove every later draw or vertex record.
- The same trace exposed a real shadow comparison mismatch. A pure two-
  dimensional depth-reference sample uses linear minification and magnification,
  while the translator filtered raw depth first and applied the guest comparison
  afterward. Those operations do not commute: percentage-closer filtering must
  compare footprint texels before filtering the comparison results. Pure depth
  bindings now retain a comparison sampler and lower explicit LOD-zero sampling
  to SPIR-V Dref only when every same-slot read-only descriptor is normalized,
  pure depth-reference 2D. Mixed, array, cube, color-view, and unnormalized
  consumers retain the non-comparison sampler and manual path. Sampler
  unnormalized-coordinate state participates in shader identity so the two
  paths cannot reuse one module. Translator identity advances to 31. Three
  existing focused contracts pass and the affected executable builds under the
  bounded `-j2` profile; no new test fixture was added.
- One subsequent strict two-edge integration exercised that translation with
  no permissive variables and zero cgroup swap. Both edges were delivered,
  `last-error` remained null, and memory peaked below 2.15 GB. The native capture
  still showed the coherent `PLAY` overlay over black with only isolated 3D
  fragments; the official scorer reported `gameplay_like=false`,
  `scene_ok=false`, `stripey=false`, entropy 0.2619, and 175 quantized colors.
  The capture was still in the loading phase and the first delta wait was
  scheduled thousands of presents after the initial floor, so it is not a
  gameplay-equivalent A/B. It nevertheless fails the restoration gate and
  proves only that the PCF correction is not sufficient for the current visual
  fault. Keep the general shadow contract, but do not claim restored 3D.
- Offline comparison of the existing 32-record exact material trace found no
  progressive draw-state drift. Every draw used a uint16 triangle list,
  zero vertex offset, a count divisible by three, and equal required/declared
  vertex records. The stride-32 mesh appeared three times with identical index
  and attribute inputs while color/depth addresses alternated as a stable
  two-surface pair; viewport, clip, cull, depth, blend, and interpolation state
  also remained fixed. This excludes changing input/state for those observed
  material draws, but not an incorrect vertex-shader result or a later HDR
  overwrite.
- The bounded material trace now also records the guest DCC registers and the
  selected Vulkan color attachment load operation. One strict, capped run
  recorded 32 exact material draws; all had DCC disabled, a zero DCC address,
  CMASK fast-clear disabled, zero clear words, and `LOAD` from an existing
  color-attachment layout. Therefore the deferred-DCC-clear behavior identified
  in another renderer is not exercised by these draws and must not be ported as
  their fix. This does not exclude a different first writer or workload using
  DCC. The two-edge route captured a credits screen rather than gameplay;
  official scoring remained `gameplay_like=false`, `scene_ok=false`, and
  `stripey=false` with entropy 2.2298. `last-error` remained null, peak host RSS
  was 1,762,099,200 bytes, and the sole process was stopped deliberately. The
  run is diagnostic evidence only and does not establish restored 3D.
- A subsequent bounded render-target lifetime trace followed the two HDR
  targets by exact host-image identity from their material writers through the
  first downsample and compositor samples. For one complete target cycle, the
  first writer after the preceding sample used an explicit host `CLEAR`; all
  following scene writers used `LOAD`, retained the same host image, and the
  downstream samples resolved that same live image as `rt-exact`. The symmetric
  target followed the same writer-to-consumer route, and no color resolve was
  observed. This excludes a stale prior-frame clear, a read/write identity
  swap, and a guest-buffer or inexact-alias handoff for the observed cycle. It
  does not prove shader output coverage or correctness, and it is not evidence
  that every target or frame follows the same route. Do not change the clear
  policy without a guest-state discriminator; the next causal frontier is the
  exact vertex/pixel shader output feeding the otherwise coherent lifetime.
- That strict run synchronized each input edge against the native delivered-tap
  counter before starting the following present delta. It reached a coherent
  main menu rather than gameplay: `last-error` remained null, host RSS peaked
  at 1,927,507,968 bytes under zero-swap limits, and the sole process was stopped
  deliberately. The official capture scorer still reported
  `gameplay_like=false`, `scene_ok=false`, and `stripey=false`. Present-only
  input timing is therefore not comparable with the earlier route: a queued
  tap can remain pending for thousands of presents until the guest polls it,
  and the second delivered edge currently enters the menu. Re-establish the
  exact gameplay edge sequence with delivered-tap synchronization before the
  next visual acceptance run; do not add an unproven third tap.
- One subsequent strict attempt enabled only the existing exact shader probe to
  census the original pixel ISA before choosing another semantic port. The
  runtime became graphics-ready but remained at present 1 with no guest pad
  reads for more than one minute; it was stopped immediately and wrote no probe
  artifact. This attempt provides no evidence for or against FREXP, SDWA, D16,
  control flow, or the visual fault. Do not treat it as a repeated renderer
  stall or rerun it by extending the timeout. A later attempt must first collect
  the bounded native stall diagnostics in the documented order and change no
  shader behavior.
- A condition-gated rerun with the same exact probe did not reproduce that
  early stall: its 15-second native watch advanced 5,419 presents with
  `last_error=null`, both requested input edges were confirmed delivered, and
  the target pixel-stage probe was written before the process was stopped.
  The scope peaked at 1.6 GiB with swap disabled, and the host journal recorded
  no GPU reset, OOM, or killed process. The original-instruction census contains
  no FREXP operation, no instruction degraded to the empty barrier, and no U64
  CMPX. Its nine conditional-mask operations all use the ordinary source format
  rather than SDWA. It contains one biased image sample and one level-zero depth
  comparison sample; the legacy dump does not retain the raw MIMG D16 bit, so
  D16 result packing remains unproven rather than excluded. Do not port FREXP,
  SDWA-CNDMASK, or U64-CMPX changes for this material shader. The present-floor
  client overshot while the guest was advancing rapidly, and no capture was
  requested, so the run is ISA evidence only and not a visual A/B or gameplay
  acceptance result.
- The next exact trace targeted the final blended writer observed before the
  HDR consumer. Eight draws used a full 1920x1080 viewport and guest scissor,
  but all were indexed triangle geometry rather than a fullscreen primitive.
  Required and declared vertex records matched; sampled positions and UVs were
  finite; the sampled 512x512 image retained one stable guest-upload host
  identity; and the relevant blend factors resolve to ordinary source-alpha /
  one-minus-source-alpha. Other instances sharing the pixel checksum used a
  different vertex-stage identity, BC1 texture, depth-write state, and disabled
  blending, confirming that the checksum alone does not identify one pipeline
  instance. This excludes an obvious fullscreen overwrite, malformed sampled
  identity, out-of-range vertex stream, or incorrect 4/5 blend-factor mapping
  in the observed records. Coverage still depends on transformed geometry, so
  it does not prove that this writer cannot cover a large part of the target.
  Do not alter its blending, UV layout, or texture materialization without a
  transformed-output or coverage discriminator.
- A separate exact trace covered the depth-reference writer that also feeds
  the HDR target. Its eight records were indexed triangle lists with matching
  required and declared vertex ranges, valid input layouts, full guest
  viewport/scissor, and stable color and depth image identities. The translated
  fragment inputs use locations 0, 1, 2, and 5, exactly matching the four guest
  interpolator settings observed for this shader; there is no off-by-one input
  binding. The depth descriptor remained the same exact 2048x1024 host depth
  image with the guest compare function and normalized sampler state used by
  the comparison path. This excludes an attachment-identity, descriptor-slot,
  or interpolation mismatch for the observed writer, but not an incorrect
  comparison result or shader arithmetic. Its tiled UVs are finite; values
  outside one unit are not by themselves an error and must not be clamped
  without a guest wrap-mode discriminator.
- Offline comparison of the cached exact material vertex modules found the
  last three translator identities byte-identical. The older module differs in
  NaN-aware clamp handling for a normalized varying, but the four
  `gl_Position` operands retain the same dependency chain. A dependency slice
  of the current position output reaches only the position input and the first
  16 dwords of two transform buffers. The preserved exact draw trace shows both
  ranges fully materialized, finite, matrix-shaped, and free of out-of-bounds
  access while camera and object transforms change coherently between draws.
  A previously inspected offset beyond that dependency slice contained zeros
  or non-matrix data and is not position evidence. This excludes a translator-
  version position regression, a swapped transform slot, and truncated matrix
  storage for the observed draws; do not revert float semantics or rewrite the
  vertex layout on that basis.
- A dependency slice of the exact material fragment output reaches the regular
  color sample, the depth-reference sample, interpolated locations 0, 1, 2,
  and 4, and four nonzero material/transform storages. The separate 24 KiB
  all-zero buffer is reachable only through the light-accumulation loop. For
  traced ordinal 0, the two values feeding that loop bound are 0 and 1, so the
  generated NaN-aware minimum, truncation, and integer conversion produce a
  zero bound and the body executes zero times. The zero table therefore cannot
  blacken that observed draw through the loop; do not seed it or invent an HLE
  producer. The final two packed-half conversions belong to the guest's
  compressed MRT export and are reconstructed as a float Vulkan color output.
  They are not MIMG D16 sample-result packing, so the independent D16 image
  packing change found in another renderer is not applicable on that evidence.
  The legacy instruction census does not retain the raw MIMG D16 bit, so a
  different D16 sample remains a separate evidence question.
- A new strict integration used one bounded process, Silent output, two input
  taps, and synchronized each edge against `delivered_taps`. After the second
  edge, a native capture at present 14,182 reached an actual race: lap, position,
  speed, damage, timer, and minimap were active, while almost the entire 3D
  world was black except for a few lit or white fragments. The scorer classified
  it as low entropy (`healthy=false`, entropy 0.6487, 353 color bins, no
  directional stripes). After another 120 presents the race advanced to its
  time-over screen, proving simulation/presentation progress but not rendering
  correctness. `last-error` remained null, the cgroup reported a 2 GiB peak
  below its 2.5 GiB hard limit with swap disabled, and the sole process was
  stopped deliberately. This is the current reproducible gameplay visual
  failure and replaces loading/menu captures as the acceptance reference; it
  is not playability.
- The opt-in render-target lifetime trace now arms the exact material depth
  attachment and, beginning with the next presented cycle, records every use
  of the same exact guest-address/host-image pair. Each bounded record includes
  raw read/write and stencil bases, host identity, depth load operation and
  initial/current layouts, test/write/compare/depth-clear/stencil-clear state,
  and HTILE range.
  A one-field identity match is emitted explicitly as `DEPTH_REMAP` or
  `DEPTH_IDENTITY_MISMATCH`; it is never silently folded into the tracked pair.
  Trace-disabled draws return before state initialization.
- A bounded strict two-input integration exercised that trace across the next
  gameplay cycle. Every observed world pass retained the same exact depth and
  stencil read/write bases, host image, and HTILE range; there was no remap,
  identity mismatch, or explicit depth clear. The first use only cleared
  stencil and disabled depth test/write, followed by reverse-Z `GEQUAL` passes
  that loaded and wrote the same depth image. This excludes split attachment
  identity, a host-image swap, and an obsolete explicit clear for the observed
  cycle. It does not prove whether the guest emitted an HTILE clear event.
- That first stencil-only clear exposed a general Vulkan contract defect:
  depth still used `LOAD`, but the combined attachment selected
  `initialLayout=UNDEFINED`. The load-op resolver now keeps
  `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` for stencil-only clears, while first-use
  and depth clears retain `CLEAR+UNDEFINED`. The existing focused contract test
  covers combined depth/stencil, stencil-less depth, and first-use cases, and
  the correction passed independent review.
- A post-fix strict capture still failed the visual gate. The race HUD, minimap,
  signs, and a few structures were visible, but most world geometry remained
  black; the scorer reported low entropy (`healthy=false`, entropy 1.239,
  340 color bins, no directional stripes). The process remained responsive and
  `last-error` was null. Therefore the invalid stencil-only render-pass state is
  fixed, but it is not the complete 3D correction. The next bounded question is
  whether an observed full-range HTILE clear write is linked, marked, and
  consumed before the reused depth image reaches the reverse-Z passes.
- A temporary bounded HTILE event trace then answered that question for the
  same cycle and was removed after use. The matching storage snapshot had the
  exact HTILE address and 196,608-byte range, but its contents did not match the
  observed metadata-clear pattern. The only mark came from new depth-image
  creation and was consumed successfully; no later guest clear mark existed for
  the reused image. Therefore exact-range matching did not discard an observed
  clear in this cycle. Do not synthesize a per-frame clear, relax the range, or
  attribute the remaining black world to a lost HTILE write on this evidence.
- The general custom-interpolation behavior from the sibling renderer was also
  compared against the exact material trace. Its four recorded interpolator
  settings are ordinary parameter locations 0, 1, 2, and 4; none selects flat
  or a fixed default. The current backend already decorates the supported
  perspective/linear center and centroid modes. Per-vertex custom interpolation
  remains an unimplemented general capability, but the trace does not record a
  consumed `is_custom` semantic or custom `V_INTERP_MOV` mode. Do not port
  fragment-barycentric handling as this material fix until those exact producer
  semantics are captured.
- The existing exact pixel-ISA artifact narrows the material output seam. The
  last three float color producers write `v15`, `v16`, and `v14`; two
  `VCvtPkrtzF16F32` instructions then pack those values for the compressed MRT0
  export. This makes the pre-pack floats a direct discriminator between an
  upstream material/lighting result and packed-half/export reconstruction. It
  does not establish that either side is wrong.
- The fragment tap changes translated SPIR-V but previously did not participate
  in the translation-module cache key. A warm cache could therefore return an
  untapped module, while isolating the whole cache forced unrelated shaders to
  recompile and changed the runtime timing. The tap selector now has an exact,
  collision-free module identity for the matching pixel shader only; normal
  module identities remain byte-for-byte compatible. The selector also accepts
  an instruction ordinal (`@N`) so an existing bounded ISA artifact that omits
  byte PCs can still name one instruction. Existing cache and compressed-export
  contracts cover persistent normal/tapped separation and ordinal selection.
- Two initial strict attempts to observe those final floats were not equivalent
  to the recorded present-8,000 route. The first isolated the entire shader
  cache and introduced global compilation delay; the second issued the first
  tap in a separate client call, allowing several thousand presents to elapse
  after the threshold. Both captured low-entropy title frames, reported no
  structured error, and remained below the 2.5 GiB no-swap limit. They are input
  timing evidence only.
- The corrected route performs threshold wait, both tap deliveries, both
  40-present deltas, and capture in one condition-driven client operation. At
  the requested present-8,090 title/PLAY slice, the exact ordinal-225 module was
  persisted with the matching pixel identity and canonical tap selector. Its
  pre-pack float output still produced only the previously observed isolated
  material fragments. Therefore `VCvtPkrtzF16F32` and compressed-MRT
  reconstruction are not the complete producer of the missing vehicle/3D; do
  not rewrite or bypass them on this evidence.
- The same exact route with ordinal 24 exported the raw four-channel
  `ImageSampleB` result. It restored substantially more colored vehicle parts
  and coverage, proving that the observed BC1 upload, biased sample, and UV path
  produce non-black material data before lighting. The silhouette remained
  incomplete, so this does not prove vertex positions or every mesh correct.
  The blackening is introduced after the base sample.
- Ordinal 205 exported the raw `ImageSampleDrefLz` result in the red channel and
  captured it as zero over the material fragments. That is not yet proof of a
  reversed compare or the final blackening: the guest immediately applies its
  shadow-strength blend and forces one when the reference lies outside
  `[0,1]`. The next causal seam is ordinal 211, after that guest correction.
  Do not invert GEQUAL, fabricate shadow depth, or disable the comparison from
  the raw sample alone.
- The exact route at ordinal 211 showed a nonzero effective shadow factor over
  real vehicle fragments after the guest strength and out-of-range correction.
  Ordinal 221, after one final RGB light-accumulator update, retained only a few
  isolated bright pieces. This excludes a globally zero effective shadow
  factor and moves the blackening seam into the material-light accumulation;
  do not invert the depth-reference comparison on this evidence.
- Ordinal 126 observed the ordinary location-4 interpolator used by `attr3`.
  It was not globally absent, but appeared only on fragmented vehicle strips.
  That capture does not distinguish an incomplete producer from legitimate
  per-material values. The final common light factor provides the sharper
  discriminator: it is computed before the RGB accumulators and multiplies
  them uniformly.
- Ordinal 218 exported that common factor after shadow composition and scalar
  strength, before attenuation, and showed a nonzero red component. Ordinal
  219, immediately after the sole `VMulF32 v1, v27, v1`, reduced that red
  coverage to isolated fragments. Therefore `v27`, the clamped dot of
  interpolated location-2 normal and a three-scalar light vector, is the
  demonstrated blackening seam for most of this material. The five bounded
  integrations completed the exact present-8,000/two-edge route without a
  structured error or OOM; peak cgroup memory ranged from about 1.55 to 2.15
  GB under the 2.5 GiB no-swap limit, and every process was stopped before the
  next run. The next evidence must
  separate the interpolated normal from the scalar light-vector binding. Do
  not negate the dot, seed ambient light, or replace the clamp without that
  producer evidence.
- The existing resource trace already resolves the light-vector side for the
  same shader: the storage bound at scalar base `s24` is readable and begins
  with finite, nonzero values `(-0.135619, 0.655012, 0.743348)`. Its exact
  `SBufferLoadDwordx8` uses null SOFFSET with immediate zero, so the known x8
  dual-offset emitter gap is not exercised by this instruction. This excludes
  a zero or displaced light-vector load for the observed draw. The remaining
  producer question is the transformed/interpolated location-2 normal and its
  agreement with the light vector's coordinate space.
- Ordinal 42 then exported the normalized location-2 normal. The direct signed
  float visualization was almost entirely black, but negative components and
  zero both clamp to black at the UNORM capture target, so this is not evidence
  that the vector is zero. Offline inspection of the already cached exact
  vertex module confirms that output location 2 is a three-component result
  normalized after its transform chain, and that output location 4 is also
  written; there is no simple missing varying export in that module. The next
  discriminator must preserve sign, or compare the transformed normal and
  light vector numerically in the same coordinate space. Until then, changing
  handedness, component order, or normal-matrix math remains unproven.
- The tap now has an opt-in signed visualization that maps each existing value
  with `0.5*x+0.5`; its bit participates in the exact diagnostic module
  identity and leaves ordinary modules and raw taps unchanged. The signed
  ordinal-42 route reached present 8,088 and exposed varied, finite normal
  components over real geometry, excluding a globally zero location-2 normal.
  The process remained below 1.66 GB with no OOM or structured error.
- The first signed ordinal-161 attempt exposed a separate diagnostic defect:
  the tap loaded four consecutive VGPR names even when only the selected
  destination existed. `spirv-val` identified undefined `v29/v30`, and Vulkan
  rejected the pipeline before present. A first guard based on globally
  declared variables was still insufficient because a neighboring VGPR could
  be declared by a later instruction but remain undefined at the tap. The tap
  now loads only components in the selected instruction's actual destination
  range and writes zero for the remaining diagnostic channels. The existing
  generator test reproduces a sparse `v27` destination plus a later `v28`
  definition, assembles the corrected module, and keeps the cache/signed checks
  together.
- Repeating the exact ordinal-161 route after that correction reached present
  8,091. Red `v27 = clamp(dot(normal, light), 0, 1)` remained near zero except
  for isolated fragments. The continuous green surface came from `v28`, which
  is not part of this scalar instruction's destination; that channel was an
  undefined diagnostic read and is not coverage or lighting evidence. The red
  result still places the observed blackening seam at the dot product, but the
  checksum-wide tap cannot attribute its dominant fragments to one vehicle or
  world draw. The next comparison must isolate the relevant draw and establish
  coordinate-space agreement between its transformed normal and light vector.
  Do not transpose or negate either input without that evidence.
- A bounded follow-up extended the existing opt-in object-buffer trace from
  two to all three rows of the normal matrix. The first traced matrix is the
  finite orthonormal transform `[1,0,0; 0,-1.19209e-7,1;
  0,-1,-1.19209e-7]`. Applying the exact vertex-module row operations and
  normalization to each draw's first sampled input normal, then dotting with
  the traced light vector, produces positive results for several large draws:
  about `0.8014` at 15,366 indices, `0.6607` at 41,910, `0.6336` at 10,080,
  and `0.8414` at 39,120. Other draws legitimately produce negative values.
  This excludes a globally zero normal transform and does not support a global
  transpose or sign inversion. It also confirms that the checksum-wide tap is
  too coarse: later draws sharing the same translated module can overwrite the
  diagnostic output. The next discriminator must select a general draw/pipeline
  instance without hard-coding a title address or changing ordinary rendering.
- That trace-only run reached present 8,001, delivered the first requested
  input edge, and recorded its capped 32 draws. The controller-status polling
  wrapper then read the protocol envelope at the wrong JSON level and stopped
  the process at present 8,160 before the second edge. It is matrix evidence
  only, not a complete input route, visual A/B, or acceptance run; do not repeat
  it merely to obtain the data already captured.
- The fragment tap now accepts the optional conjunctive draw selector
  `KYTY_FS_TAP_DRAW=indexed:N` or `auto:N`. The selector is resolved once at
  the draw boundary and carried as immutable pixel input; the selected variant
  has a distinct pixel `ShaderId`, translation-module identity, and Vulkan
  pipeline lookup, while non-selected draws retain the ordinary identities and
  source. The existing translation-cache and graphics-packet tests cover the
  exact selected/unselected key split, both draw kinds, signed visualization,
  and sparse destinations. This is a bounded diagnostic facility, not a
  rendering fallback.
- A draw-scoped ordinal-161 capture for the 15,366-index draw completed the
  exact present-8,000/two-edge route at present 8,090 and produced substantial
  red positive-dot geometry in the upper-right of the scene while leaving the
  central vehicle on its ordinary path. This proves that the earlier
  checksum-wide tap combined different objects and that this draw is not the
  central vehicle. Repeating the same bounded route for the 3,564-index draw
  also reached present 8,090 with two delivered taps and no structured error or
  blocked synchronization. In that capture almost the entire central vehicle
  disappeared, leaving only small lit fragments; the native score was
  low-entropy (`0.2352`) rather than healthy. The 3,564-index draw therefore
  owns a major part of the central vehicle and its direct clamped dot is near
  zero over most visible fragments. This isolates a per-draw lighting result;
  it does not prove that the dot, clamp, coordinate transform, or absence of
  direct light is itself incorrect. Resolve the vehicle's interpolated
  normal/light coordinate-space contract and its location-4/ambient producer
  before changing lighting semantics.
- A follow-up VS probe on the same exact route resolved the apparent varying
  gap: the guest PS settings are locations `0,1,2,4`, the VS declares five
  parameters, and `attr3` therefore correctly consumes `param4/location4`;
  location 3 is deliberately unused. The probed VS computes `param4.xyz` from
  the normalized transformed normal and object-buffer coefficients loaded at
  offsets 400 and 464, applies its float log/exp and conditional color
  conversion, clamps the three results nonnegative, and exports them. Thus
  neither a missing `param3` consumer nor a simple sequential-location remap is
  justified. The native capture again completed at present 8,089 with two taps,
  no structured error, and a 1.5 GiB cgroup peak. The remaining producer audit
  is the exact arithmetic/export contract for `param4`, including the guest EXP
  channel mask and the log/exp/CNDMASK semantics; do not synthesize ambient
  light merely because this varying can be dark.
- The required one-variable A/B for `ShaderAppendVertexStreamStorage` is now
  closed. Removing only its sole call, while retaining `ShaderParseAttrib`,
  `ShaderDetectBuffers`, DetectFetch, and the live-address resolver unchanged,
  completed the exact present-8,000/two-edge route at present 8,091 with no
  structured error and a 1.56 GiB process high-water mark. The native image
  lost the remaining vehicle geometry entirely and retained only the 2D/logo
  layer; its score fell to entropy `0.2028` with 165 color bins. The call was
  restored immediately and `fc_script` was relinked. Therefore the appended
  read-only raw stream backing is not the regression that removed the vehicle.
  Offline disassembly of the persisted exact VS module then showed separate
  Vulkan input loads for position, normal, and UV, and storage-array indices
  only for slots 0 through 2; that translated module does not consume the
  appended slot 3. The different images from the two launches therefore do not
  prove that slot 3 produced the remaining vehicle, only that removing the
  append did not restore it. Do not repeat this disable experiment. The next
  vertex-input discriminator must preserve the ordinary path and determine,
  per MUBUF load, whether DetectFetch selected a proven semantic or the live V#
  resolver actually consumed a stream span.
- A temporary extension of the already bounded object-buffer trace captured
  the 24 floats at offsets 400 through 495 for the 3,564-index vehicle draw.
  Only the fourth coefficient of each first group was nonzero, but this does
  not make `param4` a constant: the guest immediately reloads `s8:s11` from
  offset 496 and those final four floats also contribute to all three output
  channels. They were outside the captured range. Treating the observed
  fragment `attr3.g` as a VS-to-PS contradiction was therefore invalid; the
  temporary wider trace was removed and no ambient or export semantic was
  changed.
- A general indexed-draw defect found by comparison with the sibling renderer
  is now corrected: the command processor previously decoded indirect
  `instance_count`, signed `base_vertex_location`, and
  `start_instance_location` but discarded all three. Indexed rendering now
  adds the signed base exactly once to the existing guest vertex offset and
  passes instance count and first instance through chunked, ordinary, and guest
  depth/stencil-copy Vulkan draws. Automatic draws now use the persistent guest
  instance count, and an indirect count becomes the state observed by later
  direct draws. Invalid signed sums and negative effective indexed ranges are
  rejected before vertex binding. The existing vertex-offset test covers
  positive, negative, and overflowing additive offsets. The strict two-edge
  integration still failed visually: present
  8,096 scored `healthy=false`, entropy `0.3263`, with 177 color bins. Its 32
  exact material records, including the 3,564-index vehicle draw, all used
  `vertex_offset=0`, `instances=1`, and `first_instance=0`; this correction was
  not exercised by the failing material and is not its visual fix.
- The exact material shader then exposed a separate general MUBUF addressing
  defect. For `IDXEN+OFFEN`, Kyty loaded VADDR lane 1 as the element index and
  lane 0 as the byte offset; the RDNA2 contract and two independent renderer
  comparisons agree that VADDR is `[element index, byte offset]`. In the cached
  version-31 module this made a zero lane select the record while a varying
  `3 + 4*x` lane became the byte offset, producing plausible but wrong in-range
  material/light triplets. The shared MUBUF/MTBUF address setup now uses lane 0
  for the index and lane 1 for the offset when both flags are set, while
  `IDXEN`-only and `OFFEN`-only continue to use their sole lane. The existing
  `MubufImmediateOffsetStaysSeparateFromSoffset` test was extended to cover the
  combined and OFFEN-only forms; a red run first observed the exchanged loads,
  then the corrected focused run passed. Translator version 32 invalidates the
  stale cache. The regenerated exact module passes SPIR-V validation and now
  computes `index * stride + offset` from the expected producers. The prior PS
  trace retained the 16-byte stride, 31 records, and 496-byte range but omitted
  the descriptor words, so a universal last-byte clamp was not justified.
  In this exact ISA, the guest derives `v3=4*k`, keeps the byte-offset lane at
  zero, and issues four three-dword loads at record indices
  `4*k + {3,0,1,2}`. These are the four rows consumed by the following matrix
  multiply. The old lowering instead fixed the record index at zero and treated
  `4*k + {0,1,2,3}` as byte offsets, collapsing the intended rows into
  overlapping dword triplets. This is a direct data-corruption mechanism for
  the shadow/material coordinates, not merely a cache-identity difference.
- One cgroup-bounded Release+Silent strict integration exercised that corrected
  module. It reached present 8,003 and delivered the first `cross`, but the next
  40-present condition timed out after presentation stopped at 9,049. Frames
  still advanced, `last-error` was null, and sync-wait diagnostics reported no
  blocked or suspended wait. The route intentionally sent no second edge and
  made no capture; the process was stopped immediately. Several cumulative
  command/draw/dispatch maxima were multi-second, so this is an unresolved
  presentation/processing stall, not evidence that the corrected shader caused
  it. It proves neither visual improvement nor regression and must not be
  repeated without a concrete discriminator.
- The existing bounded material-storage trace now records all four V# words and
  decodes destination selection, `ADD_TID`, swizzle, index stride, OOB selection,
  and descriptor type. It remains behind the existing opt-in draw trace and its
  32-record cap; no per-frame logging was added. One subsequent exact integration
  closed the missing descriptor evidence for PS `s[16:19]`: word1 was
  `0x00100000`, word2 `0x0000001f`, and word3 `0x0004dfac`, decoding to
  `stride=16`, 31 records, `ADD_TID=0`, swizzle disabled, index stride zero,
  `OOB_SELECT=0`, and descriptor type zero for every recorded matching draw.
  Thus the corrected shader uses the simple linear `index * 16 + offset` path;
  ADD_TID, swizzle, and OOB-selection behavior cannot explain this material's
  remaining failure.
- That same Release+Silent run completed the required two input deliveries and
  both 40-present deltas, then produced a native capture at present 10,617. It
  reached the transmission-choice screen rather than the race checkpoint. The
  screen contained coherent large geometry/background forms but remained mostly
  gray and scored `healthy=false` (entropy 2.3809, 134 bins, no directional
  stripes). `last-error` was null and presentation was advancing when the sole
  process was stopped. This proves the descriptor fields and that the earlier
  presentation stall is not deterministic; it does not provide a same-scene
  v31/v32 A/B or gameplay proof for the MUBUF correction.
- A separate ordinary translator-32 run started the agent and strict guest in
  one bounded wrapper, reached the present-8,000 gate, delivered both requested
  `cross` edges, and completed both 40-present deltas. Its native capture at
  present 8,095 reached the PLAY-era checkpoint but still contained only small
  warm fragments over a black world (`healthy=false`, entropy `0.2896`, 176
  color bins, no stripe classification). The prior indexed-draw capture at the
  equivalent checkpoint had entropy `0.3263` with 177 bins. The corrected
  MUBUF record addressing changes the visible fragments but does not recover
  the central vehicle or world; keep the general address fix, but do not cite
  it as the visual correction.
- A draw-scoped ordinal-126 fragment tap for only the 3,564-index material was
  then attempted as the discriminator for the reconstructed nonzero
  `param4/location4`. The cold diagnostic variant reached only present 4,756
  inside the unchanged 60-second gate, with no blocked synchronization or
  structured error, so it received no input and produced no capture. One warm
  retry was justified after the module and pipelines had persisted: it reached
  present 8,004, delivered both requested edges, and captured at present 8,093
  with `last-error=null`. That input timing reached the main menu rather than
  the PLAY-era vehicle draw; its `hot_corruption` score (entropy `1.7525`, 78
  bins) describes the menu and cannot prove or disprove delivery of vehicle
  `param4` to the pixel shader. Do not interpret the missing tap color in a
  frame where the selected draw is absent, and do not lengthen or repeat this
  route without a new deterministic checkpoint.
- The retained explicit NaN selection for `V_MIN_F32`/`V_MAX_F32` matches the
  RDNA default-mode rule used by the exact material shader: one NaN selects the
  numeric operand and numeric inputs retain the ordinary min/max result. The
  aligned translator-32 capture above already exercised that lowering without
  recovering the vehicle. Reverting it to direct GLSL extended min/max would
  test a less explicit contract, not isolate a new cause, so no additional
  guest A/B is justified without observed NaN operands.
- Offline extraction of the persistent translator-32 modules for the two known
  HDR consumers established their exact generated contracts without another
  guest run. The downsample has one implicit-LOD sample with Bias and the active
  compositor has two such HDR samples plus four explicit LOD-zero LUT samples.
  All extracted modules validate as Vulkan 1.2 SPIR-V. Their image operations
  agree with the existing `{bias,x,y}` and LOD-zero lowerings; no exercised
  Bias/LOD semantic divergence was found in the independent renderer
  comparisons. The active compositor variant consumes smooth `Location 0`.
  A second persisted variant of the same guest checksum has no input varying
  and samples at constant `(0,0)`, which would be a material spatial difference,
  but its `ShaderId` contains a distinct input interface. Static cache identity
  therefore already separates both variants and does not prove an erroneous
  reuse.
- The existing opt-in render-target lifetime `SAMPLE` event now includes the
  consumer's effective `input_num`, interpolator settings 0 through 3, source
  descriptor and host extents, source format/view/layout, and destination host
  identity/extent/format. The event remains under `KYTY_TRACE_RT_LIFETIME` and
  its bounded counter. Non-`SAMPLE` events stop after
  `limit - min(8, max(1, floor(limit/4)))`, leaving those global slots available
  to consumer samples, so a busy producer cannot exhaust the trace before the
  boundary. It adds no readback and leaves normal rendering unchanged.
- The lifetime trace no longer depends on the pixel-shader material trace to
  arm. `KYTY_TRACE_RT_LIFETIME_MIN_PRESENT` can defer all color/depth tracking
  to a deterministic presentation boundary. Exact guest-plus-host identity is
  required for a derived target, one event remains reserved for its consumer,
  and a sampled `rt-exact` source can replace an unproductive first-two target
  choice only when it writes a distinct smaller render target. If that smaller
  target is also a tracked primary, `SAMPLE_PRIMARY_DERIVED` preserves both
  identities. `rt-inexact` and guest-upload images cannot be promoted. These
  rules are diagnostic-only and do not add Vulkan commands or image readback.
- A strict Silent run with the lifetime trace deferred to present 8,000 reached
  present 8,017 through the agent gate with an interactive phase and no
  structured error, then was stopped deliberately. No input edge was sent.
  The trace therefore observed only the two alternating 1920x1080 targets and
  their depth use; neither known downsample nor compositor shader executed,
  and no `SAMPLE`, derived arm, or derived consumer event occurred. This run
  neither confirms nor rejects the half-resolution producer-to-compositor
  identity. Do not repeat the no-input route: the next bounded integration
  must use the established two-edge PLAY route with the promoted exact-RT
  trace, then stop after the first complete derived cycle.
- One strict two-edge cycle exercised that discriminator. The half-resolution
  downsample sampled the exact 1920x1080 HDR host image with `input_num=1`,
  interpolator zero `0x00000000`, view zero, and host format 122, then wrote a
  960x540 format-122 destination. The active full-resolution compositor used
  the same input interface and exact source image and wrote a 1920x1080
  format-122 destination. A second compositor analysis call had no traced color
  attachment and the default `0x20` interpolator; it is not evidence about the
  output-producing draw.
  This closes the constant-coordinate variant, cache-interface collision,
  and an attachment identity/view/format/extent mismatch for the failing cycle.
  It leaves interpolated coordinates, scale/sample arithmetic, blend, and the
  actual output contents open. The capture still showed only title/HUD plus a
  thin horizontal fragment (`healthy=false`, entropy `0.2337`, 164 bins), so
  dynamic compositor data and a later overwrite also remain open.
- A later cgroup-bounded strict Silent two-edge cycle isolated a different
  immediate consumer of that half-resolution result. The producer sampled the
  exact full-resolution HDR guest-plus-host identity and wrote a 960x540
  format-122 target; the next consumer sampled that exact 960x540
  guest-plus-host identity in the same presentation cycle. Offline inspection
  identifies the consumer as a positive-weight, nine-tap horizontal blur, not
  the full-resolution compositor. This excludes a missing attachment bind,
  guest-upload substitution, or inexact host-image swap at this immediate
  producer/consumer edge. It does not establish the sampled texel values, the
  blur destination, or a later overwrite. The native capture at present 9,840
  retained coherent PLAY UI over a black world (`healthy=false`, entropy
  `0.2089`, 173 bins); `last-error` was null and no synchronization wait was
  blocked. Do not repeat the lifetime trace merely to re-prove this identity.
- The next material discriminator is therefore upstream of post-HDR blur. Use
  the existing draw-scoped fragment tap on the 3,564-index material's raw base
  sample (ordinal 24) and compare its spatial coverage with the retained
  draw-scoped `param4/location4` capture. Only if both inputs overlap should a
  later bounded run tap the final red/green/blue multiply-add results. This
  separates missing texture/UV coverage from final material arithmetic without
  changing depth, HTILE, clears, blend, or renderer semantics.
- The first raw-base attempt did not preserve that checkpoint: separate agent
  calls allowed the hot pre-input loop to advance from the 8,000 gate to about
  16,000 presents before the first edge was consumed. Its eventual capture at
  present 17,067 showed the credits screen, so it contains no evidence about
  the selected material despite its nonzero score. A single warm retry reached
  present 8,000 and delivered the first edge at present 8,006, but the local
  route driver then aborted because it treated the valid JSON boolean
  `tap_pending=false` as a failing `jq -e` predicate. No second edge or capture
  was requested. Both processes were stopped immediately and cleanly. The next
  attempt, if needed, must keep the complete route in one local command and
  parse that boolean with `jq -r`, not extend the gate or cite either failed
  route as shader evidence.
- The corrected atomic route then reached the 8,000 gate, confirmed exactly two
  delivered edges with both 40-present deltas, and captured at present 8,098.
  The draw-scoped ordinal-24 variant exported all four raw `ImageSampleB`
  channels and produced additional nonblack road/vehicle fragments, but still
  no coherent vehicle or world (`healthy=false`, entropy `0.3657`, 183 bins).
  `last-error` remained null, no synchronization wait was blocked, and peak
  resident memory was about 1.66 GB. A retained 32-record material trace has
  exactly one 3,564-index instance in the corresponding window, which supports
  the selector for that run. It does not make the older ordinal-126 comparison
  a same-frame vector A/B: those captures differ by eleven presents and ordinal
  126 writes only the scalar `attr3.y`, not all of location 4. Their low
  nonblack-mask overlap is therefore a lead, not proof of incorrect UVs or
  geometry coverage.
- The published-parent differential left one exercised texture candidate: the
  1024x1024 BC1 material declares eleven guest mips, while the host containment
  policy stops at the 4x4 level and omits the legal 2x2/1x1 subresource-edge
  copies. An existing focused contract was changed first and failed at the old
  `11 -> 9`, `4 -> 2`, and `3 -> 1` results. Preserving the complete guest chain
  made that contract and the full mip-layout contract pass, and both affected
  binaries built under the bounded `-j2` profile. The strict runtime then failed
  the unchanged 70-second present-8,000 gate before any input or capture: it was
  still healthy and advancing around present 5,843, but fell to about 4.5 FPS
  while per-frame upload/recreation work climbed into thousands of calls and
  tens of MiB. The experiment and test were restored and both binaries rebuilt.
  Thus sub-block BC mips remain a real general correctness gap, but blindly
  enabling them regresses the current runtime and is not the 3D fix. The next
  work must first capture the material's effective implicit LOD and identify why
  the full chain changes texture lifetime/upload churn; do not repeat the same
  full-level toggle.
- A draw-scoped fragment diagnostic then queried the implicit LOD for the exact
  biased BC1 sample without mutating guest VGPRs. Its threshold output records
  requested relative LOD before sampler/image clamping, not the hardware-final
  mip. Over the retained sample footprint, about 82% of pixels crossed the
  `host_levels + 1` threshold: the material really requests the omitted tail;
  this is not merely a descriptor-layout discrepancy. The diagnostic identity
  is isolated from normal and earlier numeric tap modules in the persistent
  shader cache.
- That new evidence justified one final full-chain retry. A Release+Silent
  strict run under the bounded runtime cgroup reached the unchanged 8,000
  gate, confirmed exactly two delivered edges, applied both 40-present deltas,
  and captured at present 8,093. The image still contained only the overlay and
  isolated gray vehicle fragments on black (`healthy=false`, entropy `0.2406`,
  160 bins). `last-error` was null and no synchronization wait was blocked.
  The post-input window also reproduced the performance regression: about
  1.68 million upload calls and 10.19 GB of aggregate staged bytes by present
  8,430, versus about 625 thousand calls and 2.60 GB near the same checkpoint
  with the containment policy. Texture lifetime itself did not explode (58
  materializations, one tracked hash change), so the aggregate upload counter
  must not be mislabeled as texture recreation. The complete-tail change and
  its temporary expectation were restored again. The missing BC tail is
  exercised but is neither sufficient to restore the scene nor safe to enable
  through the current upload path; do not repeat this toggle. Continue from the
  already evidenced post-`ImageSampleB` blackening seam.
- The storage trace was widened from 16 to 28 float words so the existing
  opt-in exact-draw route can report every coefficient consumed by this
  compositor (`f16` and `f20..f27` included). A single checksum-scoped attempt
  armed that trace only after present 8,080, but presentation stalled at 6,264
  for about 154 seconds before any input or matching draw. `last-error` remained
  null and the process was stopped deliberately. No compositor values were
  captured, the diagnostic did not arm, and the run must not be cited as a
  render result. Do not repeat or lengthen it without a new deterministic
  checkpoint.
- A later pre-input checksum-scoped route did capture the complete 28-float
  compositor storage range. All 112 bytes were materialized with no OOB read.
  The active coefficients were internally coherent: zero sample bias, packed
  LUT steps `1/1024` and `1/32`, dimension-minus-one `31`, unit gain, and a
  disabled optional radial mask. None is an all-black multiplier for finite
  nonnegative HDR input. The draw targeted a different pre-input host format
  and address from the failing gameplay-era format-122 consumer, so this closes
  only an obvious range/constant defect in that variant. It does not establish
  the gameplay coordinates, HDR samples, LUT contents, or final output.
- The Gen5 HLE `EVENT_WRITE` path had one independent packet-contract defect:
  it warned on every non-null address, discarded it, and always emitted the
  two-dword packet. Addressed type `0x39` now uses the evidenced four-dword
  form with event index one and a 64-bit aligned destination; the parser accepts
  both lengths and forwards the address. Event `0x39` validates the writable
  guest span and, pending native host occlusion queries, publishes a ready,
  monotonically increasing synthetic begin/end counter for each of sixteen
  interleaved DB pairs. The consolidated graphics diagnostics integration
  executes the four-dword form through the real PM4 parser, checks following-
  packet alignment, all sixteen ready begin/end pairs, monotonic increment,
  and rejection of a misaligned destination. It also protects ordinary events
  and retention of the short `0x38` form until an addressed consumer contract
  is evidenced.
- One cgroup-bounded Release+Silent strict route exercised the resulting build,
  reached present 11,752, delivered exactly two diagnostic input edges, and
  retained `last-error=null` with no blocked synchronization wait. The native
  capture remained nearly black apart from coherent UI (`healthy=false`,
  entropy `0.2024`, 158 bins, no stripe classification). The retained runtime
  diagnostics do not count event `0x39`, so the run does not prove that the
  workload exercised the corrected packet or synthetic result. Keep the
  general packet fix, but do not cite occlusion as the demonstrated producer or
  claim restored 3D.
- A static guest-wave audit found that `ShaderPixelRequiredSubgroupSize`
  calculates 32 or 64 for a pixel shader using native lane exchange, but
  `CreatePipelineInternal` leaves the fragment-stage `pNext` null and device
  creation does not enable subgroup-size control. The current Intel host's
  default subgroup is 32 (supported range 8..32), while the preserved trace
  does not retain the exact material's guest `PS_W32_EN` bit. This is a general
  strict-width gap, not evidence that the observed shader ran at the wrong
  width. A future fix must enable/query the feature, chain the required-size
  struct, key the pipeline by that width, and reject unsupported wave64.
- The same audit found a narrower parameter-export gap. The EXP parser retains
  `exp_enable_mask`, including for partial `ParamN` fallback forms, but
  `Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3` always stores all four source
  channels. The opt-in shader dump now prints the already-decoded mask. A
  bounded strict probe captured the exact VS as `param0 EN=0x3`, `param1
  EN=0x7`, `param2 EN=0x7`, `param3 EN=0x1`, `param4 EN=0x7`, and `Pos0
  EN=0xf`. The observed material path consumes `param4.xyz`, so every relevant
  channel is enabled and only `param4.w` is outside the guest mask. This closes
  partial `param4` contamination as the producer of the dark XYZ/normal path
  and cannot explain `gl_Position` geometry. Keep the general partial-output
  gap recorded, but require a red consumer contract before changing it.
- That probe was available before the diagnostic input gate. The bounded
  `wait-present --min 8000` request timed out at present 6,094 after 70 seconds;
  the runtime remained interactive and advancing near 9.7 FPS, `last-error`
  was null, and `sync-waits` had no blocked entries. No input or capture was
  requested, and the process was stopped immediately. This is parser evidence
  only, not a visual A/B, gameplay acceptance, or proof of a new stall.
- Static audits around the same exact producer found no additional semantic
  patch to justify: its ordinary interpolators use locations 0, 1, 2, and 4;
  custom interpolation and `V_INTERP_MOV` are not consumed; its position export,
  clip conversion, reverse-Z compare, and observed EXEC state are coherent; and
  the traced depth cycle retained one read/write/host-image/HTILE identity with
  no obsolete explicit clear. A compute/storage HTILE clear submitted before
  host writeback remains a possible temporal gap in general, but no such guest
  clear was observed for this cycle. Do not synthesize an HTILE clear, disable
  depth, swap position channels, or add generic OOB behavior on this evidence.
- The strict-storage inconsistency is closed conservatively: an unmatched
  `DirectResource` remains `Unknown` with `NoMatchingInstruction`, while only
  proven-unused `MetadataSharp` entries may be removed before binding. Focused
  unit and native graphics-integration coverage enforce that distinction. A
  single earlier strict private run with the same conservative policy timed out
  at the unchanged present gate before either input edge, so it is not
  compatibility evidence. Do not change global direct-resource retention
  without first tracing the exact binding, materialized range, and shader
  consumer.
- Expanding immutable transient vertex/index snapshots from 4 KiB to 512 KiB
  was tested as the next single-variable regression hypothesis. Temporarily
  routing only vertex and index ranges above 4 KiB back through persistent
  `GpuMemory` left the storage path and translated shaders unchanged. The
  exact route reached present 8,092 with two delivered edges and no structured
  error, but the capture contained almost only two lamp blooms and no recovered
  vehicle/world (`healthy=false`, entropy `0.9567`, 192 bins). The temporary
  threshold was restored and `fc_script` relinked. Do not attribute the current
  geometry loss to large transient vertex/index snapshots on this evidence.
- Eliding the submit/fence boundary for a `WAIT_REG_MEM` whose newest producer
  was still in the current recording was tested as a performance hypothesis.
  A bounded baseline classified 24,604 waits: 13,374 were already satisfied
  and all remaining 11,230 had a producer in the current submission; no queued,
  mismatched, unknown, or suspended waits were observed. Continuing the
  recording without publishing those callbacks stopped on `VideoOut` buffer
  `invalid_index`. Submitting the command buffer but omitting the host
  completion wait failed at the same presentation contract, so queue order
  without callback publication is also rejected. Same-queue GPU order alone
  does not publish the host completion state needed by presentation. Keep the
  boundary until GPU execution and callback publication have independent
  completion proofs.

- A later atomic, draw-scoped ordinal-126 route finally retained the selected
  3,564-index material at the requested checkpoint. The gate completed at
  present 8,000, both diagnostic input edges were confirmed as delivered, and
  the native capture completed at present 8,092 under a 1.61 GiB cgroup peak.
  The signed-independent raw `attr3.y` output covered a broad gray central
  vehicle body rather than only isolated strips. This supersedes the earlier
  menu-era ordinal-126 attempt: location-4 ambient input is present over a
  substantial part of the vehicle. It does not establish the final material
  result or playability.
- The paired draw-scoped ordinal-24 attempt compiled and persisted the exact
  tapped module, but its present-8,095 capture contained only the overlay/logo;
  the selected draw was absent. It is module/cache evidence only and must not
  be compared spatially with the valid ordinal-126 frame. Likewise, a later
  signed ordinal-220 route completed the exact two-edge sequence at present
  8,091 but captured the black transition with only the logo. Its one-bin scene
  score is not a negative accumulator value because the material draw was not
  present.
- Offline inspection closes the remaining interpolator-EXEC lead for this
  shader. `exec_lo/exec_hi` are initialized to `1/0` and are not written again
  until the guest restores them after ordinal 228. Kyty's unconditional
  `VInterpP2F32` destination store therefore executes under an active lane for
  ordinal 126; the sibling renderer's inactive-EXEC preservation cannot cause
  this material result. Keep that general divergence separate from this
  workload until an exercising shader is captured.
- The exact final material dataflow is now reconstructed. Ordinals 220--222
  compute `W = attr3.xyz + s4:s6 * L`; ordinals 223--225 first combine that
  result with the sampled base as `C = B + W * P`. The retained bounded storage
  trace already contains the values omitted from the earlier summary:
  `s4:s6 = (1.9, 1.80107, 1.45189)`, with an 80-byte readable descriptor and no
  OOB condition. The prologue load feeding `P` resolves `s32:s34 = (1,1,1)`, so
  `P` is the unscaled RGB base sample. Because the VS clamps `attr3.xyz`
  nonnegative and the post-ordinal-219 light factor is nonnegative, ordinals
  220--222 cannot cancel ambient through a negative coefficient. Do not change
  the scalar-buffer load, add ambient, or clamp this FMA on the current
  evidence. The exact-draw recovery below resolves the formerly unknown `B`
  accumulator and supersedes this intermediate seam.
- Two subsequent attempts were deliberately stopped without input or capture
  after failing the unchanged 90-second present-8,000 gate: one enabled only
  the existing checksum-bounded storage trace, and one requested draw-scoped
  signed ordinal 221. Both services were cgroup-limited and left no process
  behind. They contain no render evidence. The host swap was nearly exhausted
  afterward, so do not repeat or lengthen these frame-sensitive routes until a
  deterministic material-draw checkpoint or a concrete pre-gate performance
  cause is available.
- The recovered exact draw-18 trace closes the prior `B` accumulator as well.
  Its `s24` storage has float word 8, loaded through `s24+32`, equal to zero;
  the object-buffer float at offset 180, loaded into `s56`, is one. The exact
  module computes `v7 = trunc(min(0, 1)) = 0`, initializes `s30 = 0`, and tests
  `s30 < v7` before entering the only loop containing ordinals 105--107. The
  test is false, so the loop is skipped and the earlier zero initializations of
  `v15`, `v16`, and `v14` remain intact: `B = (0, 0, 0)` for the 3,564-index
  draw. The final result therefore reduces to `C = W * P` for this draw. A tap
  at ordinal 106 would never execute and is not a useful runtime experiment;
  do not seed the loop or change `V_MAC_F32` to make the diagnostic visible.
- The recovered 28-float object-buffer tail closes `W` as a blackening source
  too. For this exact draw, every coefficient feeding the pre-transfer
  `param4.rgb` expression is zero except the three channel offsets, each
  `0.486531`; the remaining cross terms are zero. The guest's following
  piecewise linear-to-sRGB sequence therefore exports the same positive value,
  approximately `0.726415`, in every `param4` RGB channel. Combined with
  `s4:s6 > 0`, `L >= 0`, and `B = 0`, the exact final expression obeys
  `W.rgb >= (0.726415, 0.726415, 0.726415)` and `C = W * P`. It can modestly
  rescale a nonzero base sample but cannot remove its spatial support. This
  supersedes the earlier visual inference that the final material accumulator
  itself introduced the blackening; single-channel tap captures with different
  presents/packing are not a numeric same-frame A/B.
- The remaining causal frontier is `P`: the BC1 sample's coordinates and
  derivatives, effective LOD, tiled mip contents, or sampled-image lifetime.
  The exact stride-48 UV input is not a half-format mismatch: format 64 maps to
  `R32G32_SFLOAT` at offset 40, the draw retained `vil_ok=1`, and the VS applies
  the traced identity UV transform `(scale=1,1; offset=0,0)` before exporting
  `param0.xy`. The independent renderer uses the same two-float interpretation.
  Do not rewrite format 64, clamp tiled UVs, or change perspective interpolation.
  The bounded vertex-attribute diagnostic used to omit format 64 even though
  the production Vulkan input path handles it. It now decodes format 64 as two
  raw float32 components and accounts for its eight-byte footprint; the focused
  guest-memory decoder check passes. This is diagnostic-only and does not alter
  rendering. No UV values from the exact 3,564-index draw have been captured
  with the corrected trace yet, so the runtime coordinate hypothesis remains
  open.
- A subsequent cgroup-bounded strict Release+Silent route did capture those
  values. It delivered exactly two CROSS taps with both 40-present deltas and
  matched the indexed 3,564-count draw after the 8,080 trace gate. The draw had
  `vil_ok=1`, a stride-48 stream with 2,376 declared and required records, and
  finite raw inputs: representative positions and unit-length normals were
  plausible, while format-64 UVs at offset 40 varied coherently around
  `(0.2825..0.2903, 1.0167..1.0193)`. The BC1 descriptor remained bound to a
  1024x1024 normalized 11-level guest chain. This dynamically excludes an
  unreadable or half-decoded format-64 input as the producer for the exact
  draw, but fixed vertex probes do not yet prove indexed-triangle topology,
  post-VS clip positions, implicit derivatives, or effective LOD.
  The native capture at present 8,860 retained coherent HUD and track map but
  no vehicle or world; the offline capture scorer reported `scene_ok=false`,
  `gameplay_like=false`, entropy `0.3904`, 249 quantized colors, and no stripe
  classification. `last-error` was null, no synchronization wait was blocked,
  both taps were consumed, and the runtime was stopped deliberately. This is a
  clean reproduction of the missing-3D defect, not a playability advance. The
  baseline path named by the original handoff was absent at comparison time,
  so no fresh pixel-delta claim is available.
- The draw trace now also retains raw `SPI_PS_IN_CONTROL`, the subgroup width
  required by the parsed shader, and the host subgroup range. A late-gated
  attempt did not reach present 8,000 within 70 seconds (it remained at present
  5,129, loading, with no structured error or blocked wait), so it received no
  input and was stopped. A separate checksum-scoped run then reached the hot
  pre-input loop, but the first agent edge was consumed only after the loop had
  advanced far beyond the requested checkpoint; that run is not visual-route
  evidence. It did, however, match the same indexed 3,564-count draw and the
  same VS/PS identities. All 32 bounded matches, including that draw, recorded
  `ps_in_control=0x00000004`, `guest_wave32=0`, and
  `required_subgroup=64`, while the Intel host reported default subgroup 32 and
  range 8..32. The subgroup-control extension is advertised, but this host
  cannot provide subgroup 64 and the pipeline currently requests no explicit
  width. This proves a static wave-width mismatch for the shader, but the only
  cross-lane operation in its untapped module is `V_READFIRSTLANE_B32` inside
  the loop guarded by the already recovered `s30 < v7` condition. For this
  exact draw `v7=0`, the branch exits before that operation, and its VCC branch
  condition is uniform. The wave-sensitive block is therefore not exercised
  and the mismatch cannot be cited as the 3D producer for this material. The
  persistent cache also includes the fragment-tap diagnostic identity in both
  its in-memory and disk module keys: the normal module and the earlier
  `attr3.y` tap module are distinct, and the strict run selected the untapped
  variant. Do not force the guest to wave32, request an unsupported host size,
  or implement two-subgroup emulation as a visual fix without an exercised
  wave64 consumer. Preserve the gap as a general strict-runtime limitation.
- A follow-up disassembly separated the material's `V_READFIRSTLANE_B32` from
  its other subgroup consumers. A conservative reaching-definition proof now
  direct-copies only a wave-uniform source while EXEC remains at its initial
  mask. Independent review required and verified rejection of divergent VCC,
  implicit `VCMPX` and all nine supported `S*SAVEEXEC` writers, overlapping
  multi-VGPR definitions, basic-block crossings, and incoming branches that
  bypass the producer. The existing graphics integration validates both the
  direct path and retained native path. In the rebuilt exact module the native
  ballot/count/find-first/broadcast block disappeared; `OpGroupNonUniformAny`
  for a VCC branch and `SubgroupLocalInvocationId` for a lane-addressed buffer
  remain. Consequently the runtime still reports `required_subgroup=64`; this
  is expected and does not reopen the eliminated read-first-lane exchange.
  The exact compositor source was also checked: its `SWQM_B64 exec,exec` is a
  no-op and its three VCC branches lower without a native subgroup operation,
  so compositor wave exchange is closed as a producer on current evidence.
  One cgroup-bounded strict route scheduled exactly two input edges at presents
  8,000 and 8,080, delivered both without cancellation, and captured at present
  8,152. The image was a coherent main menu rather than PLAY; it is not visual
  3D acceptance and provides no reason to broaden the uniformity proof.
- The complete BC-tail retry under the default dirty-page tracker did not pass
  its integration gate and has been restored to the 4x4-block containment
  policy. The focused RED observed the old `9/2/1` host counts and the temporary
  complete-chain implementation passed the two existing layout tests. The one
  strict Release+Silent run then stopped advancing at present 4,994 before any
  input, draw-synchronized trace, or capture; after 78 seconds without a new
  frame/present it still had `last-error=null`, no blocked sync wait, no GPU
  submission in flight, and was stopped deliberately. Its stable snapshot had
  already accumulated 2,403,542 upload calls and 17,216,392,010 staged bytes.
  The texture tracker was active (`hash_tracked_changed=1`,
  `hash_tracked_unchanged=45`), and the earlier full-tail run itself already
  reported `1/44`; therefore the premise that those experiments predated dirty
  tracking was false. Aggregate upload counters do not identify the producing
  resource, and the absent 3,564-index draw is not visual evidence, but this
  retry removes the only stated reason to keep the complete tail live. Do not
  repeat it without a new per-object upload/ownership observation that predicts
  both the pre-gate churn and a different result. The remaining sample frontier
  is coordinates/derivatives, effective LOD selection inside the retained
  chain, tiled contents, or sampled-image lifetime rather than merely exposing
  the final 2x2/1x1 host levels.
- The existing opt-in tiled-block diagnostic now hashes and writes every
  host-exposed mip in a matching compressed chain, while remaining disabled by
  default and bounded to 32 distinct chains. A strict diagnostic load of the
  exact 1024x1024 BC1 material produced complete, differently hashed levels 0
  through 8 with the expected compact sizes from 524,288 down to 8 bytes.
  Offline BC1 decoding showed a coherent atlas and coherent downsample
  progression at every level, without block swizzle, tearing, or black data.
  Sampling the approximate wrapped vertex UV near `(0.29, 0.017)` yielded a
  stable gray texel in levels 0 through 6; levels 7 and 8 remained valid
  low-resolution averages. The runtime that triggered this upload advanced far
  beyond its requested input checkpoint, so it is content evidence only and
  not a visual-route or gameplay result. This excludes deterministic detile or
  stored-mip corruption for the captured material. It does not yet prove the
  interpolated per-fragment UV, derivatives, selected LOD, sampled-image
  lifetime, or the actual `P` returned in the failing draw. The next probe must
  preserve MRT output and observe one of those remaining seams; do not change
  tiling or mip exposure on this evidence.
- An output-preserving, draw-scoped single-pixel `DebugPrintf` prototype was
  source-tested and SPIR-V validated, but Vulkan debug printf requires the
  validation path from instance creation. On this host the strict Silent run
  reached only present 4,788, then stopped presenting for more than 40 seconds
  while full memory pressure rose sharply. The cgroup remained bounded at a
  2.5 GiB hard maximum with swap disabled; no input was sent, no sample line was
  produced, and the process was stopped immediately. The prototype and runner
  plumbing were removed. This is a diagnostic-path performance dead end, not a
  graphics result or a new guest stall. Do not repeat validation-layer shader
  printf on this host; use existing bounded CPU-side traces or a genuinely
  lightweight host-owned readback mechanism instead.
- A following normal Silent build added only bounded index peeks to the existing
  material trace so representative consumed vertices could be transformed
  offline. It also failed the unchanged present-8,000 gate: at present 4,933 it
  fell to 0.19 FPS and host full-memory pressure rose, despite a stable cgroup
  footprint near 2.2 GiB and zero cgroup swap. No input was sent, the trace had
  not armed, and the process was stopped immediately. This run contains no
  index, clip-space, visual, or stall evidence. Do not wait through this
  pre-gate degradation merely to collect a late trace; retain the bounded index
  peek for a future route that reaches the material checkpoint cheaply.

## Validation gate for the next change

1. Re-capture the gameplay-era checkpoint on the exact branch.
2. Use real keyboard/controller press and release edges; do not use
   `KYTY_AUTO_CROSS` for acceptance.
3. Confirm the scene remains free of stripes, black quad footprints, white
   world output, and stale UI overlays.
4. Exercise movement in both directions and one action while presents advance.
5. Run with Vulkan validation where supported and record relevant errors.
6. Re-run the focused GraphicsPackets/GraphicsState suites.
7. Treat any new structured EXIT, host fault, or earlier visual regression as
   the new first frontier.

Process survival, a clean HUD, or a single recognizable frame is not
playability acceptance. Do not fabricate clears, alpha tests, resources,
signals, formats, or fallbacks to make the workload continue.

## Dirty-page tracking performance phase

The first optimization phase is now implemented on `codex/graphics-runtime-fixes`.
`GpuMemory::Update` registers only CPU-upload resources (`check_hash=true`) in
a fixed-capacity page tracker. Pages are armed read-only with the actual host
page size; the write-fault path increments an atomic generation and enables the
faulting page using the signal-safe VM primitive. Each GPU object stores its own
generation snapshot, so an overlapping object cannot consume another object's
dirty evidence. Registration, protection, capacity, and upload uncertainty
remain on the XXH64 fallback path.

The handler boundary is intentionally narrow: it does not take `GpuMemory` or
virtual-memory bookkeeping locks, allocate, log, hash, or call Vulkan. Host
destinations in CP WriteData/constant-RAM dumps, libc copy/set/read helpers,
and file reads call the same notification seam. DMA sources are not marked.
Object teardown unregisters ranges before object IDs are recycled. The tracker
uses tombstones for page-table removal and range reference counts to preserve
collision and overlap correctness.

During the first disabled-mode hardening pass, lazy tracker
metadata exposed a rollback path that called `UnregisterRange` without a
registration attempt; that dereferenced a null mutex and terminated the game
before its first present. Disabled tracker APIs now return through the hash
fallback contract, rollback runs only after an attempted registration, and a
regression test covers the no-metadata path.

Exact registered-range queries now bypass the per-page scan across every range.
Two controlled Release+Silent gameplay runs then sustained 500 and 1,000
presents with healthy captures, movement/action input, no frame over 50 ms,
and higher FPS than the hash-only baseline. A debugger capture then traced the
remaining intermittent fatal fault to a protected page whose tracking entry had
already been discarded. Retired page metadata now remains in the fixed table so
late faults can restore the known writable host mode. Tracking remains opt-in
at this historical checkpoint. Untracked, capacity-limited, or uncertain ranges
continue to use XXH3 automatically.

A later opt-in repetition reproduced that intermittent fault after more than
10,500 healthy presents. GDB found the fatal page still read-only with zero
references, writable original mode, and retained `Retired` metadata. This
identified a `Rearm`/final-`UnregisterRange` protection race rather than disk
cache growth or missing metadata. Rearm and range registration transitions now
share the registration mutex; final unregister claims `Disarming` before
publishing zero references; and an arming transaction rolls back read-only
protection if it observes `Retired`. A subsequent Release+Silent run sustained
more than 14,000 presents past the previous failure, with a healthy capture and
a stable 2,008-frame window at about 31 FPS (p50 33 ms, p99 36 ms, no frame
over 50 ms). At that checkpoint the tracker remained opt-in pending longer
default-path validation.

The next controlled gameplay comparison identified tracker capacity—not an
unbounded disk cache—as the remaining texture-hash fallback. The original
fixed page table had 65,536 metadata slots and a 32,768-page limit per
registered range. Once that cover filled, a stable large texture range fell
back to 3,715 full hashes (106.7 GB read, 4.72 s CPU) in 30 seconds; every
comparison was unchanged. Expanding the bounded table to 262,144 slots with a
131,072-page per-range limit eliminated all steady-state texture hashes in the
equivalent scene. The 30-second window improved from 24.39 FPS / p50 42 ms /
p99 44 ms to 28.51 FPS / p50 34 ms / p99 37 ms, with no frame over 50 ms.
A subsequent 218-second gameplay window sustained 6,915 presents, p50 32 ms,
p99 37 ms, eight frames over 50 ms, no frame over 100 ms, healthy native
captures, delivered movement/action input, and no structured error. The larger
fixed table adds bounded RAM metadata only; it does not serialize textures or
increase persistent cache writes.

Generation tracking is now enabled on the normal runtime path. Set
`KYTY_DISABLE_GPU_DIRTY_TRACKING=1` only to diagnose tracker-specific problems;
the process-wide tracker does not arm until the runtime fault handler is
installed, and disabled or uncovered ranges use the conservative hash path. A strict
Release+Silent default-path run, with neither enable nor disable variables set,
reached a coherent interactive scene and exceeded 11,000 presents. Its
45-second movement/action window advanced 1,499 frames and presents, ended at
33.40 FPS, measured p50 31 ms / p99 36 ms, had no frame over 100 ms, and
reported no structured error. During that window the process wrote about
1.07 MB to disk and the bounded persistent Vulkan cache remained 1.1 MB; the
tracker itself writes no texture data to disk.

Two later intermittent `SEGV_ACCERR` write faults exposed distinct signal-order
windows in the default tracker path. First, a host writer could restore a page
while a concurrent arming transaction had not yet committed its native
read-only protection; publishing `Writable` before that transaction finished
left a short state/protection mismatch. The tracker now preserves an explicit
arming-rollback state until the delayed protection is restored. Second, two
guest workers could fault on the same read-only page before either signal
handler completed. The first handler restored the captured writable mode, but
the already-queued sibling delivery entered after the state became `Writable`
and was treated as a real guest fault. Active coverage with a captured writable
native mode now accepts that stale delivery and reapplies the restore token.
Deterministic tests cover both orderings. A subsequent strict Release+Silent
route exceeded 10,400 presents, reached the new-game transition, and produced
no crash report. Sampled menu/transition status ranged from about 32 to 48 FPS;
that run did not yet prove controllable gameplay or a 60 FPS target.

A later long interactive run exposed a separate presentation freeze after more
than 80,000 frames. A debugger capture stopped at the first bounded
`WaitRegMem64` timeout and preserved the command stream. The wait expected
64-bit value `1`; the immediately preceding confirmed custom `WriteData`
contained that same address and value, but its early host `memcpy` had been
overwritten by a later GPU-to-CPU materialization. Confirmed 64-bit
`WriteData -> WaitMem64` pairs now publish through the exact GPU submission,
register that submission as the wait producer, and use the existing durable
label-hole protection. Other `WriteData` packets retain their established
behavior. A strict Release+Silent rerun sustained 300 seconds and passed the
previous reproduction point, reaching more than 8,100 presents with no
structured error; process disk writes remained about 10 MB.

## NGS2 state and lifetime exports

The captured import set includes `sceNgs2VoiceGetStateFlags` and
`sceNgs2RackDestroy`, but neither export was previously registered. A missing
import fallback could therefore return success without writing voice state or
retiring rack-owned streams. The state-flags export now shares the exact state
mapping used by `sceNgs2VoiceGetState`. Rack destruction unlinks the rack,
removes every associated PCM stream, returns caller-provided storage, and uses
the registered free callback for allocator-owned storage.

A strict runtime audio-boundary capture showed the workload creating Custom
Sampler, Mastering, and Reverb racks, submitting stereo PCM blocks, and issuing
the observed play command. Initial blocks were intentionally silent. A later
89,800-frame source block had a signed-PCM peak of 12,987; `Ngs2SystemRender`
produced a floating-point peak of 0.071069, and `AudioOut` delivered the same
peak to the host device queue. This locates the earlier silence before the host
backend and confirms that the final NGS2-to-device path can carry non-zero
audio. Temporary amplitude probes were removed after capture.

## Descriptor-layout cache startup phase

The descriptor cache previously created the complete Cartesian product of
layout counts on first use. With limits of 16 storage buffers, 16 sampled
images, 16 storage images, 16 samplers, one GDS buffer, and three shader
stages, that path issued 501,123 Vulkan layout creations while holding the
cache mutex. Most combinations were never requested by the workload.

Layouts are now created on demand in their existing per-stage/count slot. The
descriptor key, binding order, stage flags, pool allocation, and lifetime are
unchanged; repeated requests still reuse the same Vulkan layout. This removes
the eager combinatorial work without adding persistent data or disk writes.

A Release+Silent strict run reached a coherent interactive scene after the
change using exactly three initial input taps and no repeated automation. A
native 1280x720 capture was classified as gameplay-like, scene-correct, and
free of stripe artifacts. The first cumulative window reduced frames above
250 ms from 187 in the prior baseline to 143. After scene discovery completed,
an 89.684-second stable window advanced 2,613 presents (29.136 presents/s),
with frame p50/p95/p99 of 35/38/39 ms, a 40.239 ms maximum, and no frame above
50 ms. The comparable prior stable window advanced at 28.364 presents/s with
36/40/46 ms p50/p95/p99 and eight frames above 50 ms. These measurements
validate removal of the startup pause and preservation of rendering; they do
not establish a universal FPS result across titles or hosts.

## GPU range-query cache performance phase

Lightweight command-processor timings isolated a CPU bottleneck before Vulkan
command emission. In a Silent 30-present window, draw processing consumed
1.449 s and resource binding consumed 1.407 s. The binding time split into
609 ms preparing storage buffers, 350 ms preparing index buffers, 324 ms
preparing vertex buffers, and 121 ms preparing textures. Pipeline lookup,
render-target materialization, command emission, and the render-context mutex
were not material contributors in that window.

The three dominant paths repeated the same allocated-range, prefix, and
overlap queries for stable guest addresses. Per-heap overlap results were
already memoized, but every query still scanned the global heap list before it
could reach that cache. `GpuMemory` now keeps bounded, exact-key caches for
allocated-range validation, allocated prefixes, and aggregate overlap
snapshots. Heap topology changes invalidate the allocation caches. Object
range creation/deletion and a read-only-to-writable use transition invalidate
the overlap cache. Full-key comparison preserves correctness under hash
collisions, and all cache access remains serialized by the existing object
graph mutex.

At the same 400-present reset point, the post-change 30-present sample reduced
resource-binding time from 1.808 ms per draw to 0.826 ms per draw, a 54.3%
reduction. The sample completed 33 presents in 581 ms and used 2.74 GiB at its
peak, versus 30 presents in 2.097 s and 3.82 GiB for the pre-change sample.
Those windows executed different draw counts, so the present cadence is
progress evidence rather than a gameplay or universal 60 FPS claim. A bounded
native capture after the change preserved a complete, correctly shaped logo;
the longer title/menu checkpoint and gameplay input route remain to be
revalidated.

## Atomic transient-buffer eligibility phase

Small read-only vertex and index views previously acquired the object-graph
mutex twice: once to validate the allocated guest range and once to copy and
classify an aggregate overlap snapshot. The hot path now performs exact
allocation validation and overlap classification under one mutex acquisition.
Cache hits borrow the immutable snapshot only while that mutex is held; misses
populate and classify it without releasing the lock. Range containment remains
strict, so a span that merely crosses either edge of an allocated heap is
rejected. Existing heap/object mutation paths continue to invalidate the
bounded validation and overlap caches.

Focused tests cover empty, read-only, writable, freed, oversized, zero-sized,
and partially allocated ranges, plus cache invalidation after object mutation.
A strict Release+Silent route then reached the menu, selection screen,
introduction, and controllable gameplay with healthy native captures and no
runtime error. Sustained directional input was consumed for 180 presentations.

The earlier 601-frame gameplay sample spent 1.568 s across 221,766 transient
eligibility probes (7.07 us/probe, 2.61 ms/frame). The final atomic sample spent
700.057 ms across 191,755 probes (3.65 us/probe, 1.16 ms/frame). It also
reported 1.655 s of resource binding across 37,467 draws, versus 5.382 s across
43,589 draws in the earlier window. The scenes and draw counts differed, so
the normalized counters demonstrate a lower hot-path cost while the FPS
difference remains progress evidence rather than a universal performance
claim.

The transient-copy TOCTOU above is now closed for GPU-memory lifetime and
object-graph mutations. The pool reserves and maps a reusable entry before
entering the GPU-memory critical section; one capture operation then holds the
backing-mutation mutex and object-graph mutex while it revalidates allocation
containment, classifies the overlap snapshot, and copies the bounded guest
range directly into that mapped entry. Ineligible ranges are rejected before
allocating a new snapshot entry; if eligibility changes after preflight, the
failed atomic revalidation destroys that just-created entry rather than
charging it to the pool. Existing unused entries remain reusable. Upload byte
and copy-time counters advance only after a successful capture, while probe
timing retains separate validation and upload phases. The focused transient
suite proves byte-for-byte capture while eligible and rejection after
publishing a writable storage object. This does not serialize arbitrary guest
CPU stores to an otherwise valid range, and it has not yet been correlated
with a new strict visual capture; therefore it is a general lifetime
correction, not evidence that the 3D defect is fixed.

One residual pool-capacity defect remains recorded but is not established as
the current visual or liveness producer. The 1 MiB critical byte reserve does
not reserve pool entries: snapshots can consume the shared 1,536-entry ceiling
and deny a later mandatory upload while bytes remain. Reserve entries as well
as bytes before relying on that guarantee. Completed runtime counters do not
link this defect to the observed pre-input no-progress window, so do not patch
it as a speculative 3D or stall fix.

Transient snapshot creation also retains an older availability defect in
`GraphicsRenderCommandBuffer.cpp`: failure to map a newly created optional
snapshot buffer reaches `EXIT_IF` because the underlying Vulkan create/map
helpers do not expose a fallible result. The persistent buffer path can only
be selected when snapshot acquisition returns `nullptr`; it cannot recover
from that termination. A future correction should add a fallible transient
create/map helper, clean up any partial allocation, and return `nullptr` to the
authoritative persistent path. This behavior predates the atomic capture and
is not evidence for the current 3D corruption.

The first strict visual route after the lifetime-aware capture correction used
the established present-8,000 gate, delivered exactly two diagnostic input
edges with both 40-present deltas, and captured at present 8,082. The runtime
was interactive, both taps were consumed, `last-error` was null, no sync wait
was blocked, and the native metadata reported a 1.63 GiB host peak. The frame
contained a coherent title/logo layer and isolated warm geometry over an
otherwise black scene; the offline scorer reported `scene_ok=false`,
`gameplay_like=false`, entropy `0.3139`, 193 quantized colors, and no stripe
classification. This fails the visual restoration gate. It is not a causal
same-scene A/B with the earlier gameplay-era capture, so it neither proves nor
disproves that the atomic correction affected the exact 3,564-index draw. The
host zram was almost exhausted after the process stopped, and no retry was
attempted.

That route also exposed a telemetry-only argument-order defect. It recorded
536,128 transient probes and 534,602 hits but zero validation time because the
capture caller passed its validation duration to the overlap field. The caller
now sends `(validation, zero overlap, upload)` in the declared order. The
change rebuilt both affected executables and the existing performance-snapshot
contract passed. It changes no capture eligibility, copied bytes, Vulkan
resource, draw, or shader behavior; a later safe guest route must still confirm
the corrected runtime counter. Do not interpret the zero in the preserved run
as a lock-free or zero-cost validation path.

The recovered historical baseline is a coherent vehicle-selection screen, not
the gameplay scene containing the exact 3,564-index draw. It remains useful as
proof that the renderer can present coherent 3D geometry, but it is not a
same-scene A/B and cannot establish restoration of the missing world. A static
audit of the exact ordinary uint16 path found the expected 7,128-byte range,
triangle-list topology, zero first index and base vertex, and an unchanged
3,564-count Vulkan request. The recorded 2,376 required and declared vertex
records also agree with the full index-range maximum. No type, byte-size,
offset, chunking, primitive-restart, or transient lifetime defect is evidenced
for this draw. The existing twelve-position index peek would still be
insufficient to prove all uploaded topology; if this path is reopened, capture
one bounded source-to-upload checksum plus min/max and `0xffff` count at the
upload seam rather than adding a full index dump.

The same audit found a general uncorrected command-processor defect:
`DrawIndexOffset` does not enforce the declared `m_index_buffer_size`, unlike
the indirect path. A request whose offset plus count exceeds that limit can
therefore read beyond the guest-declared index range. The exact trace does not
retain that CP limit, so this defect is not currently attributable to the
3,564-index draw and must not be patched as a speculative visual fix. A future
correction needs an integration contract that drives the real offset-draw
packet with a deliberately undersized declared range.

The retained complete BC mip files also do not belong to the sampled image in
the exact draw: the earlier size-only diagnostic selected another 1024x1024
resource before the target material was reached. The exact trace proves only
sixteen nonzero base-level block samples and one nonzero tail block, not the
complete detiled chain used by that draw. `KYTY_DUMP_TILED_BLOCKS` now accepts
the general form `WIDTHxHEIGHT@ADDRESS` and requires a valid exact size and,
when supplied, address match before writing its already bounded files. This is
diagnostic-only and changes no detile, upload, view, sampler, or shader
semantics. Its strict parser contract covers legacy size-only, bare and
prefixed hexadecimal addresses, mismatches, malformed input, signs, whitespace,
and integer overflow. The affected integration executable rebuilt under a
1 GiB, zero-swap, single-CPU cgroup, and the isolated contract passed under a
256 MiB cgroup. The full consolidated integration later reached its pre-existing
`NoMatchingInstruction` storage-analysis expectation and failed there; it is
not a green full-suite result. No guest rerun followed because host zram was
exhausted. The next safe single-variable route is an exact-resource mip
capture, followed by offline BC decoding at the observed wrapped UV region and
the mips implied by the draw derivatives.

A clean-room static comparison against two independent Gen5 renderers found no
different Standard4KB BC swizzle or shared-tail coordinate formula to adopt:
the eight-byte block equation and thin 4 KiB tail placement materially agree.
The remaining general divergence is host exposure. Kyty detiles all eleven
declared levels for the 1024x1024 resource but intentionally creates and
uploads only levels zero through eight. The omitted levels nine and ten are a
real sampling/color limitation when implicit derivatives request them, but the
earlier full-tail production experiment did not restore black geometry and
caused severe upload churn. Do not repeat that toggle as a visual fix.

The exact-address diagnostic now writes every logical level retained by the
validated layout, including host-unexposed tail levels, while the Vulkan image
and upload region count remain under the existing containment policy. This
keeps the diagnostic bounded to the already detiled chain and allows one
offline comparison between host-clamped level eight and the complete logical
tail. It still cannot establish the selected mip without correlating the
existing LOD tap or equivalent derivative evidence. If levels nine and ten do
not contribute, close the missing-tail hypothesis for this draw; if they do,
record it as a color-exposure difference only until a same-scene capture proves
a broader effect.

The first exact-address attempt delivered no input because the agent rejected
an uppercase button name, then reached its bounded runtime limit. It produced
no target mip files and is not route or render evidence. A following strict
two-edge route confirmed that the guest address of the otherwise identical BC1
material changes between processes. The two taps were delivered and no tap
remained pending, but the late capture had already reached the main menu and is
not a visual comparison for the missing world or the 3,564-index draw. Address
matching is therefore useful only within one live process and must not be used
as a stable selector across runs.

A final data-only strict route used the stable size selector and correlated the
result with a one-draw trace from the same process. It delivered exactly two
input edges with both 40-present deltas, retained strict mode, and was stopped
immediately after the files appeared. The correlated material produced all
eleven logical mip files. Levels zero through eight are byte-for-byte identical
to the earlier same-sized dump despite its different guest address, so the old
files represented equivalent texture content rather than an unrelated image.
At the observed repeated UV rectangle, nearest samples are opaque gray for
levels zero through six, light gray at level seven, and warm light gray at
level eight. The omitted 2x2 and 1x1 tail blocks are also fully opaque and
nonblack; the final level is uniform near `(210,206,201,255)` in decoded BC1
endpoint space.

Combined with the earlier draw-scoped LOD result and failed full-chain visual
retry, this closes an empty, black, mis-detiled, or address-stale BC tail as the
current 3D producer. The host-level omission remains a general color/LOD
correctness gap, but neither the retained levels nor the complete logical tail
can erase the material's spatial support. Do not reopen it as the black-world
or progressive-stretching cause without a same-scene result that contradicts
these bytes. The next falsifiable frontier is post-VS clip position and
perspective-varying state, followed only if necessary by a full source-to-upload
index checksum.

## Selected post-VS clip aggregate phase

The next single-variable diagnostic tested whether the exact Gen5 vertex
shader and indexed draw previously associated with the damaged material emit a
non-finite or extreme `gl_Position`. The host-only selector requires the bare
16-digit shader checksum plus `indexed:3564`; it gives the selected SPIR-V and
pipeline a diagnostic identity distinct from ordinary cached modules. A
renderer-owned, host-visible 40-byte storage buffer records one fence-completed
aggregate only: invocation and non-finite counts, finite `w`, and finite
`x/w`, `y/w`, and `z/w` extrema. It does not replace a guest descriptor, alter
the draw, or log per vertex.

The contract, SPIR-V validation, blocking and nonblocking real-Vulkan fence
lifecycles, and the affected `fc_script` target passed under a zero-swap,
single-job build envelope. An independent renderer/lifetime review found no
remaining material issue before the guest run. This is integration evidence
for the diagnostic contract, not visual evidence.

One strict Release+Silent guest then ran from the dirty instrumented worktree
whose reported base revision was `f3b8718a79d911f700bbf99164259bd01ad7722b`.
The transient service used `MemoryHigh=2G`, `MemoryMax=2560M`,
`MemorySwapMax=0`, `CPUQuota=150%`, and `RuntimeMaxSec=150`. It peaked at
1,997,975,552 bytes, used zero swap, consumed 98.944606 CPU seconds over 88.597
wall-clock seconds, and was stopped deliberately after the evidence was
collected. No build ran concurrently. The relevant sanitized launch and native
agent route were:

```text
systemd-run --user --collect --no-block \
  -p MemoryHigh=2G -p MemoryMax=2560M -p MemorySwapMax=0 \
  -p CPUQuota=150% -p RuntimeMaxSec=150 -p KillMode=control-group \
  env KYTY_PRINTF_DIRECTION=Silent \
      KYTY_VS_CLIP_PROBE=0a0005c0ef41d630 \
      KYTY_VS_CLIP_PROBE_DRAW=indexed:3564 \
      KYTY_AGENT_ENDPOINT="$SOCKET" \
      fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"

kyty_agent --endpoint "$SOCKET" wait-ready --timeout-ms 30000
kyty_agent --endpoint "$SOCKET" doctor
kyty_agent --endpoint "$SOCKET" wait-present --min 8000 --timeout-ms 45000
kyty_agent --endpoint "$SOCKET" pad tap cross
kyty_agent --endpoint "$SOCKET" wait-present --delta 40 --timeout-ms 10000
kyty_agent --endpoint "$SOCKET" pad tap cross
kyty_agent --endpoint "$SOCKET" wait-present --delta 40 --timeout-ms 10000
kyty_agent --endpoint "$SOCKET" status
kyty_agent --endpoint "$SOCKET" events --last 512
kyty_agent --endpoint "$SOCKET" capture --timeout-ms 10000
```

No permissive, bring-up, fragment-shader tap, address filter, tiled dump, or
fallback variable was active. Both lowercase `cross` edges were consumed and
both 40-present waits completed. The native event ring retained a single
fence-completed record with no overflow:

```text
cs=0a0005c0ef41d630 k=i n=3564 s=2 inv=2376 nf=0 fin=1
w=1704.63:2135.57 x=0.353883:0.878215
y=-0.022453:0.00239485 z=3.4905e-06:4.6743e-06
```

All 2,376 required vertex records reached the selected shader, `w` remained
positive and finite, and every recorded normalized position remained finite
and bounded. This excludes an invalid or extreme position export as the
producer for this probed occurrence; it also agrees with the earlier coherent
uint16 index-range evidence. Do not add a clip clamp or depth workaround on
this result.

The one allowed capture was taken later, after the second edge, at present
10,523. It showed a credits/text screen rather than the damaged 3D scene; the
native scorer returned `low_entropy`, entropy 1.0671, 158 color bins, and
`healthy=false`. It is route and probe-delivery evidence only. Because the
capture is not same-frame 3D context, this run does not establish restored
geometry, playability, or a global exclusion for every later occurrence of the
shader/count pair.

The next falsifiable producer is therefore the selected vertex-to-pixel
parameter interface, especially the perspective-varying values consumed as
texture coordinates, followed by pixel interpolator mode/location. Reuse the
same one-shot aggregate and fence lifecycle; first correlate it with the
damaged 3D occurrence, then record only finite counts and extrema for the
actually consumed parameter locations. Do not reopen depth, BC tail, or clip
position without contradictory same-scene evidence, and do not introduce a
semantic correction until that aggregate identifies the failing boundary.

A bounded clean-room comparison then checked the recent explicit/custom RDNA2
interpolation work before adopting it. The persistent module identity for the
exact pixel checksum `210005b0766a27a5` was matched byte-for-byte to its cached
SPIR-V source. That guest program contains eleven `V_INTERP_P1_F32` and eleven
`V_INTERP_P2_F32` instructions, and no `V_INTERP_MOV_F32`. Its first implicit
texture sample reads X/Y from logical pixel input zero, whose captured
`SPI_PS_INPUT_CNTL` setting maps directly to producer `PARAM0` / host location
zero. Both independent Gen5 implementations inspected use the same ordinary
lowering for this case: P1 prepares interpolation and the host rasterizer's
smooth varying is consumed at P2. The newer per-vertex barycentric path is for
explicit P0/P10/P20 reads through `V_INTERP_MOV_F32`; this exact shader never
requests it.

Therefore custom-interpolation support is a real general gap in Kyty, but it is
not the producer for this draw and must not be ported as a speculative 3D fix.
The next single-variable probe is the selected VS `PARAM0.xy` export that feeds
the actual texture coordinates. Record only export count, non-finite count, and
finite X/Y extrema in the existing fence-completed aggregate. If those values
are finite and plausible, close the vertex producer and move the same aggregate
to the PS input-zero value immediately before the implicit sample; only then
test location or perspective interpolation semantics.

## Selected VS PARAM0 aggregate phase

The existing one-shot vertex aggregate was extended from ten to sixteen uints
without moving its original fields. The appended fields count selected
`EXP PARAM0` executions, reject a non-finite X or Y once per invocation, and
record finite ordered X/Y extrema. Instrumentation runs after the original
guest export store and only when both X and Y enable-mask bits are present; it
therefore accepts the exact shader's `en=0x3` export while rejecting incomplete
X-only or Y-only data. Ordinary modules retain their identity. Diagnostic
modules use the `VCPROBE3` cache revision so neither the earlier 10-word module
nor the initially incorrect full-mask-only diagnostic can be reused.

The consolidated graphics integration fixture first failed on the old 40-byte
contract, then passed after the implementation. Its final focused run covered
the contract plus real blocking and nonblocking fence lifecycles: four of four
CTest entries passed under `MemoryHigh=1G`, `MemoryMax=1536M`, zero cgroup swap,
`CPUQuota=100%`, and a single build job. The affected `fc_script` and matching
native agent were linked at `-j1`. An independent read-only review confirmed
the XY-mask gate, preservation of the original export store, SPIR-V validation,
and diagnostic cache separation.

The first corrected strict launch did not reach the required pre-input state.
It compiled dozens of missing SPIR-V modules, slowed to roughly 11 presents per
second, and was stopped deliberately at present 4,380 without sending any
input. It peaked at 2,149,519,360 bytes, used zero cgroup swap, and produced no
shader aggregate; it is cold-cache evidence only, not a failed guest occurrence.
The generated module and pipeline cache entries were retained outside the
repository.

One cache-warm strict Release+Silent launch then used the same reported base
revision `f3b8718a79d911f700bbf99164259bd01ad7722b`, selector
`0a0005c0ef41d630`, and `indexed:3564` draw. It reached present 8,000 before
input, consumed exactly two lowercase `cross` edges separated by 40-present
waits, retained all 280 events with no overflow, and was stopped deliberately
after evidence collection. No build or capture ran concurrently. The service
peaked at 2,149,466,112 bytes, consumed 58.983247 CPU seconds, and used zero
cgroup swap. The fence-completed records were:

```text
cs=0a0005c0ef41d630 k=i n=3564 s=2 p0n=2376 p0nf=0 p0fin=1
p0x=0.02:0.548827 p0y=0.0164139:1.01934

cs=0a0005c0ef41d630 k=i n=3564 s=2 inv=2376 nf=0 fin=1
w=1706.75:2137.69 x=0.353444:0.877137
y=-0.0224272:0.00239087 z=3.48586e-06:4.66701e-06
```

All 2,376 selected vertex invocations exported complete finite `PARAM0.xy`;
their range is texture-coordinate-like and not an extreme or non-finite tear.
Together with the repeated finite position aggregate, this closes malformed VS
position and malformed VS `PARAM0.xy` as producers for this occurrence. It
does not prove the later damaged 3D scene, restored rendering, or playability,
and no redundant credits capture was taken.

The next falsifiable boundary is the exact pixel shader's logical input zero
after perspective interpolation and immediately before its first implicit
texture sample. Reuse a one-shot bounded aggregate and prove the selected PS
module/pipeline identity; do not change interpolation qualifiers, clamp UVs,
rewrite sampling, or reopen depth and mip-tail theories unless that PS-side
measurement contradicts the finite producer evidence above.

## Selected PS input-zero aggregate with late diagnostic depth

The first draw-scoped pixel-input aggregate preserved the guest's opaque
`EarlyFragmentTests` execution mode. Its fence-completed result contained zero
fragment observations. Because the selected pipeline performs early depth and
stencil tests, that result proved only that no surviving fragment reached the
first implicit sample; it could not distinguish a broken interpolator from
ordinary depth rejection.

The diagnostic module now omits `EarlyFragmentTests` only while the host-owned
pixel-input probe is enabled and static analysis proves the guest shader has no
discard, storage/image write, atomic, unknown, or other retained side effect.
Otherwise it preserves early tests and refuses to observe occluded fragments.
The ordinary module retains the guest early-Z mode, and the probe writes only
its distinct host diagnostic buffer. The
diagnostic cache revision is `VCPROBE5`, so a persisted module with the earlier
execution mode cannot satisfy the new identity. A focused integration contract
first failed while the selected source still contained `EarlyFragmentTests`,
then passed after the separation. It also proves that the ordinary opaque
source keeps the decoration, the selected source observes only the first
implicit sample, and both generated modules validate as SPIR-V. The affected
integration executable and `fc_script` were rebuilt at one job under the
zero-swap memory envelope.

One strict Release+Silent run then scheduled exactly two diagnostic `cross`
edges at presents 8,000 and 8,040 before either target was reached. Both edges
were delivered, no scheduled edge was cancelled, the event ring did not
overflow, `last-error` remained null, and the sole process was stopped
deliberately after present 8,100. The cgroup peaked at 1,646,026,752 bytes and
used zero swap. The late-test diagnostic produced:

```text
ps=210005b0766a27a5 k=i n=3564 s=2 i0n=167 i0nf=0 i0fin=1
i0x=0.0200019:0.547958 i0y=0.0164139:1.01933
```

The 167 covered fragments consumed finite input-zero coordinates whose
aggregate bounds agree with the selected VS `PARAM0.xy` extrema to the recorded
precision. This excludes zero execution and a gross, extreme, or non-finite
first-sample coordinate for this occurrence. Aggregate extrema do not prove
per-fragment correspondence, derivatives, or exact perspective interpolation.
The earlier zero count was early-depth rejection, not evidence that the
interpolator produced no values.

The same selected VS aggregate spans only a thin normalized-device-coordinate
band (`y/w=-0.0224272..0.00239087`). Even though earlier notes called the
3,564-index material a vehicle draw, this observed occurrence cannot by itself
cover the full large vehicle silhouette. Do not keep using it as a proxy for
all missing geometry. The next bounded discriminator must first correlate a
same-scene native capture with the draw that actually produces or occludes the
large damaged geometry. Until that producer is identified, do not change
interpolation, clamp UVs, disable depth, invert reverse-Z, or reopen the closed
BC-tail and attachment-identity experiments.

The requested clean-room sibling comparisons do not change this frontier. The
large merged ES/GS mesh path is not exercised by the observed NGG passthrough
vertex-only state. Recent page-index/LRU/deferred-destruction changes do not
provide a missing contract over this tree's exact-range lookup, logical and
backing generations, descriptor range identity, and submission-owned deferred
deletion. Compact f16 arithmetic and f16 VOPC are already decoded and lowered
here; the exact material's final half conversions are MRT export packing. The
traced material also had DCC and CMASK fast clear disabled, so a foreign
metadata-clear state machine is not a correction for this draw. Preserve these
as explicit exclusions rather than porting architecture or timing changes
without a failing local contract.

The retained gameplay capture at present 8,860 has a same-run, 32-draw trace
for this PS/VS pair. It confirms that the 3,564-index occurrence is not the
largest geometry using the material: unique occurrences with 41,910 indices
and 27,937 declared vertex records, and with 39,120 indices and 26,079 records,
also target the same 1920x1080 color and depth attachments with the same GEQUAL
depth-write state. Their traced vertex layouts are valid stride-24 position,
packed-normal, and half-UV streams, and their sampled source positions are
finite. The capture nevertheless contains gameplay HUD over a black world.
This narrows the next correlation target to the unique 41,910-index draw first,
then the 39,120-index draw only if the first has small or off-screen post-VS
support. Use the existing bounded post-VS/PS aggregate and the exact scheduled
input route; do not add a scene-wide dump, disable depth globally, or reuse the
3,564-index result as evidence for these larger draws.

Two attempted 41,910-index probe runs produced no selected event because their
launch environment used the wrong variable name,
`KYTY_VERTEX_CLIP_PROBE`; the implemented contract is
`KYTY_VS_CLIP_PROBE`. Their absence of an event or cache marker is therefore
operator error and carries no shader, fence, or scene-timing meaning. Do not
retain the earlier inference that this draw was absent from the exact route.

A subsequent strict route used only the existing PS trace, bounded to 32 draws
from present 8,250. It delivered both scheduled inputs, reached present 8,300,
captured at present 8,448, reported no structured error or event loss, and was
stopped immediately. The capture had only UI over a black world
(`healthy=false`, entropy `0.2022`, 158 bins). The same-run trace records the
41,910-index draw at ordinal 0 and again at ordinal 29, with the same 83,820-byte
index range, 27,937 required and declared vertex records, valid stride-24
position/packed-normal/half-UV layout, and finite samples across the beginning,
quarters, and tail of the stream. It targets the same full-resolution color and
depth attachments with GEQUAL depth writes. This restores `indexed:41910` as
the first large-geometry correlation target. The next run must use the correct
`KYTY_VS_CLIP_PROBE` variable and wait directly for `vs_clip_probe`; do not add
another trace, change renderer semantics, or test the 39,120-index draw first.

## Large-draw clip result and guest Z-clipping correction

A cold-cache strict run then selected the 41,910-index occurrence with the
correct `KYTY_VS_CLIP_PROBE` contract. The one-shot aggregate completed after
the exact command-buffer fence and covered all 27,937 declared invocations:

```text
cs=0a0005c0ef41d630 k=i n=41910 s=2 inv=27937 nf=0 fin=1
w=-3490.44:100.36 x=-323.048:1252.07
y=-29.2992:9.72501 z=-0.00351684:0.00577411

cs=0a0005c0ef41d630 k=i n=41910 s=2 p0n=27937 p0nf=0 p0fin=1
p0x=-0.0910645:0.869141 p0y=0.0101471:0.958984
```

Here `x`, `y`, and `z` are post-divide coordinates. Texture coordinates remain
finite and plausible, while clip-space W crosses zero and the projected X/Y
range is extreme. That is compatible with triangles stretching across the
camera or clip planes, but an aggregate over a large world mesh does not prove
that its transform is wrong. The same run captured only UI over black and had
no structured runtime error (`healthy=false`, entropy `0.2023`, 158 bins).

Inspection of the host pipeline found a separate general contract defect:
`VkPipelineRasterizationDepthClipStateCreateInfoEXT::depthClipEnable` was hard
coded false for every graphics pipeline. The guest clip-control state already
decoded `min_z_clip_disable` and `max_z_clip_disable`, and the usual observed
state keeps both planes enabled, so the host was discarding a real guest
pipeline bit. Independent clean-room comparison confirmed the same behavioral
boundary in commit `aebf6b491353c1ae2690b48141bc5e21d105cd21`; no foreign code
was copied.

The renderer now resolves guest Z clipping once into native depth-clip and
depth-clamp state, includes both bits in graphics-pipeline identity, enables
and queries the Vulkan feature structs explicitly, and activates core
`depthClamp` only when the physical device supports it. If neither native
mechanism is available, the valid core clipping state is retained. Vulkan can
disable only the paired near/far Z planes, so an asymmetric guest request is
recorded by the resolver as inexact instead of being mistaken for an exact
translation. The depth/stencil-copy pipeline follows the same capability
boundary.

The focused graphics integration first failed because the resolver did not
exist, then passed with the capability matrix, asymmetric-state classification,
and pipeline-cache identity checks. The affected integration executable and
`fc_script` built at one job under `MemoryHigh=2G`, `MemoryMax=2560M`, zero
cgroup swap, and `CPUQuota=150%`. A second independent review found no remaining
blocking issue after unsupported `depthClamp`, `depthClipControl`, and
`colorWriteEnable` feature gating was corrected.

One strict Release+Silent validation used an empty Vulkan cache, the same
resource envelope and a 150-second watchdog. Its capture metadata reported a
1,961,455,616-byte host peak, and systemd rounded the service peak to 2 GiB;
the group never exceeded its 2.5 GiB hard limit or used cgroup swap. It
scheduled exactly two `cross` edges at presents 8,000 and 8,040; both were
delivered, none was cancelled, the event ring had no drops, and `last-error`
was null. The native capture at present 8,429 was still the `PLAY` prompt rather than gameplay
(`healthy=false`, entropy `0.2107`, 216 bins). It therefore proves transport,
input delivery and runtime survival only. It neither validates nor falsifies
the Z-clipping correction against the damaged 3D scene, and the differing UI
phase makes its score incomparable with the preceding gameplay capture. Do not
claim 3D recovery from this run or send an extra input to force the route.

The next 3D discriminator remains the selected large draw's live V# resource
resolution. Record, for this exact shader/draw only, whether each embedded
MUBUF load matched the intended stream, its descriptor base/stride/record
count, computed byte range, and final SSBO slot. If all selected loads match a
valid stream and range, close the live-resolver seam before investigating the
transform constants. Do not repeat the append-disabled A/B or reinterpret the
inconclusive UI-only Z-clipping run as gameplay evidence.

A first attempt to add that runtime MUBUF record was deliberately withdrawn
before any guest run. It expanded the existing 22-word diagnostic buffer before
the SPIR-V ordinal/layout contract and integration fixture were complete; the
bounded build failed at the expected missing internal contracts. Every
`mubuf_trace` hunk was then removed manually, and `fc_script` plus the focused
graphics integration rebuilt successfully at `-j1`; the isolated integration
mode exited zero. This is an implementation dead end only, not evidence that
the live resolver is correct or incorrect. A future attempt must define the
fixed raw layout, ordinal enumeration and cache identity first, then add the
runtime stores as the final step.

The replacement diagnostic keeps the existing exact vertex-clip selector and
adds one bounded observation to its fence-completed aggregate instead of a
per-instruction trace. The current 37-word `VCPROB7` raw layout retains the
original 31 words and lets the first executed
embedded MUBUF address setup atomically claim one record containing its static
PC and access width, live descriptor words 0 and 1, raw byte offset, final
validity, SSBO slot, and byte offset. The claim and stores exist only in a
module for which `UsesVertexClipProbe()` is true. An ordinary module generated
from the same embedded-MUBUF fixture retains the product resolver guard, has no
probe SSBO or `vertex_resolver_` symbols, and validates as SPIR-V. The host
formatter rejects a nonzero payload when no invocation claimed the record, a
zero-width claimed record, invalid validity values, and a nonzero final
slot/offset for an invalid decision. This is deliberately a one-sample
discriminator, not evidence about every lane or every MUBUF instruction.

After those contracts passed the focused graphics integration and `fc_script`
built at `-j1` under the 2.5-GiB zero-swap envelope, an independent re-review
closed both earlier diagnostic-isolation findings. One strict Release+Silent
run then used an empty Vulkan cache and the existing exact selector
`0a0005c0ef41d630` plus `indexed:41910`. It scheduled exactly two `cross` taps
at presents 8,000 and 8,040. Both were delivered, none was cancelled, and at
present 9,092 the runtime reported `last-error=null`, no event-ring drops, and
no cgroup swap. The measured in-run host peak was 2,149,576,704 bytes, below
the 2.5-GiB hard limit. The service was terminated by its 150-second watchdog;
no Kyty process survived it.

That run never emitted `vs_resolver_probe` and still reported the `loading`
phase. It therefore did not execute the selected draw on this route and is
inconclusive for both the live resolver and the 3D defect. Do not interpret the
missing record as `valid=0`, add another input, or change MUBUF semantics from
this run.

## Bounded buffer-cache advance and resolver exclusion

The subsequent performance snapshot gave a direct reason that repeated
150-second runs often failed to reach the input gate. Across 4,376 presents it
recorded 264,618 draws, 848,326 `GpuMemory::CreateObject` calls, 4,377
`VertexBuffer` `ReclaimNew` outcomes, and 2,456,274 uploads totaling 19.7 GiB.
The create counter includes cache hits and lookups, so it is not a Vulkan
allocation count. The retained slow records nevertheless showed repeated
525,888-to-848,664-byte vertex buffers crossing the existing 512-KiB immutable
snapshot ceiling.

A clean-room audit of the cited buffer-cache series found no page-index, LRU,
image-view, or unmap patch to copy. This renderer already has an exact-base
index, a multi-owner 1-MiB page candidate index with exact range
classification, submission-keyed deferred deletion, real-use retirement, and
finite image views. The reference's central single-owner span-union model is
incompatible with this tree's required VB/IB/texture/render-target alias graph;
an earlier union-span experiment already caused device loss. Keep the existing
multi-owner and writeback contracts.

The safe local improvement instead raises only immutable transient read-only
snapshots to 1 MiB. Writable, surface-connected, unallocated, truncated, and
pool-exhausted ranges still fall back to authoritative `GpuMemory`. The general
pool remains capped at 16 MiB with 1 MiB reserved for small critical uploads.
Because D16 inline detile can require a full 16-MiB scratch buffer, scratch now
has a distinct command-buffer-owned pool, reset independently and destroyed
only after the same fence. Two adversarial reviews rejected shared-pool growth
before this separation; the final review passed. The existing boundary tests,
the vertex-probe integration, and `fc_script` built and passed at `-j1` under
the zero-swap envelope.

This is a measured runtime advance, not a graphics fix. At the first
45-second checkpoint after the change, the strict route reached present 5,769
at 68.3 FPS, versus present 4,231 at about 8 FPS in the comparable pre-change
run. Both samples are workload observations, not a controlled performance
benchmark. Host memory remained near 2.15 GiB with cgroup swap zero.

A following strict run finally completed the selected fence and emitted:

```text
cs=0a0005c0ef41d630 k=i n=41910 s=2 c=0
```

Here `c=0` is a valid empty diagnostic payload: no invocation executed an
instrumented embedded-MUBUF address setup. It is not `valid=0`. The previously
extracted exact module already has separate Vulkan attribute loads for
position, normal, and UV (`attr0`, `attr1`, and `attr3`), so this draw uses the
semantic `Fetch` path and does not consume the appended stream SSBO through the
live resolver. A synthetic untagged-SGPR check also retained ordinary MUBUF on
the current translator, so there is no new red contract for wiring the dormant
attribute-validity helper. Close the live-resolver seam for this exact draw and
do not change MUBUF or `DetectFetch` from this result.

The same run reached present 8,225, delivered exactly the two scheduled taps,
reported `last-error=null` and no event loss, and produced a native capture of
the credits screen. Its automated `stripey` classification (entropy `0.9972`,
159 bins) responds to the horizontal white text and is not evidence of the 3D
tearing symptom. The frame is neither PLAY nor gameplay, so it cannot validate
the Z-clipping correction or 3D coherence. The next integration problem is
route phase under the faster renderer: reproduce the PLAY checkpoint without
a third input before choosing another renderer semantic.

Waiting for the 41,910-index fence before sending the first input was tested
once as a condition-based replacement for the now speed-sensitive absolute
present. It is not a viable route: with zero inputs the strict runtime advanced
to present 32,329 at a reported 309 FPS without ever emitting the selected
event. Memory peaked near 1.59 GiB, cgroup swap stayed zero, `last-error` was
null, and the process was stopped deliberately without input or capture. The
draw is downstream of input and cannot anchor the first edge. Do not repeat
this no-input wait or classify the absent event as a shader failure.

Increasing only the second-input separation from 40 to 80 presents proved that
the faster renderer made absolute present timing visually nondeterministic; it
did not prove a hang. Two strict runs scheduled `cross` at 8,000 and 8,080,
delivered both taps without cancellation, and emitted the selected resolver,
parameter, and clip events between them. One later native frame at present
9,161 showed `PLAY` without the vehicle, while a second run captured the logo
at present 8,982. In the second run, a 20-second no-capture watch advanced 273
presents while FPS fell from about 11.9 to 11.0; `last-error` stayed null,
`sync-waits` had no blocked wait, the event ring had no drops, cgroup current
memory remained below 2 GiB at the diagnostic checkpoint, and cgroup swap was
zero. The earlier capture timeout was therefore slow progress during the scene
transition, not a reproduced presentation deadlock.

A separate one-input strict run then scheduled only the first tap at present
8,000 and captured immediately after `vs_resolver_probe`. The event arrived
443 ms after input and again reported `c=0`; the paired clip and `param0`
aggregates were finite. Nevertheless, the native frame at present 8,169 showed
the credits screen plus a small central color fragment, not the PLAY vehicle.
The offline score reported `scene_ok=false`, `gameplay_like=false`, entropy
`0.5394`, and 167 quantized colors. This excludes the exact 41,910-index draw as
a unique visual scene fence. It remains valid evidence that the semantic-Fetch
draw executed, but it cannot identify PLAY or gameplay by itself.

`VCPROB7` then added six disjoint clip-population counters without changing
ordinary shader modules: finite invocations with non-positive W; positive-W
invocations outside XY; and, for each of the zero-to-one and
negative-one-to-one Z conventions, outside and inside counts. Raw clip-space
comparisons classify positive-W positions before the optional ratio extrema;
raw NaN/Inf alone contributes to the existing nonfinite counter. The bounded
`vs_clip_population` event is emitted only after the draw fence and the
31-word payload remains available in its original events. Focused integration
validated the 37-word offsets, bounded serialization, branch order, ordinary
module isolation, and blocking/nonblocking fence lifecycles. `fc_script` was
rebuilt serially under the same zero-swap envelope, and independent correction
review found no remaining blocker.

A strict Release+Silent run with one scheduled `cross` at present 8,000 then
selected the same 41,910-index occurrence. All 27,937 invocations were rejected
before depth: 25,677 had finite W less than or equal to zero and the remaining
2,260 had positive W but lay outside the XY clip bounds. Neither Z convention
received an invocation; the paired aggregate had `nf=0`, the input-to-result
latency was 538 ms, `last-error` was null, the event ring had no drops, and the
cgroup peak was 1,889,259,520 bytes with zero swap. For this exact occurrence,
an incorrect depth attachment, stale clear, or inconsistent HTILE/read/write
identity cannot explain the missing geometry because it never reaches depth.
The selector is not a unique PLAY fence, so this result must not be generalized
to the scene. The retained same-material trace makes the 39,120-index draw the
next bounded correlation target; no renderer semantic change is justified yet.

That next target is now closed for the same one-edge UI phase. A strict
Release+Silent run selected `indexed:39120` and emitted the aggregate 560 ms
after the tap began. Its 26,079 invocations were again wholly rejected before
depth: `wnp=24679`, `oxy=1400`, and zero outside or inside Z counts under both
conventions. The paired raw range had `nf=0`; the ring had no drops, memory was
about 1.74 GiB, and swap remained zero. A single immediate native capture
request timed out after 20 seconds without producing a file, after which the
90-second watchdog ended the service. Do not repeat the capture. Together the
41,910- and 39,120-index results exclude depth/clear/HTILE as the cause for
those exact first post-input occurrences, but neither selector is a unique
PLAY fence. They do not justify changing the transform or clip convention
without a gameplay-correlated occurrence.

To obtain that correlation without per-draw logging, the diagnostic now accepts
an optional strict decimal minimum present for each selector:
`KYTY_VS_CLIP_PROBE_MIN_PRESENT` and
`KYTY_PS_INPUT0_PROBE_MIN_PRESENT`. The absent value defaults to zero and a
malformed value disables the selector. Matching draws before the threshold are
kept ordinary before descriptor-set and shader/pipeline diagnostic identities
are assigned, so they do not consume the one-shot lifecycle. When both stages
are selected, their shared renderer waits until both minima are reached (the
effective maximum); this prevents an earlier VS-only result from permanently
consuming the PS observation. Focused integration covers the immediate,
delayed, boundary, fail-closed, and unequal-stage-threshold contracts; the
integration target and `fc_script` build serially under the 2.5-GiB zero-swap
envelope.

A strict Silent run then scheduled only the two established inputs at presents
8,000 and 8,080, delayed `indexed:41910` until present 8,500, and completed the
selected fence after both taps. The population changed materially from the
credits-phase result:

```text
inv=27937 nf=0 w=1.16858:1107.94 x=-37.123:138.061
y=-21.1079:11.353 z=7.83364e-06:0.00855622
wnp=19217 oxy=6722 oz01=0 in01=1998 ozn=0 inn=1998
```

The resolver event remained `c=0`, `param0` was finite, both inputs were
delivered without cancellation, the ring had no drops, and the last live
cgroup snapshot peaked at 1,889,488,896 bytes with zero swap. The 90-second
watchdog ended the process shortly after the event, so `last-error` could not
be queried after shutdown and must not be reported as null. This later
occurrence has 1,998 vertices inside the clip volume under either Z convention;
therefore an attachment/depth/clear/HTILE fault can still suppress its visible
fragments. The next bounded discriminator is to enable the existing pixel-input
probe for the same delayed VS/PS draw. Do not change the transform, disable
depth, or repeat the failed capture first.

The delayed pixel-input discriminator then completed on the same two-input
route. Its selected event was:

```text
ps=210005b0766a27a5 k=i n=41910 s=2 i0n=207953 i0nf=0 i0fin=1
i0x=-0.0910642:0.86913 i0y=0.0101471:0.958984
```

The diagnostic module moves this side-effect-free shader to late depth only
for observation, so these 207,953 finite invocations establish raster coverage
and valid first-varying interpolation before depth. They exclude total clip,
culling/no rasterization, and a gross/nonfinite interpolator failure for the
selected later occurrence. The event returned successfully before the
95-second watchdog; the service ended before a follow-up status or
`last-error` query, and its journal reports a rounded 2-GiB peak. Do not report
`last-error=null`. The remaining split is samples rejected by the unchanged
depth/stencil state versus later sample/color-output failure.

That statistic now uses one host-only `VK_QUERY_TYPE_OCCLUSION` pool owned by
the existing one-shot renderer. Reset occurs before the render pass; begin/end
surround all chunks of the selected draw inside the pass; result availability
is read only after its command-buffer fence. The final `depth_stencil_probe`
event records whether fixed tests are applicable, whether depth, stencil, and
depth-bounds are enabled, `ready`, `precise=0`, and `any_passed`; it never
publishes the raw non-precise payload as a sample count. A production color-only
match reports `applicable=0` and does not issue the query. Separate registered
integration modes exercise a real empty render-pass query/readback,
depth-bounds-only applicability, and the color-only path; the original fence
lifecycle remains covered separately. Ordinary draws never issue the query.

A strict VS-only delayed run preserved the guest pixel shader's normal early
tests. Its prototype event returned raw zero; under the corrected final event
contract that evidence is:

```text
cs=0a0005c0ef41d630 k=i n=41910 s=2
applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0
wnp=11071 oxy=9583 oz01=0 in01=7283 ozn=0 inn=7283
```

The companion output had `nf=0`, finite positive `z/w`, finite `param0`, and
resolver `c=0`. Both scheduled inputs were delivered without cancellation,
`last-error` was null, the ring had no drops, cgroup memory peaked at
2,037,301,248 bytes with zero swap, and the service was stopped cleanly. The
retained exact material trace has `stencil_test=0`, GEQUAL depth test/write,
LOAD, clear value zero, and no explicit clear. Therefore all covered samples
of this selected occurrence are rejected specifically by depth. This confirms
that a bad attachment value/clear/lifetime could explain its absence, but does
not prove one: several earlier GEQUAL draws target the same image and may
legitimately occlude it. One strict attempt to select the earlier 15,366-index
draw with the same present-8,500 gate emitted no event before the 95-second
watchdog. The retained sequence places that draw earlier; this is a timing dead
end, not evidence about its shader or depth result. Do not repeat the same
threshold or change depth/metadata lifetime from the 41,910 result alone.

One corrected control changed only the minimum present to 8,090 and retained
the same two scheduled inputs. It completed 1,452 ms after the second edge:

```text
cs=0a0005c0ef41d630 k=i n=15366 s=2
applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=1
wnp=0 oxy=1476 oz01=0 in01=7675 ozn=0 inn=7675
```

All 9,151 VS invocations were finite, `param0` remained finite, and the
resolver reported `c=0`. Both inputs were delivered without cancellation,
`last-error` was null, the event ring had no drops, cgroup memory peaked at
2,149,572,608 bytes with zero swap, and the service was stopped cleanly. This
proves that the shared GEQUAL depth sequence passes substantial earlier
geometry; the 41,910-index zero is not a global broken attachment or universal
clear/HTILE failure and may be ordinary occlusion by preceding draws. Stop the
depth-identity branch here. The next causal work belongs to the material/color
path of the passing 15,366-index occurrence.

That material path now has an output-preserving sample-result discriminator.
`KYTY_PS_SAMPLE_PROBE=<checksum>:@<instruction-ordinal>` is mutually exclusive
with the coordinate-input probe and requires the same exact draw plus optional
minimum-present selectors. The selected `ImageSampleB` result is aggregated in
one host-owned SSBO after `OpImageSample*` and before unchanged destination
stores. Sample mode retains the guest's `EarlyFragmentTests`, does not activate
the fragment-tap MRT visualization, and emits only bounded RGBA count,
nonfinite, and min/max fields through `ps_sample_probe`. VCPROB8 expands the
raw layout to 47 words; the pixel probe kind and absolute ordinal have distinct
module identities. A test-first integration build failed on the missing
contract, then passed after the parser, layout, SPIR-V constants, ordinal
validation, renderer event, and cache identities were implemented. During
review, two diagnostic defects were corrected before runtime: `@N` initially
counted only `ImageSampleB` operations instead of using the established
absolute instruction ordinal, and the new SSBO member indices 37--46 lacked
their SPIR-V integer constants. The focused source and binary now validate.

The first strict Silent use selected pixel checksum `210005b0766a27a5`,
`indexed:15366`, absolute ordinal 24, and minimum present 8,090. It emitted:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 ord=24
sn=737427 snf=0 sfin=1
r=0.611765:0.623529 g=0.184314:0.184314
b=0.0941176:0.105882 a=1:1
```

The paired fixed-test query reported `depth=1`, `stencil=0`, and
`any_passed=1`. This is valid evidence that the base sample was finite and
nonzero for the selected occurrence, but the occurrence was not the intended
post-two-input PLAY draw: the agent client reached present 8,689 before its
first command, the first input event was recorded at 57,490 ms, the probe at
58,590 ms, and the second input at 66,995 ms. Both taps eventually delivered,
`last-error` was null, the ring had no drops, the cgroup peaked at
2,239,434,752 bytes with zero swap, and the service was stopped immediately.
The final status had fallen below one FPS after the 737k diagnostic atomics;
this is diagnostic overhead/stall evidence, not an ordinary-render regression.
Do not cite this run as a PLAY material result or change sample/LOD/tiling from
it. The next single-variable runtime must delay reservation until after both
input edges and then re-observe ordinal 24 without a capture.

The one timing-corrected retry raised the reservation threshold to present
9,200 and kept `wait 8000 -> CROSS -> +80 presents -> CROSS -> wait event` in
one local command. With a cold diagnostic cache it reached only present 4,902
inside the unchanged 70-second gate. The command timed out before either input,
so it produced no `ps_sample_probe` or scene evidence. At stop the process was
still interactive at 8.925 FPS, `last-error` was null, the ring had no drops,
no tap was pending or delivered, cgroup memory peaked at 2,149,572,608 bytes,
and swap stayed zero. Do not lengthen or repeat that cold route. The next
attempt is contingent on a proven warm cache or another bounded route that
reaches both edges without altering renderer semantics.

## Deterministic contributing occurrence and base-sample result

The present-addressed controller scheduler removed the manual client-timing
ambiguity. The current native CLI can atomically commit exactly two `cross`
taps at presents 8,000 and 8,080 before the first frame. An initial warm strict
run kept the 8,090 minimum and selected the default first exact occurrence.
Both taps eventually delivered with no cancellation, but the paired events
were:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 ord=24 sn=0 snf=0 sfin=0
cs=0a0005c0ef41d630 k=i n=15366 s=2 depth=1 any_passed=0
```

Because sample mode retains `EarlyFragmentTests`, zero sample executions here
are explained by the same occurrence's zero depth coverage. They are not
texture, sampler, LOD, or image-lifetime evidence. Minimum present selects the
first matching draw after a moving presentation boundary and was therefore not
a stable occurrence identity.

The sample discriminator now also accepts the optional zero-based
`KYTY_PS_SAMPLE_PROBE_MATCH_ORDINAL=N`. `N=0` preserves the existing first
match; `N=1` skips one full checksum/draw/instruction match after the combined
present gate and reserves the next. The counter lives inside the renderer's
mutex-protected process-one-shot lifecycle. Skipped matches are cleared before
pipeline identity and initialize no diagnostic Vulkan resource. A bad selected
PS instruction clears both the PS and any paired VS selection before
reservation. `m=N` is emitted by both `ps_sample_probe` and the paired fixed-test
event; it does not enter shader identity because it changes selection timing,
not generated SPIR-V.

The implementation followed a RED/GREEN cycle inside the existing graphics
integration. RED failed on the absent config/lifecycle contract. GREEN covers
strict parsing/default/malformed/overflow behavior, zero-based skip semantics,
paired invalid-selection rejection, and a fresh-process Vulkan path where the
first `m=1` reservation returns false with a null descriptor layout, the second
reserves, and both completed events contain `m=1`. Serial builds of the focused
integration and `fc_script` passed under the 2.5-GiB zero-swap envelope. The
focused modes `--vertex-clip-probe-contract-only` and
`--vertex-clip-probe-match-ordinal-only` exited zero; an independent correction
review returned PASS.

One strict warm runtime then changed only `MATCH_ORDINAL` to 1 while retaining
the exact two scheduled inputs, PS/sample/draw selector, and present threshold.
It completed after the second scheduled input start with:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 ord=24
sn=1618 snf=0 sfin=1
r=0.623529:0.623529 g=0.184314:0.184314
b=0.105882:0.105882 a=1:1 m=1

cs=0a0005c0ef41d630 k=i n=15366 s=2
applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=1 m=1
```

Both taps delivered, none was cancelled, `last-error` was null, the ring had no
drops, the service reported a rounded 1.6-GiB peak with cgroup swap disabled,
and it was stopped cleanly at present 8,247 without taking a capture. This is
causal material-path evidence: the contributing occurrence reaches ordinal 24
and produces a finite, nonblack base sample. It excludes changing base image
decode, sampler, implicit bias/LOD, tiling, and global depth/clear/HTILE from
this branch. It does not establish a correct final material or a visual PLAY
advance. The next single-variable discriminator is an output-preserving
aggregate of the unchanged final MRT export; if that remains finite/nonblack,
move downstream to blend/color-attachment/target-lifetime rather than adding
more sample probes.

## Deterministic final-MRT result

The output-preserving discriminator is now implemented as
`KYTY_PS_MRT_PROBE=<checksum>:mrtN@<absolute-instruction-ordinal>`, with the
same exact draw, minimum-present, and optional zero-based match-ordinal
contract as the sample probe. It observes the assembled `%t11` value after
component selection and immediately before the existing output `OpStore`; it
does not replace the color, remove `EarlyFragmentTests`, or activate the
fragment-tap visualization. Coordinate, sample-result, MRT, and fragment-tap
diagnostics are mutually exclusive. Active-format validation rejects null
targets, inactive shader-color modes, zero channel masks, wrong MRT targets,
and wrong instruction ordinals before host reservation.

Implementation review caught and closed two false-empty paths before runtime:
an inactive target-output mode and an export with no enabled channels could
previously match the code shape while the recompiler emitted no output store.
The existing graphics integration now covers both negatives, validates the
output-preserving source and binary SPIR-V, and exercises `FinalMrtResult` in
the existing match-ordinal Vulkan mode. Serial `fc_script` and the focused
`--vertex-clip-probe-contract-only` and
`--vertex-clip-probe-match-ordinal-only` modes exited zero under the 2.5-GiB,
zero-swap envelope; independent semantic review returned PASS.

One strict warm run then selected the same exact second occurrence as the
finite base-sample run, changing only the probe point to the known final MRT0
export. Both scheduled inputs delivered, none was cancelled, and the event at
present 8,090 reported:

```text
mrt=0 ord=229 on=408 onf=0 ofin=1
r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1
```

`last-error` was null, the event ring had no drops, host memory pressure stayed
bounded, and the service was stopped immediately without a capture. This is
causal evidence that the contributing pixel invocation reaches the unchanged
MRT0 store with finite, nonblack color after material arithmetic. It is not a
visual or gameplay advance and does not prove that the attachment retained or
presented that value. Stop adding shader/sample probes on this branch. The
next single-variable work belongs downstream: verify blend/write-mask state,
the bound color attachment's identity/view/layout, and its lifetime through
the later composition that exhibits the black or dragged geometry.

## Final-MRT raster coverage and scene-fence exclusion

The final-MRT aggregate now records output-preserving host `FragCoord` X/Y
extrema in four append-only words. The shared diagnostic SSBO is 51 words;
the MRT revision changed so persistent and in-memory caches cannot reuse the
older 47-word module. A separate bounded `ps_mrt_coverage` event avoids
overflowing the 192-byte agent message while preserving the exact selector and
`m=N` provenance. The existing graphics integration was extended instead of
adding a new suite: it first failed on the absent `FragCoord` contract, then
passed source/binary SPIR-V validation, output-store ordering, all 51 explicit
member offsets, the four C++ `offsetof` values, and the three-event ordinal
lifecycle. Serial builds and both focused integration modes passed under the
zero-swap envelope. Independent review found no critical/high defect; its one
low layout-coverage finding was corrected in that existing contract.

One strict Silent run kept the exact material MRT0 selector, 8,090 threshold,
zero-based occurrence `m=1`, and the two atomic taps at presents 8,000 and
8,080. The only diagnostic change was coverage observation. After both taps
started, it emitted:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
on=129111 onf=0 ofin=1
r=0.452881:1.33496 g=0.133789:0.381104
b=0.0769043:0.191284 a=1:1 m=1

ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
cfin=1 x=427.5:1479.5 y=618.5:1079.5 m=1
```

The paired fixed-test query reported `any_passed=1 m=1`. Both inputs delivered
without cancellation, the ring had no drops, `last-error` was null, and the
bounded service used a rounded 1.7 GiB peak with swap disabled. The immediate
capture at present 8,212 showed the title logo and PLAY prompt but no vehicle;
it scored `low_entropy` (`entropy=0.2047`, 149 bins). The occurrence is
therefore disproven as a PLAY/gameplay scene fence. Separately, its 129,111
finite nonblack export invocations span a wide host region and reach the
framebuffer's lower edge. That excludes zero/tiny raster coverage and total
fixed-test rejection for this draw, while leaving primitive correctness
unproven. The vehicle contribution is lost after the unchanged MRT export:
blend/write mask, color attachment identity/view/layout, obsolete clear,
depth/HTILE identity, later read/write lifetime, or composition remain live
causes. An incorrect depth attachment or stale clear/HTILE state can explain
geometry becoming visible when depth is disabled, but this run does not yet
distinguish that mechanism from a downstream color-attachment overwrite.
Do not add more material shader/sample probes or claim a visual 3D advance.

## Post-blend attachment readback and unstable occurrence exclusion

The final-MRT diagnostic now has an optional host-only attachment observation,
enabled by `KYTY_PS_MRT_ATTACHMENT_PROBE=1`. It does not enter the shader
identity or modify guest color. Reservation accepts only the active MRT,
single-sample images carrying transfer-source usage, four explicitly supported
packed formats, a nonzero extent, and at most 64 MiB. After the render pass,
the owning graphics command buffer transitions the tracked image to transfer
source, copies it into one coherent host buffer, restores the exact prior
layout, and publishes transfer-write to host-read visibility. The image pointer
is cleared before submission completes; mapping and aggregation occur only
after that exact fence. The buffer uses a diagnostic-local Vulkan allocation
path which returns `skip_buffer` on create, memory-type, allocation, or bind
failure instead of entering the renderer's fatal general allocation path.

The existing graphics integration was extended rather than adding a new test
suite. It covers strict opt-in parsing, shader-identity separation, format-aware
raw RGB occupancy, coverage-box clamping, deterministic FNV-1a hashing, skip
status serialization, and the protocol's maximum message size. The focused
contract and match-ordinal integrations and serial `fc_script`/agent build
exited zero under zero-swap cgroups. Independent follow-up review found no
remaining leak, double-free, fatal-allocation, event-order, or message-bound
defect.

One strict Silent run then changed only that attachment observation while
retaining the 15,366-index material selector, MRT0 ordinal 229, minimum present
8,090, `m=1`, and the two atomic input targets at 8,000 and 8,080. The exact
fence completed with:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229 on=0 onf=0 ofin=0 m=1
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229 cfin=0 m=1
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
f=b10g11r11 e=1920x1080 nz=0 b=0 in=0 h=97d30483e5dd6325 ok=1 m=1
cs=0a0005c0ef41d630 k=i n=15366 s=2 any_passed=0 m=1
```

Both taps delivered, none was cancelled, the event ring had no drops,
`last-error` was null, and the service peaked at a rounded 1.7 GiB with cgroup
swap disabled. The immediate native capture at present 8,156 was the main menu
and scored `hot_corruption` (`entropy=1.7828`, 96 bins); it is not evidence for
the failing 3D checkpoint. The zero packed attachment is also not a post-blend
finding: this process selected a depth-rejected occurrence with no final-MRT
invocation, unlike the earlier `on=129111`, `any_passed=1` occurrence. This
closes minimum-present plus `m=1` as a reproducible occurrence identity. Do not
repeat it with another guessed ordinal. Before interpreting an attachment
copy, the diagnostic must boundedly reject/re-arm an empty match or identify
the contributing draw through stable attachment/depth state, then require
`on>0` and a passing fixed-test query in the same fence window.

That bounded re-arm is now implemented for the attachment opt-in. An empty
fenced FinalMrtResult attempt destroys its staging buffer after completion,
returns the process-one-shot lifecycle to idle without re-consuming the
configured match ordinal, and suppresses all raw, coverage, attachment, and
fixed-test events. At most four such attempts are discarded; a fifth attempt
is terminal. The retry count survives graphics-context teardown together with
the lifecycle's matching-occurrence count and is emitted as the single-digit
`r=N` field. The maximum-width attachment event remains within the 192-byte
wire limit. The same focused integrations passed after a RED compile on the
missing re-arm transition, and independent review found no remaining state,
buffer-lifetime, shutdown, counter-selection, event-order, or bound defect.

One further strict Silent run retained the same selector and two scheduled
input targets. After four empty matches, the terminal exact fence emitted:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
f=b10g11r11 e=1920x1080 nz=82791 b=1 in=55937
h=aa7b0eb8702d44bb ok=1 m=1 r=4
```

`b=1` is important: the coverage bounds came from this same FinalMrtResult
attempt, so the terminal selection was not another no-invocation occurrence.
The immediate attachment was also not uniformly RGB-zero, including inside
that coverage rectangle. This still does not establish that the selected draw
caused those nonzero pixels because only the post-draw image was copied; prior
writers can populate the same rectangle. The agent recovered this event, then
the cgroup's unchanged 120-second runtime cap stopped the process before a
native capture, event-history dump, or final input/status query. The journal
reported a rounded 1.8 GiB peak with zero swap and no crash report was written.
Treat input delivery and visual scene state for this attempt as unverified.

The next single-variable boundary is therefore a bounded pre/post attachment
delta on that contributing attempt, or exact identity tracking to the first
later clear/overwrite/consumer. Do not infer a correct blend/write path from
post-only occupancy, and do not change shader arithmetic, depth compare,
sample decode, or attachment formats from this result.

## Pre/post attachment delta closes the immediate-write boundary

The attachment opt-in now records a second, pre-pass snapshot only when the
selected render-pass color load-op is `LOAD` and both initial and tracked image
layouts are defined. `CLEAR`/`DONT_CARE` report `skip_load`; undefined state
reports `skip_layout`. The existing post snapshot remains available in those
cases. Each side is capped at 32 MiB, keeping total diagnostic staging at the
previous 64 MiB ceiling. Both transfers use the owning graphics command buffer,
restore the tracked layout, and are mapped only after its exact fence. Empty
retries and teardown destroy both allocations together.

The CPU aggregate compares packed RGB only and reports total changed pixels,
changed pixels inside the same attempt's fragment-coverage bounds, and the
zero-to-nonzero/nonzero-to-zero directions. Alpha-only changes are deliberately
excluded. While reviewing the seam, a separate synchronization defect was
found and corrected: restoration to `COLOR_ATTACHMENT_OPTIMAL` previously made
only color writes available. The destination access is now color attachment
read plus write, covering the implicit `LOAD` read. A tracked layout different
from the framebuffer initial layout is valid: the existing `BeginRenderPass`
path explicitly transitions to the declared initial layout and updates the
tracker before `vkCmdBeginRenderPass`.

The existing graphics integration gained the delta aggregate and maximum-width
event contract; no new test suite was added. The focused contract and
match-ordinal modes and the serial emulator/agent build exited zero under
zero-swap cgroups. Independent review of both staging buffers, fence ownership,
retry/teardown, layout transitions, and event provenance returned PASS.

The first strict Silent use delivered exactly two scheduled taps at presents
8,000 and 8,080. The selected occurrence needed no empty retry and completed
the same fence with:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
on=120494 onf=0 ofin=1 r=0.452881:1.43262 g=0.133789:0.408447
b=0.0769043:0.204102 a=1:1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
cfin=1 x=537.5:929.5 y=720.5:1079.5 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
f=b10g11r11 e=1920x1080 nz=122244 b=1 in=96923
h=e728d902f16790b5 ok=1 m=1 r=0
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
l=0 d=94978 b=1 in=94978 up=94804 dn=0 m=1 r=0
cs=0a0005c0ef41d630 k=i n=15366 s=2 applicable=1
depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=1 m=1
```

`l=0` is `LOAD`. The selected draw therefore changed 94,978 RGB pixels inside
its own finite coverage, including 94,804 RGB-zero to nonzero transitions and
zero nonzero-to-zero transitions. The post image held 122,244 nonzero pixels,
96,923 within the same coverage box, and the fixed-test query passed. This
causally excludes total depth rejection, an obsolete render-pass clear before
this draw, a uniformly black immediate target, and total color-write suppression
for this occurrence. It does not prove correct vertices, colors, or later image
lifetime.

The immediate native capture at present 8,484 still showed the logo/PLAY prompt
without the vehicle or world and scored `low_entropy` (entropy 0.2083, 171
bins). Both taps delivered, none was cancelled, `last-error` was null, the ring
had no drops, and the service was stopped deliberately. Its cgroup peaked at
1,937.6 MiB with zero swap. There is no compatibility or visual-3D advance.
The next one-variable boundary is the first later mutation or consumer of the
exact selected image before present: clear, attachment overwrite, guest alias
reuse/unmap, resolve/blit, or compositor sample. Do not reopen immediate depth,
MRT export, or blend/write-mask hypotheses without contradictory evidence.

## Lifetime selector and cross-process address instability

`KYTY_TRACE_RT_LIFETIME_COLOR_ADDR` now accepts one strict nonzero decimal or
`0x` guest address. Invalid input disables the trace. Only matching color
attachments may arm, while later lifetime matching remains guest-address **or**
host-allocation identity so rematerialization stays observable. `WRITE` records
also include `guest_count`, which distinguishes draws sharing a pixel shader.
With a color selector active, the trace returns before its depth path. This was
required for bounded runtime behavior: the first combined attempt emitted 128
unrelated depth events in addition to its color records and fell to about 8
FPS despite the nominal event limit. The focused integration and serial build
passed after the selector/provenance change and after the color-only correction,
with cgroup swap disabled.

The first address-filtered run then exposed a second trace-only defect:
descriptor-sample auto-promotion could populate an empty primary slot without
checking the color selector, so the log contained unrelated addresses. The
promotion predicate now applies the same address filter before mutating primary
lifetime state. The serial build and existing contract integration passed after
that correction. Do not interpret the misattributed pre-correction sample lines
as evidence about the requested address.

The unfiltered combined attempt recovered the terminal empty MRT result as:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
ga=00004f250000 l=0 d=0 b=0 in=0 up=0 dn=0 m=1 r=4
```

The same trace armed `0x4f250000` as a 1920x1080 format-122 target. Its first
writer used `load_op=1` (`CLEAR`), subsequent writers used `LOAD`, and event 122
sampled it with pixel shader `210001e03375e575` into a 960x540 format-122
target. Because the selected probe occurrence had no fragment invocation and
`any_passed=0`, this sequence cannot attribute the positive draw's loss. It is
only evidence for the general HDR producer-to-downsample route.

A second process used the address selector learned above. Its active full-size
HDR image instead appeared at `0x51b50000`, with a different host identity; the
15,366-index draw did not occur before the trace limit, while other material
draws and the same downsample path did. Both scheduled taps delivered, the
runtime remained interactive, the service was stopped deliberately, memory
peaked below 2.0 GiB, and swap remained zero. No capture or 3D advance was
claimed.

Guest RT addresses therefore vary between processes and are not a stable
cross-run fence. Do not repeat a lifetime run filtered by a prior process's
address. The next minimal change is same-process dynamic arming: when the exact
FinalMrtResult reservation selects its `RenderColorInfo` attachment, publish
that guest/host identity directly into the bounded lifetime state. Then the
`guest_count=15366` `WRITE` and its first later `WRITE`, `SAMPLE`, or `RESOLVE`
can be causally ordered without a guessed address or broad logging.

## Same-process MRT-probe lifetime arming

`KYTY_TRACE_RT_LIFETIME_MRT_PROBE=1` now makes the exact FinalMrtResult
reservation publish its selected color attachment directly into lifetime state.
The hook runs after the probe reservation succeeds and before descriptor binding
and `TraceRenderTargetLifetimeDraw`, so the selected draw is the first `WRITE`
after `PROBE_ARM`. Generic color arming, depth tracing, and descriptor-sample
auto-promotion are disabled in this mode. If a retry selects another guest/host
image, the trace clears derived identities and emits `PROBE_REMAP`. Combining
dynamic mode with a static color or depth selector disables the trace rather
than producing partial evidence.

The serial build and existing contract integration passed under zero-swap
cgroups after implementation and after independent review's mixed-filter
finding was corrected. The runtime used the same two scheduled taps and a
128-event color limit. It selected five empty occurrences; the terminal exact
fence reported:

```text
ps=210005b0766a27a5 k=i n=15366 s=2 mrt=0 ord=229
ga=00004e790000 l=0 d=0 b=0 in=0 up=0 dn=0 m=1 r=4
cs=0a0005c0ef41d630 k=i n=15366 s=2 any_passed=0 m=1
```

Despite that empty result, the lifetime ordering itself was exact:

```text
event=0 kind=PROBE_ARM guest_addr=0x00004e790000 host_id=726 extent=1920x1080 format=122
event=1 kind=WRITE indexed=1 guest_count=15366 ps=0x210005b0766a27a5 load_op=0 depth=1:1
event=2 kind=WRITE indexed=1 guest_count=10710 ps=0x210005b0766a27a5 load_op=0 depth=1:1
...
event=107 kind=SAMPLE ps=0x210001e03375e575 guest_addr=0x00004e790000 dst_extent=960x540
event=110 kind=SAMPLE ps=0x210006800bf364a9 guest_addr=0x00004e790000 dst_extent=1920x1080
event=112 kind=PROBE_REMAP guest_addr=0x00004f050000 host_id=2015 extent=1920x1080 format=122
event=113 kind=WRITE indexed=1 guest_count=15366 ps=0x210005b0766a27a5 load_op=0 depth=1:1
event=120 kind=SAMPLE ps=0x210001e03375e575 guest_addr=0x00004f050000 dst_extent=960x540
```

No post-probe `CLEAR` occurred; later render-pass writers all used `LOAD`. The
HDR target is sampled by the known downsample and compositor chain, and it
ping-pongs across guest/host identities between frames. This closes an obsolete
later clear as the demonstrated ordering for the empty occurrence and validates
same-process identity/remap tracking. It does not establish the loss point for
the earlier positive draw: this run had no surviving fragments, and removing or
changing the later guest draws would be invented behavior. Repeat this exact
bounded dynamic trace only when the same fence also has `d>0`, `in>0`, and
`any_passed=1`; then attribute the first later significant write and the
downsample output from that contributing occurrence.

## Exact depth-clear history closes the repeated clear branch

The MRT-probe trace now preserves the last clear for up to 64 exact
`{depth_buffer_vaddr, depth host unique_id}` identities from process start,
even when `KYTY_TRACE_RT_LIFETIME_MIN_PRESENT` starts later. A separate
256-entry ring still retains recent possible depth writers and emits only the
newest eight matching records. The clear tracker is scalar-only,
mutex-protected, and bounded. It observes normal indexed/auto render passes,
the clear-only depth-copy pass, and the direct depth-copy draw pass, including
a first-use `loadOp=CLEAR` with no explicit clear flag. `path=0` denotes a
normal pass and `path=1` a direct depth-copy pass. Matching requires both guest
depth address and host allocation identity; no address-only attribution is
accepted.

The serial `fc_script` build, existing `--vertex-clip-probe-contract-only`
integration, and `git diff --check` pass under zero-swap cgroups. Independent
review verified all render-pass seams and the fixed-table update/eviction
mechanics. It also recorded one remaining diagnostic-test limitation: the
64-identity eviction and direct-copy hooks have no isolated automated
regression. Per the integration-first bring-up policy, the retained strict run
below exercises the exact pre-threshold identity and one emitted match; it does
not claim exhaustive container coverage.

An initial attempt used the malformed selector `:0:229` instead of the required
`:mrt0@229`; it reached the main menu and delivered both inputs but emitted no
probe, so it is configuration evidence only and must not be interpreted. The
single corrected warm retry used strict Silent mode, scheduled exactly two
`cross` taps at presents 8,000 and 8,080, and selected five attempts at presents
8,109 through 8,113. Every attempt reported the same state:

```text
z_read=z_write=0x4f7d0000 depth_host_id=723
st_read=st_write=0x50090000 htile=0x36488000 size=196608
rp_load=LOAD initial=ATTACHMENT final=ATTACHMENT host_layout=ATTACHMENT
test=1 write=1 compare=GEQUAL clear=0 suppress=0 compression=1:1:1
```

For every attempt the same exact last clear was recovered before the trace
threshold:

```text
kind=PROBE_DEPTH_LAST_CLEAR present=8009 submit=8040 before_min=1 path=0
indexed=0 guest_count=3 ps=21000a706f734464
load=CLEAR initial=UNDEFINED host_layout=UNDEFINED
clear=1:0 write=0 compare=ALWAYS suppress=0
htile=1:0x36488000 compressed=0
```

No later normal or direct-copy clear replaced that record. The recent writer
window instead contained defined-layout `LOAD` passes with the same exact
identity, `GEQUAL`, depth writes, and HTILE compression. This is consistent
with the earlier temporary HTILE trace: creation/metadata clear was consumed,
and no later guest clear was established. It excludes a hidden later clear or
split read/write/host/HTILE identity for these empty attempts. It does **not**
make the present-8,009 clear obsolete: prior `GEQUAL` draws may legitimately
occlude the selected geometry, and the earlier contributing 15,366-index
occurrence passed depth and changed 94,978 attachment pixels under the same
general sequence. Do not force a per-frame clear, disable depth, or weaken
reverse-Z. Close this depth branch and return to the downstream color lifetime
of a contributing occurrence or to another independently captured renderer
contract.

The corrected run reached present 8,497, delivered both taps without
cancellation, peaked at 1,911,508,992 bytes with zero cgroup swap, and was
stopped deliberately. Its one native capture still showed the logo/PLAY screen
and scored `low_entropy` (entropy 0.2084, 171 bins). There is no visual or
playability advance.

## Sparse full-resolution compositor sample

The existing sample-result aggregate was unsafe for a full-screen consumer:
each invocation performs multiple contended atomics, and a normal 1920x1080
draw could create enough diagnostic pressure to stall the host. The sample
probe therefore accepts the optional strict boolean
`KYTY_PS_SAMPLE_PROBE_SPARSE=1`. Only the sample-result diagnostic uses it. The
selected SPIR-V executes `OpGroupNonUniformElect` at subgroup scope and wraps
the unchanged aggregate in structured control flow, recording one active host
lane per subgroup. The ordinary probe source is unchanged when the variable is
absent or zero, and malformed values reject the diagnostic.

Sparse state participates in shader/module identity. Reservation also checks
that the physical device exposes basic subgroup operations to fragment shaders
before consuming the process one-shot or allocating diagnostic resources. An
unsupported host returns to the ordinary draw through the existing failed-
reservation cleanup. The existing graphics integration covers the fail-closed
parser, separate identity, valid sparse SPIR-V, unsupported/supported Vulkan
reservation, exact command-buffer fence completion, and the emitted
`ps_sample_probe ... sparse=1` provenance. Both focused integration modes and
the serial `fc_script` build passed under the zero-swap envelope; independent
review returned PASS.

One strict warm run then selected absolute sample ordinal 15 of the active
six-index full-resolution compositor, with the exact two present-addressed
inputs and a minimum present of 8,090. The event completed after both inputs:

```text
ps=210006800bf364a9 k=i n=6 s=2 ord=15
sn=66561 snf=0 sfin=1
r=0:0.215088 g=0:0.215088 b=0:0.0562439 a=1:1 sparse=1
```

Both inputs delivered, none was cancelled, the ring had no drops,
`last-error` was null, memory peaked at 2,021,982,208 bytes with zero swap, and
the service was stopped deliberately. The one native capture at present 8,183
showed the logo/PLAY screen rather than gameplay and scored `low_entropy`
(`entropy=0.2046`, 150 color bins). This proves that the first HDR sample is
finite and not globally black for that exact compositor occurrence. It does
not prove the gameplay compositor, its final MRT value, attachment retention,
or a visual 3D advance. Do not change sample decode, coordinates, interpolation,
or HDR attachment identity from this result, and do not repeat this route only
to wait longer. The next useful compositor discriminator must be equally
bounded and observe the final output or its post-blend attachment during a
reproducibly identified failing 3D occurrence.

## Significant MRT occurrence and exact downstream identity

The attachment probe previously re-armed only when the selected MRT export had
zero invocations. That admitted a six-invocation sliver and stopped before the
large material contribution needed for lifetime attribution. The host-only
selector `KYTY_PS_MRT_ATTACHMENT_MIN_INVOCATIONS=N` now accepts one strict
positive decimal only together with `KYTY_PS_MRT_ATTACHMENT_PROBE=1`. Its
default is one, preserving the prior empty-only behavior. Results below the
threshold destroy both fenced readback buffers and re-arm the exact draw up to
the same eight-retry bound. The threshold does not participate in SPIR-V or
pipeline identity and ordinary draws are unchanged.

The existing graphics integration first failed on the missing field and retry
decision, then passed after the parser, host propagation, and bounded fence
policy were implemented. The serial `fc_script` build and focused contract
mode passed under zero-swap cgroups.

One strict Silent run requested at least 10,000 final-MRT invocations while
retaining the two scheduled input edges. It selected its first eligible
occurrence and reported 17,443 finite MRT invocations. The same exact fence
changed 17,008 packed-RGB pixels inside the coverage bounds, all from zero to
nonzero, and the fixed-test query passed. Both inputs delivered, no event was
dropped, `last-error` was null, and the service was stopped deliberately after
about one minute with a rounded 2-GiB peak and zero cgroup swap.

Same-process lifetime tracking armed the exact 1920x1080 format-122 HDR image
at that draw. The following 118 tracked mutations all retained `LOAD`, the
same guest/host image identity, and a defined attachment layout. Event 120 then
bound that exact image as `rt-exact` into the known 960x540 downsample; the
derived chain remained exact through the retained event limit. No later clear,
guest-upload substitution, rematerialized host image, or resolve was observed
before that consumer. The 128-event cap ended before a full-resolution
compositor event, so compositor identity for this particular occurrence is not
claimed.

The later native capture had already advanced to a credits screen and scored
`low_entropy`; it is not a same-scene visual result or a 3D advance. The live
boundary is now the effect of the intervening `LOAD` writers or the sampled
contents/output of the first downsample. Do not repeat the identity trace or
remove guest draws. A next diagnostic must compare one bounded later writer or
the downsample input/output without copying the full HDR attachment for every
draw.

## First post-material writer input and blocked clip observation

The first writer after the significant material occurrence is not anonymous:
it uses pixel shader `2100099068cc5c23`, vertex shader
`0a0005c092436153`, indexed triangle-list draws, and `GEQUAL` depth writes.
A strict Silent trace at present 8,090 first selected an earlier 108-index
occurrence. A second run used the existing 32-match cap and recorded the
252-index occurrences at ordinals 26 through 29 without changing renderer
semantics.

Those 252-index draws declare 250 vertex records and every sampled index stays
inside that range. The decoded layout is valid: stride 48, float3 position at
offset 0, float3 normal at offset 12, and half2 UV at offset 40. Samples from
the head, quarters, and tail are finite, as are the normal-transform/object
coefficients printed for each occurrence. All three textures are bound, the
cube BC texture reports defined sampled blocks, the depth texture retains
`depth-exact` provenance, and DCC/CMASK remain disabled. This excludes a basic
index-range, vertex-layout, missing-binding, or nonfinite input failure for
these exact draws; it does not establish their post-transform clip positions
or their color overlap with the earlier material contribution.

One opportunistic `VS_SLOT_OFF272` peek contained a NaN. That helper is
explicitly a fixed-offset skybox diagnostic and is emitted for any sufficiently
large resource; there is no evidence that this shader consumes that offset.
Do not treat the peek as a bad guest constant or patch it. The coefficients
already identified as the 512-byte object block are finite.

The existing exact vertex probe was then attempted with
`KYTY_VS_CLIP_PROBE=0a0005c092436153`, `indexed:252`, and minimum present
8,090. Its 70-second and one final 100-second bounded runs never reached the
requested present before their watchdogs stopped them, so neither produced a
`vs_clip_probe` result. Both stayed below 2.21 GiB with zero cgroup swap and
left no Kyty process. This is an inconclusive diagnostic-path limit, not a
vertex result. Do not repeat or lengthen it until the pre-threshold probe cost
or reservation behavior is understood. The live visual seam remains a bounded
writer attachment delta or the first downsample input-to-output result.

The bounded attachment-delta branch is now decisive for this writer. A normal
strict ShaderProbe run reached present 8,092 quickly and established that the
PS has one final compressed MRT0 export at absolute instruction ordinal 402.
The existing output-preserving MRT probe then selected `indexed:252` from
present 8,090 with attachment readback enabled. Its first occurrence and all
eight allowed retries reported zero final-MRT invocations and no finite
coverage. The last exact fence reported:

```text
ps=2100099068cc5c23 k=i n=252 s=2 mrt=0 ord=402 on=0 onf=0 ofin=0
ps=2100099068cc5c23 k=i n=252 s=2 mrt=0 ord=402 cfin=0
ps=2100099068cc5c23 k=i n=252 s=2 mrt=0 ord=402 ga=00004e610000 l=0 d=0 b=0 in=0 up=0 dn=0 m=0 r=8
cs=0a0005c092436153 k=i n=252 s=2 applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0
```

The attachment itself remained valid and contained 319 nonzero pixels, but
the selected writer changed none of them. Both scheduled inputs delivered,
the event ring dropped nothing, `last-error` was null, peak memory was about
1.81 GiB, cgroup swap stayed zero, and the service was stopped immediately.
These exact 252-index occurrences therefore cannot erase, stretch, or recolor
the earlier material contribution: depth rejects them before color. Do not
reopen their vertex transform or the stale `OFF272` peek. In the retained
lifetime order, the next unclassified writer is PS `210001009057ad42` with
`indexed:2112`; test its actual attachment delta before moving farther down the
chain.

That next writer is also classified. A bounded ShaderProbe showed a small
alpha-tested texture shader with its only enabled MRT0 export at absolute
ordinal 18. A first MRT run at present 8,090 found 5,422 finite output values
but a black attachment, no passed depth samples, and no color delta; because
that preceded the significant material phase, it was not used to infer later
occlusion. The same run was repeated with only the minimum present changed to
8,092. It selected at present 8,093 after the retained 252-index depth writers
and reported:

```text
ps=210001009057ad42 k=i n=2112 s=2 mrt=0 ord=18 on=858 onf=0 ofin=1 r=0.129395:0.365479 g=0.595215:0.806641 b=0.724609:0.902832 a=1:1
ps=210001009057ad42 k=i n=2112 s=2 mrt=0 ord=18 cfin=1 x=794.5:820.5 y=587.5:604.5
ps=210001009057ad42 k=i n=2112 s=2 mrt=0 ord=18 f=b10g11r11 e=1920x1080 nz=40995 b=1 in=0 ok=1 m=0 r=0
ps=210001009057ad42 k=i n=2112 s=2 mrt=0 ord=18 l=0 d=0 b=1 in=0 up=0 dn=0 m=0 r=0
cs=0a0003d02e96de06 k=i n=2112 s=2 applicable=1 depth=1 stencil=0 bounds=0 ready=1 precise=0 any_passed=0
```

The attachment was therefore already populated, the shader produced finite
candidate colors and finite coverage, but fixed depth rejected every sample
and the exact before/after image was unchanged. Both scheduled inputs
delivered, no event was dropped, `last-error` was null, peak memory was about
1.76 GiB, cgroup swap stayed zero, and no Kyty process remained. This excludes
the 2,112-index writer as the source of the progressive color smear in the
post-material phase. Continue in retained order with the single 1,536-index PS
`21000220c3cdade2`; do not disable depth or reinterpret finite candidate output
as a color write.

The 1,536-index writer is excluded as well. Its targeted shader dump completed
before a 60-second normal-run watchdog and identified one enabled MRT0 export
at ordinal 43; the process did not reach the requested present, so that dump
run carries no scene result. The exact MRT run at minimum present 8,092 did
reach the target and exhausted the first occurrence plus all eight bounded
retries. Every occurrence reported zero MRT invocations and no coverage. The
final attachment still contained 95,457 nonzero pixels, but the exact delta was
zero and fixed depth reported `any_passed=0`. Both inputs delivered, the ring
dropped nothing, `last-error` was null, peak memory was about 1.76 GiB, swap
was zero, and Kyty was stopped immediately. This procedural writer cannot
modify the retained material in the selected phase.

## Exclusion of posterior writer PS 21000870488b957d and closure of writer census

The repeated `indexed:2418` draws using PS `21000870488b957d` were investigated.
Targeted ShaderProbe identified its only enabled MRT0 export at ordinal 363. An exact
MRT run tested both match 0 (`load_op=1` / `CLEAR`) and match 1 (`LOAD`). Across all 9
bounded attempts, the shader reported zero final-MRT invocations (`on=0`), zero finite
coverage (`cfin=0`), fixed depth rejection (`any_passed=0`), and zero changed pixels
in the attachment (`d=0`).

This completes and closes the full census of posterior writers following the significant
material contribution (PS `210005b0766a27a5`, VS `0a0005c0ef41d630`, `indexed:15366`).
None of the posterior writers alters, overwrites, or erases the material pixels before
the downsample and compositor pipeline. The investigation now advances upstream to
the vertex shader `0a0005c0ef41d630`, auditing its attribute decoding, coordinate
transformation, transcendental math, and parameter interpolants for vertex corruption
and exploding geometry.

## Paired significant VS/MRT observation is menu-local

The existing vertex aggregate was paired with the exact significant MRT selector
instead of adding another probe. The strict Silent run selected VS
`0a0005c0ef41d630`, PS `210005b0766a27a5`, `indexed:15366`, MRT0 ordinal 229,
match 1, and at least 10,000 MRT invocations. The completed fence reported 9,151
finite VS invocations and no NaN/Inf in either clip position or `PARAM0.xy`:

```text
cs=0a0005c0ef41d630 k=i n=15366 s=2 inv=9151 nf=0 fin=1
w=4.25562:527.361 x=-37.6861:7.62985 y=-33.4926:22.2599
z=1.77703e-05:0.00234865

cs=0a0005c0ef41d630 k=i n=15366 s=2 p0n=9151 p0nf=0 p0fin=1
p0x=-0.088562:0.973145 p0y=0.788574:0.845215

cs=0a0005c0ef41d630 k=i n=15366 s=2
wnp=6177 oxy=1731 oz01=0 in01=1243 ozn=0 inn=1243
```

The paired PS produced 92,332 finite MRT candidates, passed fixed depth, and
changed 86,376 pixels from zero to nonzero. Both scheduled taps at presents
8,000 and 8,080 started, the event ring dropped nothing, `last-error` was null,
the service peaked at 1.9 GiB with zero cgroup swap, and Kyty was stopped
deliberately after the result and one capture.

Visual inspection is the controlling qualification: the capture at present
8,223 is the main menu, not the damaged 3D scene. Its automated
`hot_corruption` score is a false positive caused by the menu's yellow, white,
and checkerboard design. The wide post-divide range and 6,177 nonpositive-W
vertices are therefore occurrence-local menu data; they do not prove an
incorrect transform, NaN/Inf propagation, or exploding gameplay geometry. Do
not clamp position, change matrix loads, or alter clipping from this result.
The probe path itself is now proven usable. The next prerequisite is a
same-scene selector or capture correlation for an actually damaged 3D frame;
only then compare its clip/PARAM population with this menu-local observation.

## Index-buffer/render-target alias advance and gameplay correlation

A later strict run exposed a previously unclassified create order before the
selected scene completed: an existing `IndexBuffer` was wholly contained by a
new 64-KiB `RenderTexture`. The inverse create order and the corresponding
Texture alias were already linkable, but the render-target helper rejected this
single observed direction. A focused graphics integration derives the relation
from real ranges, creates the index view before the covering target, and first
failed with the same structural `DATAERR`. The policy now preserves both typed views only for
`IndexBuffer IsContainedWithin RenderTexture`; partial and exact forms remain
strict until captured. It verifies that both typed objects remain live and that
the queried topology contains the derived index and target members. The integration and the existing vertex-probe contract
then passed, and `fc_script` built with two jobs under a 1.6-GiB build cgroup
(1.3-GiB peak, zero swap).

The corrected strict route passed the former `DATAERR`, delivered exactly the
scheduled taps at presents 8,000 and 8,080, and reached present 8,150. The
paired 15,366-index probe still produced finite clip/PARAM output and a real
MRT delta. A later cold-cache run reached gameplay and captured a damaged frame
at present 8,932: HUD and isolated scene elements were coherent, while most of
the world remained black and bright 3D surfaces were disconnected. Both runs
peaked at about 2 GiB with cgroup swap disabled and ended at their watchdogs;
no Kyty process remained.

The alias is not the direct source of the retained 41,910-index geometry. The
unclassified render target covers guest range
`0x478d0000..0x478e0000`, while the traced draw reads its 83,820 index bytes
from `0x47a00030` and its stride-24 vertices from `0x47900020`. Neither stream
overlaps that render target. Keep the link correction as a real liveness and
lifetime advance, but do not add unconditional image-to-index materialization
or cite it as the 3D fix without a later trace proving a GPU-authored index
range.

The official RDNA2 ISA guide confirms that `EXEC` applies to export
instructions and that a vertex shader must export position with the final
position export marked done. This keeps export predication as a valid general
audit seam, but the exact 41,910-index result contained no NaN/Inf and does not
prove narrowed `EXEC` at its exports. Clean-room renderer comparison likewise
supports range authority, bounded containment, barriers, and deferred lifetime;
it does not supply a PS5 tiled image-to-index conversion for a contained range.

The cold-cache run selected the first 41,910-index occurrence after present
8,000. All 27,937 invocations were finite, but `wnp=25677`, `oxy=2260`, both Z
populations were zero, and fixed depth saw `any_passed=0`. That occurrence
completed before the second tap and therefore cannot be treated as the producer
of the later gameplay frame. A follow-up changed only the probe floor to present
8,500; it reached that present and gameplay but emitted no later 41,910 result
before the 115-second watchdog (2-GiB peak, zero swap). This closes checksum plus
count as a unique gameplay fence. Do not repeat or lengthen it. The next bounded
step is a gameplay-phase draw trace/correlation, not a transform clamp, depth
override, or speculative GPU-to-index copy.

## Gameplay-phase census exposes a bounded NaN vertex stream

A one-shot unique-PS census was enabled only after present 8,800 on the exact
two-tap route. It recorded 27 shader identities before the runtime slowed to
0.672 FPS at present 8,813 in the `loading` phase. Both scheduled taps had been
delivered, `last-error` was null, the event ring had no drops, peak cgroup memory
was 2,160,541,696 bytes, and cgroup swap remained zero. The process was stopped
deliberately without a capture. This run is not same-scene gameplay evidence;
the 32-entry bind trace is too intrusive for visual correlation and must not be
repeated as a census.

The census nevertheless found one exact, independently auditable anomaly. PS
`210001201ffb2924` with VS `0a000380e652b3a7` issued 234 uint16 indices over 80
declared and required records. Sampled indices 39, 58, 77, 78, and 79 all read
`NaN,NaN,NaN` from semantic-zero position storage. The maximum scanned index was
79, the position stream was a valid stride-12 Gen5 format-74 RGB32F binding, and
the second stride-zero format-56 binding occupied a different semantic. The
trace decoder copied the three dwords directly from the CPU-visible guest span;
there was no format conversion or Vulkan fetch in that observation. Two
persistent translator-33 modules for the VS also show a direct location-zero
load followed by `FMul`/`Fma`/`FAdd` into the position export without an NaN
test. Therefore the currently bound bytes contain non-finite position sentinels
and the translated arithmetic can propagate them. This does not prove whether
the guest authored those values, whether an earlier owner failed to refresh the
span, or whether this small blended draw contributes to the damaged gameplay
frame.

The later bounded trace captured the exact fused-stage state:
`float_mode=192`, `dx10_clamp=1`, and `ieee_mode=0`. It also reported no live
`GpuMemory` overlap for the 960-byte position range; the separate stride-zero
binding did retain one read-only CPU-upload owner. This disproves a current
tracked writable storage/render-target alias for the selected position bind,
but not a historical freed producer or guest CPU authorship.

The ISA evidence also narrows `MODE.DX10_CLAMP` more than the earlier working
hypothesis. It controls the NaN result of an instruction's floating `CLAMP`
output modifier; it does not sanitize VALU sources or every floating result.
Kyty now propagates the effective VS/GS/PS mode into shader identity and applies
the modifier after the operation and OMOD: numeric results clamp to `[0,1]`, a
NaN becomes positive zero only in DX10 mode, a non-DX10 NaN is preserved, and
IEEE mode suppresses the output modifier. Translator version 34 invalidates
older cache entries. The focused integration assembles and validates DX10,
non-DX10, unclamped, and IEEE variants. The exact position chain contains no
instruction clamp, so the correction deliberately leaves its NaN propagation
unchanged and is not a 3D recovery claim.

A strict Release+Silent follow-up then selected only VS
`0a000380e652b3a7`, `indexed:234`, after the two established input edges. It
completed at 80 invocations with 74 non-finite clip-position exports and only
six finite outputs. Those six were inside both supported Z clip conventions;
the paired fixed-function query reported depth enabled and no passing sample.
The event ring had no drops, both taps were delivered, a native capture
completed, and the process was stopped deliberately before its watchdog. The
service peaked at 2 GiB with cgroup swap disabled. The capture remained the
low-entropy title-logo route, so it is not gameplay or visual-recovery evidence.
For the exact draw, however, the non-finite failure is now proven before depth,
clear, HTILE, rasterization, and fragment shading. Do not repeat the census,
disable depth, or add a global NaN clamp. Trace the guest-range producer/lifetime
or establish a general AMD-to-Vulkan non-finite clip rule before changing
rendering semantics.

The empty live-owner line is now understood as a limitation of the question,
not evidence that the bytes never had a Kyty writer. Eligible vertex and index
ranges are copied into command-buffer-owned transient snapshots before binding;
the current `GpuMemory` topology query cannot retain a producer that has already
published or been reclaimed. `KYTY_TRACE_GPU_WRITER_RANGE=addr:size` keeps a
fixed 128-event history only for overlapping effective DMA, immediate
`WriteData`, constant-RAM dumps, addressed occlusion `EVENT_WRITE`, and GPU
writeback operations. The relocatable value `auto` lazily
allocates a bounded 65,536-event ring, retains every covered event, and lets the
draw query its eventual guest VA. The large ring is not reserved when disabled
or in exact mode. The trace prints at most 16 latest matches plus totals per
recorder, retained/dropped counts, matching count, and truncation. It stores no
bytes, shaders, guest paths, or unbounded per-frame output, and its disabled path
exits after one atomic check.

The first exact-range runtime used a VA from the preceding process, while the
new process allocated the 960-byte span elsewhere; `covers=0` made that result
explicitly inconclusive. A second strict Release+Silent run changed only the
watch to `auto`. At the same selected draw it reported `all=1`, `covers=1`,
62,746 retained events, zero dropped events, and zero matching overlaps for the
actual range. Its totals were 43,493 normal DMA and 19,253 immediate
`WriteData`, with custom DMA and all writeback categories at zero. This run
predated history hooks for constant-RAM dumps and addressed occlusion
`EVENT_WRITE`, so those classes are not retroactively excluded. Both bounded
input taps were delivered, `last-error` remained null, and the emulator was
stopped immediately after evidence collection. The 1.9 GiB memory peak stayed
inside the cgroup and cgroup swap remained zero. This excludes the covered Kyty
writer classes for that run. Direct guest CPU writes and deferred EOP
`WriteData` publication remain explicit blind spots, so the result does not
establish comprehensive provenance or intentional sentinel ownership. The
recorder now distinguishes constant-RAM dumps and addressed occlusion
`EVENT_WRITE`; the process-isolated command-processor integration executes both
real paths and verifies history is published only after their host write and
flush.

Primary AMD guidance supplies the next semantic discriminator: on RDNA, setting
any final vertex position to NaN is a recommended primitive-culling mechanism.
The exact VS exports 74 NaN positions after the verified unclamped arithmetic,
while Vulkan's clipping contract does not explicitly impose that AMD behavior
on the Intel host. A correct fix must therefore operate on the assembled
primitive and only on final-position NaN. Per-vertex `CullDistance` cannot model
the rule because Vulkan discards only when the same cull distance is negative at
all primitive vertices; fragment `OpKill`, input sanitization, infinity handling,
buffer rewriting, and whole-draw skips are also non-equivalent. The relocatable
history had closed its then-covered producer branch. Primitive-aware post-shader
NaN culling was therefore the next discriminator at that point, while
uncovered writer paths remained a separate provenance limitation rather than a
reason to sanitize guest data.

The required host discriminator is now complete and closes that implementation
frontier on the current Intel Vulkan path. A process-isolated, color-only 8x8
integration disabled depth, stencil, and face culling, then issued identical
finite and quiet-NaN triangles under separate occlusion queries. The finite
control produced 18 samples; changing only vertex zero's final `Position.z` to
quiet NaN produced zero. This proves the host already suppresses the relevant
NaN primitive independently of depth. Do not insert a geometry shader on this
host or claim the selected logo-route draw explains the damaged gameplay. The
next bounded run must first capture the actually damaged scene and correlate its
producer. Treat final-position infinity and finite extreme/near-zero-W output as
new, distinct hypotheses rather than extending the NaN conclusion.

The menu route has also been re-established with presentation-scheduled input.
At 80-present intervals, three `cross` taps reach track selection, four reach
difficulty, five reach the vehicle-preview `PLAY` card, and six begin the next
loading phase. All six edges were delivered without cancellation. The bounded
strict run then stayed in `phase=loading` for the remainder of its 180-second
window at roughly 3.4--3.8 FPS; present advanced from about 10,285 to 10,721,
`last-error` remained null, and the capture stayed a black logo card. The sole
process peaked at 1.9 GiB with cgroup swap disabled and was stopped deliberately.
This is not gameplay or exploding-geometry evidence. The native performance
snapshot for the same six-edge route is now complete and must not be repeated.
Over a 119-second reset window, shader work was sparse (seven SPIR-V
source/compile operations) and slow frames did not align with pipeline misses.
Command processing consumed about 95.7 seconds, draw processing 57.2 seconds,
and draw-state setup 31.0 seconds. Individual slow frames contained thousands
of `GpuMemory::CreateObject` calls and as many as roughly 20,000 uploads totaling
about 72 MiB. Approximately 99% of transient read-only probes were accepted,
but each accepted probe still copied a command-buffer-owned snapshot; it was
not a content-cache hit. Fence waits and `WAIT_REG_MEM` were numerous but their
individual waits were bounded, so this is sustained repeated work rather than
a demonstrated deadlock. The next one-variable discriminator must identify the
dominant resource type/outcome or draw-state caller that produces those repeated
creates/uploads. Do not introduce a broad cache or reuse/overwrite an in-flight
transient allocation before that producer and lifetime are proven.

## Exact transient reuse exposes the large-vertex capacity churn

The command-buffer snapshot path now has a strict exact-content reuse seam. It
considers only the most recent used entry with identical guest address, size,
and Vulkan usage, revalidates the read-only overlap contract, and compares the
current guest bytes with the immutable mapped snapshot. The first implementation
held GPU-memory mutation locks but did not protect against CPU/HLE writers,
which use dirty notifications without those locks. It could therefore accept
torn bytes. That run observed 1.72 million reuses and only 0.15 seconds of
`memcmp`, but neither its performance attribution nor its visual capture is
accepted after the race was found.

The retained implementation acquires a temporary tracker reference for the
exact range. `BeginRead` records generation before and after arming page
protection and fails closed if they differ; capture/compare postvalidates the
same observation before committing or reusing a transient entry, then releases
the reference on every path. Focused tests inject both `NotifyWrite` and a fault
inside the arming window. The tracker-less test process falls back; the strict
runtime can establish first-use snapshots without first publishing a raw
`GpuMemory` object.

That run also identified the fallback producer that the earlier aggregate
snapshot could not name. Vertex views around 1.3--3.1 MiB exceeded the old
1-MiB per-snapshot ceiling and repeatedly entered `VertexBuffer` `reclaim_new`
with 24--33 overlap candidates. It recorded 2,847 such reclaims and about
6.2 GiB of vertex hashing. Changing only the per-entry ceiling to 4 MiB, without
increasing the 16-MiB command-buffer pool, eliminated vertex reclaims in one
warm-cache route and reduced vertex hashing to about 1 MiB. That run reached
present 8,000 in roughly 15 seconds and was back in `interactive` at present
8,500 near 52 FPS after six delivered inputs.

The native frame at that checkpoint reached car selection rather than the old
black loading card. Its UI was coherent, but the central vehicle was enlarged,
gray, torn, and geometrically deformed. Because this frame was produced before
the dirty-page transaction correction, unsafe exact reuse itself is an
unexcluded producer. Preserve the artifact only as historical symptom evidence;
it is not accepted same-scene evidence for the corrected renderer.

The ceiling change is not a complete capacity solution. A second bounded route
exhausted the unchanged pool and again produced 1,850 vertex reclaims and about
4.2 GiB of vertex hashing, despite slow-create sizes of only 1.67--2.46 MiB.
Do not increase the pool without a lifetime calculation. The next falsifiable
ownership change is vertex-only contained-range reuse: one immutable larger
snapshot may serve an unchanged contained view when `vkCmdBindVertexBuffers`
receives the exact byte offset. Storage descriptors, changed contents, mutable
overlaps, and non-contained ranges must remain on their current paths. Because
faster presentation changed which transitions accepted scheduled taps, future
visual correlation must drive one tap at a time and capture the resulting
screen rather than infer scene identity from `delivered_taps`.

## Stable snapshot route and immutable vertex-update frontier

Three bounded strict runs exercised the corrected snapshot transaction. The
first reached the press prompt, delivered one edge, and later captured only the
small title logo before its 110-second watchdog. The second used no input until
credits, then one edge reached the coherent main menu. Its 86.6-second
performance snapshot recorded 1,309,061 transient probes, 1,302,623 hits,
716,848 exact reuses, 5.75 seconds of validation, 0.096 seconds of comparison,
and 2.52 seconds of snapshot upload. Command processing still consumed 70.8
seconds and draw resource binding 14.3 seconds, so temporary tracker setup is
measurable but not the primary sustained cost. Both services stayed below about
2 GiB with cgroup swap disabled.

The third route captured every transition and stopped at the first useful 3D
checkpoint. It reached the press prompt with no input, then used one edge at a
time: title logo, main menu, and after the third delivered edge plus 200
presents, the vehicle-preview `PLAY` card. UI and logo were coherent but the
vehicle was completely absent. Peak observed memory was about 2.06 GiB with
zero cgroup swap, and the sole process was stopped deliberately. No corrected
run has reproduced the earlier torn vehicle, reached car selection, or reached
gameplay. The snapshot race fix is therefore real but insufficient for 3D.

The remaining `VertexBuffer` fallback was not eliminated: one measured route
still recorded 52 `reclaim_new` outcomes and about 54.7 MiB of vertex hashing.
Audit found a general source-ownership gap in that path. `GpuMemory::Update`
starts a dirty observation, then hashes guest RAM and its VB/IB/SB/Texture
callbacks read the guest pointer again. `CreateObject` also hashes and creates
before registering its first observation. Host/HLE writers notify before their
store and do not take the GPU-memory locks; thus the next update retains dirty
evidence, but the current upload can already contain mixed bytes. Postvalidation
after publication cannot repair it.

The current one-variable experiment addresses read-only vertex/index updates
and initial creates at or below the existing 4-MiB bound. When page-fault
tracking is available, it registers before the first-create copy, postvalidates
the observation, hashes and creates from the same immutable bytes, then
transfers the tracker reference and generation to the published object. Update
uses the same immutable-source rule; a raced observation keeps the previous GPU
backing for a later retry. Textures, storage, larger buffers, and hash-fallback
ranges remain unchanged. This is a bounded discriminator, not a claim that
general CPU-to-GPU publication is atomic.

The discriminator executed in a strict no-input `ROLLING START` scene. It
recorded 268/268 stable initial captures (about 538 MiB), zero initial
fallbacks, 81/81 stable updates, and zero update deferrals. The 1280x720 native
capture retained recognizable 3D perspective, road, building, vegetation, and
HUD, but large regions were black and bright colors remained dragged or
disconnected. The scorer reported `low_entropy` (`healthy=false`, entropy
2.4349). Peak cgroup memory was 2,149,679,104 bytes with zero swap; there was no
runtime error, and the process was stopped deliberately. Bounded torn VB/IB
source bytes are therefore not sufficient to explain the remaining corruption.
The primary visible failure is now color/attachment retention or composition,
not demonstrated exploding vertex positions.

## Pass and feedback coverage for progressive black/color corruption

The lifetime trace previously recorded selected writes, texture samples, and
resolves, but not a render pass that cleared without issuing a draw.
`TraceRenderTargetLifetimePassBegin` now runs before color barriers and records
the tracked/current guest and host identities, pre-barrier layout, render-pass
initial layout/load operation, fast-clear state, clear words, extent, and
format. It emits only for an already armed bounded target and does not modify
Vulkan state.

Clean-room comparison exposed a second structural risk: a live render target
may be resolved as a sampled image and transitioned to
`SHADER_READ_ONLY_OPTIMAL` before the same draw begins its color pass. Kyty has
no established attachment-feedback layout path, while independent renderers
either use an explicit feedback contract or reject direct reuse. The bounded
`SAMPLE_ATTACHMENT_ALIAS` event now compares resolved sampled images with each
active color image before transition, applies the selected color filter, removes
descriptor-padding duplicates, and owns a separate 32-event budget so ordinary
pass events cannot starve it.

The first selector-scoped pass attempt never encountered its selected HDR
producer. A later unselected run observed only exact `LOAD` pass begins with
defined color-attachment layouts in the menu-era window; its initial alias
counter shared a starvable budget and the damaged scene was not reproduced.
Neither run falsifies stale clear, attachment feedback, or a later WAW hazard.
Re-run only with a deterministic same-scene route. A coherent
`WRITE -> PASS_BEGIN(LOAD) -> SAMPLE` without alias closes this branch; an
identity mismatch, `CLEAR`/`DONT_CARE`, `UNDEFINED`, or exact sample/attachment
alias identifies the downstream contract to repair.

## Completed color-write dependency and route exclusion

Two bounded strict attempts then tested the corrected lifetime probes without
changing renderer output. The first delivered one scheduled input and reached a
coherent difficulty screen before its watchdog. The second queued three
present-addressed inputs; all three were delivered without cancellation, yet
the final capture was still the main menu. Its `hot_corruption` score was a
false positive caused by the menu palette, not a damaged 3D frame. The process
was stopped deliberately and no emulator process remained. These runs prove
that present-addressed delivery is not a visual scene identity: input consumed
during a loading transition can be reported delivered without advancing the
expected UI state. Do not repeat the absolute-present route or use its captures
as 3D evidence. Send one edge only after a stable interactive phase and verify
the resulting screen.

The menu-window trace recorded no `SAMPLE_ATTACHMENT_ALIAS`. It observed one
full-resolution HDR image re-enter from `SHADER_READ_ONLY_OPTIMAL` with
`loadOp=CLEAR`, then a sequence of exact-identity, defined-layout `LOAD` writers
and the expected exact downsample chain. That ordering is coherent for the menu
window only. Because the damaged 3D scene was not reproduced, zero feedback
events and the initial clear do not close either hypothesis for the active
corruption.

Static review also corrected the remaining access-scope gap in normal color
pass ordering. Indexed and auto draws already emit a global post-pass Vulkan
barrier with color writes in the source scope and later shader/color reads plus
`COLOR_ATTACHMENT_OUTPUT` in the destination. The destination access mask did
not include the subsequent color attachment write. Both paths now add
`VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT`, completing the color W-to-R/W
dependency without changing layouts, load operations, depth state, shaders, or
attachments. The affected emulator and diagnostics integration build under the
bounded two-job profile; the existing ten-test graphics integration selection
passes. Independent review found the access/stage pairing valid and recommended
retaining it. This remains API-correct host evidence, not a visual restoration:
the next gate is still a strict same-scene A/B of the black/color-spread frame.

A phase-driven strict integration then supplied the first visual check of that
change. Unlike the rejected absolute-present routes, it waited for a stable
interactive state, delivered exactly three taps, observed a real loading
transition and a stable interactive return, and captured through the native
agent. Agent-ready, video, present progress, input delivery, capture, clean
shutdown, and no-early-error gates passed. The only automated gate failure was
the missing historical baseline.

The captured screen was car selection with coherent UI but no vehicle.
`scene_ok=false`, `gameplay_like=false`, entropy `2.2764`, and 156 quantized
colors. The service completed in about 83 seconds, peaked near 1.5 GiB, and
used no cgroup swap. This disproves the completed color W-to-R/W access scope as
a sufficient restoration of the missing car/3D. It does not reach the later
damaged `ROLLING START` scene and therefore cannot establish whether the same
dependency changes the progressive black/color-spread symptom. Keep the
synchronization correction for API correctness, but keep visual acceptance and
the downstream attachment/composition investigation open.

## Capture-gated HDR lifetime frontier

A subsequent strict one-edge-at-a-time route reproduced the active gameplay
symptom after the color W-to-R/W correction. The frame retained a coherent HUD,
road perspective and recognizable vehicle/scene geometry, but large world and
vehicle regions were black and small yellow, red, green and white regions were
disconnected or dragged across the frame. `last-error` remained null. The sole
service peaked at 2,156,937,216 bytes with cgroup swap at zero and was stopped
deliberately. Treat this as same-version symptom evidence for color-target
retention or composition, not as new proof of vertex corruption or NaN/Inf
position propagation.

`KYTY_TRACE_RT_LIFETIME_COLOR_FORMAT` now parses one strict nonzero decimal or
hexadecimal Vulkan format and applies it conjunctively with the optional color
address selector at both generic target arming and exact-RT sample promotion.
It excludes depth and MRT-probe modes and leaves attachment-alias observation
format-global. The first format-122 run correctly selected two alternating
1920x1080 HDR targets and recorded exact `WRITE -> PASS_BEGIN -> SAMPLE`
sequences into format-50 destinations with no identity mismatch. Repetition
consumed all 64 main events between presents 3500 and 3516, before the damaged
scene. That menu-era ordering neither confirms nor rejects a later clear,
feedback loop or remap in gameplay.

Presentation thresholds were then shown to be unsuitable for the next trace:
the same input count reached credits, difficulty, main menu, the vehicle `PLAY`
card or gameplay at materially different present numbers. A new opt-in
`KYTY_TRACE_RT_LIFETIME_AFTER_CAPTURE=N` gate therefore snapshots a monotonic
successful-manual-capture count and opens the lifetime trace only after the Nth
newer successful explicit capture, beginning with the following render
activity. It uses a strict positive decimal ordinal. Automatic first/interval
or trigger-file captures, failures, timeouts and superseded request IDs do not
advance it. Manual request selection and publication use the capture mutex and
an exact selected request ID, so an automatic capture selected before a request
cannot consume it and a timed-out request cannot complete a newer one.

The parser, selector policy, capture milestone priority and exact request-
ownership transitions live in the existing graphics diagnostics integration;
the affected diagnostics and emulator targets build with two jobs under zero-
swap cgroups, and the focused integration passes. Independent review accepted
the stabilized policy after its concurrency findings were corrected. The first
capture-gated route kept the trace closed across five successful navigation
captures, peaked at 1,993,527,296 bytes with zero cgroup swap, and ended at its
180-second watchdog before ordinal 16. It emitted no armed HDR lifetime window
for the damaged scene. Do not repeat the present-threshold experiments or infer
graphics behavior from that timeout. The next integration should use the now-
known press-prompt/`PLAY` visual checkpoints, a smaller explicit ordinal and a
single longer-but-still-cgroup-bounded watchdog, then capture one post-gate
frame and classify exact `WRITE`, `PASS_BEGIN`, `SAMPLE`, alias and identity
events before changing renderer semantics.

## Same-scene HDR boundary and depth-retention frontier

A bounded native-score selector finally correlated the lifetime trace with the
active damaged 3D scene instead of a menu, credits card or `PLAY` overlay. The
pre-arm frame showed road and multiple vehicles with large black regions; three
presents later most opaque surfaces had disappeared while yellow lamps and
small white fragments remained. The two images are symptom evidence only, not
correct gameplay.

The one-shot trace opened on the intervening full-resolution HDR frame. It
selected one 1920x1080 format-122 target and recorded 60 writes, 59 pass begins
and the first exact downsample sample. The guest address and host identity were
stable throughout. Every pass began from a defined color-attachment layout
with `loadOp=LOAD`; CMASK fast clear stayed disabled, no clear/remap/identity-
mismatch event occurred, and no sample/attachment alias was observed. The
downsample shader `210001e03375e575` sampled the exact live HDR image and wrote
the expected 960x540 derived target. Therefore the retained corruption already
exists in the HDR producer; color clear, target identity, direct feedback and
the final compositor are excluded for this exact frame.

All observed HDR writes also reported depth test and depth write enabled. The
rapid disappearance of opaque geometry while emissive fragments survive makes
stale or incorrectly published depth the next one-variable discriminator, and
is consistent with the earlier observation that disabling depth reveals
geometry. It is not proof of a depth bug yet. Lifetime depth and color selectors
are deliberately incompatible in one process; an attempted combined run was
rejected before arming, and a corrected depth-only run remained at the press
prompt until its watchdog. Reproduce the same score-selected scene, arm only
`DEPTH_EXTENT=1920x1080` and `DEPTH_FORMAT=130`, then inspect the first
`DEPTH_ARM`/`DEPTH_USE`, metadata provenance and clear source. Do not insert a
per-frame clear, disable depth, swap `DB_RENDER_CONTROL` bits or change GEQUAL
without that trace.

The D32S8 metadata experiment was also narrowed. Removing the unconditional
image-create `DepthMetaMarkClear` is retained because first Vulkan use already
resolves `UNDEFINED` safely. A separate exact-storage writeback immediately
before depth materialization was tested, did not improve the visual output and
was removed after review found a TOCTOU gap plus unhandled pending/ambiguous
owners. The storage destructor's pattern-based mark was removed as well: object
retirement is not an observed guest/GPU write and could rearm a consumed clear
for a later allocation at the same address. Explicit upload, flush and completed
writeback observations remain diagnostic inputs. The focused graphics
integration and affected targets build after this revision. At that point the
pending-event map in `DepthMeta.cpp` was still address-keyed and unbounded; the
compute-event work below supersedes that limitation with complete storage
identity and fail-closed bounded retention.

## Compute HTILE fill restores coherent geometry

A bounded follow-up traced the previously unseen GPU metadata producer rather
than inferring a clear from storage contents. The compute shader forms a linear
global index from workgroup X and local `v0`, rejects lanes beyond a parameter
count, masks the source index, loads one typed dword and stores one typed dword
to the destination. Its live dispatch had exactly one invocation for every
four-byte destination record. The source contained one zero dword; the
parameter record supplied the exact destination count and a zero mask. The
destination was the exact writable storage range later named as D32S8 HTILE.

The retained implementation recognizes that decoded instruction/dataflow
family structurally and then validates all runtime facts independently. It
requires three exact metadata-sharp bindings with the captured read/write and
typed/raw roles, identity swizzles, one-dimensional 64-thread groups, exact
coverage, immutable source/parameter snapshots, one live writable storage
owner, nonzero logical/backing generations, and ordered submission. Unknown or
indirect descriptor use, extra resources, alternative addressing, partial
coverage, ambiguous storage ownership, or a generation mismatch fail closed.
No shader checksum, guest address, title, resolution, metadata byte size, host
vendor, or permissive flag participates in the decision, and the guest compute
dispatch still executes.

The compute event carries its complete metadata range, storage generations,
producer submit and fill word in a bounded 64-entry pending map. The depth
consumer accepts it once only for the same live range/incarnation, D32S8 1x,
bit-exact positive depth zero, and an already requested guest stencil-zero
clear. It then maps the metadata event to Vulkan depth `CLEAR` without
suppressing the following guest depth writes. Address reuse or a stale
generation purges the candidate rather than permitting an ABA match. Legacy
observed upload/flush clear events remain separate and retain their prior
behavior.

One strict bounded run, without the earlier forced-clear diagnostic, recorded
the compute event as consumed on the first world depth target. Its first use
selected `CLEAR`/`UNDEFINED` with depth and stencil zero; subsequent reverse-Z
`GEQUAL` passes used `LOAD` and wrote the same depth image. Native captures over
several seconds showed coherent road, fences, buildings and vehicles across
multiple cameras. This is causal runtime evidence that the missing-geometry
route was an unmodeled compute HTILE initialization event. It is not full
playability evidence, and the diagnostic forced-clear selector and temporary
ISA/binding probes were removed.

Independent review then closed two conservative false-positive boundaries: the
structural proof now requires the initialized local-X ABI register `v0`, and a
combined D32S8 image is not consumed unless the guest also requests a zero
stencil clear. The emulator and graphics diagnostics integration rebuild with
two jobs under the bounded zero-swap profile, and the existing short integration
contract passes. Those final guard-only changes have not yet repeated the
private strict route, so do not present them as a new runtime acceptance result.

## Remaining producer-side color/material corruption

The post-depth temporal sequence changes the diagnosis. Geometry no longer
explodes or disappears, but some surfaces remain black, the palette is washed
out or highly saturated, and `PLAY`/logo layers can persist between views.
Depth initialization cannot retain color pixels. The earlier exact HDR window
already showed stable full-resolution format-122 guest/host identity, defined
`LOAD` on every pass, disabled CMASK fast clear, no clear/remap, no direct
sample/attachment alias, and an exact downsample of an image that was already
damaged. Do not reopen vertex fetch, NaN/Inf position propagation, HTILE, color
load-op, target identity, direct feedback, or final-compositor hypotheses for
that exact frame.

The next single discriminator was DCC ownership at the HDR producer. One
checksum-scoped strict run selected exactly one world draw after the scene
threshold. Its attachment was the expected 1920x1080 format-122 RenderTexture,
with stable host identity and defined `LOAD`. The live register state reported
`dcc_enable=0`, a zero DCC base, disabled CMASK fast clear, zero clear words,
blend disabled, and RGB write mask `0x7`. The watchdog ended the sole process;
there was no emulator process left. This closes DCC/CMASK and immediate blend or
write-mask state as the cause for that exact writer. The decoded-but-unmodeled
DCC fields remain a general structural gap, not this scene's next patch.

The same record initially exposed a wave-width question, but the retained exact
ISA and current translator-v34 module close it for this draw. The sole
`v_readfirstlane_b32` source is selected from scalar buffer loads and the rebuilt
SPIR-V already emits the conservative direct-copy form; the ballot, find-first,
and broadcast sequence belongs to an older translator-v32 cache entry. The only
remaining subgroup instruction in the current module is
`OpGroupNonUniformAny` for `s36 < v12`. Both operands are scalar-uniform in the
exact ISA (`s36` is the scalar loop counter and `v12` derives from a minimum of
two scalar inputs), so both host subgroup-32 halves make the same decision. The
current analyzer reports the conservative static wave64 requirement because it
does not yet classify the scalar load into VCC as uniform, but that reporting
gap cannot select two material indices or explain the observed color. Preserve
unsupported native wave64 as a general strict-runtime limitation; do not add
subgroup emulation as this material fix.

The next one-variable boundary uses existing diagnostics: select the exact
MRT0 export of this writer with the output-preserving FinalMrtResult probe and
enable its same-fence attachment observation. Finite, nonblack assembled MRT
values with a corresponding attachment delta move the investigation upstream
to ordinary material/interpolation arithmetic. Values that are finite and
nonblack before export but absent from the attachment move it to render-pass
retention. Zero or non-finite MRT values keep the defect inside the pixel
shader before export. Require a contributing occurrence and a passing fixed-
test result; do not interpret an empty or depth-rejected match.

Three bounded runs then tested reproduction rather than changing renderer
semantics. A no-input `indexed:318` selector and a no-input recurrent-draw
selector reached their watchdogs without a FinalMrtResult event; neither draw
was present in those routes, so they say nothing about its output. The same
recurrent selector with two scheduled input edges delivered both edges and
entered loading, but the instrumented route did not reach the selected draw
before its 120-second watchdog. No process survived any watchdog and no build
ran concurrently. Do not lengthen this diagnostic route or interpret a missing
event as a black/nonblack result.

The uninstrumented strict playable-regression route was also repeated as an
integration check against the earlier gray car-selection capture. It reached
graphics, advanced 38,726 presents, delivered and guest-sampled all three pad
taps, reported no early loader or host error, and ended through the controlled
180-second deadline. It did not observe the required post-input loading
transition, so the runner correctly withheld the native capture and failed the
visual gate. This is stable runtime and effective diagnostic input, not
car-selection, gameplay, or material acceptance. The immediate three-tap route
is not a substitute for the known present-addressed route; do not repeat it
expecting a different checkpoint.

Finally, the retained six-frame post-depth sequence and public reference
imagery distinguish the original failure from intentional art direction. The
sequence keeps coherent geometry across cameras; hard polygonal shadows, pure
black vehicle/building parts, vivid foliage, and crash debris also occur in the
published game presentation. The current captures still show vehicle palettes
that deserve a same-checkpoint comparison, but no longer demonstrate the old
progressive vertex/color tearing. Treat a temporal smear or a deterministic
gray car-selection model as the red visual contract; do not recolor the title
from a single saturated frame.

## Intermittent guest direct-memory write fault

The first manual present-addressed two-tap route after the depth correction
reached a stable `interactive` phase and then terminated with a write access
violation at guest RIP `0x900a1c336`, targeting `0x350c8390`. The original
symbolization subtracted the guest code base and accidentally matched an
unrelated host PIE source line; that result is invalid. A loader-only bounded
inspection proves the exact guest instruction is a 16-byte zeroing `vmovups`
to `r14+0x10`, with `r14=0x350c8380`, so the effective address exactly matches
the crash report. The target is inside a shared direct-memory `memfd`, not a
host allocator object.

The same strict route was repeated with two scheduled taps at presents 8,000
and 8,080. Both were delivered and guest-read; the process reached present
8,099 with the target page inside a read/write direct-memory mapping. A manual
native capture at present 8,327 completed with a healthy score and coherent
track, vehicles and buildings. A second capture at present 8,750 was the
intentional low-entropy `ARCADE 8` transition card, not progressive color
collapse. `last-error` remained null, and the process ended only at its
150-second watchdog with a bounded 2-GiB peak. Capture is therefore excluded as
a deterministic trigger, and this run does not reproduce the fault.

Do not patch `RefCounter`, string copy-on-write, the host heap, the guest
instruction, or direct-memory placement from the single crash. The remaining
plausible emulator-owned boundary is an intermittent page-protection transition
such as GPU dirty tracking, but current state-machine tests pass and no exact
false-negative interleaving has been demonstrated. The next occurrence must
publish a signal-safe bounded snapshot of tracker page state, reference count,
captured original mode/token and native protection before fatal termination.
Until then this is a recorded runtime defect, not a graphics or allocator fix.
