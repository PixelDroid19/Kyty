# Strict Portable Graphics Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Advance the local reference workload through its known strict HLE and PM4 frontiers without missing-symbol stubs, skipped graphics state, guessed resource layouts, or platform/vendor coupling.

**Architecture:** Keep the existing loader, HLE, `HW::Context`, renderer, and VideoOut pipeline. Extract pure guest-state decoders used by both direct and indirect PM4 paths, implement confirmed guest exports against existing Kyty primitives, and use strict local execution to identify the next evidenced rendering divergence.

**Tech Stack:** C++17, CMake/Ninja, GoogleTest through `fc_script`, Vulkan/MoltenVK, Lua run scripts.

## Global Constraints

- Tracked files must contain no proprietary title name, title identifier, personal path, or compatibility-fixture content.
- `KYTY_STUB_MISSING` and `KYTY_GFX_PERMISSIVE` are diagnostic only and cannot prove completion.
- No behavior may depend solely on a Vulkan vendor ID.
- Direct and indirect encodings of the same guest state must use one decoder.
- Unknown guest behavior fails with evidence; it is never silently skipped or guessed.
- Existing unrelated failures, including the date-dependent `Core.DateTime` test, must be reported separately and not rewritten as part of this plan.

---

### Task 1: Canonical repository operating manual and generic local runner

**Files:**
- Delete: `AGENT.md`
- Create: `AGENTS.md`
- Create: `scripts/run_guest.lua`

**Interfaces:**
- Consumes: `fc_script <lua-script> <guest-root>` argument forwarding.
- Produces: `arg[1]` as the untracked guest root; one canonical instruction file.

- [x] **Step 1: Replace the singular manual with one canonical `AGENTS.md`**

The document must contain the approved mission, verified architecture map,
strict-mode policy, evidence-first workflow, centralized decoder/layout rules,
platform and GPU capability boundaries, licensing policy, commands, proprietary
asset hygiene, and completion checklist. It must describe diagnostic flags as
behavior-invalidating tools rather than compatibility modes.

- [x] **Step 2: Add the generic runner**

```lua
local guest_root = arg[1]
if guest_root == nil or guest_root == '' then
	error('usage: fc_script scripts/run_guest.lua <guest-root>')
end

local cfg = {
	ScreenWidth = 1280;
	ScreenHeight = 720;
	Neo = true;
	VulkanValidationEnabled = false;
	ShaderValidationEnabled = false;
	ShaderOptimizationType = 'Performance';
	ShaderLogDirection = 'Silent';
	CommandBufferDumpEnabled = false;
	PrintfDirection = 'Console';
	ProfilerDirection = 'None';
}

kyty_init(cfg)
kyty_mount(guest_root, '/app0')
kyty_load_elf('/app0/eboot.bin')

for _, module in ipairs({
	'libc_1', 'libc_internal_1', 'libkernel_1', 'libVideoOut_1',
	'libSysmodule_1', 'libDiscMap_1', 'libGraphicsDriver_1', 'libPad_1',
	'libAudio_1', 'libUserService_1', 'libSystemService_1',
	'libAppContent_1', 'libSaveData_1', 'libDialog_1', 'libNet_1',
	'libPlayGo_1'
}) do
	kyty_load_symbols(module)
end

kyty_execute()
```

- [x] **Step 3: Verify tracked-content hygiene and runner argument validation**

Run:

```bash
git grep -n -F "$HOME" -- AGENTS.md scripts docs/superpowers/plans
_build_macos/fc_script scripts/run_guest.lua
```

Expected: the search returns no matches; the runner exits with the usage error
before attempting to open a guest executable.

- [x] **Step 4: Build**

Run: `ninja -C _build_macos`

Expected: exit 0.

- [x] **Step 5: Commit**

```bash
git add AGENT.md AGENTS.md scripts/run_guest.lua
git commit -m "docs: centralize portable emulator workflow"
```

### Task 2: Shared PM4 context-state decoders

