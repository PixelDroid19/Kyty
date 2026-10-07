# Fragment wave compiler implementation plan

> Execute inline with `superpowers:executing-plans`. The user requires one agent and preservation of the current checkout.

**Goal:** Translate guest pixel IR to a compute module that retains its architectural 64-lane execution and pixel metadata.

**Architecture:** Keep guest stage Pixel and choose host stage Compute explicitly. Consume a bounded host transport containing allocated lanes, initial EXEC, coverage, raw initial VGPR words and raw interpolation parameter triples. Produce masked MRT records for a later native resolve. This compiler interface does not enable a renderer strategy.

**Tech stack:** C++, SPIR-V, Vulkan 1.4, paired 64-on-32 execution.

**Spec:** `docs/fragment-wave-execution.md`.

## Global constraints

- Keep native translation behavior and guest Pixel metadata intact.
- Require compute linear derivative groups and full physical subgroups of 32 when executing the generated module.
- Capture must supply actual P10, P20 and P0 words; do not infer them from final interpolants or invent helper inputs.
- Separate allocated lanes, initial EXEC and coverage. Clamp later EXEC to allocated lanes, allowing WQM helpers.
- Reject storage writes, atomics, interlock, barriers and unverified exports.
- Validate transport dimensions and shader buffer bounds; retain partial exports through component masks.
- Keep original GPU integration fixtures and protected captures outside Git. Unit tests remain deferred by user instruction.
- No runtime activation or compatibility claim before capture and resolve integration acceptance.

## Review focus

- Source/destination aliasing must load parameters and sources before stores.
- Inactive and helper destinations must not produce attachment writes.
- Compressed conversion shadows must remain separate for each bank.
- Multiple terminal blocks must converge on one output epilog.
- Truncated buffers and excess dispatch groups must not access out of bounds.

## Task 1: Explicit compiler context and admission

Files: `ShaderSpirv.h`, `ShaderSpirvInternal.h`, `ShaderSpirv.cpp`, `ShaderComputeWaveAnalysis.h/.cpp`, new `ShaderFragmentWaveAnalysis.cpp`.

- [x] Record the missing host-compute context and interpolation/export admission in an external replay.
- [x] Add `ShaderFragmentComputeInfo` and `SpirvGenerateFragmentComputeSource`, preserving guest Pixel IR and bindings.
- [x] Admit exact interpolation and compressed MRT tuples, multiple terminal blocks, and the existing read-only paired operations.
- [x] Reject observable writes, invalid modifiers, unsupported parameters and non-instruction branch targets.

## Task 2: Transport and instruction adapters

Files: new `ShaderSpirvFragment.cpp`, `ShaderSpirvGenerator.cpp`, `ShaderSpirvComputeWave.cpp`, `ShaderSpirvWaveGeneric.cpp`, `ShaderSpirvOperands.cpp`, `ShaderSpirvBlockDispatch.cpp`.

- [x] Emit compute scaffolding and host-only transport descriptors using the explicit context.
- [x] Validate input/output buffer lengths before the uniform wave begins; load both raw register banks and masks.
- [x] Resolve proven unobserved P1/P2 pairs from native attributes and parameter moves from captured P10/P20/P0 words under EXEC.
- [x] Store compressed MRT values and enabled-component masks under coverage and EXEC, then flush once at termination.
- [x] Keep packed-half shadows separate for both banks; retain subgroup participation during branches and kills.

## Task 3: Validation and evidence

- [x] Build the owned target with `ninja -j2`; execute no unit suites.
- [x] Assemble/validate the complete private pixel module for Vulkan 1.4 and inspect its descriptor layout.
- [x] Compare original GPU fixtures against independent interpolation/export references, including helpers, partial masks, both banks, multiple exits and undersized buffers.
- [ ] Review the complete diff, run boundaries/provenance gates, document exact limitations and commit only verified changes locally.

## Remaining integration

The renderer still needs compact capture, native resolve coverage/MRT/stencil gates and strategy selection. Usable performance, scored gameplay and two stable five-minute runs remain acceptance requirements.

### Interpolation ruling

Ruling: use the captured native final attribute for an unobserved P1/P2 pair with unchanged initial barycentrics. The ISA gives arithmetic expressions but does not resolve intermediate rounding here. Admission proves the two instructions share a destination/attribute/component, no intermediate consumer or execution change exists, and their I/J inputs remain the native initial registers. Other arithmetic interpolation remains unsupported. Parameter moves consume actual raw P10/P20/P0 words. Each attribute transport therefore uses sixteen words: four final native values followed by twelve raw parameter words.

### Validation evidence

The full private module has 286,670 words and passes Vulkan 1.4 assembly/validation. The transport replay passes 19 modules, 160 cases and 356,862 comparisons. The shared wave replay passes 320 cases and 42,064 register observations. Fifteen malformed admissions fail and the SGPR reuse control passes. Complete-module GPU dispatch and renderer performance remain unverified.
