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
// All false when the CFG cannot be resolved.
struct ShaderLaneMaskPlan
{
	std::vector<bool> local;
	std::vector<bool> gather;
};

[[nodiscard]] ShaderLaneMaskPlan ShaderLaneMaskPlanOf(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_MASK_ANALYSIS_H_ */
