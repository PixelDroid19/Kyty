#include "ShaderSpirvInternal.h"

#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvScJoin.h"
#include "ShaderSpirvTemplates.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Branch / loop emission only. Multi-join ownership lives in ShaderSpirvScJoin.*

static bool instruction_is_conditional_branch(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz: return true;
		default: return false;
	}
}

static bool instruction_changes_control_flow(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SSetpcB64:
		case ShaderInstructionType::SSwappcB64: return true;
		default: break;
	}
	return false;
}

static bool loop_exit_is_in_header_block(const ShaderCode& code, const ShaderLabel& backedge, const ShaderInstruction& exit)
{
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() > backedge.GetDst() && label.GetDst() <= exit.pc)
		{
			return false;
		}
	}

	return true;
}

static String8 find_backward_loop_merge(const ShaderCode& code, const ShaderLabel& backedge)
{
	String8 merge;

	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc < backedge.GetDst() || inst.pc >= backedge.GetSrc() || !instruction_is_conditional_branch(inst))
		{
			continue;
		}

		const auto exit = ShaderLabel(inst);
		if (exit.GetDst() <= backedge.GetSrc())
		{
			continue;
		}

		EXIT_NOT_IMPLEMENTED(!loop_exit_is_in_header_block(code, backedge, inst));

		if (merge.Size() == 0)
		{
			merge = exit.ToString();
			continue;
		}

		EXIT_NOT_IMPLEMENTED(merge != exit.ToString());
	}

	return merge;
}

static uint32_t find_backward_loop_for_exit(const ShaderCode& code, const ShaderLabel& exit)
{
	uint32_t owner = 0;

	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::SBranch)
		{
			continue;
		}

		const auto backedge = ShaderLabel(inst);
		if (backedge.GetDst() >= backedge.GetSrc() || find_backward_loop_merge(code, backedge) != exit.ToString())
		{
			continue;
		}

		EXIT_NOT_IMPLEMENTED(owner != 0);
		owner = backedge.GetSrc();
	}

	return owner;
}


KYTY_RECOMPILER_FUNC(Recompile_SBranch_Label)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);

	const auto branch = ShaderLabel(inst);
	String8    label  = branch.ToString();

	if (branch.GetDst() < inst.pc)
	{
		String8 continue_label = String8::FromPrintf("loop_continue_%04" PRIx32, inst.pc);
		String8 merge_label    = find_backward_loop_merge(code, branch);
		const bool has_exit    = merge_label.Size() != 0;
		if (!has_exit)
		{
			merge_label = String8::FromPrintf("loop_merge_%04" PRIx32, inst.pc);
		}

		static const char* loop_text = R"(
                OpBranch %<continue>
       %<continue> = OpLabel
                OpBranch %<label>
        <unreachable_merge>
)";

		*dst_source +=
		    String8(loop_text)
		        .ReplaceStr("<continue>", continue_label)
		        .ReplaceStr("<label>", label)
		        .ReplaceStr("<unreachable_merge>",
		                    has_exit ? ""
		                             : String8::FromPrintf("%%%s = OpLabel\n                OpUnreachable", merge_label.c_str()));
		return true;
	}

	// Retarget SBranch to a multi-predecessor join onto the sc_join merge that
	// owns this case/default, so each selection reconverges before the guest join.
	{
		const uint32_t join_pc = branch.GetDst();
		{
			Vector<uint32_t> sc_join_srcs;
			ScJoinCollectSources(code, join_pc, &sc_join_srcs);
			const uint32_t owner_pc = ScJoinFindOwner(code, inst.pc, join_pc, sc_join_srcs);
			if (owner_pc != 0)
			{
				label = ScJoinMergeName(join_pc, owner_pc);
			}
		}
	}

	static const char* text = R"(
                OpBranch %<label>
)";

	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);

	return true;
}

