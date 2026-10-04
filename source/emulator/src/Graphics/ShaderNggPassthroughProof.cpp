#include "Emulator/Graphics/ShaderNggPassthroughProof.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <array>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

using Bits = ShaderNggKnownBits;
using Reject = ShaderNggPassthroughRejection;
using Kind = ShaderNggPassthroughStepKind;
using Type = ShaderInstructionType;
using Operand = ShaderOperandType;

constexpr unsigned kVccLo = 104;
constexpr unsigned kExecLo = 106;
constexpr unsigned kM0 = 108;
constexpr unsigned kScc = 109;

uint64_t LowMask(unsigned bits)
{
	return bits == 64 ? UINT64_MAX : (uint64_t {1} << bits) - 1u;
}

Bits Constant(uint64_t value, unsigned width = 32)
{
	return {value & LowMask(width), LowMask(width), {}};
}

Bits Boolean(unsigned possibilities, const ShaderNggScalarDependencies& dependencies)
{
	// Bit 0 of possibilities means false is possible; bit 1 means true.
	Bits result {possibilities == 2u ? 1u : 0u, possibilities == 3u ? 0xfffffffeu : 0xffffffffu, dependencies};
	return result;
}

Bits Nonzero(const Bits& input, unsigned width)
{
	const auto mask = LowMask(width);
	return Boolean((input.value & mask) != 0u ? 2u : (input.known_mask & mask) == mask ? 1u : 3u,
	               input.initial_dependencies);
}

Bits Bitwise(const Bits& a, const Bits& b, bool is_or)
{
	const auto ones = is_or ? a.value | b.value : a.value & b.value;
	const auto az = a.known_mask & ~a.value;
	const auto bz = b.known_mask & ~b.value;
	return {ones, ones | (is_or ? az & bz : az | bz), a.initial_dependencies | b.initial_dependencies};
}

Bits Shift(const Bits& a, const Bits& count, unsigned width, bool left)
{
	Bits result;
	result.initial_dependencies = a.initial_dependencies | count.initial_dependencies;
	const auto count_mask = width - 1u;
	if ((count.known_mask & count_mask) != count_mask) { return result; }
	const auto n = static_cast<unsigned>(count.value & count_mask);
	const auto mask = LowMask(width);
	result.value = (left ? a.value << n : a.value >> n) & mask;
	result.known_mask = left ? ((a.known_mask << n) | LowMask(n)) & mask
	                         : (a.known_mask >> n) | (mask & ~LowMask(width - n));
	return result;
}

// s_sub_i32 sets SCC on signed overflow; s_sub_u32 sets it on the unsigned borrow out of bit 31 (RDNA2 12.1).
Bits Subtract(const Bits& a, const Bits& b, Bits* scc, bool unsigned_borrow)
{
	// RDNA2 12.1: modulo-32 subtraction; SCC is the signed overflow or the unsigned borrow.
	// Propagate the set of possible borrows one bit at a time. This derives
	// low bits through unknown high system fields without choosing those fields.
	Bits result;
	result.initial_dependencies = a.initial_dependencies | b.initial_dependencies;
	unsigned borrow = 1u; // Only borrow-in zero is initially possible.
	unsigned overflow = 0;
	for (unsigned bit = 0; bit < 32; ++bit)
	{
		const uint64_t mask = uint64_t {1} << bit;
		unsigned values = 0;
		unsigned next_borrow = 0;
		for (unsigned av = 0; av < 2; ++av)
		{
			if ((a.known_mask & mask) != 0u && av != ((a.value >> bit) & 1u)) { continue; }
			for (unsigned bv = 0; bv < 2; ++bv)
			{
				if ((b.known_mask & mask) != 0u && bv != ((b.value >> bit) & 1u)) { continue; }
				for (unsigned c = 0; c < 2; ++c)
				{
					if ((borrow & (1u << c)) == 0u) { continue; }
					const auto v = (av - bv - c) & 1u;
					values |= 1u << v;
					next_borrow |= 1u << (av < bv + c ? 1u : 0u);
					if (bit == 31) { overflow |= 1u << (av != bv && av != v ? 1u : 0u); }
				}
			}
		}
		if (values != 3u)
		{
			result.known_mask |= mask;
			if (values == 2u) { result.value |= mask; }
		}
		borrow = next_borrow;
	}
	*scc = Boolean(unsigned_borrow ? borrow : overflow, result.initial_dependencies);
	return result;
}

bool Plain(const ShaderOperand& op)
{
	return op.multiplier == 1.0f && !op.absolute && !op.negate && !op.clamp && op.swizzle == 6u && !op.dpp &&
	       op.dpp_ctrl == 0u && op.dpp_row_mask == 0u && op.dpp_bank_mask == 0u && !op.dpp_fetch_inactive && !op.dpp_bound_ctrl;
}

