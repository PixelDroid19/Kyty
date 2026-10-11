#include "Emulator/Graphics/ShaderImageGradientProof.h"

#include "ShaderNativeWaveInternal.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;
using Bits    = std::bitset<256>;

constexpr uint32_t kNoInstruction = std::numeric_limits<uint32_t>::max();

struct RegisterRange
{
	unsigned first = 0;
	unsigned count = 0;
};

struct StaticControlFlow
{
	std::map<uint32_t, uint32_t>       instruction_by_pc;
	std::vector<std::vector<uint32_t>> successors;
};

bool EmptyOperand(const ShaderOperand& operand)
{
	return operand.type == Operand::Unknown && operand.constant.u == 0u && operand.register_id == 0 && operand.size == 0 &&
	       operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u &&
	       !operand.dpp && operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u &&
	       !operand.dpp_fetch_inactive && !operand.dpp_bound_ctrl && operand.dpp_unmodeled_bits == 0u;
}

bool PlainOperand(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u &&
	       !operand.dpp && operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u &&
	       !operand.dpp_fetch_inactive && !operand.dpp_bound_ctrl && operand.dpp_unmodeled_bits == 0u;
}

bool PlainRegister(const ShaderOperand& operand, Operand type, unsigned size)
{
	return PlainOperand(operand) && operand.type == type && operand.size == static_cast<int>(size) && operand.register_id >= 0 &&
	       operand.constant.u == 0u;
}

bool PlainVgpr(const ShaderOperand& operand, unsigned size = 1)
{
	return PlainRegister(operand, Operand::Vgpr, size) && static_cast<unsigned>(operand.register_id) + size <= 256u;
}

bool PlainScalarSource(const ShaderOperand& operand)
{
	if (!PlainOperand(operand)) { return false; }
	switch (operand.type)
	{
		case Operand::Sgpr:
			return operand.size == 1 && operand.register_id >= 0 && operand.register_id < 128 && operand.constant.u == 0u;
		case Operand::LiteralConstant:
		case Operand::IntegerInlineConstant:
		case Operand::FloatInlineConstant:
			return operand.size == 0 && operand.register_id == 0;
		default: return false;
	}
}