/* XXX: Execz, Scc0, Scc1, Vccz, Vccnz */
KYTY_RECOMPILER_FUNC(Recompile_SCbranch_XXX_Label)
{
	EXIT_NOT_IMPLEMENTED(index + 1 >= code.GetInstructions().Size());

	const auto& inst      = code.GetInstructions().At(index);
	const auto& next_inst = code.GetInstructions().At(index + 1);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	const char* branch_param[2] = {param[0], param[1]};
	if ((inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz) &&
	    ShaderVccBranchIsWaveUniform(code, index))
	{
		branch_param[0] = inst.type == ShaderInstructionType::SCbranchVccz
		                     ? "%cc_u_<index> = OpLoad %uint %vcc_lo\n%cc_b_<index> = OpIEqual %bool %cc_u_<index> %uint_0"
		                     : "%cc_u_<index> = OpLoad %uint %vcc_lo\n%cc_b_<index> = OpINotEqual %bool %cc_u_<index> %uint_0";
		branch_param[1] = "";
	}

	// TODO(): analyze control flow graph
	auto label            = ShaderLabel(inst);
	auto dst_block        = code.ReadBlock(label.GetDst());
	auto next_block       = code.ReadBlock(next_inst.pc);
	bool discard          = dst_block.is_discard;
	auto label_next_block = ShaderLabel(next_block.last);
	auto label_dst_block  = ShaderLabel(dst_block.last);

	// A conditional branch that targets an earlier PC is a loop continue edge.
	// Emitting OpSelectionMerge with the loop header as the merge block creates a
	// CFG cycle (header is both merge and dominator ancestor) and stack-overflows
	// spirv-opt's GetBlockDepth. Pair with OpLoopMerge at the header (WriteLabel).
	if (label.GetDst() < inst.pc)
	{
		EXIT_NOT_IMPLEMENTED(discard);

		const String8 continue_label = String8::FromPrintf("loop_continue_%04" PRIx32, inst.pc);
		const String8 merge_label    = String8::FromPrintf("loop_merge_%04" PRIx32, inst.pc);
		const String8 label_str      = label.ToString();

		static const char* text_loop_continue = R"(
        <param0>
        <param1>
               OpBranch %<continue>
       %<continue> = OpLabel
               OpBranchConditional %cc_b_<index> %<label> %<merge>
       %<merge> = OpLabel
)";

		*dst_source += String8(text_loop_continue)
		                   .ReplaceStr("<param0>", branch_param[0])
		                   .ReplaceStr("<param1>", branch_param[1])
		                   .ReplaceStr("<continue>", continue_label)
		                   .ReplaceStr("<merge>", merge_label)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<label>", label_str);

		return true;
	}

	bool if_else = next_block.is_valid && !next_block.is_discard && dst_block.is_valid && !dst_block.is_discard &&
	               ((next_block.last.type == ShaderInstructionType::SBranch && label_next_block.GetDst() >= dst_block.pc &&
	                 label_next_block.GetDst() <= dst_block.last.pc) ||
	                (dst_block.last.type == ShaderInstructionType::SBranch && label_dst_block.GetDst() >= next_block.pc &&
	                 label_dst_block.GetDst() <= next_block.last.pc));

	String8 label_str = label.ToString();
	String8 label_merge =
	    if_else ? (dst_block.last.type == ShaderInstructionType::SBranch ? label_dst_block.ToString() : label_next_block.ToString()) : "";
	// Shared joins with more than two predecessors are switch reconvergence, not
	// a two-sided diamond. Keep the simple forward-conditional shape instead.
	if (if_else && label_merge.Size() != 0)
	{
		const auto& labels       = code.GetLabels();
		const auto  merge_label  = (dst_block.last.type == ShaderInstructionType::SBranch ? label_dst_block : label_next_block);
		int         join_sources = 0;
		for (const auto& join_label: labels)
		{
			if (!join_label.IsDisabled() && join_label.GetDst() == merge_label.GetDst())
			{
				++join_sources;
			}
		}
		if (join_sources > 2)
		{
			if_else     = false;
			label_merge = "";
		}
	}
	const uint32_t loop_backedge = find_backward_loop_for_exit(code, label);

	// Promote a forward conditional to if/else only for a true diamond: the
	// block before the taken target is an unconditional branch to a join that
	// has exactly two incoming edges. Switch-style cascades (many cases sharing
	// one join) must not use this path — they nest SelectionMerges onto the
	// same reconvergence chain and fail structured CFG validation/optimization.
	if (!if_else && label.GetDst() > inst.pc)
	{
		const auto& instructions = code.GetInstructions();
		const auto& labels       = code.GetLabels();
		for (uint32_t i = 1; i < instructions.Size(); i++)
		{
			if (instructions.At(i).pc != label.GetDst())
			{
				continue;
			}
			const auto& previous = instructions.At(i - 1);
			if (previous.type != ShaderInstructionType::SBranch)
			{
				break;
			}
			auto previous_branch = ShaderLabel(previous);
			if (previous_branch.GetDst() <= label.GetDst())
			{
				break;
			}
			int join_sources = 0;
			for (const auto& join_label: labels)
			{
				if (!join_label.IsDisabled() && join_label.GetDst() == previous_branch.GetDst())
				{
					++join_sources;
				}
			}
			if (join_sources == 2)
			{
				if_else     = true;
				label_merge = previous_branch.ToString();
			}
			break;
		}
	}

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	}
	// L2: /* merge */
	//	...
	static const char* text_variant_a = R"(
        <param0>
        <param1>
               OpSelectionMerge %<label> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L2:
	//		...
	//		discard;
	//	}
	// L1: /* merge */
	//	...
	static const char* text_variant_b = R"(
        <param0>
        <param1>
               OpSelectionMerge %t230_<index> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	} else
	//	{
	// 		L2:
	//		...
	//	}
	//	 /* merge */
	static const char* text_variant_c = R"(
        <param0>
        <param1>
               OpSelectionMerge %<merge> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	static const char* text_loop_exit = R"(
        <param0>
        <param1>
               OpLoopMerge %<label> %loop_continue_<backedge> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	// Forward SCbranch (not a loop exit): never use a shared guest label as
	// OpSelectionMerge. variant_a with merge=taken creates illegal nested
	// constructs when several SCbranches reconverge through multi-label chains.
	// Use a unique sc_join_<join>_<src>; WriteLabel materializes it.
	// Loop exits keep the guest merge label — OpLoopMerge must not target sc_join.
	bool    switch_case       = false;
	bool    switch_empty_case = false;
	String8 switch_merge;
	if (!if_else && !discard && loop_backedge == 0 && label.GetDst() > inst.pc)
	{
		// ScJoinFindReconvergence returns taken_dst for soft empty-case, a later
		// shared join for skip-over (loop then s_branch past mid-label), or 0 if
		// the edge cannot be structured. Never force merge=taken when fallthrough
		// cannot reach it — that yields illegal selection constructs.
		const uint32_t join_pc = ScJoinFindReconvergence(code, label.GetDst(), next_inst.pc);
		if (join_pc != 0)
		{
			switch_case       = true;
			switch_merge      = ScJoinMergeName(join_pc, inst.pc);
			switch_empty_case = (join_pc == label.GetDst());
		}
	}

	const char* text = text_variant_a;
	if (discard)
	{
		text = text_variant_b;
	}
	if (if_else)
	{
		text = text_variant_c;
	}
	if (switch_case)
	{
		if (switch_empty_case)
		{
			// if (cc) goto join; else fallthrough — true edge is the synthetic merge.
			text      = text_variant_a;
			label_str = switch_merge;
		} else
		{
			// if (cc) case; else fallthrough; both meet at switch_merge.
			text        = text_variant_c;
			label_merge = switch_merge;
		}
	}
	if (loop_backedge != 0)
	{
		// Guest label is the structured loop merge (not sc_join).
		text      = text_loop_exit;
		label_str = label.ToString();
	}

	*dst_source += String8(text)
	                   .ReplaceStr("<param0>", branch_param[0])
	                   .ReplaceStr("<param1>", branch_param[1])
	                   .ReplaceStr("<merge>", label_merge)
	                   .ReplaceStr("<backedge>", String8::FromPrintf("%04" PRIx32, loop_backedge))
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<label>", label_str);

	return true;
}

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchScc0_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchScc1_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }
//
// KYTY_RECOMPILER_FUNC(Recompile_SCbranchVccz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

