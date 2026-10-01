#include "Kyty/Core/MagicEnum.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <array>
#include <vector>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

using VectorSet = std::array<bool, 256>;

struct NeutralRegion
{
	uint32_t  begin = 0;
	uint32_t  end   = 0;
	VectorSet written {};
};

bool Plain(const ShaderOperand& operand)
{
	return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp && operand.swizzle == 6u && !operand.dpp &&
	       operand.dpp_ctrl == 0u && operand.dpp_row_mask == 0u && operand.dpp_bank_mask == 0u && !operand.dpp_fetch_inactive &&
	       !operand.dpp_bound_ctrl;
}

bool Unused(const ShaderOperand& operand)
{
	return Plain(operand) && operand.size == 0 && (operand.type == ShaderOperandType::Unknown || operand.type == ShaderOperandType::Null);
}

bool Pair(const ShaderOperand& operand)
{
	const bool scalar =
	    operand.type == ShaderOperandType::Sgpr && operand.register_id >= 0 && operand.register_id <= 102 && (operand.register_id & 1) == 0;
	const bool special =
	    (operand.type == ShaderOperandType::ExecLo || operand.type == ShaderOperandType::VccLo) && operand.register_id == 0;
	return Plain(operand) && operand.size == 2 && (scalar || special);
}

bool SamePair(const ShaderOperand& left, const ShaderOperand& right)
{
	return Pair(left) && Pair(right) && left.type == right.type && left.register_id == right.register_id;
}

bool Overlaps(const ShaderOperand& destination, const ShaderOperand& pair)
{
	if (destination.size <= 0)
	{
		return false;
	}
	if (pair.type == ShaderOperandType::Sgpr)
	{
		return destination.type == pair.type && destination.register_id < pair.register_id + 2 &&
		       pair.register_id < destination.register_id + destination.size;
	}
	return destination.type == pair.type || (pair.type == ShaderOperandType::VccLo && destination.type == ShaderOperandType::VccHi) ||
	       (pair.type == ShaderOperandType::ExecLo && destination.type == ShaderOperandType::ExecHi);
}

bool Writes(const ShaderInstruction& instruction, const ShaderOperand& pair)
{
	return Overlaps(instruction.dst, pair) || Overlaps(instruction.dst2, pair);
}

bool WritesExec(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	return instruction.dst.type == ShaderOperandType::ExecLo || instruction.dst.type == ShaderOperandType::ExecHi ||
	       instruction.dst2.type == ShaderOperandType::ExecLo || instruction.dst2.type == ShaderOperandType::ExecHi ||
	       name.StartsWith("VCmpx") || name.ContainsStr("Saveexec");
}

bool Branch(ShaderInstructionType type)
{
	return type == ShaderInstructionType::SBranch || type == ShaderInstructionType::SCbranchScc0 ||
	       type == ShaderInstructionType::SCbranchScc1 || type == ShaderInstructionType::SCbranchVccz ||
	       type == ShaderInstructionType::SCbranchVccnz || type == ShaderInstructionType::SCbranchExecz ||
	       type == ShaderInstructionType::SCbranchExecnz;
}

bool Transfer(const ShaderInstruction& instruction)
{
	const auto name = Core::EnumName8(instruction.type);
	return name.ContainsStr("branch") || name.ContainsStr("Branch") || instruction.type == ShaderInstructionType::SEndpgm ||
	       instruction.type == ShaderInstructionType::SSetpcB64 || instruction.type == ShaderInstructionType::SSwappcB64;
}

bool LabelAt(const ShaderCode& code, uint32_t pc)
{
	for (const auto* labels: {&code.GetLabels(), &code.GetIndirectLabels()})
	{
		for (const auto& label: *labels)
		{
			if (!label.IsDisabled() && label.GetDst() == pc)
			{
				return true;
			}
		}
	}
	return false;
}

bool ScalarMove(const ShaderInstruction& instruction, const ShaderOperand& destination, const ShaderOperand& source)
{
	return instruction.type == ShaderInstructionType::SMovB64 && instruction.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	       instruction.src_num == 1 && SamePair(instruction.dst, destination) && SamePair(instruction.src[0], source) &&
	       Unused(instruction.dst2) && Unused(instruction.src[1]) && Unused(instruction.src[2]) && Unused(instruction.src[3]) &&
	       !instruction.vop_sdwa && instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u;
}

bool ScalarMetadata(const ShaderInstruction& instruction)
{
	return !instruction.vop_sdwa && instruction.vop_sdwa_ctrl == 0u && instruction.vop3_op_sel == 0u && instruction.vop3_omod == 0u &&
	       instruction.ds_offset == 0u && instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u;
}

