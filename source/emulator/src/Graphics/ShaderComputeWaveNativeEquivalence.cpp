#include "Emulator/Graphics/ShaderComputeWaveNativeEquivalence.h"

#include "Kyty/Core/MagicEnum.h"

#include <algorithm>
#include <bitset>
#include <string_view>
#include <vector>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

// Scalar state units: s0..s105, then vcc_lo, vcc_hi, m0 and scc. EXEC is
// deliberately absent: the native path keeps it per lane, so a stale EXEC in a
// skipped host subgroup is exactly the all-inactive state the guest had there.
constexpr int kSgprUnits = 106;
constexpr int kVccLo     = 106;
constexpr int kVccHi     = 107;
constexpr int kM0        = 108;
constexpr int kScc       = 109;
constexpr int kUnits     = 110;

using UnitSet = std::bitset<kUnits>;

// Per-unit value class at a program point.
constexpr uint8_t kMask = 1u;
constexpr uint8_t kData = 2u;

struct UnitState
{
	uint8_t units[kUnits] {};
	bool    reached = false;
};

ShaderComputeWaveAnalysisResult Failure(uint32_t pc, const char* reason)
{
	ShaderComputeWaveAnalysisResult result {};
	result.unsupported_pc = pc;
	result.reason         = reason;
	return result;
}

bool NameStartsWith(std::string_view name, std::string_view prefix)
{
	return name.substr(0, prefix.size()) == prefix;
}

bool IsVectorType(ShaderInstructionType type)
{
	const auto name = magic_enum::enum_name(type);
	return !name.empty() && name.front() == 'V';
}

bool IsLaneCrossingType(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::VReadfirstlaneB32:
		case ShaderInstructionType::VReadlaneB32:
		case ShaderInstructionType::VWritelaneB32:
		case ShaderInstructionType::VMbcntHiU32B32:
		case ShaderInstructionType::VMbcntLoU32B32:
		case ShaderInstructionType::SWqmB64:
		case ShaderInstructionType::DsAppend:
		case ShaderInstructionType::DsConsume:
		case ShaderInstructionType::SSetpcB64:
		case ShaderInstructionType::SSwappcB64:
		case ShaderInstructionType::SSendmsg: return true;
		default: return false;
	}
}

bool IsMaskLogicType(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SAndB64:
		case ShaderInstructionType::SOrB64:
		case ShaderInstructionType::SXorB64:
		case ShaderInstructionType::SAndn2B64:
		case ShaderInstructionType::SOrn2B64:
		case ShaderInstructionType::SNandB64:
		case ShaderInstructionType::SNorB64:
		case ShaderInstructionType::SXnorB64:
		case ShaderInstructionType::SNotB64:
		case ShaderInstructionType::SMovB64:
		// Selects between two 64-bit values by a uniform SCC.
		case ShaderInstructionType::SCselectB64: return true;
		default: return false;
	}
}

bool IsSaveexecType(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SAndSaveexecB64:
		case ShaderInstructionType::SAndn1SaveexecB64:
		case ShaderInstructionType::SAndn2SaveexecB64:
		case ShaderInstructionType::SNandSaveexecB64:
		case ShaderInstructionType::SNorSaveexecB64:
		case ShaderInstructionType::SOrSaveexecB64:
		case ShaderInstructionType::SOrn2SaveexecB64:
		case ShaderInstructionType::SXnorSaveexecB64:
		case ShaderInstructionType::SXorSaveexecB64: return true;
		default: return false;
	}
}

bool IsSccReaderType(ShaderInstructionType type)
{
	switch (type)
	{
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCselectB32:
		case ShaderInstructionType::SCselectB64:
		case ShaderInstructionType::SCmovB32:
		case ShaderInstructionType::SCmovB64:
		case ShaderInstructionType::SAddcU32: return true;
		default: return false;
	}
}

bool IsLaneConditionalBranch(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SCbranchExecz || type == ShaderInstructionType::SCbranchExecnz ||
	       type == ShaderInstructionType::SCbranchVccz || type == ShaderInstructionType::SCbranchVccnz;
}

bool IsBranchType(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SBranch || type == ShaderInstructionType::SCbranchScc0 ||
	       type == ShaderInstructionType::SCbranchScc1 || IsLaneConditionalBranch(type);
}

// Vector instructions whose src[2] is a per-lane carry or select mask.
bool IsVectorMaskConsumer(ShaderInstructionType type)
{
	return type == ShaderInstructionType::VCndmaskB32 || type == ShaderInstructionType::VAddCoCiU32 ||
	       type == ShaderInstructionType::VSubrevCoCiU32;
}

