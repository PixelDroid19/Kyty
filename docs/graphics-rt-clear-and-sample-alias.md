# Color clears, RenderTexture sample aliases, and WaitRegMem fences

This document describes the **stable graphics contracts** used when Gen5 titles
sample color attachments that also exist as live render targets, and when the
command processor waits on GPU-published fences. It is for implementers and
reviewers. Session notes, title IDs, and scratch captures do not belong here.

Related code:

| Concern | Primary ownership |
| --- | --- |
| Clear-word decode and attachment load-ops | `source/emulator/include/Emulator/Graphics/Utils.h` (`DecodeGuestColorClearWords`, `ResolveColorAttachmentLoadOps`) |
| Non-exact GPU object overlap policy | `source/emulator/include/Emulator/Graphics/Objects/GpuMemory.h` (`GpuMemoryFindObjectsAcceptsRelation`, `PreferGpuMemoryAliasIndex`) |
| Sample bind → RenderTexture / DepthStencil | `source/emulator/src/Graphics/GraphicsRender.cpp` |
| EOP / Label publish and write-back holes | `source/emulator/src/Graphics/Objects/Label.cpp` |
| `WaitRegMem32` / `WaitRegMem64` | `source/emulator/src/Graphics/GraphicsRun.cpp` |

Tests live under `UnitTestEmulatorGraphicsState` (clear load-ops, find-relation
policy, alias preference, Label force-complete / WaitRegMem masks).

## 1. Guest color clear words

AMD `CB_COLOR*_CLEAR_WORD0/1` hold a raw clear pixel (two dwords). Kyty must
decode the packing that Mesa/RADV use for the attachment’s Vulkan format:

| Format | Packing |
| --- | --- |
| `R16G16B16A16_SFLOAT` | WORD0 = f16(R)\|(f16(G)<<16), WORD1 = f16(B)\|(f16(A)<<16) |
| `R8G8B8A8_*` | WORD0 = R\|(G<<8)\|(B<<16)\|(A<<24), WORD1 = 0 |
| `B8G8R8A8_*` | WORD0 BGRA8 similarly |

**Contract:** for those known packings, `CLEAR_WORD0/1 = 0` is **transparent
black** (`A = 0`), not opaque black. Inventing `A = 1` on an RGBA8 intermediate
that is later sampled with alpha blend produces **opaque black quads** around
sprites and props.

`ResolveColorAttachmentLoadOps` clears on first bind (`UNDEFINED`) and when
rebinding after sampling (`SHADER_READ_ONLY_OPTIMAL`). Within-frame draws that
stay in `COLOR_ATTACHMENT_OPTIMAL` still `LOAD`.

Unsupported formats must not invent channels: callers either skip decode or
fail structured. Do not bitcast WORD0/1 as float32 R/G for float RTs.

### Implementation checklist

1. Add or extend a focused unit test under
   `EmulatorGraphicsState.ColorAttachmentLoadOps*` that asserts `A = 0` for
   zero words on each known format (including rebind-after-sample).
2. Change only `Utils.h` decode / load-ops (and call sites that pass `VkFormat`).
3. Rebuild `fc_script` and run the filter before claiming a visual fix.
4. Re-capture the **scene that owns the defect** (tip art can look fine while
   gameplay tiles remain wrong).

## 2. Sample → RenderTexture aliasing

When a texture descriptor’s guest range overlaps a live `RenderTexture`
allocation, the sampler must bind that Vulkan image instead of uploading empty
GPU-owned guest memory (which paints opaque black).

### Find policy

`GpuMemoryFindObjects(..., exact=false)` accepts:

- `Equals`
- `IsContainedWithin` (existing RT sits inside the sample query)
- `Contains` **only** when the RT and sample share the same base address
  (size-mismatch Equals miss)

Unconditional `Contains` matched multiple overlapping parent RTs and historically
tripped `EXIT_NOT_IMPLEMENTED(rtex.Size() > 1)`, aborting the host process mid-
load. Offset-into-parent cropped views remain unsupported until a cropped-view
contract exists. `Crosses` is rejected for this sample path.