KYTY_RECOMPILER_FUNC(Recompile_SEndpgm_Empty)
{
	// const auto* info = spirv->GetPsInputInfo();

	// EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);

	static const char* text = R"(
       OpReturn
)";

	EXIT_NOT_IMPLEMENTED(index < 2);

	const auto& prev_prev_inst = code.GetInstructions().At(index - 2);
	const auto& prev_inst      = code.GetInstructions().At(index - 1);

	bool after_kill =
	    (prev_prev_inst.type == ShaderInstructionType::SMovB64 && prev_prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	     prev_prev_inst.dst.type == ShaderOperandType::ExecLo && prev_prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
	     prev_prev_inst.src[0].constant.i == 0 && prev_inst.type == ShaderInstructionType::Exp &&
	     ShaderIsNullMrtDoneFormat(prev_inst.format));

	if (!after_kill)
	{
		*dst_source += String8(text);
	}

	return true;
}

// Gen5 NGG fetch prologs may end with s_setpc_b64 into a separately allocated
// main. Full chain linking is not yet modeled; treat the instruction as a block
// terminator so SPIR-V stays valid (CFG already classifies SSetpcB64 as a
// terminator). Without this, the emitter table miss aborts translation for any
// prolog-shaped ES binary.
KYTY_RECOMPILER_FUNC(Recompile_SSetpcB64_Saddr)
{
	static const char* text = R"(
       OpReturn
)";
	*dst_source += String8(text);
	return true;
}

