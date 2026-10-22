#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// What a lane reduction folds its seeds with. MaxF32 folds a NaN seed as -inf: V_MAX_F32 returns the other
// operand of a quiet NaN, while a host subgroup maximum of a NaN is undefined.
enum class ShaderLaneReductionOp
{
	Or,
	MaxF32,
};

// A whole-wave reduction: a region with every lane in EXEC, entered by a V_CNDMASK_B32 seed `cond ? x : identity`,
// whose row-shift DPP and PERMLANE steps only fold the seeds of other lanes together, read back by static
// V_READLANE_B32 after EXEC is restored. Scalar loads that keep EXEC may sit among the steps and run as written.
// Only instructions that name no register of the steps and keep EXEC may sit between the restore and the reads.
//
// Each read folds the seed over a fixed set of guest lanes. A host subgroup of any width computes it as one
// subgroup reduction: the condition holds only covered lanes, so a lane the subgroup does not hold (absent from
// a partially populated wave) seeds the identity. The steps are not run on the host, so their per-lane results
// must be dead: every path overwrites them in every launched lane before any other instruction reads them.
// ShaderFindFragmentLaneReductions proves OR reductions of fragment programs; the NGG front body proof proves
// MaxF32 ones of vertex programs (ShaderNggFrontBody.h).
struct ShaderLaneReduction
{
	struct Read
	{
		uint32_t index = 0; // the V_READLANE_B32
		uint64_t lanes = 0; // guest lanes whose seed it folds
	};
	ShaderLaneReductionOp op         = ShaderLaneReductionOp::Or;
	uint32_t              seed_index = 0; // the V_CNDMASK_B32 that seeds the region
	uint32_t              restore    = 0; // the EXEC restore that closes the region
	int                   seed       = 0; // VGPR the seed is written to; it still holds the seed at every read
	std::vector<uint32_t> steps;          // the network instructions between seed_index and restore
	std::vector<Read>     reads;

	[[nodiscard]] bool IsStep(uint32_t index) const;
};

struct ShaderLaneReductions
{
	std::vector<ShaderLaneReduction> reductions;

	// A network step of a reduction: its effect is carried by the reductions of the reads.
	[[nodiscard]] bool IsStep(uint32_t index) const;
	// The reduction a V_READLANE_B32 reads, with the lanes it folds, or nullptr.
	[[nodiscard]] const ShaderLaneReduction* ReadAt(uint32_t index, uint64_t* lanes) const;
};

// Every proven whole-wave OR reduction of a lane-local fragment program (`masks` from the same code).
[[nodiscard]] ShaderLaneReductions ShaderFindFragmentLaneReductions(const ShaderCode& code, const ShaderFragmentMaskFlow& masks);

// The steps of `reduction.op` from seed_index + 1 to restore, which must all be steps or scalar loads that keep
// EXEC, and the static reads that follow restore, for a guest wave of guest_wave_size lanes. Proves neither the
// region nor that the steps are dead; fills seed, restore, steps and reads. False without a read.
[[nodiscard]] bool ShaderProveLaneReductionNetwork(const ShaderCode& code, uint32_t guest_wave_size, uint32_t seed_index, uint32_t restore,
                                                   ShaderLaneReduction* reduction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
