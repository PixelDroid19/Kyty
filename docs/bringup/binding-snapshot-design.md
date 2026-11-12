# Binding over a versioned snapshot: design study (2026-11-12)

Status: read-only study, nothing built or measured. Every "saved" figure below is an estimate
bounded by the measured row it replaces, not a measurement. Paths are under
`source/emulator/src/Graphics/` unless stated. Line numbers are at the working-tree state read for this study.

Target: 11.36 us/draw -> 3-6 us/draw, 2,805 draws in 16.7 ms (needs <= 5.9 us all-in, with zero
time outside the draw entry; the 2026-11-10 notes put ~8.4 ms/frame outside it on another title, so the
draw budget is really lower and unmeasured on this scene).

## 0. What already exists (changes the shape of the question)

1. By-address storage reads are built. A read-only, dword-only, proven-flow V# is passed as a device address
   in the V# row, not as a descriptor: `ShaderStorageBufferReadsByAddress` (Shader.cpp:2716),
   `StorageGuestPagesAddress` (GraphicsRenderBind.cpp:3052), `WriteStorageAddressRow` (3062), by-address
   branch (3158, 3333-3345). Gated by `ShaderSetStorageAddressReads(guest_device_address_supported)`
   (Window.cpp:3431). bufferDeviceAddress is already enabled when guest-address is supported (Window.cpp:2425, 3378).
   Even then the slot still binds a zero-carrier VkBuffer (3340) so the descriptor array stays valid: the set
   is still built and keyed per draw.
2. A guest-address translation path exists in the translator: `guest_device_address()` binary search over a
   sorted table of imported chunks (ShaderSpirvGuestAddress.cpp:48-110, `PhysicalStorageBuffer` pointers, built by
   `GuestDeviceAddressBuildDirectory`, GuestDeviceAddress.cpp:1385). It maps arbitrary guest VAs read by
   shader code (scalar loads through pointers), not V#/T# descriptors.
3. Vertex streams do not go through descriptors: `BindVertexBuffers` (Bind.cpp:2672) issues one
   `vkCmdBindVertexBuffers` per stream with the imported chunk VkBuffer + offset
   (`GuestDeviceAddressResolveDrawInput`, GuestDeviceAddress.cpp:1617, thread-local 64 KiB run cache, lock-free
   on hit). The remaining cost per stream is `GpuMemoryRangeHoldsGuestBytes` (Objects/GpuMemory.cpp:765 ->
   GpuMemoryFind.cpp:358): takes `GpuMemory::m_mutex`, `ValidateAllocatedRangeLocked` (GpuMemory.cpp:236, binary search
   of `m_heap_index`) and `m_host_result_pages.Any`. The same pair guards index buffers (Draw.cpp:166) and storage
   (Bind.cpp:3056).
4. Descriptor sets are cached by key: `DescriptorCache::GetDescriptor` (GraphicsRenderDescriptor.cpp:511) builds a
   probe from `DescriptorKey()` of each storage buffer, `{unique_id, view}` of each image, sampler ids, GDS
   ids (593-640), then hash/probe/trim. Recorded measurements (cp-draw-plan.md, Arc A770): driver-side set work
   is ~0.5 us; prebuilt-hit binding 0.29-0.41 us/draw, descriptor-buffer 0.40-0.53; push descriptors cost ~9 us/call on
   ANV (excluded). So the emulator cache, not the driver, is the avoidable part.
5. Descriptor indexing is NOT enabled today (no `descriptorIndexing`/`runtimeDescriptorArray`/`UPDATE_AFTER_BIND`
   anywhere in the Graphics sources); `docs/bringup/bindless-tables-design.md` designs PARTIALLY_BOUND tables for
   GPU-selected descriptors but is design-only. `docs/bringup/cp-draw-plan.md` stages 1-4 already cover storage in a push
   set (stage 1, excluded on ANV), memo of T# resolution (3a built, 3b excluded) and bindless images (stage 4, parked).

## A. Raw guest addresses / descriptor-array indices instead of per-draw sets

Idea: V# = 16 bytes in push constants carrying a device address (already so for by-address rows);
T#/S# = u32 indices into one big array; per draw no set build, no per-stream lookup.

