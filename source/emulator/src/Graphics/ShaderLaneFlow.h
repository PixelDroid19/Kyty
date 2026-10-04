#ifndef EMULATOR_SRC_GRAPHICS_SHADER_LANE_FLOW_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_LANE_FLOW_H_

#include "Emulator/Graphics/Shader.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <initializer_list>

#ifdef KYTY_EMU_ENABLED

// Shared vocabulary of the lane-locality flow proofs (fused vertex front body and
// fragment mask flow): the scalar word numbering, the taint lattice and the state
// merged where control flow joins.
namespace Kyty::Libs::Graphics::LaneFlow {

// Scalar word numbering, identical to ShaderNggScalarDependencies.
constexpr unsigned kSgprWords   = 104;
constexpr unsigned kVccLoWord   = 104;
constexpr unsigned kExecLoWord  = 106;
constexpr unsigned kM0Word      = 108;
constexpr unsigned kSccWord     = 109;
constexpr unsigned kWords       = 110;
constexpr int      kVgprs       = 256;

// Taint of a scalar word.
constexpr uint8_t kClean    = 0;
constexpr uint8_t kInfo     = 1; // launch-dependent number (counts, M0, system words)
constexpr uint8_t kMask     = 2; // EXEC-derived lane bits
constexpr uint8_t kDiverged = 4; // written under lane-divergent control flow: differs between lanes after the join
constexpr uint8_t kMixed    = 8; // a register pair whose words disagree on being a mask
constexpr uint8_t kWide     = 16; // a mask that may include helper lanes (reached through S_WQM, a complement or an OR)

[[nodiscard]] bool StartsWith(ShaderInstructionType type, const char* prefix);
[[nodiscard]] bool AnyPrefix(ShaderInstructionType type, std::initializer_list<const char*> prefixes);
// Scalar word range of an operand; false when the operand is not a scalar register.
[[nodiscard]] bool ScalarRange(const ShaderOperand& op, unsigned* first, unsigned* count);
[[nodiscard]] bool IsConstant(const ShaderOperand& op);
[[nodiscard]] bool IsScalarLoad(ShaderInstructionType type);
[[nodiscard]] bool IsScalarAlu(ShaderInstructionType type);
[[nodiscard]] bool ReadsSccImplicitly(ShaderInstructionType type);
// Every scalar ALU form defines SCC except moves, selects and the program counter.
[[nodiscard]] bool WritesScc(ShaderInstructionType type);
// S_MOV/S_AND/S_OR/... on lane masks: pointwise, so a mask in gives a mask out.
[[nodiscard]] bool IsMaskAlgebra(ShaderInstructionType type);
[[nodiscard]] bool IsSelect(ShaderInstructionType type);
// V_CMPX on RDNA writes only EXEC (EXEC &= compare); the decoder's VCC/SGPR destination is not written.
[[nodiscard]] bool WritesExecOnly(ShaderInstructionType type);
[[nodiscard]] bool IsVectorLoad(ShaderInstructionType type);
// Image reads whose result depends only on this lane's address and on scalar resources: explicit
// LOD (or none, for loads), no derivatives, no write.
[[nodiscard]] bool IsLaneLocalImageRead(ShaderInstructionType type);
[[nodiscard]] bool IsLaneExchange(ShaderInstructionType type);
// Condition operand of a vector select or carry-in: one bit per lane, read as this lane's bit.
[[nodiscard]] bool IsLaneBit(const ShaderInstruction& inst, int source);
// Texture fetches whose LOD comes from quad derivatives.
[[nodiscard]] bool UsesImplicitDerivatives(ShaderInstructionType type);
[[nodiscard]] bool WritesMemory(ShaderInstructionType type);
[[nodiscard]] bool VgprRange(const ShaderOperand& op, unsigned* first, unsigned* count);

struct FlowState
{
	std::array<uint8_t, kWords> scalar {};
	std::bitset<kVgprs>         defined;
	std::bitset<kVgprs>         uniform;
	// Scalar words that hold a lane compare whose result is the same for every lane (built from uniform sources only).
	std::bitset<kWords>         uniform_mask;
	bool                        vcc_uniform = false;
	bool                        reachable   = true;

	// Joins another path's state into this one. Returns whether this state changed, which is
	// what a fixpoint over back edges needs to know.
	bool Merge(const FlowState& other);
};

} // namespace Kyty::Libs::Graphics::LaneFlow

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_SRC_GRAPHICS_SHADER_LANE_FLOW_H_