bool VectorMetadata(const ShaderInstruction& instruction)
{
	return !instruction.vop_sdwa && instruction.vop_sdwa_ctrl == 0u && instruction.vop3_omod == 0u && instruction.ds_offset == 0u &&
	       instruction.ds_encoding_control == 0u && instruction.ds_encoding_registers == 0u;
}

bool VectorRegister(const ShaderOperand& operand)
{
	return operand.type == ShaderOperandType::Vgpr && operand.size == 1 && operand.register_id >= 0 && operand.register_id < 256;
}

bool VectorDestination(const ShaderInstruction& instruction)
{
	return VectorRegister(instruction.dst) && Plain(instruction.dst) && Unused(instruction.dst2);
}

bool SourceCount(const ShaderInstruction& instruction)
{
	return instruction.src_num >= 0 && instruction.src_num <= 4;
}

bool BypassesDefinition(const ShaderCode& code, uint32_t first_pc, uint32_t last_pc)
{
	for (const auto& instruction: code.GetInstructions())
	{
		if (!Transfer(instruction) || instruction.type == ShaderInstructionType::SEndpgm)
		{
			continue;
		}
		if (!Branch(instruction.type) || instruction.format != ShaderInstructionFormat::Label || instruction.src_num != 1 ||
		    instruction.src[0].type != ShaderOperandType::LiteralConstant || !Plain(instruction.src[0]))
		{
			return true;
		}
		const int64_t target = static_cast<int64_t>(instruction.pc) + 4 + instruction.src[0].constant.i;
		if (target > first_pc && target <= last_pc && (instruction.pc < first_pc || instruction.pc > last_pc))
		{
			return true;
		}
	}
	for (const auto* labels: {&code.GetLabels(), &code.GetIndirectLabels()})
	{
		for (const auto& label: *labels)
		{
			if (!label.IsDisabled() && label.GetDst() > first_pc && label.GetDst() <= last_pc &&
			    (label.GetSrc() < first_pc || label.GetSrc() > last_pc))
			{
				return true;
			}
		}
	}
	return false;
}

bool SavedMaskIsAllocated(const ShaderCode& code, uint32_t before, const ShaderOperand& mask, const ShaderOperand& exec)
{
	const auto& instructions = code.GetInstructions();
	for (uint32_t cursor = before; cursor > 0;)
	{
		const auto& instruction = instructions.At(--cursor);
		if (ScalarMove(instruction, mask, exec))
		{
			return !BypassesDefinition(code, instruction.pc, instructions.At(before).pc);
		}
		if (Writes(instruction, mask))
		{
			return false;
		}
	}
	return false;
}

bool SavedMaskEqualsExec(const ShaderCode& code, uint32_t begin, const ShaderOperand& mask, uint32_t* anchor)
{
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	for (uint32_t cursor = begin; cursor > 0;)
	{
		const auto& instruction = code.GetInstructions().At(--cursor);
		const bool  copied      = ScalarMove(instruction, mask, exec);
		const bool  restored    = ScalarMove(instruction, exec, mask);
		if ((copied || (restored && SavedMaskIsAllocated(code, cursor, mask, exec))) &&
		    !BypassesDefinition(code, instruction.pc, code.GetInstructions().At(begin).pc))
		{
			*anchor = cursor;
			return true;
		}
		if (WritesExec(instruction) || Writes(instruction, mask) || Transfer(instruction) || LabelAt(code, instruction.pc))
		{
			return false;
		}
	}
	return false;
}

bool MaskSubset(const ShaderCode& code, uint32_t begin, uint32_t anchor, const ShaderOperand& condition, const ShaderOperand& mask)
{
	if (SamePair(condition, mask))
	{
		return true;
	}
	if (!Pair(condition) || condition.type != ShaderOperandType::Sgpr)
	{
		return false;
	}
	for (uint32_t cursor = begin; cursor > anchor + 1u;)
	{
		const auto& instruction = code.GetInstructions().At(--cursor);
		if (ScalarMove(instruction, condition, mask))
		{
			return true;
		}
		if (Writes(instruction, condition))
		{
			return instruction.type == ShaderInstructionType::SAndB64 && instruction.format == ShaderInstructionFormat::Sdst2Ssrc02Ssrc12 &&
			       instruction.src_num == 2 && SamePair(instruction.dst, condition) && Unused(instruction.dst2) &&
			       Pair(instruction.src[0]) && Pair(instruction.src[1]) &&
			       (SamePair(instruction.src[0], mask) || SamePair(instruction.src[1], mask)) && Unused(instruction.src[2]) &&
			       Unused(instruction.src[3]) && ScalarMetadata(instruction);
		}
		if (Transfer(instruction) || LabelAt(code, instruction.pc))
		{
			return false;
		}
	}
	return false;
}