**Files:**
- Create: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Create: `source/emulator/src/Graphics/GraphicsState.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRun.cpp`
- Modify: `source/unit_test/CMakeLists.txt`
- Modify: `source/unit_test/src/UnitTest.cpp`
- Create: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: raw context-register values and `HW::Context&`.
- Produces: `State::SetGenericScissorTl`, `SetGenericScissorBr`,
  `SetModeControl`, and `SetBlendControl`.

- [x] **Step 1: Write failing state-decoder tests**

```cpp
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Pm4.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorGraphicsState);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsState, DecodesObservedIndirectContextState)
{
	HW::Context context;

	State::SetGenericScissorTl(context, 0x80020001u);
	State::SetGenericScissorBr(context, 0x00140010u);
	State::SetModeControl(context, 0x00080007u);
	State::SetBlendControl(context, 0, 0x40010605u);

	const auto& viewport = context.GetScreenViewport();
	EXPECT_EQ(viewport.generic_scissor_left, 1);
	EXPECT_EQ(viewport.generic_scissor_top, 2);
	EXPECT_EQ(viewport.generic_scissor_right, 16);
	EXPECT_EQ(viewport.generic_scissor_bottom, 20);
	EXPECT_FALSE(viewport.generic_scissor_window_offset_enable);

	const auto& mode = context.GetModeControl();
	EXPECT_TRUE(mode.cull_front);
	EXPECT_TRUE(mode.cull_back);
	EXPECT_TRUE(mode.face);
	EXPECT_TRUE(mode.provoking_vtx_last);

	const auto& blend = context.GetBlendControl(0);
	EXPECT_TRUE(blend.enable);
}

UT_END();
```

Add `src/emulator/*.cpp` to the unit-test glob, add
`${CMAKE_SOURCE_DIR}/emulator/include` as a private include directory, and add
`UT_LINK(EmulatorGraphicsState);` to `UnitTest.cpp`.

- [x] **Step 2: Run the filtered test and verify failure**

Run:

```bash
cmake -S source -B _build_macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C _build_macos
_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=EmulatorGraphicsState.*
```

Expected: compilation fails because `GraphicsState.h` or the `State` functions
do not yet exist.

- [x] **Step 3: Implement focused decoders**

`GraphicsState.h` declares the four functions under
`Kyty::Libs::Graphics::State`. `GraphicsState.cpp` decodes fields exclusively
through the constants in `Pm4.h` and updates the existing `HW::Context` setters.
The TL and BR scissor registers update their halves independently so indirect
register ordering does not destroy state.

- [x] **Step 4: Route direct and indirect packets through the shared functions**

Replace field decoding inside `hw_ctx_set_generic_scissor`,
`hw_ctx_set_mode_control`, and `hw_ctx_set_blend_control` with calls to the
shared functions. Register these exact indirect handlers:

```cpp
g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_TL] =
    [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetGenericScissorTl(*cp->GetCtx(), value); };
g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_BR] =
    [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetGenericScissorBr(*cp->GetCtx(), value); };
g_hw_ctx_indirect_func[Pm4::PA_SU_SC_MODE_CNTL] =
    [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetModeControl(*cp->GetCtx(), value); };
g_hw_ctx_indirect_func[Pm4::CB_BLEND0_CONTROL] =
    [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetBlendControl(*cp->GetCtx(), 0, value); };
```

- [x] **Step 5: Run focused tests and build**

Run:

```bash
ninja -C _build_macos
_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=EmulatorGraphicsState.*
```

Expected: filtered suite passes and the build exits 0.

- [ ] **Step 6: Commit**

```bash
git add source/emulator/include/Emulator/Graphics/GraphicsState.h \
  source/emulator/src/Graphics/GraphicsState.cpp \
  source/emulator/src/Graphics/GraphicsRun.cpp source/unit_test
git commit -m "graphics: share direct and indirect state decoders"
```

### Task 3: Checked direct-memory release contract

