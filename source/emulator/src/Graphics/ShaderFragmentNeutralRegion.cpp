#include "Kyty/Core/MagicEnum.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"

#include <algorithm>
#include <array>
#include <vector>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

using VectorSet = std::array<bool, 256>;

struct NeutralRegion
{
	uint32_t  begin       = 0;
	uint32_t  end         = 0;
	VectorSet written     {};
	VectorSet zero        {};
	bool      zero_closed = false;
	bool      tier_safe   = true;
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

// Row shift/rotate/mirror/broadcast controls stay inside the 16-lane row.
// Shifts below 0x121 can leave the row; everything else the banked emitter
// supports is always in bounds. 0x100 is reserved and the step-0 encodings at
// 0x110 and 0x120 sit outside the ISA ranges, so they stay rejected too.
bool RowCtrlSupported(uint32_t control)
{
	return (control >= 0x101u && control <= 0x10fu) || (control >= 0x111u && control <= 0x11fu) ||
	       (control >= 0x121u && control <= 0x12fu) || control == 0x140u || control == 0x141u;
}

bool RowCtrlAlwaysInBounds(uint32_t control)
{
	return control >= 0x121u;
}

// A row-shift operand can observe the lane outside the row boundary only when
// bound_ctrl is off; otherwise the destination is written or zeroed. For the
// injected-zero lowering to preserve the keep-old semantics, the ALU must map
// (0, old dst) to the old value: bitwise or/xor whose other source IS dst.
bool RowOpKeepOldSafe(const ShaderInstruction& instruction)
{
	const auto& source = instruction.src[0];
	if (!source.dpp || source.dpp_ctrl <= 0xffu || !RowCtrlSupported(source.dpp_ctrl) || source.dpp_row_mask != 15u ||
	    source.dpp_bank_mask != 15u)
	{
		return false;
	}
	if (RowCtrlAlwaysInBounds(source.dpp_ctrl))
	{
		return true;
	}
	if (source.dpp_bound_ctrl)
	{
		return false;
	}
	const bool identity_or = instruction.type == ShaderInstructionType::VOrB32 || instruction.type == ShaderInstructionType::VXorB32;
	return identity_or && instruction.src_num >= 2 && VectorRegister(instruction.src[1]) &&
	       instruction.src[1].register_id == instruction.dst.register_id;
}

// A lane the captured wave never populated reads the neutral initializer and
// nothing else: the initializer's condition is a subset of the capture, so such
// a lane takes the zero constant. From there every source that feeds a later
// result must itself be proven zero in those lanes for the result to stay zero.
// Permutation tables (src1/src2) choose which lane supplies the value but never
// add one, so only src0 is a data source for them.
bool GhostZeroSources(const ShaderInstruction& instruction, const VectorSet& zero)
{
	const int last = instruction.type == ShaderInstructionType::VPermlane16B32 ||
	                         instruction.type == ShaderInstructionType::VPermlanex16B32
	                     ? 1
	                     : instruction.src_num;
	for (int source = 0; source < last; ++source)
	{
		const auto& operand = instruction.src[source];
		if (ZeroConstant(operand))
		{
			continue;
		}
		if (!VectorRegister(operand) || !zero[operand.register_id])
		{
			return false;
		}
	}
	return true;
}

bool ClosedRegion(const ShaderCode& code, uint32_t begin, const ShaderOperand& saved, NeutralRegion* region)
{
	region->begin = begin;
	region->written = {};
	region->zero    = {};
	region->written[code.GetInstructions().At(begin + 1u).dst.register_id] = true;
	region->zero[code.GetInstructions().At(begin + 1u).dst.register_id]    = true;
	for (uint32_t cursor = begin + 2u; cursor < code.GetInstructions().Size(); ++cursor)
	{
		const auto& instruction = code.GetInstructions().At(cursor);
		if (LabelAt(code, instruction.pc))
		{
			return false;
		}
		if (Restores(instruction, saved))
		{
			region->end         = cursor;
			region->zero_closed = true;
			for (uint32_t reg = 0; reg < region->written.size(); ++reg)
			{
				region->zero_closed = region->zero_closed && (!region->written[reg] || region->zero[reg]);
			}
			return true;
		}
		if (!PureVector(instruction, region->written))
		{
			return false;
		}
		const auto& source = instruction.src[0];
		if (source.dpp && source.dpp_ctrl > 0xffu && !RowOpKeepOldSafe(instruction))
		{
			region->tier_safe = false;
		}
		if ((instruction.type == ShaderInstructionType::VPermlane16B32 || instruction.type == ShaderInstructionType::VPermlanex16B32) &&
		    (instruction.vop3_op_sel & 1u) != 0u)
		{
			region->tier_safe = false;
		}
		const bool keeps_old = source.dpp && source.dpp_ctrl > 0xffu && !source.dpp_bound_ctrl &&
		                       !RowCtrlAlwaysInBounds(source.dpp_ctrl);
		const int dst = instruction.dst.register_id;
		// A later write replaces the lane value: the register keeps its proven
		// ghost zero only when this instruction also derives it from proven
		// zeros (or, for a keep-old DPP, from the zero it already held).
		region->zero[dst]    = GhostZeroSources(instruction, region->zero) && (!keeps_old || region->zero[dst]);
		region->written[dst] = true;
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

bool RewritesVector(const ShaderInstruction& instruction, int reg)
{
	for (const auto& dst: {instruction.dst, instruction.dst2})
	{
		if (dst.type == ShaderOperandType::Vgpr && dst.register_id >= 0 && reg >= dst.register_id &&
		    reg < dst.register_id + std::max(dst.size, 1))
		{
			return true;
		}
	}
	return false;
}

// A VGPR holds a guest-defined value in every lane after a proven region wrote
// it with full EXEC. Narrower later writes keep the other lanes, as on hardware.
// The region must be the unconditional predecessor of the read: any label or
// overwriting instruction between them invalidates the proof.
bool DefinedBeforeRead(const ShaderCode& code, const std::vector<NeutralRegion>& regions, uint32_t index, int reg)
{
	for (uint32_t cursor = index; cursor > 0;)
	{
		const auto& instruction = code.GetInstructions().At(cursor);
		if (LabelAt(code, instruction.pc) || RewritesVector(instruction, reg))
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

namespace {

// Whether `index` sits inside a proven region (save/restore pair included).
// In-region instructions execute with EXEC widened to the whole host wave, so
// lane exchanges only need the injection lowering, never an EXEC test.
bool WaveRegionAt(const std::vector<NeutralRegion>& regions, uint32_t index, const NeutralRegion** out)
{
	for (const auto& region: regions)
	{
		if (index >= region.begin && index <= region.end)
		{
			*out = &region;
			return true;
		}
	}
	return false;
}

// The lane a read names may be unpopulated on a narrower host subgroup. Its
// value is the proven neutral zero only when the source register was written
// by a zero-closed region and nothing could bypass that write.
bool GhostZeroBeforeRead(const ShaderCode& code, const std::vector<NeutralRegion>& regions, uint32_t index, int reg)
{
	for (uint32_t cursor = index; cursor > 0;)
	{
		const auto& instruction = code.GetInstructions().At(cursor);
		if (LabelAt(code, instruction.pc) || RewritesVector(instruction, reg))
		{
			return false;
		}
		--cursor;
		for (const auto& region: regions)
		{
			if (region.end == cursor && region.zero_closed && region.zero[reg])
			{
				return true;
			}
		}
	}
	return false;
}

bool ConstantLane(const ShaderOperand& operand, uint32_t* lane)
{
	const bool literal = operand.type == ShaderOperandType::LiteralConstant || operand.type == ShaderOperandType::IntegerInlineConstant;
	if (!literal || operand.size != 0)
	{
		return false;
	}
	*lane = operand.constant.u;
	return true;
}

// The proven ghost-zero state of `reg` at instruction `index` inside `region`:
// the prefix is replayed with the same rules ClosedRegion applied, so a read
// before the region end observes exactly what the ghost lanes carry there.
bool RegionZeroAt(const ShaderCode& code, const NeutralRegion& region, uint32_t index, int reg)
{
	if (reg < 0 || index <= region.begin + 1u)
	{
		return false;
	}
	VectorSet zero {};
	zero[code.GetInstructions().At(region.begin + 1u).dst.register_id] = true;
	for (uint32_t cursor = region.begin + 2u; cursor < index && cursor <= region.end; ++cursor)
	{
		const auto& instruction = code.GetInstructions().At(cursor);
		const auto& source      = instruction.src[0];
		const bool  keeps_old   = source.dpp && source.dpp_ctrl > 0xffu && !source.dpp_bound_ctrl &&
		                          !RowCtrlAlwaysInBounds(source.dpp_ctrl);
		const int dst = instruction.dst.register_id;
		if (dst >= 0 && dst < static_cast<int>(zero.size()))
		{
			zero[dst] = GhostZeroSources(instruction, zero) && (!keeps_old || zero[dst]);
		}
	}
	return zero[reg];
}

} // namespace

bool ShaderFragmentWaveInsideRegion(const ShaderCode& code, uint32_t index)
{
	if (code.GetType() != ShaderType::Pixel || index >= code.GetInstructions().Size())
	{
		return false;
	}
	const auto           regions = CollectRegions(code);
	const NeutralRegion* found   = nullptr;
	return WaveRegionAt(regions, index, &found) && found->tier_safe && found->zero_closed;
}

bool ShaderFragmentWaveGhostZero(const ShaderCode& code, uint32_t index, int vgpr)
{
	if (code.GetType() != ShaderType::Pixel || index >= code.GetInstructions().Size() || vgpr < 0 || vgpr >= 256)
	{
		return false;
	}
	const auto           regions = CollectRegions(code);
	const NeutralRegion* region  = nullptr;
	if (WaveRegionAt(regions, index, &region) && region->tier_safe && region->zero_closed)
	{
		return RegionZeroAt(code, *region, index, vgpr);
	}
	return GhostZeroBeforeRead(code, regions, index, vgpr);
}

// Runs a Wave64 fragment program as a partially populated guest wave on a
// 32-lane host subgroup: grouping is the rasterizer's choice, a correct shader
// cannot depend on it, so the host wave is a legal wave64 population. Exact
// lowering needs every lane-indexed operation to stay inside a proven region
// (EXEC full there) or to read proven-neutral ghost lanes.
ShaderComputeWaveAnalysisResult ShaderAnalyzeFragmentNativeWaveTier(const ShaderCode& code)
{
	ShaderComputeWaveAnalysisResult result;
	result.supported = code.GetType() == ShaderType::Pixel;
	if (!result.supported)
	{
		return result;
	}
	bool       uses_wave = false;
	uint32_t   last_lane_op = 0;
	const auto regions  = CollectRegions(code);
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto&   instruction = code.GetInstructions().At(index);
		const NeutralRegion* region = nullptr;
		const bool    inside      = WaveRegionAt(regions, index, &region) && region->tier_safe && region->zero_closed;
		const bool    permute =
		    instruction.type == ShaderInstructionType::VPermlane16B32 || instruction.type == ShaderInstructionType::VPermlanex16B32;
		const bool row_dpp = instruction.src_num > 0 && instruction.src[0].dpp && instruction.src[0].dpp_ctrl > 0xffu;
		const bool readlane = instruction.type == ShaderInstructionType::VReadlaneB32 && instruction.src_num >= 2;
		// A masked bit count folds the architectural 64-lane EXEC into scalar
		// state; on a narrower host wave that value depends on the grouping.
		const bool popcount = instruction.type == ShaderInstructionType::VMbcntLoU32B32 ||
		                      instruction.type == ShaderInstructionType::VMbcntHiU32B32;
		// A real s_barrier orders the whole guest wave; Vulkan offers no legal
		// control barrier in the fragment stage, so the tier cannot lower it.
		// The Imm-format SOPP pseudo-ops are parser-level no-ops instead.
		const bool real_barrier = instruction.type == ShaderInstructionType::SBarrier &&
		                          instruction.format == ShaderInstructionFormat::Empty;
		uses_wave = uses_wave || permute || row_dpp || readlane || popcount;
		if (permute || row_dpp || readlane)
		{
			last_lane_op = index;
		}
		if (real_barrier)
		{
			result.supported      = false;
			result.unsupported_pc = instruction.pc;
			result.reason         = "s_barrier cannot be lowered in the fragment stage";
			return result;
		}
		if (popcount)
		{
			result.supported      = false;
			result.unsupported_pc = instruction.pc;
			result.reason         = "masked bit count is grouping sensitive";
			return result;
		}
		if (inside)
		{
			// Inside the region EXEC is full, so lane ops run directly. A
			// readlane still reaches guest lanes the host wave never populated;
			// those read the proven neutral zero, which RegionZeroAt evaluates
			// at this exact point.
			if (!readlane)
			{
				continue;
			}
			uint32_t lane = 0;
			if (ConstantLane(instruction.src[1], &lane) && lane < 32u)
			{
				continue;
			}
			const auto& source = instruction.src[0];
			if ((ConstantLane(instruction.src[1], &lane) && lane >= 64u) || !VectorRegister(source) ||
			    !RegionZeroAt(code, *region, index, source.register_id))
			{
				result.supported      = false;
				result.unsupported_pc = instruction.pc;
				result.reason         = "readlane inside a neutral region over a ghost lane without a proven zero";
				return result;
			}
			continue;
		}
		if (row_dpp || permute)
		{
			result.supported      = false;
			result.unsupported_pc = instruction.pc;
			result.reason         = "lane exchange outside a proven neutral region";
			return result;
		}
		if (!readlane)
		{
			continue;
		}
		uint32_t lane = 0;
		if (!ConstantLane(instruction.src[1], &lane) || lane >= 32u)
		{
			const auto& source = instruction.src[0];
			if ((ConstantLane(instruction.src[1], &lane) && lane >= 64u) || !VectorRegister(source) ||
			    !GhostZeroBeforeRead(code, regions, index, source.register_id))
			{
				result.supported      = false;
				result.unsupported_pc = instruction.pc;
				result.reason         = "readlane over the ghost half without a proven neutral value";
				return result;
			}
		}
	}
	result.supported = result.supported && uses_wave;
	// A kill before the last lane exchange demotes lanes the shuffles still
	// read. Host-side demotion comes only from an OpKill emitter: the discard
	// tail (a null export following a discard block) or a literal exec=0 write.
	if (result.supported)
	{
		for (uint32_t index = 0; index <= last_lane_op; ++index)
		{
			const auto& instruction = code.GetInstructions().At(index);
			const bool  null_tail   = instruction.format == ShaderInstructionFormat::NullVmDone;
			const bool  mrt_null_done =
			    instruction.format >= ShaderInstructionFormat::Mrt0OffOffComprVmDone &&
			    instruction.format <= ShaderInstructionFormat::Mrt7OffOffComprVmDone;
			const bool  null_export = instruction.type == ShaderInstructionType::Exp &&
			                         (null_tail || (mrt_null_done && index > 0 &&
			                                        code.ReadBlock(code.GetInstructions().At(index - 1u).pc).is_discard));
			const bool exec_kill = instruction.dst.type == ShaderOperandType::ExecLo && instruction.src_num > 0 &&
			                       instruction.src[0].type == ShaderOperandType::LiteralConstant &&
			                       instruction.src[0].constant.u == 0u;
			if (null_export || exec_kill)
			{
				result.supported      = false;
				result.unsupported_pc = instruction.pc;
				result.reason         = "a discard or exec kill precedes a lane exchange";
				return result;
			}
		}
	}
	return result;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