bool Empty(const ShaderOperand& op)
{
	return op.type == Operand::Unknown && op.size == 0 && op.register_id == 0 && op.constant.u == 0u && Plain(op);
}

bool EncodedConstant(const ShaderOperand& op)
{
	return op.type == Operand::LiteralConstant || op.type == Operand::IntegerInlineConstant || op.type == Operand::FloatInlineConstant;
}

bool ConstantValueValid(const ShaderOperand& op)
{
	if (op.type == Operand::LiteralConstant) { return true; }
	if (op.type == Operand::IntegerInlineConstant) { return op.constant.i >= -16 && op.constant.i <= 64; }
	if (op.type == Operand::FloatInlineConstant)
	{
		switch (op.constant.u)
		{
			case 0x3f000000u:
			case 0xbf000000u:
			case 0x3f800000u:
			case 0xbf800000u:
			case 0x40000000u:
			case 0xc0000000u:
			case 0x40800000u:
			case 0xc0800000u:
			case 0x3e22f983u: return true;
			default: return false;
		}
	}
	return false;
}

bool CommonControls(const ShaderInstruction& inst)
{
	if (inst.vop3_op_sel != 0u || inst.vop3_omod != 0u || inst.vop3p_op_sel_hi != 0xffu || inst.vop_sdwa ||
	    inst.vop_sdwa_ctrl != 0u || inst.mimg_address_num != 0 || inst.mimg_dmask != 0u || inst.mimg_dimension != 0u ||
	    inst.mimg_explicit_lod || inst.mimg_return_old_value || inst.smem_imm_offset != 0 || inst.smem_flags != 0xffu ||
	    inst.buffer_imm_offset != 0u || inst.buffer_idxen || inst.buffer_offen || inst.buffer_return_old_value ||
	    inst.buffer_flags != 0xffu || inst.mtbuf_format != 0xffu || inst.mtbuf_components != 0u || inst.mtbuf_format_is_gen5 ||
	    inst.ds_offset != 0u || inst.ds_encoding_control != 0u || inst.ds_encoding_registers != 0u || !Empty(inst.dst2))
	{
		return false;
	}
	for (const auto& op: inst.mimg_address) { if (!Empty(op)) { return false; } }
	if (!Plain(inst.dst)) { return false; }
	for (int source = 0; source < 4; ++source)
	{
		const auto& op = inst.src[source];
		if (!Plain(op)) { return false; }
		// Keep the shared validator's canonical ENDPGM padding exception, but
		// no stale register/value/control is otherwise allowed past src_num.
		const bool end_padding = source == 0 && inst.type == Type::SEndpgm && inst.sopp_opcode == 1u &&
		                         op.type == Operand::LiteralConstant && op.size == 0 && op.register_id == 0 && op.constant.u == 0u;
		if (source >= inst.src_num && !Empty(op) && !end_padding) { return false; }
	}
	return inst.type == Type::Exp || (inst.exp_control == 0xffu && inst.exp_enable_mask == 15u);
}

int ScalarIndex(const ShaderOperand& op)
{
	switch (op.type)
	{
		case Operand::Sgpr: return op.register_id;
		case Operand::VccLo: return kVccLo;
		case Operand::VccHi: return kVccLo + 1;
		case Operand::ExecLo: return kExecLo;
		case Operand::ExecHi: return kExecLo + 1;
		case Operand::M0: return kM0;
		default: return -1;
	}
}

bool ScalarDestination(const ShaderOperand& op, unsigned width)
{
	if (op.size != static_cast<int>(width / 32u) || ScalarIndex(op) < 0) { return false; }
	return width == 32 || op.type == Operand::Sgpr || op.type == Operand::VccLo || op.type == Operand::ExecLo;
}

int VectorArity(Type type)
{
	// RDNA2 12.7/12.8: only full-word, lane-local, non-carry forms. No
	// implicit destination input, comparison/mask output, subgroup operation,
	// memory operation or floating-point mode change is included in this set.
	switch (type)
	{
		case Type::VMovB32:
		case Type::VCvtF32I32:
		case Type::VCvtF32U32: return 1;
		case Type::VAndB32:
		case Type::VOrB32:
		case Type::VXorB32:
		case Type::VLshlrevB32:
		case Type::VLshrrevB32:
		case Type::VAddI32:
		case Type::VSubI32:
		case Type::VAddF32:
		case Type::VSubF32:
		case Type::VMulF32: return 2;
		default: return 0;
	}
}