// Scalar memory side effects are not gated by EXEC.
bool HasScalarSideEffect(const ShaderInstruction& instruction)
{
	const auto name = magic_enum::enum_name(instruction.type);
	return NameStartsWith(name, "SStore") || NameStartsWith(name, "SBufferStore") || NameStartsWith(name, "SAtomic") ||
	       NameStartsWith(name, "SBufferAtomic") || NameStartsWith(name, "SDcache");
}

enum class OperandUnits
{
	None,
	Units,
	Exec,
	Invalid,
};

// Maps one operand to the scalar units it names. EXEC is reported separately.
OperandUnits CollectUnits(const ShaderOperand& operand, UnitSet* units)
{
	switch (operand.type)
	{
		case ShaderOperandType::Unknown:
		case ShaderOperandType::Null:
		case ShaderOperandType::LiteralConstant:
		case ShaderOperandType::IntegerInlineConstant:
		case ShaderOperandType::FloatInlineConstant:
		case ShaderOperandType::Vgpr: return OperandUnits::None;
		case ShaderOperandType::Sgpr:
		{
			const int count = operand.size > 0 ? operand.size : 1;
			if (operand.register_id < 0 || count > kSgprUnits || operand.register_id > kSgprUnits - count)
			{
				return OperandUnits::Invalid;
			}
			for (int unit = 0; unit < count; ++unit)
			{
				units->set(static_cast<size_t>(operand.register_id + unit));
			}
			return OperandUnits::Units;
		}
		case ShaderOperandType::VccLo:
			units->set(kVccLo);
			if (operand.size >= 2)
			{
				units->set(kVccHi);
			}
			return OperandUnits::Units;
		case ShaderOperandType::VccHi: units->set(kVccHi); return OperandUnits::Units;
		case ShaderOperandType::M0: units->set(kM0); return OperandUnits::Units;
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi: return OperandUnits::Exec;
		case ShaderOperandType::Scc: units->set(kScc); return OperandUnits::Units;
		default: return OperandUnits::Invalid;
	}
}

bool OperandIsMaskConstant(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::IntegerInlineConstant && (operand.constant.i == 0 || operand.constant.i == -1);
}

bool OperandIsConstant(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::LiteralConstant || operand.type == ShaderOperandType::IntegerInlineConstant ||
	       operand.type == ShaderOperandType::FloatInlineConstant;
}

struct DecodedInstruction
{
	UnitSet  uses;
	UnitSet  defs;
	UnitSet  mask_uses; // units that must hold masks here
	UnitSet  data_uses; // units that must hold uniform data here
	UnitSet  mask_defs;
	UnitSet  data_defs;
	UnitSet  copy_defs; // SMovB64/mask logic over data: inherits source class
	bool     writes_scc_from_mask = false;
	bool     writes_scc_data      = false;
	bool     vector               = false;
	uint32_t target               = 0;
	bool     has_target           = false;
};