bool IsBranch(Type type)
{
	return type == Type::SBranch || type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz ||
	       type == Type::SCbranchVccnz || type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

bool IsConditionalBranch(Type type)
{
	return type == Type::SCbranchScc0 || type == Type::SCbranchScc1 || type == Type::SCbranchVccz || type == Type::SCbranchVccnz ||
	       type == Type::SCbranchExecz || type == Type::SCbranchExecnz;
}

bool IsUnmodeledControlOrRegisterWrite(Type type)
{
	switch (type)
	{
		case Type::Unknown:
		case Type::SBarrier:
		case Type::SSetpcB64:
		case Type::SSwappcB64:
		// These operations use a run-time register index or modify one lane of
		// a banked VGPR, so their complete write set is not represented by dst.
		case Type::VMovrelsB32:
		case Type::VMovreldB32:
		case Type::VMovrelsdB32:
		case Type::VMovrelsd2B32:
		case Type::VWritelaneB32: return true;
		default: return false;
	}
}

bool PlainInstructionMetadata(const ShaderInstruction& instruction)
{
	if (instruction.sopp_opcode != 0xffu || instruction.vop3_op_sel != 0u || instruction.vop3_omod != 0u ||
	    instruction.vop3p_op_sel_hi != 0xffu || instruction.vop_sdwa || instruction.vop_sdwa_ctrl != 0u ||
	    instruction.mimg_address_num != 0 || instruction.mimg_dmask != 0u || instruction.mimg_dimension != 0u ||
	    instruction.mimg_explicit_lod || instruction.mimg_offset || instruction.mimg_return_old_value || instruction.smem_imm_offset != 0 ||
	    instruction.smem_flags != 0xffu || instruction.flat_offset != 0 || instruction.buffer_imm_offset != 0u ||
	    instruction.buffer_idxen || instruction.buffer_offen || instruction.buffer_return_old_value || instruction.buffer_flags != 0xffu ||
	    instruction.mtbuf_format != 0xffu || instruction.mtbuf_components != 0u || instruction.mtbuf_format_is_gen5 ||
	    instruction.ds_offset != 0u || instruction.ds_encoding_control != 0u || instruction.ds_encoding_registers != 0u ||
	    instruction.exp_control != 0xffu || instruction.exp_enable_mask != 0x0fu || !EmptyOperand(instruction.dst2))
	{
		return false;
	}
	for (const auto& address: instruction.mimg_address)
	{
		if (!EmptyOperand(address)) { return false; }
	}
	return true;
}

bool PlainUnusedSources(const ShaderInstruction& instruction, int first_unused)
{
	for (int source = first_unused; source < 4; ++source)
	{
		if (!EmptyOperand(instruction.src[source])) { return false; }
	}
	return true;
}

bool CanonicalWqm(const ShaderInstruction& instruction)
{
	return instruction.type == Type::SWqmB64 && instruction.format == ShaderInstructionFormat::Sdst2Ssrc02 && instruction.src_num == 1 &&
	       PlainInstructionMetadata(instruction) && PlainRegister(instruction.dst, Operand::ExecLo, 2) &&
	       PlainRegister(instruction.src[0], Operand::ExecLo, 2) && PlainUnusedSources(instruction, 1);
}

bool FixedQuadBroadcast(const ShaderOperand& operand)
{
	if (operand.type != Operand::Vgpr || operand.register_id < 0 || operand.register_id >= 256 || operand.size != 1 ||
	    operand.constant.u != 0u || operand.multiplier != 1.0f || operand.absolute || operand.negate || operand.clamp ||
	    operand.swizzle != 6u || !operand.dpp || operand.dpp_row_mask != 0x0fu || operand.dpp_bank_mask != 0x0fu ||
	    operand.dpp_fetch_inactive || !operand.dpp_bound_ctrl || operand.dpp_unmodeled_bits != 0u)
	{
		return false;
	}
	switch (operand.dpp_ctrl)
	{
		case 0x00u:
		case 0x55u:
		case 0xaau:
		case 0xffu: return true;
		default: return false;
	}
}

bool VgprRange(const ShaderOperand& operand, RegisterRange* range)
{
	if (operand.type != Operand::Vgpr) { return false; }
	if (operand.register_id < 0 || operand.size <= 0 || static_cast<unsigned>(operand.register_id) + static_cast<unsigned>(operand.size) > 256u)
	{
		return false;
	}
	range->first = static_cast<unsigned>(operand.register_id);
	range->count = static_cast<unsigned>(operand.size);
	return true;
}

bool CollectVgprWrites(const ShaderInstruction& instruction, std::array<RegisterRange, 2>* ranges, unsigned* count)
{
	*count = 0;
	const bool buffer_instruction = ShaderInstructionTypeStartsWith(instruction.type, "Buffer") ||
	                               ShaderInstructionTypeStartsWith(instruction.type, "TBuffer");
	constexpr uint8_t unmodeled_buffer_write_flags = 0x85u; // LDS reroutes data; TFE adds status; bit 7 is unmodeled.
	if (buffer_instruction && (instruction.buffer_flags & unmodeled_buffer_write_flags) != 0u) { return false; }
	for (const auto* destination: {&instruction.dst, &instruction.dst2})
	{
		if (destination->type != Operand::Vgpr) { continue; }
		if (*count >= ranges->size() || !VgprRange(*destination, &(*ranges)[*count])) { return false; }
		++*count;
	}
	return true;
}

bool BranchTarget(const ShaderInstruction& instruction, uint32_t* target)
{
	if (!IsBranch(instruction.type) || instruction.format != ShaderInstructionFormat::Label || instruction.src_num != 1 ||
	    instruction.src[0].type != Operand::LiteralConstant || !PlainOperand(instruction.src[0]))
	{
		return false;
	}
	const int64_t address = static_cast<int64_t>(instruction.pc) + 4 + instruction.src[0].constant.i;
	if (address < 0 || address > UINT32_MAX || (address & 3) != 0) { return false; }
	*target = static_cast<uint32_t>(address);
	return true;
}

bool BuildStaticControlFlow(const ShaderCode& code, StaticControlFlow* flow)
{
	const auto& instructions = code.GetInstructions();
	if (instructions.IsEmpty()) { return false; }
	flow->successors.resize(instructions.Size());
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		if ((instruction.pc & 3u) != 0u || instruction.pc > UINT32_MAX - 4u ||
		    (index != 0u && instruction.pc <= instructions.At(index - 1u).pc) || IsUnmodeledControlOrRegisterWrite(instruction.type))
		{
			return false;
		}
		if (!flow->instruction_by_pc.emplace(instruction.pc, index).second) { return false; }
		std::array<RegisterRange, 2> unused_ranges {};
		unsigned unused_count = 0;
		if (!CollectVgprWrites(instruction, &unused_ranges, &unused_count)) { return false; }
	}

	using Label = std::pair<uint32_t, uint32_t>; // destination PC, source PC
	std::multiset<Label> expected_direct_labels;
	std::multiset<Label> expected_fallthrough_labels;
	for (uint32_t index = 0; index < instructions.Size(); ++index)
	{
		const auto& instruction = instructions.At(index);
		if (IsBranch(instruction.type))
		{
			uint32_t target_pc = 0;
			if (!BranchTarget(instruction, &target_pc)) { return false; }
			const auto target = flow->instruction_by_pc.find(target_pc);
			if (target == flow->instruction_by_pc.end()) { return false; }
			flow->successors[index].push_back(target->second);
			if (target_pc != 0u || instruction.pc != 0u) { expected_direct_labels.emplace(target_pc, instruction.pc); }
			if (IsConditionalBranch(instruction.type))
			{
				if (index + 1u >= instructions.Size() || instructions.At(index + 1u).pc != instruction.pc + 4u) { return false; }
				flow->successors[index].push_back(index + 1u);
				expected_fallthrough_labels.emplace(instruction.pc + 4u, instruction.pc);
			}
		}
		else if (instruction.type == Type::SEndpgm)
		{
			continue;
		}
		else
		{
			if (index + 1u >= instructions.Size()) { return false; }
			flow->successors[index].push_back(index + 1u);
		}
	}

	std::multiset<Label> actual_direct_labels;
	for (uint32_t index = 0; index < code.GetLabels().Size(); ++index)
	{
		const auto& label = code.GetLabels().At(index);
		if (!label.IsDisabled()) { actual_direct_labels.emplace(label.GetDst(), label.GetSrc()); }
	}
	std::multiset<Label> actual_fallthrough_labels;
	for (uint32_t index = 0; index < code.GetIndirectLabels().Size(); ++index)
	{
		const auto& label = code.GetIndirectLabels().At(index);
		if (!label.IsDisabled()) { actual_fallthrough_labels.emplace(label.GetDst(), label.GetSrc()); }
	}
	return actual_direct_labels == expected_direct_labels && actual_fallthrough_labels == expected_fallthrough_labels;
}