bool ModeledInstruction(Type type)
{
	if (VectorArity(type) != 0) { return true; }
	switch (type)
	{
		case Type::SMovB32:
		case Type::SMovB64:
		case Type::SLshrB64:
		case Type::SBfeU32:
		case Type::SAndB32:
		case Type::SLshlB32:
		case Type::SOrB32:
		case Type::SLshrB32:
		case Type::SSubI32:
		case Type::SSubU32:
		case Type::SInstPrefetch:
		case Type::VNop:
		case Type::SWaitcnt:
		case Type::SSendmsg:
		case Type::Exp:
		case Type::SEndpgm:
		// Classify these as control-flow refusals at their original source PC.
		case Type::SBranch:
		case Type::SCbranchScc0:
		case Type::SCbranchScc1:
		case Type::SCbranchVccz:
		case Type::SCbranchVccnz:
		case Type::SCbranchExecz:
		case Type::SCbranchExecnz:
		case Type::SSetpcB64:
		case Type::SSwappcB64: return true;
		default: return false;
	}
}

int ParameterIndex(ShaderInstructionFormat::Format format)
{
	// Format tokens are not contiguous enum values; compare the complete
	// semantic tuple instead of inferring a target from the original opcode.
	for (unsigned p = 0; p < 32; ++p)
	{
		const auto expected = ShaderInstructionFormat::FormatDefine(
		    {ShaderInstructionFormat::Param0 + p, ShaderInstructionFormat::S0, ShaderInstructionFormat::S1,
		     ShaderInstructionFormat::S2, ShaderInstructionFormat::S3});
		if (static_cast<uint64_t>(format) == expected) { return static_cast<int>(p); }
	}
	return -1;
}

struct VectorValue
{
	uint64_t defined = 0;
	uint64_t primitive = 0;
	uint64_t vertex_id = 0;
	uint64_t instance_id = 0;
};

class Analyzer
{
public:
	explicit Analyzer(const ShaderNggPassthroughCounts& counts, bool prologue_only = false): prologue_only_(prologue_only)
	{
		result.counts = counts;
		for (unsigned i = 0; i < scalar.size(); ++i) { scalar[i].initial_dependencies.set(i); }
	}

	ShaderNggPassthroughProof Run(const ShaderCode& code);

private:
	bool Fail(Reject rejection, const char* reason)
	{
		result.rejection = rejection;
		result.reason = reason;
		result.rejection_index = step.instruction_index;
		result.rejection_pc = step.pc;
		return false;
	}

	Bits Pair(unsigned index) const
	{
		return {scalar[index].value | (scalar[index + 1].value << 32u),
		        scalar[index].known_mask | (scalar[index + 1].known_mask << 32u),
		        scalar[index].initial_dependencies | scalar[index + 1].initial_dependencies};
	}

	bool ReadScalar(const ShaderOperand& op, unsigned width, Bits* value) const;
	bool Scalar(const ShaderInstruction& inst);
	bool Instruction(const ShaderInstruction& inst);
	bool Export(const ShaderInstruction& inst);
	bool Vector(const ShaderInstruction& inst, int arity);
	bool VertexSource(const ShaderOperand& op, VectorValue* value);
	bool ExecMask(uint64_t expected);
	bool Immediate(const ShaderInstruction& inst, uint32_t* value);
	bool PrologueComplete(const ShaderInstruction& inst) const;
	void FinishPrologue();
	void Retain(Kind kind)
	{
		step.kind = kind;
		step.retain = true;
		result.retained_instruction_indices.push_back(step.instruction_index);
	}

	ShaderNggPassthroughProof result;
	ShaderNggPassthroughStep step;
	std::array<Bits, 110> scalar {};
	std::array<VectorValue, 256> vector {};
	std::bitset<256> pending_export_sources;
	bool pending_export = false;
	bool primitive_input_intact = true;
	bool ended = false;
	bool prologue_only_ = false;
};

bool Analyzer::ReadScalar(const ShaderOperand& op, unsigned width, Bits* value) const
{
	if (EncodedConstant(op))
	{
		if (op.register_id != 0 || op.size != (width == 32 ? 0 : 2) || !ConstantValueValid(op)) { return false; }
		if (width == 64)
		{
			// Inline integer -1 denotes all 64 one bits. Literal and floating
			// pair expansion is deliberately outside this small proof domain.
			if (op.type != Operand::IntegerInlineConstant || op.constant.i < -16 || op.constant.i > 64) { return false; }
			*value = Constant(static_cast<uint64_t>(static_cast<int64_t>(op.constant.i)), 64);
		} else
		{
			*value = Constant(op.constant.u);
		}
		return true;
	}
	if (op.size != static_cast<int>(width / 32u)) { return false; }
	const int index = ScalarIndex(op);
	if (index >= 0)
	{
		if (!ScalarDestination(op, width)) { return false; }
		*value = width == 64 ? Pair(static_cast<unsigned>(index)) : scalar[index];
		return true;
	}
	if (width != 32) { return false; }
	if (op.type == Operand::Scc) { *value = scalar[kScc]; return true; }
	if (op.type == Operand::Null) { *value = Constant(0); return true; }
	if (op.type == Operand::ExecZ || op.type == Operand::VccZ)
	{
		*value = Nonzero(Pair(op.type == Operand::ExecZ ? kExecLo : kVccLo), result.counts.guest_wave_size);
		value->value = (value->value ^ 1u) & value->known_mask;
		return true;
	}
	return false;
}

