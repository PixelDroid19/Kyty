# Per-draw plan: dynamic buffer bases and bindless textures (design, 2026-10-28)

Status: design. Stage 3a (the sampled lookup-miss memo) is implemented and measured; the rest is not.

## Why

`PPSA21567 - Astro Bot` dunes cost about 230 us of command-processor
time per draw on the Intel A770 host. 60 fps at the dune draw count needs
about 27 us. A cpu-clock profile of the CP thread (24,250 samples, dunes,
`agent/astro-split-wave64` at `5112aeec9`) shares out as follows (inclusive):

| Function | Share | What it does per draw |
| --- | --- | --- |
| `BindDescriptors` | 41.5% | Calls the three rows below plus small helpers |
| `PrepareTextures` | 20.3% | Per T#: `FindRenderTexture` 4.8, `GpuMemoryCreateObject` 3.9, `FindOverlappingStorageTextures` 1.7, `FindLatestStorageTexture` 0.9 |
| `ShaderGetInputInfoPS` (+VS 2.1) | 13.1% | `ShaderParseUsage2` 9.6: dynamic scalar resources 3.0, sampler association 1.1, EUD end 0.9, scalar spans 0.8 |
| `PrepareStorageBuffers` | 10.9% | `CaptureTransientSnapshotBuffer` 8.9 (a data copy) |
| `DescriptorCache::GetDescriptor` | 7.1% | Trim 1.9, index 1.5, allocate and update on a miss |
| Unattributed (`[unknown]` self) | 27% | Driver and unresolved frames, still to be split |

The evidence from Experiment 0 (2026-10-27, 4 titles) sets the shape of a plan:

