#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderNggPassthroughProof.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct ShaderNggFrontBodyProof
{
	bool        lane_local = false;
	uint32_t    refused_pc = 0;
	const char* reason     = nullptr; // Static text, set whenever lane_local is false.
};

// Lane-locality of the part of a fused NGG front that follows its wave-count prologue
// (ShaderAnalyzeNggPassthroughPrologue), from instruction first_index to S_ENDPGM.
//
// Entering the body, EXEC is the vertex-count mask and every scalar word in
// wave_scalars carries launch-dependent data (counts, M0, VCC, SCC, system SGPRs).
// The proof is a forward, flow-sensitive walk with forward branches only:
//  - launch-dependent scalars may be copied by scalar ALU but never reach a vector
//    instruction, a scalar-memory address, a branch condition or EXEC; redefining a
//    word with a scalar load or a constant ends its dependence;
//  - EXEC-derived words are lane bits: consumed by a vector select or carry-in, or
//    combined by mask algebra under a wave-uniform SCC;
//  - a VCC branch needs VCC to be a comparison of wave-uniform values (scalars free of
//    launch data, constants, and lane ALU on those), so every lane takes the same path;
//  - vector sources are defined on every path; only scalar loads, lane ALU, vertex
//    fetch and exports are admitted; no stores, atomics, LDS or lane exchange.
// Under those rules an isolated invocation computes what lane i of any wave computes.
[[nodiscard]] ShaderNggFrontBodyProof ShaderProveNggFrontBodyLaneLocal(const ShaderCode& code, uint32_t first_index,
                                                                      const ShaderNggScalarDependencies& wave_scalars);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_