Depth lookups use `only_first`; color sample bind collects matches and then
selects one image.

### Choosing among multiple matches

`PreferGpuMemoryAliasIndex(object_sizes, count, sample_size)`:

- If `sample_size > 0`: prefer the **smallest** object that still covers
  `sample_size` (tightest cover). If none cover, prefer the **largest** under-
  sample object.
- If `sample_size == 0`: sizes are a comparable proxy only (for example pixel
  area); prefer the smallest proxy. Call sites that lack a true sample size
  should prefer passing guest **byte** sizes from GpuMemory blocks plus the
  sample byte length whenever available.

Do **not** abort the process on `Size() > 1`. Aborting turns a graphics alias
bug into “the game will not start.”

### Implementation checklist

1. Encode the relation policy in
   `EmulatorGraphicsState.FindObjectsNonExactAcceptsContainedSampleInRenderTarget`.
2. Encode selection in
   `EmulatorGraphicsState.PreferGpuMemoryAliasPicksTightestCover`.
3. Wire selection in the sample-bind path in `GraphicsRender.cpp` after
   `FindRenderTexture`.
4. Barriers that must touch **every** overlapping RT keep `only_first=false` and
   iterate all images; only the single-bind sample path picks one alias.

## 3. Labels, EOP fences, and WaitRegMem

`WaitRegMem32/64` polls a guest address until `(value & mask) == (ref & mask)`.
The producer is normally an EOP / Label store scheduled through `LabelSet` and
published in `LabelManager::FireCallbacks` **after** WriteBack side effects, so
a later StorageBuffer write-back cannot zero an earlier fence.

Durable fence words that share a StorageBuffer range must be registered as
write-back **holes** (`LabelWriteBackCopy` / `MemcpySkipAbsoluteRanges`) so
GPU→CPU copies do not clobber them.

### Timeout behavior

A bounded poll that never sees the fence is an **error worth diagnosing**
(agent event `wait_reg_mem64_timeout`, stderr). It must not be “fixed” by:

- inventing `KernelSetEventFlag` / cond signal from the host, or
- writing the expected fence value from the waiter.

Those change guest scheduling and hide the missing Label/EOP producer. Prefer
draining completed labels (`LabelCompleteSubmitted`, `LabelDrainCompleted`) and
keeping the wait diagnosable while the producer path is fixed.

## 4. How to validate a change

```bash
# Host build dir must match this checkout
ninja -C _build_macos fc_script   # or _build_linux

_build_macos/fc_script '{kyty_run_tests()}' \
  --gtest_filter='EmulatorGraphicsState.ColorAttachmentLoadOps*:EmulatorGraphicsState.PreferGpuMemoryAlias*:EmulatorGraphicsState.FindObjectsNonExact*'

# Authorized private fixture only; path stays in the environment
export KYTY_GUEST_ROOT=...
export KYTY_AGENT_SOCK=/tmp/kyty-agent.sock
export KYTY_NATIVE_CAPTURE_DIR=...   # untracked

_build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
_build_macos/agent/kyty_agent --sock "$KYTY_AGENT_SOCK" status
_build_macos/agent/kyty_agent --sock "$KYTY_AGENT_SOCK" capture
```

Judge captures against a same-scene reference: opaque black sprite boxes and
missing tile rectangles outrank color-score heuristics. Warm god-rays / bloom
confined to light sources are usually intentional art, not “yellow world”
corruption.

## 5. Opt-in diagnostics

| Env | Role |
| --- | --- |
| `KYTY_SLOT_TRACE` | Soft-lock dumps (Flip-idle slot table / CondWait). Observation only. |
| `KYTY_EOP_TRACE` | Bounded EOP fire / fence address logs. |

Do not enable stub, permissive, or trap-skipping modes as acceptance evidence.
See `docs/graphics-captures.md` and `docs/agent-tools.md` for capture and
realtime agent workflows.