ShaderComputeWaveAnalysisResult Decode(const ShaderInstruction& instruction, DecodedInstruction* out)
{
	out->vector = IsVectorType(instruction.type);
	const auto name = magic_enum::enum_name(instruction.type);
	// V_CMPX writes only EXEC; the decoder's VCC destination is a placeholder.
	const bool exec_compare = out->vector && NameStartsWith(name, "VCmpx");
	out->writes_scc_data    = NameStartsWith(name, "SCmp") || instruction.type == ShaderInstructionType::SAbsI32;
	if (IsLaneCrossingType(instruction.type))
	{
		return Failure(instruction.pc, "instruction reads another lane or a wave-wide count");
	}
	if (instruction.type >= ShaderInstructionType::SAndSaveexecB32 && instruction.type <= ShaderInstructionType::SOrn1SaveexecB32)
	{
		return Failure(instruction.pc, "32-bit SAVEEXEC changes only the lower half of a wave64 mask");
	}
	if (instruction.src_num < 0 || instruction.src_num > 4 || instruction.mimg_address_num < 0 || instruction.mimg_address_num > 13)
	{
		return Failure(instruction.pc, "instruction operand count is outside the analyzed range");
	}

	const bool mask_logic = IsMaskLogicType(instruction.type);
	const bool saveexec   = IsSaveexecType(instruction.type);
	bool       any_mask   = false;
	bool       any_data   = false;
	UnitSet    logic_sources;

	auto use_operand = [&](const ShaderOperand& operand, int source) -> bool
	{
		if (operand.dpp || operand.type == ShaderOperandType::VccZ || operand.type == ShaderOperandType::ExecZ)
		{
			return false;
		}
		UnitSet    units;
		const auto kind = CollectUnits(operand, &units);
		if (kind == OperandUnits::Invalid)
		{
			return false;
		}
		out->uses |= units;
		const bool mask_source = (mask_logic || saveexec) || (out->vector && source == 2 && IsVectorMaskConsumer(instruction.type));
		if (kind == OperandUnits::Exec)
		{
			// EXEC is only a mask, so only a per-lane mask consumer may read it.
			if (!mask_source)
			{
				return false;
			}
			any_mask = true;
			return true;
		}
		if (mask_logic)
		{
			if (OperandIsConstant(operand))
			{
				any_data = any_data || !OperandIsMaskConstant(operand);
			}
			logic_sources |= units;
			return true;
		}
		if (mask_source)
		{
			if (OperandIsConstant(operand) && !OperandIsMaskConstant(operand))
			{
				return false;
			}
			out->mask_uses |= units;
			return true;
		}
		out->data_uses |= units;
		return true;
	};

	for (int source = 0; source < instruction.src_num; ++source)
	{
		if (!use_operand(instruction.src[source], source))
		{
			return Failure(instruction.pc, "operand is not representable per lane");
		}
	}
	for (int address = 0; address < instruction.mimg_address_num; ++address)
	{
		if (!use_operand(instruction.mimg_address[address], -1))
		{
			return Failure(instruction.pc, "image address operand is not representable per lane");
		}
	}
	if (IsSccReaderType(instruction.type))
	{
		out->uses.set(kScc);
		out->data_uses.set(kScc);
	}
	if (instruction.type == ShaderInstructionType::SCbranchVccz || instruction.type == ShaderInstructionType::SCbranchVccnz)
	{
		out->uses.set(kVccLo);
		out->uses.set(kVccHi);
		out->mask_uses.set(kVccLo);
		out->mask_uses.set(kVccHi);
	}

	for (const auto* destination: {&instruction.dst, &instruction.dst2})
	{
		if (exec_compare && destination == &instruction.dst)
		{
			continue;
		}
		if (destination->dpp || destination->type == ShaderOperandType::VccZ || destination->type == ShaderOperandType::ExecZ ||
		    destination->type == ShaderOperandType::Scc)
		{
			return Failure(instruction.pc, "destination is not representable per lane");
		}
		UnitSet    units;
		const auto kind = CollectUnits(*destination, &units);
		if (kind == OperandUnits::Invalid)
		{
			return Failure(instruction.pc, "destination is not representable per lane");
		}
		if (kind == OperandUnits::Exec)
		{
			// Only per-lane mask writers may target EXEC.
			if (!(mask_logic || saveexec || out->vector))
			{
				return Failure(instruction.pc, "EXEC written by a non-mask scalar instruction");
			}
			continue;
		}
		out->defs |= units;
		if (out->vector || saveexec)
		{
			// Vector results in scalar registers are compare/carry masks.
			out->mask_defs |= units;
		} else if (mask_logic)
		{
			out->copy_defs |= units;
		} else
		{
			out->data_defs |= units;
		}
	}

	if (mask_logic)
	{
		out->mask_uses |= logic_sources; // provisional; resolved by the lattice
		out->writes_scc_from_mask =
		    instruction.type != ShaderInstructionType::SMovB64 && instruction.type != ShaderInstructionType::SCselectB64;
		// Keep the class hint: a constant outside {0, -1} forces data.
		if (any_data)
		{
			out->copy_defs.reset();
			out->data_defs |= out->defs;
			out->data_uses |= logic_sources;
			out->mask_uses &= ~logic_sources;
		}
		if (any_mask)
		{
			// An EXEC source makes the whole operation a mask operation.
			if (any_data)
			{
				return Failure(instruction.pc, "mask operation mixes EXEC with a non-mask constant");
			}
			out->copy_defs.reset();
			out->mask_defs |= out->defs;
		}
	}
	if (IsBranchType(instruction.type))
	{
		if (instruction.format != ShaderInstructionFormat::Label || instruction.src_num != 1)
		{
			return Failure(instruction.pc, "branch is not a decoded PC-relative label");
		}
		const int64_t value = static_cast<int64_t>(instruction.pc) + 4 + instruction.src[0].constant.i;
		if (value < 0 || value > static_cast<int64_t>(UINT32_MAX))
		{
			return Failure(instruction.pc, "branch target is outside the program");
		}
		out->target     = static_cast<uint32_t>(value);
		out->has_target = true;
		out->uses.reset();
		out->data_uses.reset();
		out->mask_uses.reset();
		if (IsSccReaderType(instruction.type))
		{
			out->uses.set(kScc);
			out->data_uses.set(kScc);
		}
		if (instruction.type == ShaderInstructionType::SCbranchVccz || instruction.type == ShaderInstructionType::SCbranchVccnz)
		{
			out->uses.set(kVccLo);
			out->uses.set(kVccHi);
			out->mask_uses.set(kVccLo);
			out->mask_uses.set(kVccHi);
		}
	}
	return {true, 0, {}};
}

} // namespace

