# Kyty Gen5 runtime graphics investigation handoff

Updated: 2026-07-20

Status: the runtime advances into sustained gameplay-era presentation without
a process-killing error. The opaque black sprite/prop rectangles are absent
after correcting pixel-kill depth ordering. A persistent, device-qualified
Vulkan pipeline cache now removes most repeated driver pipeline compilation
cost across runs. Full playability acceptance and sustained-FPS optimization
remain open.

This document intentionally excludes private workload names, identifiers,
paths, binaries, screenshots, shader hashes, and raw logs. Keep those only in
ignored scratch and address the workload through `$KYTY_GUEST_ROOT`.

## Verified advances

The current graphics branch contains four isolated, tested changes:

| Commit | Contract | Verification |
| --- | --- | --- |
| `990b9a40` | Decode and lower Gen5 `ds_read2_b32` with two dword-scaled offsets over byte-addressed Workgroup LDS | Focused parser/SPIR-V test plus strict runtime |
| `14633fe6` | Preserve the layout of GPU-owned RenderTextures across update re-entry | Focused graphics state test plus strict runtime |
| `9cc21524` | Preserve discard semantics for null MRT0–3 export tails | Focused shader/SPIR-V test plus strict runtime |
| `9b026e53` | Keep pixel-kill shaders on late Vulkan depth commit while retaining early fragment tests for opaque shaders | Red/green SPIR-V test plus gameplay-era native capture |

On the exact tracked branch state, Linux Release passed 205 focused
GraphicsPackets/GraphicsState tests. The earlier strict Release+Silent baseline
without `KYTY_BRINGUP_*`, automatic input, or permissive fallbacks exceeded
2,300 presents. New gameplay-era visual captures used automatic Cross only to
reach the scene and therefore are discovery evidence, not input acceptance.
No structured EXIT, host fault, or Vulkan device loss was observed.

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

The expected result at this handoff is 205 passing tests. A runtime change is
accepted only when the focused tests pass and a strict re-run either preserves
the gameplay-era checkpoint or advances the first failure.

## Problem-to-solution guide

| Symptom | Proven producer | Resolution | Regression evidence |
| --- | --- | --- | --- |
| Structured exit: unknown `ds_read2_b32` | Gen5 DS parser/SPIR-V generator lacked the two-result LDS read | Decode both 8-bit offsets, scale each by four bytes, retain byte-addressed `vaddr`, and load consecutive destination VGPRs from Workgroup memory | Focused packet-to-SPIR-V test and strict runtime advancement |
| White or horizontally corrupted world after a valid earlier frame | RenderTexture update re-entry reset a GPU-owned tiled image to `VK_IMAGE_LAYOUT_UNDEFINED` | Preserve the current image layout on Update; use `UNDEFINED` only for initial creation or an evidenced invalidation/discard | Focused GPU memory/render-target state test |
| Striped or missing output around multi-render-target shaders | Null MRT export tails lost their discard/no-write semantics during SPIR-V generation | Preserve null MRT0–3 tails as no-write exports instead of fabricating color output or truncating the export contract | Focused shader/SPIR-V export test |
| Opaque black rectangles in transparent sprite or prop bounds | Kill-enabled `EarlyZThenLateZ` pixel shaders were emitted with Vulkan `EarlyFragmentTests`, allowing depth commit before `OpKill` | Omit `EarlyFragmentTests` for pixel-kill shaders so discarded fragments cannot write depth; retain it for opaque early-Z shaders | Red/green SPIR-V test and native gameplay-era capture |
| Large first-run stalls recur after restarting Kyty | `VkPipelineCache` was always created empty and never persisted | Validate the standard cache header against vendor/device/UUID, load compatible bounded data, and save dirty cache data atomically at a rate limit | Header tests plus isolated cold/warm driver measurements |
| Pipeline-cache writes can exceed the session budget after an I/O failure | A failed temporary-file write, flush, or replace did not consume budget and was retried on every pipeline lookup | Charge every disk attempt conservatively, rate-limit retries, and stop attempting after 64 MiB per process | Budget saturation test plus strict runtime disk counters |
| Reload exits while a sampled texture overlaps live color/depth aliases | The texture crosses the color RT/storage pair and the depth metadata plane, but the mixed-parent policy did not recognize the exact DepthStencil relation | Link only the captured `DepthStencilBuffer Crosses Texture` metadata alias; materialize the image from the existing color surface | Exact policy test plus input-driven strict runtime beyond the former exit |
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

The current performance frontier is separate from graphics correctness. A
Release+Silent gameplay sample showed roughly 6 FPS after warm-up even though
the menu exceeded 100 FPS. Read-only probes attributed approximately
105–122 ms/s to full-range memory hashing and 140–180 ms/s to immediate
submit/fence waits; neither alone explains the remaining frame time. Pipeline
miss bursts explain severe transient freezes, while persistent low FPS still
requires producer-level work in shader reuse, GPU memory tracking, command
submission, and resource upload. Change one contract at a time and compare
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
