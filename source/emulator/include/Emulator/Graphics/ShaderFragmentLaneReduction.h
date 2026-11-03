#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentMaskFlow.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// What a lane reduction folds its seeds with. Or, And, UMin, UMax, SMin and SMax are idempotent: a lane folded
// twice in the network changes nothing, so a read set may overlap another. Add and Xor are not idempotent, so they
// are not admitted. MinF32 is not admitted. NaN handling makes the fold order-dependent: RDNA2 Shader ISA, section
// 12.7 (VOP2 Instructions), V_MIN_F32 (opcode 15), returns S1 when both inputs are NaN, so the payload depends on
// operand order, and under IEEE_MODE a signalling NaN is quieted by the first fold that sees it, so a later fold
// returns its other operand and the value depends on order. Zeros do not: both zero orders return -0. A host
// subgroup minimum fixes no operand order, and the proof reads only the shader code, which does not hold IEEE_MODE
// (a pixel register field). No fragment or NGG path needs MinF32 either. MaxF32 folds a NaN seed as -inf. V_MAX_F32
// (opcode 16) has the same both-NaN and signalling-NaN rules, so for NaN seeds the guest result is order-dependent,
// while the host lowering substitutes -inf and a host subgroup maximum of NaN inputs is undefined. The NGG MaxF32
// proof is not changed here.
enum class ShaderLaneReductionOp
{
	Or,
	And,
	UMin,
	UMax,
	SMin,
	SMax,
	MaxF32,
};

// The seed of a lane outside a read's set, which the subgroup reduction folds as the identity of the op.
[[nodiscard]] uint32_t ShaderLaneReductionIdentity(ShaderLaneReductionOp op);

// A whole-wave reduction: a region with every lane in EXEC, entered by a V_CNDMASK_B32 seed `cond ? x : identity`,
// whose row-shift DPP and PERMLANE steps only fold the seeds of other lanes together, read back by static
// V_READLANE_B32 after EXEC is restored. Scalar loads that keep EXEC may sit among the steps and run as written.
// Only instructions that name no register of the steps and keep EXEC may sit between the restore and the reads.
//
// Each read folds the seed over a fixed set of guest lanes. A host subgroup of any width computes it as one
// subgroup reduction: the condition holds only covered lanes, so a lane the subgroup does not hold (absent from
// a partially populated wave) seeds the identity. The steps are not run on the host, so their per-lane results
// must be dead: every path overwrites them in every launched lane before any other instruction reads them.
// ShaderFindFragmentLaneReductions proves the idempotent reductions of fragment programs; the NGG front body proof
// proves MaxF32 ones of vertex programs (ShaderNggFrontBody.h).
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

// Every proven whole-wave idempotent reduction of a lane-local fragment program (`masks` from the same code).
[[nodiscard]] ShaderLaneReductions ShaderFindFragmentLaneReductions(const ShaderCode& code, const ShaderFragmentMaskFlow& masks);

// The steps from seed_index + 1 to restore, which must all be steps or scalar loads that keep EXEC, and the static
// reads that follow restore, for a guest wave of guest_wave_size lanes. The op is the one name all folds of the steps
// share, or the op the caller sets in reduction->op when there is no fold. The seed must be the identity of that op,
// which a read's lanes outside its set contribute. Proves neither the region nor that the steps are dead; fills op,
// seed, restore, steps and reads. False without a read.
[[nodiscard]] bool ShaderProveLaneReductionNetwork(const ShaderCode& code, uint32_t guest_wave_size, uint32_t seed_index, uint32_t restore,
                                                   ShaderLaneReduction* reduction);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_LANE_REDUCTION_H_