What can go the address route:
- Storage buffers read-only dword (already by-address): extend to every read-only raw/typed access the proof can lift
  (`dword_loads_only`, `exact_matches`, `!row_redefined`, ...). Writable storage stays on slots: write ordering,
  write mask (the "shared sink"), snapshot publication and overlay logic live on the slot path.
- Vertex streams: could become vertex pulling (shader BDA loads). Not recommended: the CPU cost today is one
  `vkCmdBindVertexBuffers` per stream (cheap, already on imported pages) plus the validity check (B). Pulling moves format
  conversion into translated shaders (every attribute format, normalized/scaled/int paths), raises GPU cost, and does
  not delete `ShaderVertexInputInfo` construction (vertex input 1.12 us is state setup, not binding). Skip.
- Index buffers: need `vkCmdBindIndexBuffer`; no address route (index 0.23 binding + 0.46 prep).

What cannot: sampled/storage images (need VkImage + layout + format/swizzle views), render targets, depth,
GDS, write-mask sink. T#/S# need a descriptor array (C), not addresses.

Per-draw costs replaced (estimates, upper bounds):
| Row (measured) | Replaced by | Est. saved us/draw |
|---|---|---|
| storage 0.47 | by-address coverage widening, drop zero carriers | 0.1-0.25 (unknown what fraction already by-address in this scene) |
| descriptor finalize 0.54 | storage part of key shrinks; set hit rate rises | 0.1-0.3 |
| vertex buffers 1.04 | nothing (not a descriptor path) | 0 |
Ceiling for A alone: about 0.5; realistic 0.2-0.4.

Requirements: bufferDeviceAddress (present), shaderInt64 (present, Window.cpp:3378). Portable on ANV, RADV, NVIDIA, Windows
drivers that already run the guest-address path. Risk: the by-address row bakes a device address into push
constants, so it must be re-derived when imports are removed (g_chunk_removals) and the "bytes held" check must stay
(B). Touches: translator output (bump `kShaderTranslatorVersion`, pipeline-cache invalidation, shader-id hash at
Shader.cpp:5957-5970), tests that pin binding numbers (`cp-draw-plan` notes some exist), GPU page-fault behavior of an address read
past a freed chunk (robustness: out-of-bounds device address read is undefined, unlike a bounded descriptor; the
existing `backed_bytes` word 2 clamp must keep being enforced in the translator).

## B. Epochs instead of per-draw validation

Candidates (all today take a lock or hash per draw/stream):
1. `GpuMemoryRangeHoldsGuestBytes` per vertex stream/index/storage (GpuMemoryFind.cpp:358). Inputs: heap index
   (`m_heap_index`, changes only on map/unmap) and `m_host_result_pages` (changes only in `SetHostResultPagesCounted`,
   GpuMemoryFind.cpp:760). Both are mutated under `m_mutex`. Proposal: one `std::atomic<uint64_t> m_guest_bytes_epoch`
   bumped at each mutation of either structure; extend the existing thread-local run (`GuestDeviceAddressResolveDrawInput`
   `Run`, already tagged by `g_chunk_removals`) with `{epoch, range proven clean}` so that a stream inside a clean
   64 KiB run needs one acquire load and two compares, no mutex. Race: same window as today (check, then unlock, then
   use); not a regression.
   Caveat: `m_host_result_pages` is a page-set; a run is "clean" only if no counted object overlaps the whole run. Runs
   adjacent to writable storage stay on the slow path (correct, slower). Epoch churn rate is unknown: if writable storage
   objects are created/destroyed per frame, hit rate is bounded by that rate (one bump per object change, not per draw).
   Est. saved: 0.3-0.6 of vertex buffers 1.04 (+0.05-0.1 index 0.23). Mutex + two searches x ~2-3 streams x ~60-100 ns is
   my estimate of the mutex share; the split of the 1.04 between validity, `ResolveVertexStreamRead`, and the driver call was not measured.
2. Coalesce `vkCmdBindVertexBuffers` into one call per draw (not epoch, same row): 0.1-0.2, ANV driver cost per call not measured here.
3. Guest-address read preparation 0.72 (`GuestDeviceAddressPrepareForRead`, GuestDeviceAddress.cpp:2272; it mutates the
   registry and can retry the draw, per parallel-recording-design): skip when every range the draw reads sits in a chunk
   already imported at the current registry epoch and nothing was queued. Needs a registry epoch (`g_chunk_removals` is
   removal-only; additions need their own). Est. 0.3-0.5, unverified: the row mixes the import decision with
   table publication and I did not read which part dominates.
