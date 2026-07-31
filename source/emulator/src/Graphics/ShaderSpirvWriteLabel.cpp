// Guest label materialization for structured SPIR-V CFG.
//
// Emission order at a shared reconvergence PC:
//
//   1) loop-merge guest labels  (OpLoopMerge targets of nested loops)
//   2) synthetic sc_join_*      (OpSelectionMerge targets of outer SCbranch)
//   3) remaining guest labels   (outer join / loop headers)
//
// Nested loops live inside outer selections. Emitting sc_join before the loop
// merge places the loop merge outside the selection construct and produces
// "branches to the selection construct, but not to the selection header".

#include "ShaderSpirvInternal.h"

#include "ShaderSpirvScJoin.h"

#include <cinttypes>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

bool instruction_is_conditional_branch(const ShaderInstruction& inst)
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

bool previous_is_terminator(const ShaderInstruction& prev)
{
	return prev.type == ShaderInstructionType::SEndpgm || prev.type == ShaderInstructionType::SSetpcB64 ||
	       prev.type == ShaderInstructionType::SBranch;
}

String8 pick_guest_join_label(uint32_t join_pc, const Vector<ShaderLabel*>& guest_labels)
{
	if (guest_labels.Size() > 0)
	{
		return guest_labels[0]->ToString();
	}
	// Do not fall back to a loop-merge name — that would make sc_join close onto
	// the nested loop merge and recreate the illegal CFG edge.
	return String8::FromPrintf("label_%04" PRIx32, join_pc);
}

// Outermost selection merge (parent == 0). Nested loop merges branch here.
String8 pick_sc_join_root(const ShaderCode& code, uint32_t join_pc, const Vector<uint32_t>& order,
                          const Vector<uint32_t>& sc_join_srcs)
{
	if (order.Size() == 0)
	{
		return {};
	}
	for (int i = order.Size(); i > 0; i--)
	{
		const uint32_t src = order[i - 1];
		if (ScJoinFindParent(code, src, join_pc, sc_join_srcs) == 0)
		{
			return ScJoinMergeName(join_pc, src);
		}
	}
	return ScJoinMergeName(join_pc, order[order.Size() - 1]);
}

bool sbranch_loop_has_labeled_exit(const ShaderCode& code, const ShaderLabel& backedge)
{
	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc < backedge.GetDst() || inst.pc >= backedge.GetSrc() || !instruction_is_conditional_branch(inst) ||
		    !operand_is_constant(inst.src[0]))
		{
			continue;
		}
		if (ShaderLabel(inst).GetDst() > backedge.GetSrc())
		{
			return true;
		}
	}
	return false;
}

void classify_backward_loop_header(const ShaderCode& code, const ShaderLabel& label, bool* is_header,
                                   bool* is_uncond_sbranch)
{
	*is_header         = false;
	*is_uncond_sbranch = false;
	if (label.GetSrc() <= label.GetDst())
	{
		return;
	}
	for (const auto& source_inst: code.GetInstructions())
	{
		if (source_inst.pc != label.GetSrc())
		{
			continue;
		}
		if (source_inst.type == ShaderInstructionType::SBranch)
		{
			*is_header         = true;
			*is_uncond_sbranch = true;
		} else if (instruction_is_conditional_branch(source_inst))
		{
			*is_header = true;
		}
		return;
	}
}

} // namespace