bool Analyzer::Scalar(const ShaderInstruction& inst)
{
	unsigned width = 32;
	int arity = 2;
	bool move = false;
	switch (inst.type)
	{
		case Type::SMovB32: move = true; arity = 1; break;
		case Type::SMovB64: move = true; arity = 1; width = 64; break;
		case Type::SLshrB64: width = 64; break;
		case Type::SBfeU32:
		case Type::SAndB32:
		case Type::SLshlB32:
		case Type::SOrB32:
		case Type::SLshrB32:
		case Type::SSubI32:
		case Type::SSubU32: break;
		default: return Fail(Reject::UnsupportedInstruction, "instruction is outside the closed scalar/vector/export proof subset");
	}
	const auto format = width == 64 ? (move ? ShaderInstructionFormat::Sdst2Ssrc02 : ShaderInstructionFormat::Sdst2Ssrc02Ssrc1)
	                               : (move ? ShaderInstructionFormat::SVdstSVsrc0 : ShaderInstructionFormat::SVdstSVsrc0SVsrc1);
	if (inst.format != format || inst.src_num != arity || inst.sopp_opcode != 0xffu || !ScalarDestination(inst.dst, width))
	{
		return Fail(Reject::UnsupportedScalarOperand, "scalar instruction requires its exact full-word operand tuple");
	}
	for (int i = 0; i < arity; ++i)
	{
		if (!ReadScalar(inst.src[i], i == 0 ? width : 32, &step.scalar_sources[i]))
		{
			return Fail(Reject::UnsupportedScalarOperand, "scalar source kind/span or 64-bit constant expansion is unsupported");
		}
		step.scalar_source_mask |= 1u << i;
		result.scalar_dependencies |= step.scalar_sources[i].initial_dependencies;
	}
	const auto& a = step.scalar_sources[0];
	const auto& b = step.scalar_sources[1];
	Bits value;
	Bits scc = scalar[kScc];
	switch (inst.type)
	{
		case Type::SMovB32:
		case Type::SMovB64: value = a; break;
		case Type::SAndB32: value = Bitwise(a, b, false); break;
		case Type::SOrB32: value = Bitwise(a, b, true); break;
		case Type::SLshlB32: value = Shift(a, b, 32, true); break;
		case Type::SLshrB32: value = Shift(a, b, 32, false); break;
		case Type::SLshrB64: value = Shift(a, b, 64, false); break;
		case Type::SSubI32: value = Subtract(a, b, &scc, false); break;
		case Type::SSubU32: value = Subtract(a, b, &scc, true); break;
		case Type::SBfeU32:
		{
			// RDNA2 12.1: offset[4:0], width[22:16]. Widths greater
			// than one word are conservatively refused rather than host-shifted.
			if ((b.known_mask & 0x007f001fu) != 0x007f001fu)
			{
				return Fail(Reject::UnsupportedControl, "S_BFE_U32 offset/width must be known within the proved field domain");
			}
			const auto field_width = static_cast<unsigned>((b.value >> 16u) & 127u);
			if (field_width > 32) { return Fail(Reject::UnsupportedControl, "S_BFE_U32 field width exceeds this proof's 32-bit domain"); }
			value = Bitwise(Shift(a, b, 32, false), Constant(LowMask(field_width)), false);
			break;
		}
		default: return Fail(Reject::UnsupportedInstruction, "missing scalar transfer function");
	}
	if (!move && inst.type != Type::SSubI32 && inst.type != Type::SSubU32) { scc = Nonzero(value, width); }
	const int index = ScalarIndex(inst.dst);
	if ((index == static_cast<int>(kExecLo) || index == static_cast<int>(kExecLo + 1)) && pending_export)
	{
		return Fail(Reject::ExportDependency, "EXEC write before outstanding exports are drained by S_WAITCNT expcnt(0)");
	}
	step.scalar_result = value;
	scalar[index] = {value.value & 0xffffffffu, value.known_mask & 0xffffffffu, value.initial_dependencies};
	if (width == 64) { scalar[index + 1] = {value.value >> 32u, value.known_mask >> 32u, value.initial_dependencies}; }
	scalar[kScc] = scc;
	return true;
}