bool ZeroConstant(const ShaderOperand& operand)
{
	return Plain(operand) && operand.size == 0 && operand.constant.u == 0u &&
	       (operand.type == ShaderOperandType::LiteralConstant || operand.type == ShaderOperandType::IntegerInlineConstant);
}

bool Contribution(const ShaderOperand& operand)
{
	return !operand.dpp && (operand.type != ShaderOperandType::Vgpr || VectorRegister(operand));
}

bool InitializerShape(const ShaderInstruction& instruction)
{
	return instruction.type == ShaderInstructionType::VCndmaskB32 && instruction.format == ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2 &&
	       instruction.src_num == 3 && ShaderClassifyComputeWaveInstruction(instruction) == ShaderComputeWaveInstructionKind::BankedAlu &&
	       VectorDestination(instruction) && VectorMetadata(instruction) && instruction.vop3_op_sel == 0u && Unused(instruction.src[3]);
}

// Lanes outside the condition receive the zero constant. The condition is
// a subset of the saved mask, itself a subset of the captured allocation.
bool Initializer(const ShaderCode& code, uint32_t begin, uint32_t anchor, const ShaderOperand& mask, const ShaderOperand& saved)
{
	const auto& instruction = code.GetInstructions().At(begin + 1u);
	return InitializerShape(instruction) && ZeroConstant(instruction.src[0]) && Contribution(instruction.src[1]) &&
	       !Overlaps(saved, instruction.src[2]) && MaskSubset(code, begin, anchor, instruction.src[2], mask);
}

bool DefinedSource(const ShaderOperand& operand, const VectorSet& defined)
{
	return operand.type != ShaderOperandType::Vgpr || (VectorRegister(operand) && defined[operand.register_id]);
}

bool BitwiseVector(const ShaderInstruction& instruction)
{
	const bool bitwise  = instruction.type == ShaderInstructionType::VMovB32 || instruction.type == ShaderInstructionType::VAndB32 ||
	                      instruction.type == ShaderInstructionType::VOrB32 || instruction.type == ShaderInstructionType::VXorB32;
	const auto kind     = ShaderClassifyComputeWaveInstruction(instruction);
	const bool admitted = kind == ShaderComputeWaveInstructionKind::BankedDpp || kind == ShaderComputeWaveInstructionKind::BankedAlu ||
	                      kind == ShaderComputeWaveInstructionKind::BankedVector || kind == ShaderComputeWaveInstructionKind::BankedGeneric;
	return bitwise && admitted && VectorMetadata(instruction) && instruction.vop3_op_sel == 0u;
}

bool DefinedSources(const ShaderInstruction& instruction, const VectorSet& defined)
{
	for (int source = 0; source < instruction.src_num; ++source)
	{
		const bool routed = source != 0 && instruction.src[source].dpp;
		if (routed || !DefinedSource(instruction.src[source], defined))
		{
			return false;
		}
	}
	return true;
}

// A pure instruction writes one VGPR and reads only fully defined registers.
// Without a full overwrite, a DPP destination keeps its previous value, so it
// must already be defined in every lane.
bool PureVector(const ShaderInstruction& instruction, const VectorSet& defined)
{
	if (!SourceCount(instruction) || !VectorDestination(instruction))
	{
		return false;
	}
	if (!ShaderComputeWavePermutationSupported(instruction) && !BitwiseVector(instruction))
	{
		return false;
	}
	return DefinedSources(instruction, defined) && (!instruction.src[0].dpp || defined[instruction.dst.register_id]);
}

bool Restores(const ShaderInstruction& instruction, const ShaderOperand& saved)
{
	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;
	return ScalarMove(instruction, exec, saved);
}

bool ClosedRegion(const ShaderCode& code, uint32_t begin, const ShaderOperand& saved, NeutralRegion* region)
{
	region->begin                                                          = begin;
	region->written                                                        = {};
	region->written[code.GetInstructions().At(begin + 1u).dst.register_id] = true;
	for (uint32_t cursor = begin + 2u; cursor < code.GetInstructions().Size(); ++cursor)
	{
		const auto& instruction = code.GetInstructions().At(cursor);
		if (LabelAt(code, instruction.pc))
		{
			return false;
		}
		if (Restores(instruction, saved))
		{
			region->end = cursor;
			return true;
		}
		if (!PureVector(instruction, region->written))
		{
			return false;
		}
		region->written[instruction.dst.register_id] = true;
	}
	return false;
}

