#ifndef EMULATOR_SRC_GRAPHICS_SHADERSPIRVSCJOIN_H_
#define EMULATOR_SRC_GRAPHICS_SHADERSPIRVSCJOIN_H_

// Guest multi-predecessor join analysis for structured SPIR-V selections.
//
// Used by ShaderSpirvControlFlow.cpp (SBranch / SCbranch) and
// ShaderSpirvWriteLabel.cpp (sc_join_* materialization order).
//
// Keep this header free of SPIR-V text emission so join ownership can be unit
// tested and iterated without touching recompiler templates.

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/Shader.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Static branch edges (SBranch / SCbranch*) that land on pc.
int ScJoinCountLabelSources(const ShaderCode& code, uint32_t pc);

// First multi-join reached linearly from start_pc without a divergent branch.
// Handles "case: ...; s_branch join" and fallthrough into a multi-join.
uint32_t ScJoinFindTakenPathMultiJoin(const ShaderCode& code, uint32_t start_pc);

// Next multi-join at or after start_pc in program order (ignores nested CF).
uint32_t ScJoinFindNextMultiJoin(const ShaderCode& code, uint32_t start_pc);

// Reconvergence for a forward conditional.
// Returns taken_dst for soft empty-case (fallthrough reaches taken), a later
// shared join for skip-over, or 0 if the edge cannot be structured.
// fallthrough_pc is the instruction after the branch.
uint32_t ScJoinFindReconvergence(const ShaderCode& code, uint32_t taken_dst, uint32_t fallthrough_pc);

// True if forward SCbranch at src_pc reconverges at join_pc; optional taken_dst.
bool ScJoinEdgeTakenDst(const ShaderCode& code, uint32_t src_pc, uint32_t join_pc, uint32_t* taken_dst);

// Nested cascade / skip-over: child_src sits under parent_src at the same join.
bool ScJoinIsNestedIn(const ShaderCode& code, uint32_t parent_src, uint32_t child_src, uint32_t join_pc);

// Owner sc_join source for a terminator/fallthrough pc at join_pc.
uint32_t ScJoinFindOwner(const ShaderCode& code, uint32_t pc, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs);

// Tightest enclosing parent among sc_join_srcs, or 0 (guest join).
uint32_t ScJoinFindParent(const ShaderCode& code, uint32_t src, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs);

// Collect every forward conditional edge that reconverges at join_pc.
void ScJoinCollectSources(const ShaderCode& code, uint32_t join_pc, Vector<uint32_t>* out_srcs);

// Synthetic merge name: sc_join_<join>_<src>.
String8 ScJoinMergeName(uint32_t join_pc, uint32_t src_pc);

// Nesting depth of src among sc_join_srcs (number of enclosing parents).
int ScJoinNestingDepth(const ShaderCode& code, uint32_t src, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs);

// Order sources deepest-first for emission (children before parents).
void ScJoinOrderForEmission(const ShaderCode& code, uint32_t join_pc, const Vector<uint32_t>& sc_join_srcs,
                            Vector<uint32_t>* out_order);

// True when a forward conditional is a structured loop exit (jumps past a
// backward SBranch that closes a loop containing the exit).
bool ScJoinIsLoopExitEdge(const ShaderCode& code, const ShaderInstruction& exit_inst);

// Guest label at a join PC that is the structured OpLoopMerge target (not an
// sc_join). Used by WriteLabel to emit loop merges before selection sc_joins.
bool ScJoinIsLoopMergeLabel(const ShaderCode& code, const ShaderLabel& label);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_SRC_GRAPHICS_SHADERSPIRVSCJOIN_H_ */