4. Index range/stream prep 0.46 + "before vertex input" 0.55: transient-snapshot reuse already exists. A snapshot
   valid-while-epoch memo (keyed by range + `GpuDirtyPageTracker` generation, GpuMemory.h:1404 `guest_generation`) could
   make repeats free. Est. 0.15-0.3. Caveat: guest rewrites tables next to commands, so the dirty generation must be per
   page/run, not global; a global counter would change almost every draw and give no hit (unverified, depends on the tracker design).
5. Write-back / pending-write-back checks per draw (Draw.cpp:4043 `GuestDeviceAddressPendingWriteBackOutsideStorage`):
   replaceable by "no GPU write submitted since last check" counter: small, not in any measured row; skip until measured.

Requirements: none beyond atomics. Portable everywhere. Risk: a missed bump is a silent stale read of guest bytes (the
failure shows as corruption, not a crash), so every mutation site of both structures must bump inside `m_mutex`, and a
debug mode should cross-check epoch hit vs the locked answer. Touches: GpuMemory tests, strict runs on titles with
writable-storage-over-guest-pages (the "line revert" and "write mask" records), AMD/Intel difference in the cache-line revert issue.
Ceiling for B: about 1.0-1.6; realistic 0.6-1.0.

## C. Persistent texture descriptor heap

Idea: one large sampled-image array and one sampler array (descriptor indexing: runtimeDescriptorArray,
shaderSampledImageArrayNonUniformIndexing, descriptorBindingPartiallyBound, descriptorBindingSampledImageUpdateAfterBind,
descriptorBindingUpdateUnusedWhilePending; all core 1.2 and the repo already requires Vulkan 1.4, per cp-draw-plan Constraints).
Slot = {image unique_id + view index + layout class} (the same identity `GetDescriptor` already hashes, Descriptor.cpp:620-628);
S# deduped by contents into the sampler array. Per draw: look up/insert each (T#-key -> slot) and push the u32 index.
Translator change on every image access (indexed array access, NonUniform decoration only if divergent); numeric-type banks
(`split_numeric_types`, Descriptor.cpp:541-565) become index ranges or separate heaps; depth/stencil, 3D, array, storage images need
their own typed arrays (Vulkan arrays are typed), so at least 8-10 heaps, mutable descriptors (VK_EXT_mutable_descriptor_type) would
unify them but that extension is not a given on all targets.
Slot lifetime: a slot may be rewritten only after every submission that used it completes (the existing deferred-delete machinery
`DeferUncachedSets`/`TrimSets` is the model); a recycled image view must get a new slot (generation in the key) because PrepareTextures
can change layout/view per draw (e.g. `BindSampledMinLodView`, Bind.cpp:3432).
Replaces: image/sampler part of the set key and set alloc/update. Recorded premise (cp-draw-plan stage 1): 95-96% of set misses
came from buffers and images were already seen; new image part 0.7% of misses. So a heap alone gives little unless
the storage part is also removed (A): the set is rebuilt because of buffers. Texture row 0.50 is T#->image resolution
(`PrepareTextures`, Bind.cpp:3576), which the heap does not remove; the guest-upload dirty-page check (91% of slots) must keep running.
Est. saved: finalize share 0.2-0.4 (only together with A), texture resolution 0.1-0.25 via slot memo keyed by T# words
+ object-graph generation
(`GpuMemoryObjectGraphGeneration`, GpuMemory.cpp:848; 3a exists for misses only, 3b was excluded at <1% of CP time on another title).
Ceiling for C alone: ~0.5-0.7; realistic 0.3-0.5.
Portability: update-after-bind + large heaps: ANV and RADV expose large limits; NVIDIA yes; Intel Arc on Windows and the
`fix/windows-intel-device-lost` context make update-after-bind a risk (concurrent descriptor update while in flight is a driver-exercised
path, unverified here). Alternative: VK_EXT_descriptor_buffer (exposed by ANV per measurement; RADV/NVIDIA likely,
Windows Intel unverified),
measured only at recording-time parity with sets (0.40-0.53 vs 0.29-0.41), no win by itself.