// The write after which EXEC is exactly the vertex-count mask, once the allocation
// request and the primitive forward are proved and the exports are drained.
bool Analyzer::PrologueComplete(const ShaderInstruction& inst) const
{
	if (result.allocation_index == ShaderNggPassthroughNoInstruction ||
	    result.primitive_export_index == ShaderNggPassthroughNoInstruction || pending_export ||
	    ScalarIndex(inst.dst) != static_cast<int>(kExecLo))
	{
		return false;
	}
	const auto exec = Pair(kExecLo);
	const auto wave = LowMask(result.counts.guest_wave_size);
	return (exec.known_mask & wave) == wave && (exec.value & wave) == result.vertex_mask;
}

void Analyzer::FinishPrologue()
{
	// SGPR 0..7, VCC_LO, VCC_HI, EXEC_LO, EXEC_HI, M0, SCC carry hardware wave state.
	ShaderNggScalarDependencies wave_words;
	for (unsigned word = 0; word < 8u; ++word) { wave_words.set(word); }
	for (unsigned word = kVccLo; word <= kScc; ++word) { wave_words.set(word); }
	for (unsigned word = 0; word < scalar.size(); ++word)
	{
		if ((scalar[word].initial_dependencies & wave_words).any()) { result.wave_dependent_scalars.set(word); }
	}
	result.prologue_proven = true;
	result.prologue_end_index = step.instruction_index;
}

bool Analyzer::ExecMask(uint64_t expected)
{
	const auto exec = Pair(kExecLo);
	const auto wave = LowMask(result.counts.guest_wave_size);
	result.scalar_dependencies |= exec.initial_dependencies;
	if ((exec.known_mask & wave) != wave)
	{
		return Fail(Reject::UnknownExec, "active architectural EXEC bits are not all proved; initial EXEC is unknown");
	}
	step.active_mask_known = true;
	step.active_mask = exec.value & wave;
	if (step.active_mask != expected)
	{
		return Fail(Reject::ExecMaskMismatch, "architectural EXEC activity differs from the explicit ES/GS count mask");
	}
	return true;
}

bool Analyzer::VertexSource(const ShaderOperand& op, VectorValue* value)
{
	if (EncodedConstant(op))
	{
		if (op.size != 0 || op.register_id != 0 || !ConstantValueValid(op))
		{
			return Fail(Reject::MalformedInstruction, "retained constant is not an encoded full-word inline/literal source");
		}
		value->defined = result.vertex_mask;
		return true;
	}
	if (op.type != Operand::Vgpr)
	{
		return Fail(Reject::ScalarVectorInput, "retained vertex instructions may not read scalar/SCC/VCC/EXEC numerically, even if known");
	}
	if (op.size != 1) { return Fail(Reject::MalformedInstruction, "retained VGPR source must have exactly one word"); }
	*value = vector[op.register_id];
	if ((value->defined & result.vertex_mask) != result.vertex_mask)
	{
		return Fail(Reject::UndefinedVectorInput, "VGPR is not defined on every planned ES vertex lane");
	}
	if ((value->primitive & result.vertex_mask) != 0u)
	{
		return Fail(Reject::PrimitiveVectorInput, "packed primitive input cannot escape into the independent vertex residue");
	}
	if ((value->vertex_id & result.vertex_mask) != 0u) { result.required_initial_vgprs.set(5); }
	if ((value->instance_id & result.vertex_mask) != 0u) { result.required_initial_vgprs.set(8); }
	return true;
}

bool Analyzer::Vector(const ShaderInstruction& inst, int arity)
{
	const auto format = arity == 1 ? ShaderInstructionFormat::SVdstSVsrc0 : ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
	if (inst.format != format || inst.src_num != arity || inst.dst.type != Operand::Vgpr || inst.dst.size != 1 ||
	    inst.sopp_opcode != 0xffu)
	{
		return Fail(Reject::MalformedInstruction, "lane-local instruction requires its exact non-carry full-word tuple");
	}
	if (!ExecMask(result.vertex_mask)) { return false; }
	if (inst.dst.register_id == 0 && result.primitive_export_index == ShaderNggPassthroughNoInstruction)
	{
		return Fail(Reject::PrimitiveSourceModified, "v0 may not be written before forwarding its original packed primitive");
	}
	if (pending_export_sources.test(inst.dst.register_id))
	{
		return Fail(Reject::ExportDependency, "VGPR write before an export using that source is drained by S_WAITCNT expcnt(0)");
	}
	VectorValue value;
	for (int i = 0; i < arity; ++i)
	{
		VectorValue source;
		if (!VertexSource(inst.src[i], &source)) { return false; }
		value.vertex_id |= source.vertex_id;
		value.instance_id |= source.instance_id;
	}
	const auto mask = result.vertex_mask;
	auto& dst = vector[inst.dst.register_id];
	dst.defined |= mask;
	dst.primitive &= ~mask;
	dst.vertex_id = (dst.vertex_id & ~mask) | (value.vertex_id & mask);
	dst.instance_id = (dst.instance_id & ~mask) | (value.instance_id & mask);
	if (inst.dst.register_id == 0) { primitive_input_intact = false; }
	Retain(Kind::VertexAlu);
	return true;
}

