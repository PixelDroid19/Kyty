#ifndef EMULATOR_SRC_GRAPHICS_SHADER_DEAD_VGPR_MASK_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_DEAD_VGPR_MASK_H_

#include "Emulator/Graphics/Shader.h"

#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Per instruction: true when a pure register-writing vector/sampled-image instruction's destination
// VGPR guard (`OpSelect %exec_lane_lo`) cannot be observed by any inactive lane — either EXEC
// already holds all lanes, every destination VGPR is dead after the instruction across all paths,
// or the write executes at entry mask inside an acyclic single-entry/single-exit selection region
// where every written VGPR is dead at the region merge and not consumed by a later cross-lane or
// implicit-derivative operation in that region.
[[nodiscard]] std::vector<bool> ShaderAnalyzeDeadVgprDstGuards(const ShaderCode& code, const std::vector<bool>& exec_full,
                                                                 bool entry_exec_full = false, bool lane_private_masks = false);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_SRC_GRAPHICS_SHADER_DEAD_VGPR_MASK_H_
