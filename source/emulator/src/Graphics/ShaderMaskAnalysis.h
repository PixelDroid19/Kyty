#ifndef EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

// Proves that a VOP3B reverse-borrow SGPR source pair was written by one of
// the currently scalarized full-pair compare emitters in the same straight-
// line block. Native wave32 also permits an independently produced VCC_HI
// word. VCC and EXEC low pairs are explicit backend mask contracts.
[[nodiscard]] bool ShaderReverseBorrowMaskHasProvenance(const ShaderCode& code, uint32_t instruction_index,
                                                       bool native_wave32 = false);

// Where a split wave gathers its lane masks, per instruction index.
// local: a lane-mask producer (V_CMP, V_CMPX, carry out) whose result is only
// ever read one lane bit at a time by the lane that owns it (V_CNDMASK,
// carry-in, EXEC of vector instructions), also through pointwise S_*_B64 mask
// algebra whose results feed no SCC or other wave-wide reader, or through an
// in-block S_*_B64 gather. Such a mask needs no wave-wide gather.
// gather: a pointwise S_*_B64 whose result a wave-wide reader needs and which
// combines in-block local masks; it gathers its own result once.
// part_skipped: the instruction lies in a range a forward S_CBRANCH_EXECZ
// skips and that one part of a split wave may skip alone, on its own lanes:
// the range holds no rendezvous and defines no scalar word read after it, so
// the branch needs no complete EXEC. Only set when part-local skips are asked,
// which takes a wave64 guest: a wave32 compare writes one word of its pair.
// All false when the CFG cannot be resolved.
struct ShaderLaneMaskPlan
{
	std::vector<bool> local;
	std::vector<bool> gather;
	std::vector<bool> part_skipped;
};

[[nodiscard]] ShaderLaneMaskPlan ShaderLaneMaskPlanOf(const ShaderCode& code, bool part_local_skips = false);

// Successor instruction indices per instruction; false when a branch target
// cannot be resolved.
[[nodiscard]] bool ShaderInstructionSuccessors(const ShaderCode& code, std::vector<std::vector<uint32_t>>* successors);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_ */