**Files:**
- Modify: `source/emulator/include/Emulator/Kernel/Memory.h`
- Modify: `source/emulator/src/Kernel/Memory.cpp`
- Modify: `source/emulator/src/Libs/LibKernel.cpp`
- Create: `source/unit_test/src/emulator/UnitTestEmulatorKernelMemory.cpp`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Consumes: physical start address and allocation length.
- Produces: `KernelCheckedReleaseDirectMemory(int64_t, size_t)` and NID
  `hwVSPCmp5tM`; invalid arguments return `KERNEL_ERROR_EINVAL`, absent ranges
  return `KERNEL_ERROR_ENOENT`.

- [x] **Step 1: Write the failing contract test**

```cpp
TEST(EmulatorKernelMemory, CheckedReleaseReportsGuestErrors)
{
	int64_t address = 0;
	ASSERT_EQ(Kernel::Memory::KernelAllocateDirectMemory(
	              0x10000, 0x40000, 0x10000, 0x10000, 12, &address),
	          LibKernel::KERNEL_OK);
	EXPECT_EQ(Kernel::Memory::KernelCheckedReleaseDirectMemory(address, 0x10000),
	          LibKernel::KERNEL_OK);
	EXPECT_EQ(Kernel::Memory::KernelCheckedReleaseDirectMemory(address, 0x10000),
	          LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(Kernel::Memory::KernelCheckedReleaseDirectMemory(address, 0),
	          LibKernel::KERNEL_ERROR_EINVAL);
}
```

Register it with `UT_LINK(EmulatorKernelMemory);`.

- [x] **Step 2: Run the filtered test and verify failure**

Run: `_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=EmulatorKernelMemory.*`

Expected: build failure for the missing checked-release declaration.

- [x] **Step 3: Implement guest-error behavior**

Add `KernelCheckedReleaseDirectMemory`. Share the actual release operation with
`KernelReleaseDirectMemory`, but return `KERNEL_ERROR_ENOENT` instead of aborting
when `PhysicalMemory::Release` rejects the exact range. Preserve GPU wait/free
and callbacks for successful mapped allocations.

Register:

```cpp
LIB_FUNC("hwVSPCmp5tM", Memory::KernelCheckedReleaseDirectMemory);
```

- [x] **Step 4: Run tests and build**

Run:

```bash
ninja -C _build_macos
_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=EmulatorKernelMemory.*
```

Expected: focused test passes.

- [ ] **Step 5: Commit**

```bash
git add source/emulator/include/Emulator/Kernel/Memory.h \
  source/emulator/src/Kernel/Memory.cpp source/emulator/src/Libs/LibKernel.cpp \
  source/unit_test
git commit -m "kernel: implement checked direct memory release"
```

### Task 4: Confirmed Gen5 SH-register and compute-dispatch exports

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/Graphics.h`
- Modify: `source/emulator/src/Graphics/Graphics.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRun.cpp`
- Modify: `source/emulator/src/Libs/LibGraphicsDriver.cpp`
- Create: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`
- Modify: `source/unit_test/src/UnitTest.cpp`

**Interfaces:**
- Produces: `GraphicsCbSetShRegistersDirect` for a list of register/value pairs,
  `GraphicsCbDispatch` for standard `IT_DISPATCH_DIRECT`, and a shared dispatch
  decoder accepting both standard and existing custom packet envelopes.

- [x] **Step 1: Add failing packet-encoder tests**

Test pure helpers that encode into caller-provided DWORD spans. For contiguous
SH registers `{0x20c, A}, {0x20d, B}`, assert a four-DWORD `IT_SET_SH_REG`
packet. For dispatch `(2, 3, 4, 0)`, assert five DWORDs and mode `0x41` after
applying the verified modifier mask.

- [x] **Step 2: Verify the focused test fails**

Run: `_build_macos/fc_script "{kyty_run_tests()}" --gtest_filter=EmulatorGraphicsPackets.*`

