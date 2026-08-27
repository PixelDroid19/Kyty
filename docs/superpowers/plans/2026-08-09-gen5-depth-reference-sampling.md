# Gen5 Depth-Reference Sampling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Execute inline; do not delegate tasks.

**Goal:** Carry a proven Gen5 depth-reference sample from shader analysis through comparison-enabled Vulkan sampler creation and valid SPIR-V emission, then advance the strict runtime frontier.

**Architecture:** Classify sampler operation from each instruction consumer and store it with the materialized sampler resource. Use that operation in sampler-cache identity and Vulkan creation, require a depth-compatible sampled view, and emit the existing decoded address contract as an explicit level-zero depth-reference sample.

**Tech Stack:** C++17, Vulkan 1.x, SPIR-V assembly generation, GoogleTest, CMake/Ninja, native Kyty integration diagnostics.

## Global Constraints

- Keep strict unsupported behavior; do not add fallback images, samplers, formats, or skipped instructions.
- Preserve regular sampling and reject mixed regular/depth-reference use of one sampler register.
- Keep private workload identifiers, local paths, captures, and external implementation names out of tracked text and commits.
- Use the existing build directory only for baseline evidence; configure `_build_linux_gen5_dref` for this implementation.
- Commit and push each independently green task before starting the next semantic change.

---

### Task 1: Classify sampler consumers

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/include/Emulator/Graphics/Shader.h`
- Modify: `source/emulator/src/Graphics/ShaderStorageAnalysis.h`
- Modify: `source/emulator/src/Graphics/ShaderStorageAnalysis.cpp`
- Modify: `source/emulator/src/Graphics/ShaderResources.cpp`
- Modify: `source/emulator/src/Graphics/Shader.cpp`
- Test: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`

**Interfaces:**
- Produces: `ImageSampleOperation::{Regular,DepthReference,Mixed}`.
- Produces: `AnalyzeShaderSamplerOperation(const ShaderCode&, int start_register)` returning the operation evidenced by instructions whose sampler operand begins at `start_register`.
- Produces: `ShaderSamplerResources::operations[RES_MAX]`, aligned with `samplers[index]`.
- Consumes: `ShaderInstructionType::ImageSampleDrefLz` as depth-reference evidence; all other sampler-consuming image instructions as regular evidence.

- [ ] **Step 1: Write the failing direct-analysis tests**

Add three focused cases to `UnitTestEmulatorGraphicsState.cpp` using constructed `ShaderInstruction` values:

```cpp
EXPECT_EQ(AnalyzeShaderSamplerOperation(code_with_regular_sample, 20), ImageSampleOperation::Regular);
EXPECT_EQ(AnalyzeShaderSamplerOperation(code_with_dref_sample, 20), ImageSampleOperation::DepthReference);
EXPECT_EQ(AnalyzeShaderSamplerOperation(code_with_both_samples, 20), ImageSampleOperation::Mixed);
```

Each instruction must use `src[2] = {Sgpr, 20, 4}` so the test exercises the real operand association.

- [ ] **Step 2: Run the focused test and capture RED**

Run:

```sh
cmake -S source -B _build_linux_gen5_dref -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C _build_linux_gen5_dref kyty_unit_test
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsState.ClassifiesSamplerOperation*'
```

Expected: compile failure because `AnalyzeShaderSamplerOperation` and `Mixed` do not exist.

- [ ] **Step 3: Implement classification and propagation**

Add `Mixed` to the existing enum. Implement one scan that ignores unrelated sampler registers, merges equal evidence, and returns `Mixed` only when both operations consume the same four-SGPR range. Store the result for static/direct samplers; carry the consuming operation through `ShaderDynamicSLoadUse` for dynamic descriptors. Reject `Mixed` before shader generation with a structured diagnostic containing only stage/register/operation data.

- [ ] **Step 4: Run focused GREEN and neighboring resource tests**

Run:

