#include "ShaderLaneFlow.h"

#include "Kyty/Core/MagicEnum.h"

#include "ShaderNativeWaveInternal.h"

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics::LaneFlow {

using Type    = ShaderInstructionType;
using Operand = ShaderOperandType;

bool StartsWith(Type type, const char* prefix)
{
	return Core::EnumName8(type).StartsWith(prefix);
}

bool AnyPrefix(Type type, std::initializer_list<const char*> prefixes)
{
	for (const char* prefix: prefixes)
	{
		if (StartsWith(type, prefix)) { return true; }
	}
	return false;
}

bool ScalarRange(const ShaderOperand& op, unsigned* first, unsigned* count)
{
	*count = op.size > 0 ? static_cast<unsigned>(op.size) : 1u;
	switch (op.type)
	{
		case Operand::Sgpr:
			if (op.register_id < 0) { return false; }
			*first = static_cast<unsigned>(op.register_id);
			return *first + *count <= kSgprWords;
		case Operand::VccLo: *first = kVccLoWord; return *count <= 2u;
		case Operand::VccHi: *first = kVccLoWord + 1u; return *count <= 1u;
		case Operand::ExecLo: *first = kExecLoWord; return *count <= 2u;
		case Operand::ExecHi: *first = kExecLoWord + 1u; return *count <= 1u;
		case Operand::M0: *first = kM0Word; return *count <= 1u;
		case Operand::Scc: *first = kSccWord; *count = 1u; return true;
		default: return false;
	}
}

bool IsConstant(const ShaderOperand& op)
{
	return op.type == Operand::LiteralConstant || op.type == Operand::IntegerInlineConstant || op.type == Operand::FloatInlineConstant ||
	       op.type == Operand::Null || op.type == Operand::Unknown;
}

bool IsScalarLoad(Type type)
{
	return StartsWith(type, "SLoad") || StartsWith(type, "SBufferLoad");
}

bool IsScalarAlu(Type type)
{
	// S_MOVK/S_ADDK/S_CMPK take a 16-bit immediate; only the M0-relative moves index registers.
	return !StartsWith(type, "SMovrel") &&
	       AnyPrefix(type, {"SMov", "SCmov", "SCselect", "SAnd", "SOr", "SXor", "SNand", "SNor", "SXnor", "SNot", "SLsh", "SAshr",
	                        "SBfe", "SBfm", "SBcnt", "SFf", "SFl", "SMin", "SMax", "SAdd", "SSub", "SMul", "SAbs", "SSext", "SCmp",
	                        "SBitcmp", "SGetpc", "SWqm", "SBrev", "SPack"});
}

bool ReadsSccImplicitly(Type type)
{
	return StartsWith(type, "SCselect") || StartsWith(type, "SCmov") || type == Type::SAddcU32;
}

bool WritesScc(Type type)
{
	return !AnyPrefix(type, {"SMov", "SCmov", "SCselect", "SGetpc"});
}

bool IsMaskAlgebra(Type type)
{
	// Pointwise on lane bits, including the forms that also replace EXEC (SAnd*Saveexec
	// yields the old mask in the destination and the combined mask in EXEC).
	return AnyPrefix(type, {"SMov", "SAnd", "SOr", "SXor", "SNand", "SNor", "SXnor", "SNot", "SWqm"}) && !StartsWith(type, "SMovrel");
}

bool WritesExecOnly(Type type)
{
	return StartsWith(type, "VCmpx");
}

bool IsSelect(Type type)
{
	return StartsWith(type, "SCselect");
}

bool IsBufferLoad(Type type)
{
	return StartsWith(type, "BufferLoad") || StartsWith(type, "TBufferLoad");
}

bool IsVectorLoad(Type type)
{
	return IsBufferLoad(type) || StartsWith(type, "GlobalLoad");
}

bool IsLaneLocalImageRead(Type type)
{
	switch (type)
	{
		case Type::ImageLoad:
		case Type::ImageSampleL:
		case Type::ImageSampleLz:
		case Type::ImageSampleLzO:
		case Type::ImageSampleDrefLz: return true;
		default: return false;
	}
}

bool IsLaneExchange(Type type)
{
	return AnyPrefix(type, {"VReadlane", "VReadfirstlane", "VWritelane", "VPermlane", "VPermlanex", "VMbcnt", "VCmpx"});
}

bool IsLaneBit(const ShaderInstruction& inst, int source)
{
	return source == 2 && (inst.type == Type::VCndmaskB32 || inst.format == ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2);
}

bool UsesImplicitDerivatives(Type type)
{
	return type == Type::ImageSample || type == Type::ImageSampleB || StartsWith(type, "ImageGather4");
}

bool WritesMemory(Type type)
{
	return AnyPrefix(type, {"BufferStore", "BufferAtomic", "TBufferStore", "ImageStore", "ImageAtomic", "Ds", "Global", "Flat", "Scratch"});
}

bool VgprRange(const ShaderOperand& op, unsigned* first, unsigned* count)
{
	if (op.type != Operand::Vgpr || op.register_id < 0 || op.size <= 0) { return false; }
	*first = static_cast<unsigned>(op.register_id);
	*count = static_cast<unsigned>(op.size);
	return *first + *count <= static_cast<unsigned>(kVgprs);
}

bool FlowState::Merge(const FlowState& other)
{
	if (!other.reachable) { return false; }
	if (!reachable)
	{
		*this = other;
		return true;
	}
	bool changed = false;
	for (unsigned word = 0; word < kWords; ++word)
	{
		const auto merged = static_cast<uint8_t>(scalar[word] | other.scalar[word]);
		changed           = changed || merged != scalar[word];
		scalar[word]      = merged;
	}
	const auto still_defined = defined & other.defined;
	const auto still_uniform = uniform & other.uniform;
	const bool still_vcc     = vcc_uniform && other.vcc_uniform;
	const auto still_masks   = uniform_mask & other.uniform_mask;
	changed                  = changed || still_defined != defined || still_uniform != uniform || still_vcc != vcc_uniform ||
	          still_masks != uniform_mask;
	defined                  = still_defined;
	uniform                  = still_uniform;
	uniform_mask             = still_masks;
	vcc_uniform              = still_vcc;
	return changed;
}

} // namespace Kyty::Libs::Graphics::LaneFlow

#endif // KYTY_EMU_ENABLED
