#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ANALYSIS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class ShaderComputeWaveInstructionKind
{
	Unsupported,
	End,
	ScalarHint,
	ScalarCopy,
	ScalarShift,
	ScalarResourceLoad,
	ScalarMask,
	WaveBranch,
	BankedVector,
	BankedAlu,
	PackedMask,
	WaveLane,
	BankedLds,
	WorkgroupBarrier,
	SatisfiedWait,
	ScalarBufferLoad,
	BankedBufferLoad,
	BankedSdwaExtract,
	PackedExecMask,
	// Proven per-lane instruction emitted once per bank from its native emitter.
	BankedGeneric,
	// Per-lane LDS instruction emitted once per bank from its native emitter and
	// followed by a subgroup barrier, which restores in-wave DS ordering.
	BankedGenericLds,
	// Scalar instruction on uniform SGPR/VCC words emitted once from its native
	// emitter; paired mode keeps those registers in architectural form.
	ScalarGeneric,
	// Any VOPC compare: native per-lane predicate per bank, balloted into the
	// architectural two-word mask (EXEC only for v_cmpx).
	BankedGenericCompare,
	// v_mbcnt_{lo,hi}: lane-position masked bit count over the 64-lane wave.
	WaveCount,
	// ds_append/ds_consume on GDS: one per-wave counter update, broadcast.
	WaveAppend,
	// ds_write_addtid_b32/ds_read_addtid_b32: per-lane LDS slot addressed by
	// M0[15:0] + offset + TID*4; the emitter walks both banks itself because
	// the flat lane index differs per bank.
	BankedAddtidLds,
	// v_add_co_ci_u32 / v_subrev_co_ci_u32: banked result plus carry mask pair.
	BankedCarry,
	// DPP moves and bitwise ALU with architectural source and destination masks.
	BankedDpp,
	PixelInterpolation,
	PixelExport,
};

struct ShaderComputeWaveAnalysisResult
{
	bool                supported      = false;
	uint32_t            unsupported_pc = 0;
	Kyty::Core::String8 reason;
};

[[nodiscard]] ShaderComputeWaveInstructionKind ShaderClassifyComputeWaveInstruction(const ShaderInstruction& instruction);
// Strategy-independent rejection of malformed register spans and controls that
// these lowerings cannot represent. Must precede both native and generic choice.
[[nodiscard]] bool ShaderInstructionLoweringPreconditions(const ShaderInstruction& instruction);
[[nodiscard]] ShaderComputeWaveAnalysisResult  ShaderAnalyzeComputeWaveCode(const ShaderCode& code, const ShaderComputeInputInfo& input);
[[nodiscard]] ShaderComputeWaveAnalysisResult  ShaderAnalyzeFragmentWaveCode(const ShaderCode& code, const ShaderPixelInputInfo& pixel,
                                                                             const ShaderComputeInputInfo& host,
                                                                             uint32_t                      parameter_register = UINT32_MAX);
[[nodiscard]] ShaderComputeWaveInstructionKind ShaderClassifyFragmentWaveInstruction(const ShaderInstruction&    instruction,
                                                                                     const ShaderPixelInputInfo& pixel);
[[nodiscard]] bool ShaderFragmentInterpolationPairSupported(const ShaderCode& code, uint32_t index, const ShaderPixelInputInfo& pixel);
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentParameterBase(const ShaderCode& code,
                                                                                 uint32_t          parameter_register = UINT32_MAX);
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentExports(const ShaderCode& code);
// Virtualization is safe only when the initial parameter-state word reaches
// the fixed M0 selector alone. Later SGPR uses need a complete overwrite in
// the unconditional entry prefix; an observable M0 read rejects this route.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentVirtualParameterState(const ShaderCode& code,
                                                                                         uint32_t          parameter_register);
// A closed, side-effect-free region may initialize uncaptured lanes itself.
// Only its proven full-EXEC entry may bypass the ordinary allocation clamp.
[[nodiscard]] bool ShaderFragmentNeutralRegionSupported(const ShaderCode& code, uint32_t index);
// Reads that ignore EXEC or fetch inactive lanes can observe lanes absent from
// a partial captured wave. Each needs a source that a proven neutral region
// initialized in every lane; otherwise the program is rejected with its PC.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentPartialWaveReads(const ShaderCode& code);
// A Wave64 fragment program on a host subgroup of at most 32 lanes is a
// partially populated guest wave: which pixels share a wave is the
// rasterizer's choice and not a program input. This tier is exact only when
// every lane-indexed instruction (row DPP, PERMLANE, and READLANE reaching
// lanes 32-63 or a dynamic index) stays inside a proven neutral region or
// reads a register the region initialized to its neutral zero for lanes the
// host wave does not hold.
[[nodiscard]] ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentNativeWaveTier(const ShaderCode& code);
// `index` inside a proven neutral region, or a VGPR proven to hold the neutral
// zero on lanes the captured wave never populated before `index`.
[[nodiscard]] bool ShaderFragmentWaveInsideRegion(const ShaderCode& code, uint32_t index);
[[nodiscard]] bool ShaderFragmentWaveGhostZero(const ShaderCode& code, uint32_t index, int vgpr);
// True for the packed U32 compare family whose architectural destination is
// EXEC (v_cmpx_*_u32). The plain VOPC parse surfaces a VccLo placeholder.
[[nodiscard]] bool ShaderComputeWaveTypeIsExecCompare(ShaderInstructionType type);
// A VALU instruction whose result for each lane depends only on that lane's
// VGPRs and on uniform SGPR/VCC words or constants, and that writes only VGPRs.
// Quad DPP sources additionally use the bank's architectural source-EXEC
// word; ordinary EXEC guards use only the destination lane's mask bit.
[[nodiscard]] bool ShaderComputeWaveGenericVectorSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericLdsSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericScalarSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericCompareSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveDppInstructionSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWavePermutationSupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ANALYSIS_H_