bool Analyzer::Immediate(const ShaderInstruction& inst, uint32_t* value)
{
	if (inst.format != ShaderInstructionFormat::Imm || inst.src_num != 1 || !Empty(inst.dst) ||
	    inst.src[0].type != Operand::LiteralConstant || inst.src[0].size != 0 || inst.src[0].register_id != 0 ||
	    inst.src[0].constant.u > 0xffffu)
	{
		return Fail(Reject::MalformedInstruction, "control instruction requires an exact 16-bit immediate and no destination");
	}
	*value = inst.src[0].constant.u;
	return true;
}

bool Analyzer::Export(const ShaderInstruction& inst)
{
	if (!Empty(inst.dst) || inst.sopp_opcode != 0xffu)
	{
		return Fail(Reject::InvalidExport, "export has an unexpected destination or scalar control identity");
	}
	if (result.allocation_index == ShaderNggPassthroughNoInstruction)
	{
		return Fail(Reject::AllocationSequence, "export precedes a proved GS allocation request");
	}
	if (inst.format == ShaderInstructionFormat::PrimVsrc0OffOffOffDone)
	{
		if (result.primitive_export_index != ShaderNggPassthroughNoInstruction)
		{
			return Fail(Reject::DuplicateExport, "primitive forwarding must occur exactly once");
		}
		if (inst.exp_control != 2u || inst.exp_enable_mask != 1u || inst.src_num != 1)
		{
			return Fail(Reject::InvalidExport, "primitive export requires DONE, EN=1, no VM/COMPR and one source");
		}
		if (!primitive_input_intact || inst.src[0].type != Operand::Vgpr || inst.src[0].register_id != 0 || inst.src[0].size != 1)
		{
			return Fail(Reject::PrimitiveSourceModified, "primitive export must forward the unchanged original v0 identity");
		}
		if (!ExecMask(result.primitive_mask)) { return false; }
		result.primitive_export_index = step.instruction_index;
		result.required_initial_vgprs.set(0);
		step.kind = Kind::PrimitiveForward;
	} else
	{
		if (result.primitive_export_index == ShaderNggPassthroughNoInstruction)
		{
			return Fail(Reject::AllocationSequence, "vertex export precedes primitive forwarding");
		}
		if (!ExecMask(result.vertex_mask)) { return false; }
		Kind kind;
		const int parameter = ParameterIndex(inst.format);
		if (inst.format == ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
		{
			if (result.position_export_index != ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::DuplicateExport, "POS0 must occur exactly once");
			}
			if (inst.exp_enable_mask != 15u || inst.exp_control != 2u || inst.src_num != 4)
			{
				return Fail(Reject::InvalidExport, "POS0 requires four defined full-precision channels and DONE without VM");
			}
			kind = Kind::PositionExport;
		} else if (inst.format == ShaderInstructionFormat::Pos1OffOffVsrc0Off)
		{
			if (result.layer_export_index != ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::DuplicateExport, "POS1 may occur at most once");
			}
			if (inst.exp_enable_mask != 4u || inst.exp_control != 0u || inst.src_num != 1 ||
			    result.position_export_index != ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::InvalidExport, "POS1 requires the EN=4 layer form before final DONE position export");
			}
			kind = Kind::LayerExport;
		} else if (parameter >= 0)
		{
			if ((result.parameter_mask & (1u << parameter)) != 0u)
			{
				return Fail(Reject::DuplicateExport, "parameter target is exported more than once");
			}
			if (inst.exp_enable_mask != 15u || inst.exp_control != 0u || inst.src_num != 4)
			{
				return Fail(Reject::InvalidExport, "parameter export requires full EN=0xf and no VM/DONE/COMPR");
			}
			kind = Kind::ParameterExport;
		} else
		{
			return Fail(Reject::InvalidExport, "export target is outside the position/layer/full-parameter proof contract");
		}
		for (int i = 0; i < inst.src_num; ++i)
		{
			VectorValue source;
			if (inst.src[i].type != Operand::Vgpr || inst.src[i].size != 1)
			{
				return Fail(Reject::InvalidExport, "export data must come from full-word VGPR sources");
			}
			if (!VertexSource(inst.src[i], &source)) { return false; }
		}
		if (kind == Kind::PositionExport) { result.position_export_index = step.instruction_index; }
		if (kind == Kind::LayerExport)
		{
			result.layer_export_index = step.instruction_index;
			result.requires_layer_output_contract = true;
		}
		if (kind == Kind::ParameterExport) { result.parameter_mask |= 1u << parameter; }
		Retain(kind);
	}
	// RDNA2 11.4: EXP reads EXEC and source VGPRs asynchronously. Nonzero
	// expcnt waits cannot establish which export TYPE has completed, so only
	// expcnt(0) (or ENDPGM's implicit drain) releases these conservative hazards.
	pending_export = true;
	for (int i = 0; i < inst.src_num; ++i) { pending_export_sources.set(inst.src[i].register_id); }
	return true;
}