bool RegionEntry(const ShaderCode& code, uint32_t index)
{
	const auto& instruction = code.GetInstructions().At(index);
	const auto& mask        = instruction.src[0];
	const auto& saved       = instruction.dst;
	return instruction.type == ShaderInstructionType::SOrn2SaveexecB64 && instruction.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	       instruction.src_num == 1 && Pair(mask) && mask.type == ShaderOperandType::Sgpr && Pair(saved) &&
	       saved.type != ShaderOperandType::ExecLo && !Overlaps(saved, mask) && Unused(instruction.dst2) && Unused(instruction.src[1]) &&
	       Unused(instruction.src[2]) && Unused(instruction.src[3]) && ScalarMetadata(instruction) && !LabelAt(code, instruction.pc) &&
	       !LabelAt(code, code.GetInstructions().At(index + 1u).pc);
}

bool AnalyzeRegion(const ShaderCode& code, uint32_t index, NeutralRegion* region)
{
	if (code.GetType() != ShaderType::Pixel || index >= code.GetInstructions().Size() || code.GetInstructions().Size() - index < 2u ||
	    !RegionEntry(code, index))
	{
		return false;
	}
	const auto& instruction = code.GetInstructions().At(index);
	uint32_t    anchor      = 0;
	return SavedMaskEqualsExec(code, index, instruction.src[0], &anchor) &&
	       Initializer(code, index, anchor, instruction.src[0], instruction.dst) && ClosedRegion(code, index, instruction.dst, region);
}

std::vector<NeutralRegion> CollectRegions(const ShaderCode& code)
{
	std::vector<NeutralRegion> regions;
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		NeutralRegion region;
		if (AnalyzeRegion(code, index, &region))
		{
			regions.push_back(region);
		}
	}
	return regions;
}

bool InsideRegion(const std::vector<NeutralRegion>& regions, uint32_t index)
{
	for (const auto& region: regions)
	{
		if (index >= region.begin && index <= region.end)
		{
			return true;
		}
	}
	return false;
}

// Reads that ignore EXEC (v_readlane) or fetch inactive lanes (DPP FI,
// PERMLANE FI, ISA 12.12 and 13.3.9) can observe lanes the capture never held.
// DPP and PERMLANE with FI=0 treat inactive lanes as unreadable, so uncaptured
// lanes (which never become active outside a proven region) behave as on a
// hardware wave whose absent lanes have EXEC=0. Quad-local DPP controls stay
// inside a captured quad, whose four lanes are always transported together.
bool ObservesInactiveLanes(const ShaderInstruction& instruction)
{
	const bool permute =
	    instruction.type == ShaderInstructionType::VPermlane16B32 || instruction.type == ShaderInstructionType::VPermlanex16B32;
	if (instruction.src_num <= 0)
	{
		return false;
	}
	const auto& source    = instruction.src[0];
	const bool  row_fetch = source.dpp && source.dpp_fetch_inactive && source.dpp_ctrl > 0xffu;
	return instruction.type == ShaderInstructionType::VReadlaneB32 || row_fetch || (permute && (instruction.vop3_op_sel & 1u) != 0u);
}

bool RegionDefines(const std::vector<NeutralRegion>& regions, uint32_t cursor, int reg)
{
	for (const auto& region: regions)
	{
		if (region.end == cursor && region.written[reg])
		{
			return true;
		}
	}
	return false;
}

// A VGPR holds a guest-defined value in every lane after a proven region wrote
// it with full EXEC. Narrower later writes keep the other lanes, as on hardware.
// The region must be the unconditional predecessor of the read: any label
// between them would allow entry that skipped the initialization.
bool DefinedBeforeRead(const ShaderCode& code, const std::vector<NeutralRegion>& regions, uint32_t index, int reg)
{
	for (uint32_t cursor = index; cursor > 0;)
	{
		if (LabelAt(code, code.GetInstructions().At(cursor).pc))
		{
			return false;
		}
		if (RegionDefines(regions, --cursor, reg))
		{
			return true;
		}
	}
	return false;
}

} // namespace

bool ShaderFragmentNeutralRegionSupported(const ShaderCode& code, uint32_t index)
{
	NeutralRegion region;
	return AnalyzeRegion(code, index, &region);
}

ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentPartialWaveReads(const ShaderCode& code)
{
	ShaderComputeWaveAnalysisResult result;
	result.supported = true;
	if (code.GetType() != ShaderType::Pixel)
	{
		return result;
	}
	const auto regions = CollectRegions(code);
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& instruction = code.GetInstructions().At(index);
		if (!ObservesInactiveLanes(instruction) || InsideRegion(regions, index))
		{
			continue;
		}
		const auto& source = instruction.src[0];
		if (!VectorRegister(source) || !DefinedBeforeRead(code, regions, index, source.register_id))
		{
			result.supported      = false;
			result.unsupported_pc = instruction.pc;
			result.reason         = "reads lanes outside the captured wave without a proven full-wave initialization";
			return result;
		}
	}
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