```sh
ninja -C _build_linux_gen5_dref kyty_unit_test
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsState.ClassifiesSamplerOperation*:EmulatorGraphicsState.Gen5DirectImageSampleBindsTextureAndSampler:EmulatorGraphicsPackets.ParsesImageSampleDrefLzAddressContract'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit and push**

```sh
git add source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/include/Emulator/Graphics/Shader.h source/emulator/src/Graphics/ShaderStorageAnalysis.h source/emulator/src/Graphics/ShaderStorageAnalysis.cpp source/emulator/src/Graphics/ShaderResources.cpp source/emulator/src/Graphics/Shader.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp
git commit -m "fix(graphics): classify Gen5 sampler operations"
git push origin feature/gen5-3d-rendering
```

### Task 2: Create comparison samplers without cache aliasing

**Files:**
- Modify: `source/emulator/src/Graphics/GraphicsRenderInternal.h`
- Modify: `source/emulator/src/Graphics/GraphicsRenderPipeline.cpp`
- Modify: `source/emulator/src/Graphics/GraphicsRenderBind.cpp`
- Modify: `source/emulator/include/Emulator/Graphics/GraphicsState.h`
- Modify: `source/emulator/src/Graphics/GraphicsState.cpp`
- Test: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`
- Test: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`

**Interfaces:**
- Changes: `SamplerCache::GetSamplerId(const ShaderSamplerResource&, State::ImageSampleOperation)`.
- Changes: cache `Sampler` stores `operation` and compares it as part of identity.
- Produces: `ResolveSamplerCompareOp(uint8_t)` mapping descriptor values 0 through 7 to the corresponding `VkCompareOp` semantic used during sampler creation.
- Consumes: `ShaderSamplerResources::operations[index]` from Task 1.

- [ ] **Step 1: Write failing sampler-state tests**

Add assertions that depth-reference operation enables comparison for all eight encoded functions while regular operation disables it. Extend the graphics diagnostic to request the same descriptor twice with `Regular` and `DepthReference` and assert distinct sampler IDs plus a reused ID for two identical depth-reference requests.

- [ ] **Step 2: Run the focused test and capture RED**

Run:

```sh
ninja -C _build_linux_gen5_dref kyty_unit_test kyty_graphics_diagnostics_integration
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsState.ResolvesDepthReferenceSamplerComparison'
_build_linux_gen5_dref/integration_test/kyty_graphics_diagnostics_integration
```

Expected: compile failure at the old one-argument `GetSamplerId` interface or failed identity assertion.

- [ ] **Step 3: Implement operation-aware sampler creation**

Change lookup and insertion to compare descriptor words plus operation. Set `compareEnable` only for `DepthReference` and map the descriptor function to Vulkan's eight compare operations. If unnormalized coordinates disable comparison, return a structured failure before calling `vkCreateSampler`; never create a regular substitute.

- [ ] **Step 4: Run focused GREEN**

Repeat the two commands from Step 2. Expected: unit and live Vulkan diagnostic pass.

- [ ] **Step 5: Commit and push**

```sh
git add source/emulator/src/Graphics/GraphicsRenderInternal.h source/emulator/src/Graphics/GraphicsRenderPipeline.cpp source/emulator/src/Graphics/GraphicsRenderBind.cpp source/emulator/include/Emulator/Graphics/GraphicsState.h source/emulator/src/Graphics/GraphicsState.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp source/integration_test/src/GraphicsDiagnosticsIntegration.cpp
git commit -m "fix(graphics): bind comparison-enabled Gen5 samplers"
git push origin feature/gen5-3d-rendering
```

### Task 3: Emit validated depth-reference SPIR-V

**Files:**
- Modify: `source/emulator/src/Graphics/ShaderSpirvImage.cpp`
- Test: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp`

**Interfaces:**
- Consumes: `ImageSampleDrefLz`, its decoded dimension, one-component mask, and operation-aware sampler binding from Tasks 1 and 2.
- Produces: `OpImageSampleDrefExplicitLod` with `Lod %float_0_000000` for 2D, 2D-array, and cube shapes.

- [ ] **Step 1: Replace the rejection fixture with positive and strict-negative cases**

For each supported shape, construct the existing shader fixture with `input.bind.samplers.operations[0] = ImageSampleOperation::DepthReference`. Assert source contains `OpImageSampleDrefExplicitLod`, `Lod %float_0_000000`, and the expected vector width. Keep negative death cases for regular sampler operation, non-`0x1` mask, and three-dimensional descriptor shape.

- [ ] **Step 2: Run the focused test and capture RED**

Run:

```sh
ninja -C _build_linux_gen5_dref kyty_unit_test
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsPackets.MaterializesImageSampleDrefLz*:EmulatorGraphicsPackets.RejectsImageSampleDrefLz*'
```

Expected: positive cases die through the missing emitter.

- [ ] **Step 3: Implement the minimal emitter**

Load depth reference from address component zero and coordinates from the remaining components. Use the flat sampled-image type for 2D and the array sampled-image type for 2D array/cube. Emit a scalar float result and store it to destination component zero. Return `false` for any unsupported dimension, mask, operand type, missing binding, or non-depth-reference sampler operation.

- [ ] **Step 4: Run focused GREEN and SPIR-V validation**

Run:

```sh
ninja -C _build_linux_gen5_dref kyty_unit_test
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsPackets.ParsesImageSampleDrefLzAddressContract:EmulatorGraphicsPackets.MaterializesImageSampleDrefLz*:EmulatorGraphicsPackets.RejectsImageSampleDrefLz*'
ctest --test-dir _build_linux_gen5_dref --output-on-failure -R 'Spirv|GraphicsDiagnostics'
```

Expected: all selected tests and registered validators pass.

- [ ] **Step 5: Commit and push**

```sh
git add source/emulator/src/Graphics/ShaderSpirvImage.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsPackets.cpp
git commit -m "fix(graphics): emit Gen5 depth-reference samples"
git push origin feature/gen5-3d-rendering
```