bool Analyzer::Instruction(const ShaderInstruction& inst)
{
	uint32_t immediate = 0;
	switch (inst.type)
	{
		case Type::SInstPrefetch:
			if (!Immediate(inst, &immediate)) { return false; }
			// The decoder shares this type with S_NOP and trace instructions.
			// Use its preserved semantic discriminator; never accept that whole
			// bucket as a no-op. ISA 12.5: NOP 0..15, PREFETCH modes 1..3.
			if (!((inst.sopp_opcode == 0u && immediate <= 15u) ||
			      (inst.sopp_opcode == 32u && immediate >= 1u && immediate <= 3u)))
			{
				return Fail(Reject::UnsupportedControl, "only defined S_NOP or S_INST_PREFETCH scheduling hints are proved inert");
			}
			step.kind = Kind::SchedulingHint;
			return true;
		case Type::VNop:
			// ISA: V_NOP does nothing; it reads and writes no lane, register or memory.
			step.kind = Kind::SchedulingHint;
			return true;
		case Type::SWaitcnt:
			if (!Immediate(inst, &immediate)) { return false; }
			if (inst.sopp_opcode != 12u || (immediate & 0x80u) != 0u)
			{
				return Fail(Reject::UnsupportedControl, "wait requires S_WAITCNT with only defined RDNA2 counter fields");
			}
			// No admitted instruction issues memory. Waiting has no arithmetic
			// effect; export hazards, including the PRIM source, still matter.
			if ((immediate & 0x70u) == 0u) { pending_export = false; pending_export_sources.reset(); }
			step.kind = Kind::Wait;
			return true;
		case Type::SSendmsg:
			if (!Immediate(inst, &immediate)) { return false; }
			if (inst.sopp_opcode != 16u || immediate != 9u)
			{
				return Fail(Reject::UnsupportedControl, "only S_SENDMSG GS_ALLOC_REQ (9) has a modeled effect");
			}
			if (result.allocation_index != ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::AllocationSequence, "exactly one allocation request is supported");
			}
			step.scalar_sources[0] = scalar[kM0]; // Implicit message payload.
			step.scalar_source_mask = 1u;
			result.scalar_dependencies |= scalar[kM0].initial_dependencies;
			if (scalar[kM0].known_mask != 0xffffffffu)
			{
				return Fail(Reject::UnknownAllocation, "all M0 allocation payload bits must be known");
			}
			if (scalar[kM0].value != result.allocation_payload)
			{
				return Fail(Reject::AllocationMismatch, "M0 does not equal the exact planned (primitive_count << 12) | vertex_count");
			}
			// ISA 11.3 / 12.5.1 and STATUS.EXPORT_RDY: this token represents
			// the real request. Exports stall until space is available; it is
			// NOT an omitted instruction or a renderer allocation implementation.
			result.allocation_index = step.instruction_index;
			step.kind = Kind::AllocationRequest;
			return true;
		case Type::Exp: return Export(inst);
		case Type::SEndpgm:
			if (inst.sopp_opcode != 1u || inst.format != ShaderInstructionFormat::Empty || inst.src_num != 0 || !Empty(inst.dst))
			{
				return Fail(Reject::MalformedInstruction, "proof requires the ordinary parsed S_ENDPGM terminator");
			}
			if (step.instruction_index + 1u != result.instruction_count)
			{
				return Fail(Reject::ControlFlow, "instructions follow the terminator; no suffix is silently ignored");
			}
			if (result.primitive_export_index == ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::MissingPrimitive, "program terminates without proved primitive forwarding");
			}
			if (result.position_export_index == ShaderNggPassthroughNoInstruction)
			{
				return Fail(Reject::MissingPosition, "program terminates without a defined full POS0 export");
			}
			pending_export = false; // ISA 12.5: implicit S_WAITCNT 0.
			pending_export_sources.reset();
			ended = true;
			Retain(Kind::End);
			return true;
		case Type::SBranch:
		case Type::SCbranchScc0:
		case Type::SCbranchScc1:
		case Type::SCbranchVccz:
		case Type::SCbranchVccnz:
		case Type::SCbranchExecz:
		case Type::SCbranchExecnz:
		case Type::SSetpcB64:
		case Type::SSwappcB64: return Fail(Reject::ControlFlow, "branches and program transfers are outside the straight-line proof");
		default:
			const int arity = VectorArity(inst.type);
			return arity == 0 ? Scalar(inst) : Vector(inst, arity);
	}
}

