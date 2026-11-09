#ifndef EMULATOR_SRC_GRAPHICS_SHADER_SPLIT_LDS_ORDER_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_SPLIT_LDS_ORDER_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

// Where a split wave orders the LDS accesses of its parts, per instruction
// index: a rendezvous before or after the DS instruction. One guest wave runs
// its lanes instruction by instruction, so an access must be over in every
// part before a later access that conflicts with it starts. Two accesses
// conflict when one of them writes and their bytes may meet in different
// lanes; reads, disjoint ranges and each lane's own slot never do. S_BARRIER
// orders everything across it.
struct ShaderSplitLdsOrder
{
	std::vector<bool> before;
	std::vector<bool> after;
};

// DS instructions that access LDS, the ones an order applies to.
[[nodiscard]] bool ShaderSplitLdsOrdered(ShaderInstructionType type);

// `threads` is the guest local size; at entry the first `thread_ids` VGPRs
// hold the lane's thread coordinates.
[[nodiscard]] ShaderSplitLdsOrder ShaderSplitLdsOrderOf(const ShaderCode& code, const uint32_t threads[3], int thread_ids);

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_SHADER_SPLIT_LDS_ORDER_H_ */
