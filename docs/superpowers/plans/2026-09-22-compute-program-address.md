# Compute program address implementation plan

> Use the user's existing Luna Max collaboration workflow with root integration,
> serialized builds and independent review. No new worktree or commit.

**Goal:** Translate compute S_GETPC_B64 with the current dispatch's program address.

**Architecture:** Append one aligned metadata block to the existing push/UBO
transport. Cache only its layout, never its address. Emit a two-word unsigned
addition of the runtime base and the next instruction's byte offset. Graphics
GETPC remains unsupported because fused instruction offsets are not original PCs.

**Tech stack:** C++, existing SPIR-V generator, Vulkan numeric compute harness.

**Spec:** AGENTS.md and docs/BRINGUP.md strict bring-up invariants; AMD RDNA2
SOP1 S_GETPC_B64 contract D.u64 = PC + 4.

## Constraints and review focus

- Preserve dirty main and all unrelated user changes; no private data in git.
- Root owns production integration and -j2 builds; writers own disjoint tests.
- Keep emitter and new tests in small focused files.
- Verify relocation through the same shader module/pipeline, including carry.
- Preserve SCC and EXEC; reject unsupported destination pairs and graphics use.
- Validate the full metadata span before writing; exercise push and UBO layouts.
- Cache keys include presence/layout, not the runtime address.

## Task 1: Runtime GETPC contract

Files: Shader.h, Shader.cpp, ShaderParseSOP1.cpp, ShaderSpirvGenerator.cpp,
ShaderSpirvDispatch.cpp, ShaderSpirvEmitters.h, ShaderTranslationCache.h,
GraphicsRenderBind.cpp, GraphicsRenderInternal.h; new ShaderProgramAddress.h/.cpp
and ShaderSpirvProgramAddress.cpp; focused UnitTestEmulatorShaderGetpc.cpp;
existing modular shader_compute integration harness.

Interface: ShaderBindResources gains program_base_used, program_base_offset_dw,
program_base. ShaderCalcBindingIndices appends 16 bytes when used. The shared
bool ShaderWriteProgramBaseMetadata(const ShaderBindResources&, uint32_t*, uint32_t)
validates capacity and packs low/high/zero/zero without partial writes on failure.

- [x] Compile and observe the focused real-parser regression fail on the current
      one-word destination/source-bearing placeholder.
- [x] Parse opcode 0x1f as SGetpcB64, format Sdst2, destination size2, no sources.
      Do not consume a literal from its unused source field.
- [x] Populate compute metadata from current cs_regs.data_addr only when the
      parsed shader contains GETPC; append the layout flag/offset to bind identity.
- [x] Pack the current address through the same helper in BindDescriptors and
      the numeric harness. Increase bounded metadata capacity by four words.
- [x] Emit base-low plus (pc+4), carry, base-high plus carry, two destination
      stores; no SCC/EXEC writes and no Int64 capability requirement.
- [x] Validate generated push and spilled-UBO modules. Dispatch one cached
      pipeline with bases 0x1fffffffc and 0x1234567800000010; expected pairs
      {0,2} and {0x14,0x12345678} for instruction PC0.
- [x] Independent Luna Max review of actual task diff, then root build,
      focused regressions, both GPU integrations and strict guest run.

Commands use the existing root-owned build directory and external evidence area.
No launch, frame or narrower test result counts as gameplay acceptance.