- The draw key (pipeline, shaders, render state) repeats 94-99% against frame N-1/N-2.
- Exact user-SGPR values never repeat (0%): tables live in rings, so pointers move every draw.
- Among key hits:
  - all descriptor words are identical only 0.4-0.9% of the time (26% in one beat 'em up);
  - only buffer bases change in 44-92%;
  - V# `num_records` changes in 21-55%;
  - T# or S# words change in 1-2% (24% in the beat 'em up).
- 98% of descriptor-set misses differ only in the storage-buffer `unique_id` (rotating ring slots).
- Whole-frame replay and exact memoization are excluded.

So the plan has to treat what changes (buffer bases, sizes and storage objects) as per-draw data,
and memoize what stays (the resource recipe, T#/S# resolution and the image/sampler set).

## Constraints

- Descriptor words are gathered when the draw is decoded. Titles rewrite tables next to their
  commands, so a word read later can be from the next draw.
- A memo is valid only when the words it was built from are byte-equal **and** every host object it
  names is still the same generation. A recycled view, an invalidated texture or a re-described
  parent must miss.
- No title-specific keys and no permissive skips. A miss falls back to today's full path, which
  stays the single source of truth.
- Vulkan 1.4 is already required (`Window.cpp:1989`), so push descriptors are core on every host we
  target (ANV, RADV, NVIDIA). `maxPushDescriptors` is at least 32 and has to be queried.

## How V#, T# and S# reach the shader today

- Each V# is 16 bytes in the per-draw push constants, or in the `%vsharp` UBO when the block is
  larger than 128 bytes. Word 0 carries the slot index, not the guest base
  (`GraphicsRenderBind.cpp:3227`).
- The shader addresses `buf[slot]` at `index*stride + offset`. It does not add the guest base.
- Each V# binds its own VkBuffer with offset 0 and the V# range: a zero carrier, a static span, a
  transient snapshot or a persistent object (`GraphicsRenderBind.cpp:2979-3241`).
- The descriptor-set key holds `{unique_id, range}` per storage buffer, the vsharp key, and the image
  and sampler entries. That is why a ring slot change misses the whole set, images included.

## Stages

Each stage stands on its own, is measured on the dunes (us/draw) and swept across the matrix before
the next one starts.

### Stage 1: storage buffers and vsharp in a push-descriptor set

The aim is to make the cached set image/sampler-only so that it hits, and to write the per-draw
buffers without allocating a set or hashing anything.

- The pipeline layout gains one `PUSH_DESCRIPTOR` set, shared by the VS and PS of a graphics
  pipeline (Vulkan allows one per layout). It holds the stage's `%buf` / `%buf_uint` binding and its
  `%vsharp` UBO.
- Translator: the storage-buffer array and the vsharp block move to that set index. Each stage gets
  its own binding numbers. The translator version is bumped.
- Host: `BindDescriptors` fills `VkDescriptorBufferInfo`s from `PrepareStorageBuffers` as now and
  calls `vkCmdPushDescriptorSet`. `GetDescriptor` keys only images and samplers.
- Guard: when buffers exceed `maxPushDescriptors`, the layout keeps today's single-set form. The
  layout plan decides this per shader, not per draw.
- Expected gain: most of `GetDescriptor` (7.1%) plus the misses' `vkUpdateDescriptorSets`. The gain
  is small, but stage 3 needs stable image sets.

### Stage 2: resource recipe per shader (replaces per-draw `ShaderParseUsage2`)

- `ShaderParseUsage2` works out the same thing every draw for one shader: which user SGPRs, which
  pointer derefs, at which offsets, how many dwords, and for which slot and type.
- Record that as a flat list of load steps in `ShaderCodeResourceMemo`, once per (shader, user-SGPR
  count, a config subset still to be enumerated).
- Per draw: run the steps, reading guest words into `bind`, then apply the parts that depend on
  values:
  - the EUD end comes from the words;
  - the dynamic-scalar consumers depend on a word, so the condition is recorded in the step.
- Guard: a step whose outcome depends on a word value records that dependency. When the value takes
  another branch, the draw runs the full parse and the recipe is extended.
- Expected gain: most of the 13.1% + 2.1% input-info time.

### Stage 3: T#/S# resolution memo per slot

Measurement (temporary counters, gw70/gw71, dunes, 3.4M sampled slots late in the run), keyed by
the 8 T# words, the bound flags, the sRGB choice, the stage and the scaled-sampling refusal:

- Only about 1,000 distinct keys.
- The same key resolves to the same image with unchanged content and write stamps 97.3% of the time.
  A further 2.5% is the same image with new content, and 0.3% is a different image.
- By path:
  - guest uploads are 91% of slots;
  - render-target and depth hits change image only when an object block is created or deleted;
  - storage picks change with write stamps and no graph change, so storage slots stay on the full
    path.
- Guest uploads: the same key alternates between texture objects with the object graph unchanged.
  A memo of the image itself would need its own guard. The upload path's dirty-page check also
  has to keep running.

**Stage 3a, implemented: lookup-miss memo.**

- A thread-local, direct-mapped table records sampled descriptors whose GPU-owned lookups (stencil
  plane, depth, render target, storage) found nothing.
- The record is valid while `GpuMemoryObjectGraphGeneration()` is unchanged. The generation
  advances in `GpuMemory::CreateBlock` and `DeleteBlock`.
- On a hit those lookups are skipped. The video-out lookup, the guest upload
  (`GpuMemoryCreateObject` with its dirty-page check) and the stamp-ordered copies after it still
  run.
- The skipped lookups only recorded submission uses (deferred-deletion retention) of candidates they
  then rejected, which are not bound.

Result on `PPSA21567 - Astro Bot` dunes (Intel A770, 340 s, gameplay route, 60 fps cap):

| Run | `draw_descriptor_texture` | `draw_resource_binding` | CP total |
| --- | --- | --- | --- |
| gw64, gw68 | 23.4, 19.1 us/draw | 48.4, 45.9 us/draw | 232-243 us/draw |
| gw72 | 14.1 us/draw | 40.9 us/draw | 232 us/draw |

- The texture stage costs 26-40% less.
- The CP total did not move: `fence_wait` (57 us/draw) and run-to-run noise hide the saving.
- The dune capture matches gw64.

**Stage 3b, not started: memo of the resolved image.** For render-target and depth slots, key by
the same words plus the object-graph generation, and replay `RecordUse` for the cached object.

### Stage 4 (only if stage 3 leaves set churn): bindless images

- Images and samplers go into one large `UPDATE_AFTER_BIND | PARTIALLY_BOUND` table (descriptor
  indexing is core 1.2). The shader reads the per-draw index from push constants.
- This removes image-set allocation entirely. It needs a translator change for every image access,
  so it is only worth doing when stage 3 data shows image sets still miss.

### Not in this plan

- `CaptureTransientSnapshotBuffer` (8.9%) is a copy of guest data. It shrinks only when the GPU
  reads guest pages in place (the dma-buf import path), not through caching. It is tracked
  separately.
- 27% of samples are unattributed. A profile with driver symbols has to split them before the
  dune target can be planned for real.

## Honest ceiling

If stages 1-3 remove the expected shares, CP time drops by about 35-40%, to roughly 140-150
us/draw. That is not 27 us. 60 fps also needs:

- the transient-copy lever;
- the unattributed 27% split and reduced;
- fewer host draws per guest draw (`ResolveIndexedVertexRecords` 4.4%, `PrepareGuestDeviceAddressForRead` 3.9%).