### Task 4: Enforce depth-view compatibility

**Files:**
- Modify: `source/emulator/include/Emulator/Graphics/Shader.h`
- Modify: `source/emulator/src/Graphics/GraphicsRenderBind.cpp`
- Test: `source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp`
- Test: `source/integration_test/src/GraphicsDiagnosticsIntegration.cpp`

**Interfaces:**
- Consumes: sampler operation and texture descriptor index already paired by the instruction stream.
- Produces: a pure compatibility decision for depth-reference operation, sampled image shape, numeric type, and resolved Vulkan image/view kind.

- [ ] **Step 1: Write failing compatibility tests**

Require depth-reference 2D and array descriptors with depth views to pass. Require color, unsigned-integer, three-dimensional, missing-view, and regular/depth mismatches to fail. Add one live diagnostic that binds a depth image and confirms descriptor update succeeds with the comparison sampler.

- [ ] **Step 2: Run tests and capture RED**

Run:

```sh
ninja -C _build_linux_gen5_dref kyty_unit_test kyty_graphics_diagnostics_integration
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsState.ResolvesDepthReferenceImageView*'
_build_linux_gen5_dref/integration_test/kyty_graphics_diagnostics_integration
```

Expected: missing compatibility interface or acceptance of a color view.

- [ ] **Step 3: Implement strict view selection**

Select only `VIEW_DEPTH_TEXTURE` or `VIEW_DEPTH_TEXTURE_ARRAY` for depth-reference consumers, preserve regular view selection unchanged, and reject before descriptor update when the proven depth view is absent or the shape/numeric type is incompatible.

- [ ] **Step 4: Run focused and neighboring GREEN**

Repeat Step 2 and run:

```sh
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsState.*Sampler*:EmulatorGraphicsPackets.*ImageSample*'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit and push**

```sh
git add source/emulator/include/Emulator/Graphics/Shader.h source/emulator/src/Graphics/GraphicsRenderBind.cpp source/unit_test/src/emulator/UnitTestEmulatorGraphicsState.cpp source/integration_test/src/GraphicsDiagnosticsIntegration.cpp
git commit -m "fix(graphics): require depth views for comparison samples"
git push origin feature/gen5-3d-rendering
```

### Task 5: Strict runtime and catalog acceptance

**Files:**
- Modify only if the frontier advances and a sanitized evidence update is needed: `docs/kyty-runtime-graphics-investigation-handoff.md`
- Keep logs, captures, endpoints, guest roots, and shader dumps outside the repository.

**Interfaces:**
- Consumes: the built `fc_script`, native agent, scripted controller edges, capture scorer, and the same strict guest transition used for the red runtime.
- Produces: a captured later first failure or a verified in-world 3D checkpoint, plus three catalog regression results.

- [ ] **Step 1: Build the affected runtime targets**

```sh
ninja -C _build_linux_gen5_dref fc_script kyty_agent kyty_unit_test kyty_graphics_diagnostics_integration
```

Expected: all targets link without warnings introduced by this change.

- [ ] **Step 2: Run complete focused graphics validation**

```sh
_build_linux_gen5_dref/kyty_unit_test --gtest_filter='EmulatorGraphicsPackets.*:EmulatorGraphicsState.*'
ctest --test-dir _build_linux_gen5_dref --output-on-failure -R 'GraphicsDiagnostics|Spirv'
python3 scripts/check_graphics_tables.py
git diff --check
```

Expected: every selected test and gate passes.

- [ ] **Step 3: Repeat the exact strict first-failure route**

Launch with `PrintfDirection=Silent`, no permissive flags, an untracked endpoint/capture directory, and the private root supplied only through `KYTY_GUEST_ROOT`. Deliver one deliberate input edge through the native agent, wait for continued presents, capture, score, and query `last-error`.

Expected: no `ImageSampleDrefLz` emitter failure. Record the first later failure verbatim in scratch, or record a native in-world 3D capture with continued presents and visible scene change after input.

- [ ] **Step 4: Run strict catalog regression**

For each established 2D fixture, use its documented reproducible input route, capture the gameplay scene, verify continued presents and effective input, and compare Silent-mode frame rate against its recorded frontier. Any regression blocks publication of the implementation commits.

- [ ] **Step 5: Review publication safety and publish the final frontier**

```sh
git status --short
git diff --check
git log --format='%H %s%n%b' main..HEAD
git diff main...HEAD -- . ':!docs/superpowers/specs/2026-08-09-gen5-depth-reference-sampling-design.md' ':!docs/superpowers/plans/2026-08-09-gen5-depth-reference-sampling.md'
```

Scan added text for private identifiers, local paths, protected artifacts, and external implementation names. If a sanitized frontier note is warranted, commit it separately as `docs(graphics): record depth-sampling frontier`, push the branch, then verify the remote branch hash equals local `HEAD`.
