#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderFragmentLaneReduction.h"
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
// The proof is a forward, flow-sensitive walk; a loop is walked until the state at its header
// holds for every iteration:
//  - launch-dependent scalars may be copied by scalar ALU but never reach a vector
//    instruction, a scalar-memory address, a branch condition or EXEC; redefining a
//    word with a scalar load or a constant ends its dependence;
//  - EXEC-derived words are lane bits: consumed by a vector select or carry-in, or
//    combined by mask algebra under a wave-uniform SCC;
//  - a VCC branch needs VCC to be a comparison of wave-uniform values (scalars free of
//    launch data, constants, and lane ALU on those), so every lane takes the same path;
//  - vector sources are defined on every path; only scalar loads, lane ALU, vertex
//    fetch and exports are admitted; no stores, LDS or lane exchange, and no atomic but the
//    one of a wave maximum (ShaderFindNggFrontMaxReductions).
// Under those rules an isolated invocation computes what lane i of any wave computes, and a
// wave maximum leaves memory as the guest wave does.
[[nodiscard]] ShaderNggFrontBodyProof ShaderProveNggFrontBodyLaneLocal(const ShaderCode& code, uint32_t first_index, uint32_t guest_wave_size,
                                                                      const ShaderNggScalarDependencies& wave_scalars);

// The wave maxima of a vertex program that feed a monotone atomic, as compilers lower
// `if (WaveActiveAnyTrue(x > 0) && u) { m = WaveActiveMax(x); if (x == m) InterlockedMax(buf[i], asuint(m)); }`:
//     V_CMP_LT_F32 vcc, 0, x;  V_CMP_GT_U64 a, vcc, 0;  S_AND_B64 vcc, a, (u ? EXEC : 0);  S_CBRANCH_VCCZ join
//     S_ORN2_SAVEEXEC_B64 e, EXEC;  V_CNDMASK_B32 v, -inf, x, e;  max steps;  S_MOV_B64 EXEC, e
//     two V_READLANE_B32 r0, r1 covering every lane;  V_MAX_F32 m, r0, r1;  V_CMPX_EQ_F32 m, x
//     BUFFER_ATOMIC_UMAX m at a wave-uniform index, no return;  S_MOV_B64 EXEC, e;  join:
// A host subgroup holds some of the guest wave's lanes and computes the maximum M_S of its own. Every subgroup
// that enters holds a lane with x > 0, so M_S > 0 and the unsigned order of its bits is the float order, and the
// subgroup holding the guest maximum M enters and stores it: memory ends at umax(old, M) as on the guest. Nothing
// else of the region may be observed: the proof treats its scalars as launch data and its VGPRs as undefined
// after the join, and refuses an export inside it. The emitter skips the steps and folds the reads
// (ShaderLaneReduction) only for a program this proof admitted.
[[nodiscard]] ShaderLaneReductions ShaderFindNggFrontMaxReductions(const ShaderCode& code, uint32_t guest_wave_size);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_NGG_FRONT_BODY_H_
