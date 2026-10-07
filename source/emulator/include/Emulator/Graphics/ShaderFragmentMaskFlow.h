#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_MASK_FLOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_MASK_FLOW_H_

#include "Emulator/Graphics/Shader.h"

#include <cstdint>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

struct ShaderFragmentMaskFlow
{
	// Every observation of an EXEC/VCC-derived word in the program is either a
	// per-lane bit (the condition of a vector select) or mask algebra whose result
	// is again such a word. No mask reaches an ordinary numeric use, and no scalar
	// written under lane-divergent control flow is read after the paths rejoin.
	bool lane_local = false;
	// First instruction that breaks the proof, with a static reason. Set whenever
	// lane_local is false.
	uint32_t    refused_pc = 0;
	const char* reason     = nullptr;
	// refused_pc names the instruction that broke the proof (not a whole-program precondition).
	bool located = false;
	// Per instruction, only when lane_local: the EXEC write is accounted for by the
	// proof. Mask algebra writes (narrowing, saved-mask restores) and the two writes
	// of a validated whole-quad wrapper (save EXEC, S_WQM_B64 EXEC, restore EXEC) are
	// closed; a lone S_WQM_B64 widening EXEC is not.
	std::vector<bool> closed_exec_write;
};

// Flow-sensitive mask provenance for a fragment program (S_ENDPGM last, no memory write
// anywhere). Backward branches are loops; the states are iterated to a fixpoint.
//
// Under the host model each fragment invocation stands for one guest lane of an
// existing quad, so EXEC and every mask derived from it is one bit per invocation.
//  - A register overwritten by a numeric value stops being a mask, so an SGPR pair
//    reused as a save area after it served as a buffer descriptor is a mask only
//    after the save. SCC is cleared only by a compare.
//  - A select under a non-mask SCC is mask algebra, and so are AND/OR/XOR/ANDN2/ORN2
//    and the SAVEEXEC forms; a numeric value may never be assigned to EXEC.
//  - A branch on EXEC (S_CBRANCH_EXECZ/NZ) or on a VCC that is not a comparison of
//    scalar numbers is lane-divergent: the lanes that skip the block keep the old value
//    of every scalar the block writes, while on the guest those scalars are overwritten
//    once any lane runs it. Such a scalar is marked diverged at the join and may be
//    overwritten but not read by a vector instruction, a scalar load, a branch
//    condition, EXEC or a lane bit. A uniform branch (constant SCC, comparison of
//    scalar numbers) needs no such rule.
//  - A loop whose exit or repeat is lane divergent (a branch on EXEC, or on a VCC that is not
//    a uniform comparison) runs a different number of iterations for a lane that left early on
//    the host (its subgroup's count) and on the guest (its wave's). Every scalar the body may
//    write is diverged once control leaves it; lanes still inside see their own iterations, so
//    a loop-carried scalar is fine. Other loops (a branch on a clean SCC) repeat uniformly.
//  - Derivative texture fetches are refused inside a divergent region or divergent loop.
//  - S_WQM_B64 is the identity on an existing quad lane; it is only closed inside a
//    validated save/widen/restore bracket with no export and no save-area write.
[[nodiscard]] ShaderFragmentMaskFlow ShaderAnalyzeFragmentMaskFlow(const ShaderCode& code);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_FRAGMENT_MASK_FLOW_H_