void Spirv::WriteLabel(int index)
{
	if (index <= 0)
	{
		return;
	}

	const auto& instructions = m_code.GetInstructions();
	const auto& inst         = instructions.At(index);
	auto&       labels       = m_code.GetLabels();
	const auto& prev         = instructions.At(index - 1);

	// Partition guest labels at this PC.
	Vector<ShaderLabel*> loop_merges;
	Vector<ShaderLabel*> guest_labels;
	for (uint32_t i = labels.Size(); i > 0; i--)
	{
		auto& label = labels[i - 1];
		if (label.IsDisabled() || label.GetDst() != inst.pc)
		{
			continue;
		}
		if (ScJoinIsLoopMergeLabel(m_code, label))
		{
			loop_merges.Add(&label);
		} else
		{
			guest_labels.Add(&label);
		}
	}

	Vector<uint32_t> sc_join_srcs;
	ScJoinCollectSources(m_code, inst.pc, &sc_join_srcs);
	Vector<uint32_t> sc_join_order;
	if (sc_join_srcs.Size() > 0)
	{
		ScJoinOrderForEmission(m_code, inst.pc, sc_join_srcs, &sc_join_order);
	}

	const String8 guest_join   = pick_guest_join_label(inst.pc, guest_labels);
	const String8 sc_join_root = pick_sc_join_root(m_code, inst.pc, sc_join_order, sc_join_srcs);
	const String8 after_loop   = !sc_join_root.IsEmpty() ? sc_join_root : guest_join;

	// pending_branch: previous block still needs a terminator OpBranch %next.
	// skip_branch_to_next: next OpLabel is already the target of a prior OpBranch
	// (do not emit a second OpBranch before it).
	bool pending_branch      = !previous_is_terminator(prev);
	bool skip_branch_to_next = false;
	int  labels_num          = 0;

	// ----- Phase 1: loop-merge guest labels --------------------------------
	// Must precede sc_joins so OpLoopMerge targets stay inside the outer selection.
	for (uint32_t i = 0; i < loop_merges.Size(); i++)
	{
		const String8 name = loop_merges[i]->ToString();
		if (pending_branch && !skip_branch_to_next)
		{
			m_source += String8::FromPrintf("               OpBranch %%%s\n", name.c_str());
		}
		skip_branch_to_next = false;
		pending_branch      = false;
		m_source += String8::FromPrintf("       %%%s = OpLabel\n", name.c_str());
		labels_num++;

		if (i + 1 < loop_merges.Size())
		{
			// Chain sibling loop merges; next iteration emits OpBranch %next.
			pending_branch = true;
		} else if (!sc_join_root.IsEmpty() || guest_labels.Size() > 0)
		{
			// Leave the loop into the selection merge chain / guest join.
			// Only branch when a real successor block will be materialised —
			// never to a synthetic label_%04x that has no OpLabel.
			m_source += String8::FromPrintf("               OpBranch %%%s\n", after_loop.c_str());
			skip_branch_to_next = true; // after_loop OpLabel is next
			pending_branch      = false;
		} else
		{
			// Sole label at this PC is the loop merge: host the guest body here.
			pending_branch = false;
		}
	}

	// ----- Phase 2: synthetic sc_join merges -------------------------------
	bool sc_joins_branch_to_guest = false;
	if (sc_join_order.Size() > 0)
	{
		// Linear fallthrough (no loop merges) enters the owning sc_join.
		if (loop_merges.Size() == 0 && pending_branch && !skip_branch_to_next)
		{
			const uint32_t owner = ScJoinFindOwner(m_code, prev.pc, inst.pc, sc_join_srcs);
			const uint32_t entry_src =
			    (owner != 0) ? owner : sc_join_order[sc_join_order.Size() - 1];
			m_source += String8::FromPrintf("               OpBranch %%%s\n",
			                                ScJoinMergeName(inst.pc, entry_src).c_str());
			skip_branch_to_next = true;
			pending_branch      = false;
		}

		// Materialize each sc_join as its own block. Inbound edges come from
		// OpSelectionMerge / case closers / phase1, not linear sibling fallthrough.
		for (uint32_t s = 0; s < sc_join_order.Size(); s++)
		{
			const uint32_t src    = sc_join_order[s];
			const auto     name   = ScJoinMergeName(inst.pc, src);
			const uint32_t parent = ScJoinFindParent(m_code, src, inst.pc, sc_join_srcs);
			// Root closes onto a real guest label when one exists; otherwise the
			// root hosts the guest instruction body (no synthetic branch target).
			const String8 next = (parent != 0)   ? ScJoinMergeName(inst.pc, parent)
			                     : (guest_labels.Size() > 0) ? guest_join
			                                                : String8();

			// Consume skip flag from phase1/owner entry.
			skip_branch_to_next = false;

			m_source += String8::FromPrintf("       %%%s = OpLabel\n", name.c_str());
			if (!next.IsEmpty())
			{
				m_source += String8::FromPrintf("               OpBranch %%%s\n", next.c_str());
			}
			labels_num++;
			if (parent == 0 && !next.IsEmpty())
			{
				sc_joins_branch_to_guest = true;
				skip_branch_to_next      = true; // guest_join OpLabel is next
			}
		}
		pending_branch = false;
	}

	// ----- Phase 3: remaining guest labels (headers / outer join) ----------
	for (uint32_t g = 0; g < guest_labels.Size(); g++)
	{
		auto& label = *guest_labels[g];

		const bool discard = m_code.ReadBlock(label.GetDst()).is_discard;

		// Root sc_joins already OpBranch to the first guest label; do not emit a
		// second terminator. Same for phase1→guest when no sc_joins.
		bool skip_branch = discard || skip_branch_to_next || sc_joins_branch_to_guest ||
		                   ((previous_is_terminator(prev)) && labels_num == 0);
		skip_branch_to_next      = false;
		sc_joins_branch_to_guest = false;

		if (!skip_branch)
		{
			m_source += String8::FromPrintf("               OpBranch %%%s\n", label.ToString().c_str());
		}
		m_source += String8::FromPrintf("       %%%s = OpLabel\n", label.ToString().c_str());
		labels_num++;

		// Backward SBranch / SCbranch targets are loop headers: emit OpLoopMerge.
		bool backward_loop_source    = false;
		bool backward_uncond_sbranch = false;
		classify_backward_loop_header(m_code, label, &backward_loop_source, &backward_uncond_sbranch);

		if (backward_loop_source)
		{
			if (backward_uncond_sbranch && sbranch_loop_has_labeled_exit(m_code, label))
			{
				// OpLoopMerge deferred to the labeled exit site.
				continue;
			}

			// SPIR-V requires OpLoopMerge immediately followed by OpBranch or
			// OpBranchConditional. Guest body becomes a dedicated successor.
			const String8 continue_label = String8::FromPrintf("loop_continue_%04" PRIx32, label.GetSrc());
			const String8 merge_label    = String8::FromPrintf("loop_merge_%04" PRIx32, label.GetSrc());
			const String8 body_label     = String8::FromPrintf("loop_body_%04" PRIx32, label.GetSrc());
			m_source += String8::FromPrintf("               OpLoopMerge %%%s %%%s None\n"
			                                "               OpBranch %%%s\n"
			                                "       %%%s = OpLabel\n",
			                                merge_label.c_str(), continue_label.c_str(), body_label.c_str(),
			                                body_label.c_str());
		}

		if (discard)
		{
			label.Disable();
			break;
		}
	}

	// No labels/sc_joins at all but pending fallthrough: nothing to do (guest
	// instruction stream continues in the current block).
	(void)pending_branch;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