bool UniformInput(const ShaderOperand& operand, const Bits& uniform)
{
	if (operand.type == Operand::Vgpr)
	{
		if (!PlainVgpr(operand)) { return false; }
		return uniform[static_cast<size_t>(operand.register_id)];
	}
	return PlainScalarSource(operand);
}

bool IsCanonicalVectorAlu(const ShaderInstruction& instruction, Type type, int source_count)
{
	const auto expected_format = type == Type::VMovB32 ? ShaderInstructionFormat::SVdstSVsrc0 : ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	return instruction.type == type && instruction.format == expected_format && instruction.src_num == source_count &&
	       PlainInstructionMetadata(instruction) && PlainVgpr(instruction.dst) && PlainUnusedSources(instruction, source_count);
}

bool FullVectorSource(const ShaderOperand& operand)
{
	if (operand.multiplier != 1.0f || operand.clamp || operand.swizzle != 6u || operand.dpp || operand.dpp_ctrl != 0u ||
	    operand.dpp_row_mask != 0u || operand.dpp_bank_mask != 0u || operand.dpp_fetch_inactive || operand.dpp_bound_ctrl ||
	    operand.dpp_unmodeled_bits != 0u)
	{
		return false;
	}
	switch (operand.type)
	{
		case Operand::Vgpr:
			return operand.size == 1 && operand.register_id >= 0 && operand.register_id < 256 && operand.constant.u == 0u;
		case Operand::Sgpr:
			return operand.size == 1 && operand.register_id >= 0 && operand.register_id < 128 && operand.constant.u == 0u;
		case Operand::LiteralConstant:
		case Operand::IntegerInlineConstant:
		case Operand::FloatInlineConstant: return operand.size == 0 && operand.register_id == 0;
		default: return false;
	}
}