ShaderComputeWaveAnalysisResult ShaderAnalyzeComputeWaveNativeEquivalence(const ShaderCode& code)
{
	const auto& instructions = code.GetInstructions();
	const auto  count        = instructions.Size();
	if (code.GetType() != ShaderType::Compute || count == 0)
	{
		return Failure(count != 0 ? instructions.At(0).pc : 0u, "native-equivalence analysis requires non-empty compute code");
	}
	// Decoded PCs normally follow byte order. Keep the linear path for manually
	// constructed or reordered IR, and preserve the first match for duplicate PCs.
	const auto* first = instructions.GetDataConst();
	const auto* last  = first + count;
	const bool ordered = std::is_sorted(first, last, [](const auto& a, const auto& b) { return a.pc < b.pc; });
	auto find = [&](uint32_t pc) -> int64_t
	{
		if (ordered)
		{
			const auto* match = std::lower_bound(first, last, pc, [](const auto& instruction, uint32_t value)
			                                    { return instruction.pc < value; });
			return match != last && match->pc == pc ? static_cast<int64_t>(match - first) : -1;
		}
		for (uint32_t index = 0; index < count; ++index)
		{
			if (instructions.At(index).pc == pc) { return index; }
		}
		return -1;
	};

	// The decoder records each conditional branch's fallthrough edge as an
	// indirect label; the CFG below already contains those edges. Anything else
	// would be control flow this analysis does not model.
	for (const auto& label: code.GetIndirectLabels())
	{
		if (label.IsDisabled())
		{
			continue;
		}
		const auto index = find(label.GetSrc());
		const bool fallthrough = index >= 0 && IsBranchType(instructions.At(static_cast<uint32_t>(index)).type) &&
		                         instructions.At(static_cast<uint32_t>(index)).type != ShaderInstructionType::SBranch &&
		                         label.GetDst() == instructions.At(static_cast<uint32_t>(index)).pc + 4;
		if (!fallthrough)
		{
			return Failure(label.GetSrc(), "indirect-label metadata is not a conditional-branch fallthrough");
		}
	}

	std::vector<DecodedInstruction>    decoded(count);
	std::vector<std::vector<uint32_t>> successors(count);
	for (uint32_t index = 0; index < count; ++index)
	{
		const auto& instruction = instructions.At(index);
		const auto  result      = Decode(instruction, &decoded[index]);
		if (!result.supported)
		{
			return result;
		}
	}

	std::vector<int64_t> target_index(count, -1);
	for (uint32_t index = 0; index < count; ++index)
	{
		const auto& instruction = instructions.At(index);
		if (instruction.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (decoded[index].has_target)
		{
			target_index[index] = find(decoded[index].target);
			if (target_index[index] < 0)
			{
				return Failure(instruction.pc, "branch target is not an instruction boundary");
			}
			successors[index].push_back(static_cast<uint32_t>(target_index[index]));
			if (instruction.type == ShaderInstructionType::SBranch)
			{
				continue;
			}
		}
		if (index + 1 >= count)
		{
			return Failure(instruction.pc, "control reaches the end of the program without SEndpgm");
		}
		successors[index].push_back(index + 1);
	}

	// Forward mask/data classification. Entry SGPRs, VCC and M0 are data.
	std::vector<UnitState> in(count);
	for (int unit = 0; unit < kUnits; ++unit)
	{
		in[0].units[unit] = kData;
	}
	in[0].reached = true;
	bool changed  = true;
	while (changed)
	{
		changed = false;
		for (uint32_t index = 0; index < count; ++index)
		{
			if (!in[index].reached)
			{
				continue;
			}
			const auto& d     = decoded[index];
			UnitState   state = in[index];
			uint8_t     copy  = 0;
			for (int unit = 0; unit < kUnits; ++unit)
			{
				if (d.mask_uses.test(unit) && !d.copy_defs.none())
				{
					copy |= state.units[unit];
				}
			}
			for (int unit = 0; unit < kUnits; ++unit)
			{
				if (d.mask_defs.test(unit))
				{
					state.units[unit] = kMask;
				} else if (d.data_defs.test(unit))
				{
					state.units[unit] = kData;
				} else if (d.copy_defs.test(unit))
				{
					state.units[unit] = copy != 0 ? copy : kMask;
				}
			}
			if (!d.vector && !d.defs.none() && !IsBranchType(instructions.At(index).type))
			{
				// Scalar ALU results conservatively clobber SCC; mask logic makes it mask-derived.
				state.units[kScc] = d.writes_scc_from_mask ? static_cast<uint8_t>(kMask) : state.units[kScc];
			}
			if (d.writes_scc_data)
			{
				state.units[kScc] = kData;
			}
			for (auto successor: successors[index])
			{
				auto& next = in[successor];
				if (!next.reached)
				{
					next    = state;
					changed = true;
					continue;
				}
				for (int unit = 0; unit < kUnits; ++unit)
				{
					const uint8_t merged = next.units[unit] | state.units[unit];
					if (merged != next.units[unit])
					{
						next.units[unit] = merged;
						changed          = true;
					}
				}
			}
		}
	}
	for (uint32_t index = 0; index < count; ++index)
	{
		if (!in[index].reached)
		{
			continue;
		}
		const auto& d  = decoded[index];
		const auto& st = in[index];
		uint8_t     logic_class = 0;
		for (int unit = 0; unit < kUnits; ++unit)
		{
			if (d.data_uses.test(unit) && st.units[unit] != kData)
			{
				return Failure(instructions.At(index).pc, "a scalar register that may hold a lane mask is read as uniform data");
			}
			if (d.mask_uses.test(unit))
			{
				if (!d.copy_defs.none())
				{
					logic_class |= st.units[unit];
				} else if (st.units[unit] != kMask)
				{
					return Failure(instructions.At(index).pc, "a per-lane mask consumer reads a register that may hold uniform data");
				}
			}
		}
		if (logic_class == (kMask | kData))
		{
			return Failure(instructions.At(index).pc, "a 64-bit scalar operation mixes lane masks with uniform data");
		}
	}

	// Backward scalar liveness.
	std::vector<UnitSet> live_in(count);
	changed = true;
	while (changed)
	{
		changed = false;
		for (uint32_t index = count; index-- > 0;)
		{
			UnitSet out;
			for (auto successor: successors[index])
			{
				out |= live_in[successor];
			}
			UnitSet kills = decoded[index].defs;
			if (decoded[index].writes_scc_data)
			{
				kills.set(kScc);
			}
			const auto updated = decoded[index].uses | (out & ~kills);
			if (updated != live_in[index])
			{
				live_in[index] = updated;
				changed        = true;
			}
		}
	}

	for (uint32_t index = 0; index < count; ++index)
	{
		const auto& instruction = instructions.At(index);
		if (!IsLaneConditionalBranch(instruction.type))
		{
			continue;
		}
		const auto target = target_index[index];
		if (target <= static_cast<int64_t>(index))
		{
			return Failure(instruction.pc, "lane-conditional branch is not a forward region");
		}
		UnitSet written;
		for (auto inner = static_cast<int64_t>(index) + 1; inner < target; ++inner)
		{
			const auto& region_instruction = instructions.At(static_cast<uint32_t>(inner));
			if (HasScalarSideEffect(region_instruction))
			{
				return Failure(region_instruction.pc, "scalar memory side effect inside a lane-conditional region");
			}
			const auto& region_decoded = decoded[static_cast<size_t>(inner)];
			written |= region_decoded.defs;
			// Any scalar ALU result may also write SCC.
			if ((!region_decoded.vector && !region_decoded.defs.none()) || region_decoded.writes_scc_data)
			{
				written.set(kScc);
			}
		}
		if (written.test(kM0))
		{
			return Failure(instruction.pc, "M0 is written inside a lane-conditional region");
		}
		if ((written & live_in[static_cast<size_t>(target)]).any())
		{
			return Failure(instruction.pc, "a scalar value written inside a lane-conditional region is live at its join");
		}
	}
	return {true, 0, {}};
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