ShaderNggPassthroughProof Analyzer::Run(const ShaderCode& code)
{
	step.instruction_index = step.pc = ShaderNggPassthroughNoInstruction;
	result.instruction_count = code.GetInstructions().Size();
	if (result.counts.guest_wave_size != 32 && result.counts.guest_wave_size != 64)
	{
		Fail(Reject::InvalidWaveSize, "guest wave size must be explicitly 32 or 64");
		return std::move(result);
	}
	if (result.counts.es_vertex_count == 0 || result.counts.gs_primitive_count == 0 ||
	    result.counts.es_vertex_count > result.counts.guest_wave_size || result.counts.gs_primitive_count > result.counts.guest_wave_size)
	{
		Fail(Reject::InvalidCounts, "ES vertex and GS primitive counts must each be in 1..guest_wave_size");
		return std::move(result);
	}
	if (code.GetType() != ShaderType::Vertex || code.IsVsEmbedded() || code.IsPsEmbedded())
	{
		Fail(Reject::UnsupportedStage, "proof requires explicit non-embedded vertex IR");
		return std::move(result);
	}
	if (result.instruction_count > ShaderNggPassthroughInstructionLimit)
	{
		Fail(Reject::InstructionLimit, "complete program exceeds the 1024-instruction proof bound");
		return std::move(result);
	}
	if (code.GetContinuationPc() != UINT32_MAX || !code.GetDebugPrintfs().IsEmpty())
	{
		step.pc = code.GetContinuationPc();
		Fail(Reject::ControlFlow, "continuations and debug side effects require a different complete-program proof");
		return std::move(result);
	}
	result.vertex_mask = LowMask(result.counts.es_vertex_count);
	result.primitive_mask = LowMask(result.counts.gs_primitive_count);
	result.allocation_payload = (result.counts.gs_primitive_count << 12u) | result.counts.es_vertex_count;
	scalar[3].value = (result.counts.gs_primitive_count << 8u) | result.counts.es_vertex_count;
	scalar[3].known_mask = 0xffffu;
	vector[0] = {result.primitive_mask, result.primitive_mask, 0, 0};
	vector[5] = {result.vertex_mask, 0, result.vertex_mask, 0};
	vector[8] = {result.vertex_mask, 0, 0, result.vertex_mask};
	result.steps.reserve(result.instruction_count);
	result.retained_instruction_indices.reserve(result.instruction_count);
	for (uint32_t i = 0; i < result.instruction_count; ++i)
	{
		const auto& inst = code.GetInstructions().At(i);
		step = {};
		step.instruction_index = i;
		step.pc = inst.pc;
		step.exec_before = Pair(kExecLo);
		if ((inst.pc & 3u) != 0u || (i != 0 && inst.pc <= code.GetInstructions().At(i - 1).pc) ||
		    !ShaderInstructionLoweringPreconditions(inst))
		{
			Fail(Reject::MalformedInstruction, "shared lowering preconditions or ordered unique instruction PCs failed");
			return std::move(result);
		}
		if (!ModeledInstruction(inst.type))
		{
			Fail(Reject::UnsupportedInstruction, "instruction is outside the closed proof subset; memory/resources/lane operations cannot be removed");
			return std::move(result);
		}
		if (!CommonControls(inst))
		{
			Fail(Reject::MalformedInstruction, "proof requires plain full-word controls and exact empty operand/metadata tails");
			return std::move(result);
		}
		if (!Instruction(inst)) { return std::move(result); }
		step.exec_after = Pair(kExecLo);
		step.scc_after = scalar[kScc];
		result.steps.push_back(step);
		if (prologue_only_ && PrologueComplete(inst))
		{
			FinishPrologue();
			return std::move(result);
		}
	}
	if (prologue_only_)
	{
		Fail(Reject::MissingEnd, "the program ends before its EXEC is the vertex-count mask");
		return std::move(result);
	}
	if (!ended)
	{
		Fail(Reject::MissingEnd, "complete program must include S_ENDPGM; a truncated prefix is not a proof");
		return std::move(result);
	}
	if (!code.GetLabels().IsEmpty() || !code.GetIndirectLabels().IsEmpty())
	{
		Fail(Reject::ControlFlow, "unexpected control-flow labels in straight-line IR");
		return std::move(result);
	}
	result.proven = true;
	result.primitive_forwarding_proved = true;
	result.independent_vertex_transforms_proved = true;
	return std::move(result);
}

} // namespace

ShaderNggPassthroughProof ShaderAnalyzeNggPassthrough(const ShaderCode& code, const ShaderNggPassthroughCounts& counts)
{
	return Analyzer(counts).Run(code);
}

ShaderNggPassthroughProof ShaderAnalyzeNggPassthroughPrologue(const ShaderCode& code, const ShaderNggPassthroughCounts& counts)
{
	return Analyzer(counts, true).Run(code);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