bool FullyDefinesVectorDestination(const ShaderInstruction& instruction, const Bits& written_in_wqm)
{
	if (instruction.type == Type::VMovB32 && IsCanonicalVectorAlu(instruction, Type::VMovB32, 1))
	{
		const auto& source = instruction.src[0];
		if (FixedQuadBroadcast(source)) { return written_in_wqm.test(static_cast<size_t>(source.register_id)); }
		return FullVectorSource(source);
	}

	if (instruction.type == Type::VSubF32 && IsCanonicalVectorAlu(instruction, Type::VSubF32, 2))
	{
		const auto& source0 = instruction.src[0];
		const bool   source0_defined = FixedQuadBroadcast(source0)
		                                  ? written_in_wqm.test(static_cast<size_t>(source0.register_id))
		                                  : FullVectorSource(source0);
		return source0_defined && FullVectorSource(instruction.src[1]);
	}

	const bool mad = instruction.type == Type::VMadF32 && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2 &&
	                 instruction.src_num == 3;
	const bool mac = instruction.type == Type::VMacF32 && instruction.format == ShaderInstructionFormat::SVdstSVsrc0SVsrc1 &&
	                 instruction.src_num == 2;
	if ((mad || mac) && PlainInstructionMetadata(instruction) && PlainVgpr(instruction.dst) &&
	    PlainUnusedSources(instruction, instruction.src_num))
	{
		// v_mac_f32 reads VDST as its accumulator, so require an earlier full
		// write in the same WQM interval before treating its result as defined.
		if (mac && !written_in_wqm.test(static_cast<size_t>(instruction.dst.register_id))) { return false; }
		for (int source = 0; source < instruction.src_num; ++source)
		{
			// These two observed full-dword forms permit source abs/negate. They
			// establish a complete write footprint only, not quad uniformity.
			if (!FullVectorSource(instruction.src[source])) { return false; }
		}
		return true;
	}
	return false;
}

bool PrefixTransfer(const ShaderInstruction& instruction, Bits* uniform, Bits* written_in_wqm)
{
	std::array<RegisterRange, 2> writes {};
	unsigned write_count = 0;
	if (!CollectVgprWrites(instruction, &writes, &write_count)) { return false; }
	if (write_count == 0u) { return true; }

	bool   output_uniform = false;
	unsigned uniform_destination = 0;
	if (write_count == 1u && writes[0].count == 1u && IsCanonicalVectorAlu(instruction, Type::VMovB32, 1))
	{
		const auto& source = instruction.src[0];
		if (FixedQuadBroadcast(source))
		{
			output_uniform = written_in_wqm->test(static_cast<size_t>(source.register_id));
		}
		else
		{
			output_uniform = UniformInput(source, *uniform);
		}
		uniform_destination = writes[0].first;
	}
	else if (write_count == 1u && writes[0].count == 1u && IsCanonicalVectorAlu(instruction, Type::VSubF32, 2))
	{
		const auto& dpp_source = instruction.src[0];
		const bool source0_uniform = FixedQuadBroadcast(dpp_source)
		                                 ? written_in_wqm->test(static_cast<size_t>(dpp_source.register_id))
		                                 : UniformInput(dpp_source, *uniform);
		output_uniform = source0_uniform && UniformInput(instruction.src[1], *uniform);
		uniform_destination = writes[0].first;
	}

	const bool fully_defined = FullyDefinesVectorDestination(instruction, *written_in_wqm);
	for (unsigned write = 0; write < write_count; ++write)
	{
		for (unsigned reg = writes[write].first; reg < writes[write].first + writes[write].count; ++reg)
		{
			uniform->reset(reg);
			written_in_wqm->reset(reg);
		}
	}
	if (fully_defined && write_count == 1u && writes[0].count == 1u) { written_in_wqm->set(writes[0].first); }
	if (output_uniform && fully_defined) { uniform->set(uniform_destination); }
	return true;
}