Expected: missing encoder/helper symbols.

- [x] **Step 3: Implement packet helpers and HLE wrappers**

The SH helper sorts a copied register list, groups only consecutive offsets,
and emits one `IT_SET_SH_REG` packet per group. It rejects zero entries,
overflow, and offsets outside the SH register space. The HLE wrapper reserves
the exact total size once, then encodes.

The dispatch helper emits:

```cpp
cmd[0] = KYTY_PM4(5, Pm4::IT_DISPATCH_DIRECT, 0);
cmd[1] = group_x;
cmd[2] = group_y;
cmd[3] = group_z;
cmd[4] = (modifier & 0xA038u) | 0x41u;
```

Register the confirmed NIDs in `LibGraphicsDriver.cpp`.

- [x] **Step 4: Centralize dispatch parsing**

Register `IT_DISPATCH_DIRECT` in `g_cp_op_func`. Make the standard and custom
envelopes validate their own header/length and call one function that forwards
the four decoded values to `CommandProcessor::DispatchDirect`.

- [x] **Step 5: Run focused tests and build**

Run:

```bash
ninja -C _build_macos
_build_macos/fc_script "{kyty_run_tests()}" \
  --gtest_filter=EmulatorGraphicsPackets.*:EmulatorGraphicsState.*
```

Expected: focused suites pass.

- [ ] **Step 6: Commit**

```bash
git add source/emulator/include/Emulator/Graphics/Graphics.h \
  source/emulator/src/Graphics/Graphics.cpp \
  source/emulator/src/Graphics/GraphicsRun.cpp \
  source/emulator/src/Libs/LibGraphicsDriver.cpp source/unit_test
git commit -m "graphics: implement confirmed Gen5 command exports"
```

### Task 5: Strict integration frontier and next resource-layout plan

**Files:**
- Modify only files implicated by the newly observed strict failure.
- Do not track the local fixture, output logs, screenshots, or generated shaders.

**Interfaces:**
- Consumes: `scripts/run_guest.lua` and an untracked local fixture path.
- Produces: an exact strict frontier or 120 correct consecutive flips.

- [ ] **Step 1: Run strict without diagnostic relaxations**

Run locally:

```bash
_build_macos/fc_script scripts/run_guest.lua "$KYTY_GUEST_ROOT"
```

Expected: no call to the four implemented missing paths and no unknown-register
error for `0x90`, `0x91`, `0x205`, or `0x1e0`.

- [ ] **Step 2: Classify the first new frontier**

Record the first unsupported import, packet, register, shader instruction,
surface descriptor, Vulkan error, crash address, or visual divergence. Trace it
backward to the producer before changing code.

- [ ] **Step 3: If strict reaches presentation, capture visual evidence**

Verify presentation extent, geometry, colors, and stability. A non-black pixel
count is insufficient. Require a recognizable correctly proportioned image and
120 completed flips.

- [ ] **Step 4: Write the next evidence-specific plan**

If the frontier is surface layout, the next plan must include captured sanitized
format, dimensions, pitch, mip count, tile mode, sample count, expected offsets,
and the exact table/fallback removal. If it is another subsystem, plan only that
root cause with its failing test.

- [ ] **Step 5: Run regression verification**

Run:

```bash
git diff --check
ninja -C _build_macos
_build_macos/fc_script "{kyty_run_tests()}" \
  --gtest_filter=EmulatorGraphicsState.*:EmulatorKernelMemory.*:EmulatorGraphicsPackets.*
```

Expected: all commands pass. Report the pre-existing unfiltered `Core.DateTime`
failure separately.

## Deviations

## Deviations
- Restored GraphicsCbReleaseMem to 7-dword `0xc0051060` (encoder had diverged to 8 dwords; CP/tests still 7).
- Verification on Linux `_build_linux` (host has no `_build_macos`).
- Strict `run_guest.lua` deferred: `KYTY_GUEST_ROOT` unset in this environment.
- Local commit only; no push.