## D. Rows A-C do not cover (the rest of the 11.36)

| Row | us | Lever | Est. saved |
|---|---|---|---|
| vertex input 1.12 + pixel input 0.80 | 1.92 | draw plan (shader-keyed analysis memo, cp-draw-plan stage 2, priced 1.5 total in 2026-11-12 note) | 1.0-1.5 |
| shader ids 0.53 + pipeline lookup 0.20 | 0.73 | memo keyed by (user-data/shader-register words, code-page generation); 99.97%/99.83% repeat | 0.4-0.55 |
| emission 1.39 | 1.39 | dedupe redundant dynamic state / push constants vs previous draw, fewer vk calls | 0.3-0.5 (no per-call split measured) |
| materialization 0.48 + BeginRenderPass | 0.48 | skip when framebuffer + layouts equal previous draw | 0.1-0.2 |
| unattributed 1.2 | 1.2 | needs a profile first | unknown |

## E. Staged plan (each stage measurable alone, strict correctness unchanged)

Measure each with the existing per-draw stage counters on the same scene, route and 60 fps cap (`kyty_agent` runs, one at a
time, short); compare us/draw and a scored capture; AMD iGPU via RADV as correctness check.
1. Stage 1, epoch-guarded guest-bytes validity (B1, B2): lowest risk, no translator change, no Vulkan feature.
   Gate: vertex buffers row 1.04 -> <= 0.6; zero capture diff; debug cross-check vs locked answer reports 0 mismatches.
2. Stage 2, registry epoch for guest-address read preparation + index snapshot memo (B3, B4). Gate: rows 0.72 / 0.46 drop; no new retries.
3. Stage 3, draw plan for vertex/pixel input and pipeline key memo (D rows 1-2), already designed in cp-draw-plan
   stage 2; independent of binding.
4. Stage 4, state-emission dedupe and materialization skip (D rows 3-4).
5. Stage 5 (optional, only if finalize 0.54 + storage 0.47 still > 0.7 after 1-4): widen by-address storage (A), then one bindless
   sampled-image heap behind a capability check (C), with the slot path as the single fallback source of truth. Translator bump.
   Do not start with C: the set misses are driven by buffers.
6. Stage 6: only if 1-5 land above ~6 us: front/back split (priced 11.36 -> ~9 us alone at ~9 us/draw back stage; after stages 1-4 the
   back stage is binding + materialization + emission, so it needs re-pricing).

## F. Total

| Stage | us/draw saved (estimate range) |
|---|---|
| 1 B1+B2 | 0.4-0.8 |
| 2 B3+B4 | 0.4-0.8 |
| 3 draw plan + pipeline memo | 1.4-2.0 |
| 4 emission + materialization | 0.4-0.7 |
| 5 A + C (optional) | 0.4-0.9 |
| Sum | 3.0-5.2 (some overlap, take 10-20% off) |

Plausible result: ~6.5-8.5 us/draw (about 18-24 ms for 2,805 draws), i.e. ~40-55 fps on the CP at best for this scene, not
60. 5.9 us is reachable only if every upper bound lands and the unattributed 1.2 us shrinks; 3 us is not reachable by binding
changes: the sum of removable work (about 5 us) leaves a floor near 6 us (decode, shader-register reads, emission, driver calls). 3-4 us
would need recording moved off the CP thread entirely (replayable draw plan, the "redesign" in
parallel-recording-design) on top of these stages.

## G. Unverified (explicit)

- All saved figures; none of the sub-row splits (mutex vs search vs driver call) were measured.
- Fraction of this scene's storage buffers already by-address; fraction of streams in a clean 64 KiB run; epoch bump rate per frame.
- Whether the GPU dirty-page tracker offers a per-run generation cheap enough (B4).
- Update-after-bind and descriptor-buffer behavior on Windows Intel and current RADV/NVIDIA builds (limits, device-lost risk).
- Whether the "~1.2 unattributed" is the sum of per-call profiler overhead (possible: counters wrap calls).
- GPU-side cost of any bindless/address indirection (not CPU-priced here).

Stage 1 result (2026-11-12): the guest-bytes generation is built; the sandbox world measured 9.80 -> 9.07 us
per draw in one run against one baseline in the same scene (`docs/titles/sandbox.md`).