bool PlainWideningPrefix(const ShaderCode& code, uint32_t target_index, uint32_t* boundary_index, Bits* uniform_at_boundary,
	                     uint32_t* wqm_index)
{
	const auto& instructions = code.GetInstructions();
	Bits        uniform;
	Bits        written_in_wqm;
	bool        saw_wqm = false;
	*wqm_index        = kNoInstruction;

	for (uint32_t index = 0; index < target_index; ++index)
	{
		const auto& instruction = instructions.At(index);
		if (instruction.type == Type::SEndpgm) { return false; }
		if (!saw_wqm)
		{
			if (IsBranch(instruction.type) || (ShaderInstructionWritesExec(instruction) && !CanonicalWqm(instruction))) { return false; }
			if (instruction.type == Type::SWqmB64)
			{
				if (!CanonicalWqm(instruction)) { return false; }
				saw_wqm   = true;
				*wqm_index = index;
			}
			continue;
		}

		if (IsBranch(instruction.type) || (ShaderInstructionWritesExec(instruction) && !CanonicalWqm(instruction)))
		{
			*boundary_index      = index;
			*uniform_at_boundary = uniform;
			return true;
		}
		if (!PrefixTransfer(instruction, &uniform, &written_in_wqm)) { return false; }
	}

	if (!saw_wqm) { return false; }
	*boundary_index      = target_index;
	*uniform_at_boundary = uniform;
	return true;
}

bool ImageSampleCdGradients(const ShaderCode& code, uint32_t instruction_index, std::array<unsigned, 4>* gradients)
{
	const auto& instructions = code.GetInstructions();
	if (code.GetType() != ShaderType::Pixel || instruction_index >= instructions.Size()) { return false; }
	const auto& instruction = instructions.At(instruction_index);
	unsigned    dmask_components = 0;
	for (unsigned mask = instruction.mimg_dmask; mask != 0u; mask &= mask - 1u) { ++dmask_components; }
	if (instruction.type != Type::ImageSampleCd || instruction.format != ShaderInstructionFormat::VdataVaddr6StSsMimgDmask ||
	    instruction.mimg_dimension != 1u || instruction.src_num != 3 || instruction.mimg_address_num < 0 ||
	    (instruction.mimg_address_num != 0 && instruction.mimg_address_num != 9 && instruction.mimg_address_num != 13) ||
	    instruction.mimg_dmask == 0u || (instruction.mimg_dmask & 0xf0u) != 0u ||
	    instruction.mimg_explicit_lod || instruction.mimg_offset || instruction.mimg_return_old_value ||
	    (instruction.mimg_address_num == 0 ? !PlainVgpr(instruction.src[0], 6) : !PlainVgpr(instruction.src[0], 1)) ||
	    !PlainRegister(instruction.src[1], Operand::Sgpr, 8) ||
	    !PlainRegister(instruction.src[2], Operand::Sgpr, 4) || !EmptyOperand(instruction.src[3]) ||
	    instruction.dst.type != Operand::Vgpr || instruction.dst.size <= 0 ||
	    static_cast<unsigned>(instruction.dst.size) != dmask_components ||
	    !PlainVgpr(instruction.dst, static_cast<unsigned>(instruction.dst.size)) || !EmptyOperand(instruction.dst2))
	{
		return false;
	}
	if (instruction.mimg_address_num != 0)
	{
		if (instruction.mimg_address[0].register_id != instruction.src[0].register_id) { return false; }
		for (unsigned address = 0; address < 6u; ++address)
		{
			if (!PlainVgpr(instruction.mimg_address[address])) { return false; }
		}
		for (unsigned gradient = 0; gradient < gradients->size(); ++gradient)
		{
			(*gradients)[gradient] = static_cast<unsigned>(instruction.mimg_address[gradient].register_id);
		}
		return true;
	}
	for (unsigned gradient = 0; gradient < gradients->size(); ++gradient)
	{
		(*gradients)[gradient] = static_cast<unsigned>(instruction.src[0].register_id) + gradient;
	}
	return true;
}

