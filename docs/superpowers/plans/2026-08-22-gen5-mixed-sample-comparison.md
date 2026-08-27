# Gen5 Mixed Sample Comparison Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Execute inline and serialize all writers.

**Goal:** Allow one Gen5 image/sampler descriptor to serve regular samples and `SAMPLE_C_LZ` without misclassifying a color image as a Vulkan depth image.

**Architecture:** Keep `ImageSampleOperation::Mixed` as instruction-use evidence. Bind Mixed resources through the ordinary typed image and non-comparison sampler paths because `Recompile_ImageSampleDrefLz` already performs the guest comparison in SPIR-V ALU; reserve the depth-image array for descriptors proven to have a depth view.

**Tech Stack:** C++17, Vulkan, SPIR-V generation, GoogleTest, Ninja, native `kyty_agent` capture.

**Spec:** `docs/BRINGUP.md`

## Global Constraints

- Work only in `Kyty-gen5-3d-world`; preserve the existing seven-file HTILE diff.
- No permissive flags, dummy resources, format substitutions, title/checksum/address conditions, or external code copying.
- Keep the semantic scope to Mixed image/sampler binding and the shader-cache identity it changes. Do not alter D16, HTILE, DCC, position exports, or interpolation.
- Keep builds at `-j2` inside `MemoryHigh=2G`, `MemoryMax=3G`, `MemorySwapMax=0`; never run build and guest together.
- Runtime evidence stays under ignored scratch. Do not commit private paths, captures, logs, or workload identifiers.

---

### Task 1: Freeze the Mixed binding contract

**Files:**
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Consumes: `ResolveDepthReferenceImageView`, `State::ImageSampleOperation`.
- Produces: regression evidence that floating-point Mixed 2D/array color/depth views use ordinary typed sampling and that incompatible numeric types, shapes and views remain rejected.

- [ ] **Step 1: Change the stale Mixed expectation to the captured contract**

In `ResolvesDepthReferenceImageViewCompatibility`, require Mixed descriptors to preserve the ordinary compatible 2D and array view classes. Keep integer and 3D Mixed resources, missing views, shape/view mismatches, and non-floating pure depth-reference images rejected; the implemented Dref emitter supports floating-point 2D and 2D-array operands only.

- [ ] **Step 2: Add the host-sampler binding expectation**

Add `MixedImageSamplingUsesRegularHostSampler` and assert that a new pure `ResolveSamplerBindingOperation` maps `Regular`, `DepthReference`, and `Mixed` to `Regular`. This protects the existing ALU-comparison emitter from accidental Vulkan comparison-sampler binding.

- [ ] **Step 3: Run RED**

```sh
systemd-run --user --scope -p MemoryHigh=2G -p MemoryMax=3G -p MemorySwapMax=0 -p CPUQuota=200% \
  ninja -C _build_linux_gen5_world -j2 kyty_unit_test
```

Expected: compile failure because `ResolveSamplerBindingOperation` does not exist, or the Mixed view assertion fails under the old rejection.

### Task 2: Align binding with the existing SPIR-V contract

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/Shader.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderBind.cpp`
- Modify: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`

**Interfaces:**
- Produces: `State::ResolveSamplerBindingOperation(ImageSampleOperation)` and a complete shader identity for the selected texture sample operation.
- Preserves: `ImageSampleOperation::Mixed` in shader metadata; ordinary typed image arrays and ALU comparison for Mixed consumers.

- [ ] **Step 1: Implement the pure sampler resolver**

Return `Regular` for all current operations. `DepthReference` and `Mixed` must use a non-comparison Vulkan sampler because `SAMPLE_C_LZ` samples explicitly and applies `DEPTH_COMPARE_FUNC` in generated ALU.

- [ ] **Step 2: Admit Mixed views through the ordinary image compatibility branch**

Treat floating-point 2D/array `Mixed` resources like ordinary typed images in `ResolveDepthReferenceImageView`; do not reinterpret `Color2D` as depth. Preserve strict rejection for integer resources, 3D and missing/incompatible views.

- [ ] **Step 3: Use the resolver in `PrepareSamplers`**

Remove the unconditional Mixed exit and pass the pure resolved operation to `SamplerCache::GetSamplerId`. Leave the cache's direct Mixed rejection intact so all callers must normalize deliberately.

- [ ] **Step 4: Run GREEN and neighboring shader tests**

Include each texture descriptor's `sample_operation` in `ShaderGetBindIds`, and prove that otherwise-identical `Regular`, `DepthReference`, and `Mixed` descriptors produce distinct shader identities.

- [ ] **Step 5: Run GREEN and neighboring shader tests**

```sh
_build_linux_gen5_world/kyty_unit_test --gtest_filter='EmulatorGraphicsState.ResolvesDepthReferenceImageViewCompatibility:EmulatorGraphicsState.MixedImageSamplingUsesRegularHostSampler:EmulatorGraphicsPackets.MaterializesImageSampleDrefLzForDepthReferenceSamplers:EmulatorGraphicsState.ClassifiesSamplerOperation*'
```

Expected: all selected tests pass.

### Task 3: Verify the strict runtime frontier

**Files:**
- Modify only if the runtime advances: `docs/kyty-runtime-graphics-investigation-handoff.md`

**Interfaces:**
- Consumes: rebuilt `fc_script`, native agent, two-step menu route, native capture and score.
- Produces: removal of the exact Mixed BC1 binding exit and either a later structured failure or a captured 3D checkpoint.

- [ ] **Step 1: Build affected targets under the bounded cgroup**

```sh
systemd-run --user --scope -p MemoryHigh=2G -p MemoryMax=3G -p MemorySwapMax=0 -p CPUQuota=200% \
  ninja -C _build_linux_gen5_world -j2 fc_script kyty_unit_test
```

- [ ] **Step 2: Run focused graphics regression and gates**

```sh
_build_linux_gen5_world/kyty_unit_test --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
python3 scripts/check_graphics_tables.py --self-test
git diff --check
```

- [ ] **Step 3: Repeat the exact strict route**

Use `PrintfDirection=Silent`, the native agent and ignored capture directory. Capture `PRESS X`, deliver the bounded menu inputs, require continued presents, capture the first 3D scene, score it, and query `last-error`.

Expected: the old `unsupported depth-reference image binding ... operation=2 ... format=169 tile=5` exit is absent. A later strict exit becomes the next frontier; a capture is visual evidence only, not playability by itself.

- [ ] **Step 4: Review scope and record the result**

Inspect the complete diff, ensure no private identifiers or reference-project names entered tracked files, and update the sanitized handoff with the disproven “depth-reference implies depth view” assumption only after the runtime result is known. Do not commit or push until the existing HTILE diff and this change have one coherent runtime verdict.
