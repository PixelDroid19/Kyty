#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// A whole-wave OR reduction of a Wave64 or Wave32 fragment program: a proven neutral region (EXEC widened to
// every lane, a V_CNDMASK_B32 seed `cond ? x : 0`) whose row-shift DPP and PERMLANE steps only OR the seeds of
// other lanes together, read back by static V_READLANE_B32 after EXEC is restored. Only instructions that name
// no register of the steps and keep EXEC may sit between the restore and the reads.
//
// Each read is the OR of the seed over a fixed set of guest lanes. A host subgroup of any width computes it
// as one subgroup reduction: the condition holds only covered lanes, so a helper lane and a lane the
// subgroup does not hold (absent from a partially populated wave) both seed zero. The steps are not run on
// the host, so their per-lane results must be dead: every path overwrites them in every launched lane
// before any other instruction reads them.
struct ShaderFragmentLaneReduction
{
	struct Read
	{
		uint32_t index = 0; // the V_READLANE_B32
		uint64_t lanes = 0; // guest lanes whose seed it ORs
	};
	uint32_t          seed_index = 0; // the V_CNDMASK_B32 that follows the region entry
	uint32_t          restore    = 0; // the EXEC restore that closes the region
	int               seed       = 0; // VGPR the seed is written to; it still holds the seed at every read
	std::vector<Read> reads;
};

struct ShaderFragmentLaneReductions
{
	std::vector<ShaderFragmentLaneReduction> reductions;

	// An instruction between a seed and its restore: its effect is carried by the reductions of the reads.
	[[nodiscard]] bool IsStep(uint32_t index) const;
	// The reduction a V_READLANE_B32 reads, with the lanes it ORs, or nullptr.
	[[nodiscard]] const ShaderFragmentLaneReduction* ReadAt(uint32_t index, uint64_t* lanes) const;
};

// Every proven whole-wave OR reduction of a lane-local fragment program (`masks` from the same code).
[[nodiscard]] ShaderFragmentLaneReductions ShaderFindFragmentLaneReductions(const ShaderCode& code, const ShaderFragmentMaskFlow& masks);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