// WriteLabel lives in ShaderSpirvWriteLabel.cpp (loop-merge → sc_join → guest).

void Spirv::ModifyCode()
{
	struct DiscardLabel
	{
		ShaderControlFlowBlock block;
		int                    num = 0;
	};
	const auto&          labels = m_code.GetLabels();
	Vector<DiscardLabel> dls;
	for (const auto& l: labels)
	{
		if (!l.IsDisabled())
		{
			int  num = 0;
			auto pc  = l.GetDst();
			for (const auto& l2: labels)
			{
				if (l2.GetDst() == pc)
				{
					num++;
				}
			}
			EXIT_IF(num == 0);
			if (num > 1)
			{
				if (auto block = m_code.ReadBlock(pc); block.is_discard)
				{
					if (!dls.Contains(pc, [](auto d, auto pc) { return d.block.pc == pc; }))
					{
						dls.Add(DiscardLabel({block, num - 1}));
					}
				}
			}
		}
	}
	for (const auto& dl: dls)
	{
		auto block = m_code.ReadIntructions(dl.block);
		for (int i = 0; i < dl.num; i++)
		{
			// Duplicate discard block if there are different branches with the same label
			m_code.GetInstructions().Add(block);
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)

bool Spirv::CanLoadPackedHalfForExport(int export_index, ShaderOperand op) const
{
	if (op.type != ShaderOperandType::Vgpr || op.size != 1 || export_index <= 0)
	{
		return false;
	}

	const int   reg          = op.register_id;
	const auto& insts        = m_code.GetInstructions();
	const int   insts_count  = static_cast<int>(insts.Size());
	const int   search_start = (export_index < insts_count ? export_index - 1 : insts_count - 1);

	for (int i = search_start; i >= 0; i--)
	{
		const auto& prev = insts.At(i);

		if (instruction_changes_control_flow(prev))
		{
			return false;
		}

		if (!instruction_writes_vgpr(prev, reg))
		{
			continue;
		}

		return prev.type == ShaderInstructionType::VCvtPkrtzF16F32 && operand_covers_vgpr(prev.dst, reg);
	}

	return false;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
