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
	// v_add_co_ci_u32 / v_subrev_co_ci_u32: banked result plus carry mask pair.
	BankedCarry,
};

struct ShaderComputeWaveAnalysisResult
{
	bool                supported      = false;
	uint32_t            unsupported_pc = 0;
	Kyty::Core::String8 reason;
};

[[nodiscard]] ShaderComputeWaveInstructionKind ShaderClassifyComputeWaveInstruction(const ShaderInstruction& instruction);
[[nodiscard]] ShaderComputeWaveAnalysisResult  ShaderAnalyzeComputeWaveCode(const ShaderCode& code, const ShaderComputeInputInfo& input);
// True for the packed U32 compare family whose architectural destination is
// EXEC (v_cmpx_*_u32). The plain VOPC parse surfaces a VccLo placeholder.
[[nodiscard]] bool ShaderComputeWaveTypeIsExecCompare(ShaderInstructionType type);
// A VALU instruction whose result for each lane depends only on that lane's
// VGPRs and on uniform SGPR/VCC words or constants, and that writes only VGPRs.
// It reads neither EXEC nor another lane, so running its native per-invocation
// lowering once per bank is exact.
[[nodiscard]] bool ShaderComputeWaveGenericVectorSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericLdsSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericScalarSupported(const ShaderInstruction& instruction);
[[nodiscard]] bool ShaderComputeWaveGenericCompareSupported(const ShaderInstruction& instruction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_COMPUTE_WAVE_ANALYSIS_H_