bool NoExternalEdgeIntoPrefix(const StaticControlFlow& flow, uint32_t boundary_index)
{
	for (uint32_t source = boundary_index; source < flow.successors.size(); ++source)
	{
		for (const auto target: flow.successors[source])
		{
			if (target < boundary_index) { return false; }
		}
	}
	return true;
}

bool ClearGradientWrites(const ShaderInstruction& instruction, const std::array<unsigned, 4>& gradients, Bits* state)
{
	std::array<RegisterRange, 2> writes {};
	unsigned write_count = 0;
	if (!CollectVgprWrites(instruction, &writes, &write_count)) { return false; }
	for (unsigned write = 0; write < write_count; ++write)
	{
		const unsigned end = writes[write].first + writes[write].count;
		for (unsigned gradient = 0; gradient < gradients.size(); ++gradient)
		{
			if (gradients[gradient] >= writes[write].first && gradients[gradient] < end) { state->reset(gradient); }
		}
	}
	return true;
}

bool QuadUniformAtSample(const ShaderCode& code, const StaticControlFlow& flow, uint32_t boundary_index, uint32_t sample_index,
	                     const std::array<unsigned, 4>& gradients, const Bits& uniform_before_boundary)
{
	if (boundary_index > sample_index || !NoExternalEdgeIntoPrefix(flow, boundary_index)) { return false; }

	const auto& instructions = code.GetInstructions();
	std::vector<Bits> states(instructions.Size());
	std::vector<bool> reachable(instructions.Size(), false);
	std::vector<uint32_t> worklist;
	states[boundary_index].reset();
	for (unsigned gradient = 0; gradient < gradients.size(); ++gradient)
	{
		if (uniform_before_boundary[gradients[gradient]]) { states[boundary_index].set(gradient); }
	}
	reachable[boundary_index] = true;
	worklist.push_back(boundary_index);

	for (size_t cursor = 0; cursor < worklist.size(); ++cursor)
	{
		const uint32_t source = worklist[cursor];
		Bits           outgoing = states[source];
		if (!ClearGradientWrites(instructions.At(source), gradients, &outgoing)) { return false; }
		for (const auto target: flow.successors[source])
		{
			if (target < boundary_index) { return false; }
			if (!reachable[target])
			{
				states[target]    = outgoing;
				reachable[target] = true;
				worklist.push_back(target);
				continue;
			}
			const Bits merged = states[target] & outgoing;
			if (merged != states[target])
			{
				states[target] = merged;
				worklist.push_back(target);
			}
		}
	}

	if (!reachable[sample_index]) { return false; }
	for (unsigned gradient = 0; gradient < gradients.size(); ++gradient)
	{
		if (!states[sample_index].test(gradient)) { return false; }
	}
	return true;
}

} // namespace

bool ShaderImageSampleCdHasQuadUniformGradients(const ShaderCode& code, uint32_t instruction_index)
{
	std::array<unsigned, 4> gradients {};
	if (!ImageSampleCdGradients(code, instruction_index, &gradients)) { return false; }

	StaticControlFlow flow;
	if (!BuildStaticControlFlow(code, &flow)) { return false; }

	Bits      uniform_at_boundary;
	uint32_t  boundary_index = kNoInstruction;
	uint32_t  wqm_index      = kNoInstruction;
	if (!PlainWideningPrefix(code, instruction_index, &boundary_index, &uniform_at_boundary, &wqm_index) ||
	    boundary_index == kNoInstruction || wqm_index == kNoInstruction ||
	    !NoExternalEdgeIntoPrefix(flow, boundary_index))
	{
		return false;
	}
	return QuadUniformAtSample(code, flow, boundary_index, instruction_index, gradients, uniform_at_boundary);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
