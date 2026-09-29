#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"

#include "Kyty/Core/MagicEnum.h"

#include "ShaderSpirvInternal.h"

#include <cctype>
#include <cstdio>
#include <vector>
#include <set>
#include <string>

#ifdef KYTY_EMU_ENABLED

KYTY_ENUM_RANGE(Kyty::Libs::Graphics::ShaderInstructionType, 0, static_cast<int>(Kyty::Libs::Graphics::ShaderInstructionType::ZMax));

namespace Kyty::Libs::Graphics {
namespace {

bool IsIdChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsNativeVgprName(const std::string& id)
{
	if (id.size() < 2 || id[0] != 'v')
	{
		return false;
	}
	for (size_t i = 1; i < id.size(); ++i)
	{
		if (std::isdigit(static_cast<unsigned char>(id[i])) == 0)
		{
			return false;
		}
	}
	return true;
}

// Result ids the snippet defines (`%id = ...`), including its labels. Native
// templates may place several instructions on one line, so every `%id =` in
// the text counts, not only the first on a line.
std::set<std::string> DefinedIds(const std::string& text)
{
	std::set<std::string> ids;
	for (size_t pos = text.find('%'); pos != std::string::npos; pos = text.find('%', pos + 1))
	{
		size_t end = pos + 1;
		while (end < text.size() && IsIdChar(text[end]))
		{
			++end;
		}
		size_t eq = end;
		while (eq < text.size() && (text[eq] == ' ' || text[eq] == '\t'))
		{
			++eq;
		}
		if (end > pos + 1 && eq < text.size() && text[eq] == '=')
		{
			ids.insert(text.substr(pos + 1, end - pos - 1));
		}
	}
	return ids;
}

// Renames native VGPRs to the bank's register and snippet-local ids to
// bank-unique ids. EXEC loads become the lane's bank bit; any other EXEC use
// makes the snippet unrepresentable.
bool RewriteForBank(const std::string& text, const std::set<std::string>& defined, const std::string& bank,
                    const std::string& exec_bit, const std::string& zero, std::string* out)
{
	std::string result;
	result.reserve(text.size() + text.size() / 4);
	size_t pos = 0;
	while (pos < text.size())
	{
		if (text[pos] != '%')
		{
			result += text[pos++];
			continue;
		}
		size_t end = pos + 1;
		while (end < text.size() && IsIdChar(text[end]))
		{
			++end;
		}
		const std::string id = text.substr(pos + 1, end - pos - 1);
		if (id == "wave_dpp_source_exec")
		{
			// Source activity uses the entire architectural mask word. The
			// ordinary EXEC names below represent this destination lane's bit.
			result += "%exec_" + (bank == "low" ? std::string("lo") : std::string("hi"));
		} else if (id == "exec_lo" || id == "exec_hi")
		{
			static const std::string kLoad = "OpLoad %uint ";
			if (result.size() < kLoad.size() || result.compare(result.size() - kLoad.size(), kLoad.size(), kLoad) != 0)
			{
				return false;
			}
			result.resize(result.size() - kLoad.size());
			result += "OpCopyObject %uint %" + (id == "exec_lo" ? exec_bit : zero);
		} else if (IsNativeVgprName(id) || defined.count(id) != 0)
		{
			result += "%" + id + "_" + bank;
		} else
		{
			result += "%" + id;
		}
		pos = end;
	}
	*out = std::move(result);
	return true;
}

// Type of a register variable the if-conversion may store to, or null.
const char* RegisterStoreType(const std::string& name)
{
	auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
	if (starts("v") && (name.find("_low") != std::string::npos || name.find("_high") != std::string::npos))
	{
		return "float";
	}
	if (starts("s") || name == "vcc_lo" || name == "vcc_hi" || name == "exec_lo" || name == "exec_hi" || name == "scc" || name == "m0")
	{
		return (starts("s") && name.find_first_not_of("0123456789", 1) != std::string::npos) ? nullptr : "uint";
	}
	return nullptr;
}

std::vector<std::string> SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t                   pos = 0;
	while (pos <= text.size())
	{
		const size_t end = text.find('\n', pos);
		lines.push_back(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
		if (end == std::string::npos)
		{
			break;
		}
		pos = end + 1;
	}
	return lines;
}

std::string Trim(const std::string& line)
{
	const size_t a = line.find_first_not_of(" \t");
	const size_t b = line.find_last_not_of(" \t");
	return a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
}

// Replaces straight-line EXEC guards
//   OpSelectionMerge %M None / OpBranchConditional %C %T %M / %T = OpLabel /
//   <body> / OpBranch %M / %M = OpLabel
// whose body only computes values and stores to registers, by the body with
// every store turned into store(select(C, new, old)). This keeps per-bank
// re-emission from creating two basic blocks per guest instruction.
std::string IfConvertGuards(const std::string& text, const std::string& prefix)
{
	auto lines = SplitLines(text);
	std::string out;
	int         serial = 0;
	for (size_t i = 0; i < lines.size(); i++)
	{
		const auto merge_line = Trim(lines[i]);
		bool       converted  = false;
		if (merge_line.rfind("OpSelectionMerge %", 0) == 0 && i + 2 < lines.size())
		{
			const auto merge  = merge_line.substr(17, merge_line.find(' ', 17) - 17);
			const auto branch = Trim(lines[i + 1]);
			const auto label  = Trim(lines[i + 2]);
			char       cond[128] = {};
			char       then_label[128] = {};
			char       else_label[128] = {};
			if (std::sscanf(branch.c_str(), "OpBranchConditional %%%127s %%%127s %%%127s", cond, then_label, else_label) == 3 &&
			    ("%" + std::string(else_label)) == merge && label == "%" + std::string(then_label) + " = OpLabel")
			{
				std::string body;
				bool        ok  = true;
				size_t      end = i + 3;
				for (; end < lines.size(); end++)
				{
					const auto line = Trim(lines[end]);
					if (line == "OpBranch " + merge)
					{
						break;
					}
					if (line.find("OpLabel") != std::string::npos || line.find("OpBranch") != std::string::npos ||
					    line.find("OpSelectionMerge") != std::string::npos || line.find("OpLoopMerge") != std::string::npos ||
					    line.find("OpAtomic") != std::string::npos || line.find("OpImageWrite") != std::string::npos ||
					    line.find("OpFunctionCall") != std::string::npos || line.find("OpReturn") != std::string::npos)
					{
						ok = false;
						break;
					}
					if (line.rfind("OpStore %", 0) == 0)
					{
						char target[128] = {};
						char value[128]  = {};
						const char* type = nullptr;
						if (std::sscanf(line.c_str(), "OpStore %%%127s %%%127s", target, value) != 2 ||
						    (type = RegisterStoreType(target)) == nullptr)
						{
							ok = false;
							break;
						}
						const auto id = prefix + "_ifc" + std::to_string(serial++);
						body += "%" + id + "_old = OpLoad %" + type + " %" + target + "\n%" + id + " = OpSelect %" + type + " %" + cond +
						        " %" + value + " %" + id + "_old\nOpStore %" + target + " %" + id + "\n";
						continue;
					}
					body += lines[end] + "\n";
				}
				// Removed labels must not be referenced outside the region.
				auto referenced_outside = [&](const std::string& name)
				{
					for (size_t k = 0; k < lines.size(); k++)
					{
						if (k >= i && k <= end + 1)
						{
							continue;
						}
						const auto at = lines[k].find(name);
						if (at != std::string::npos && (at + name.size() == lines[k].size() || !IsIdChar(lines[k][at + name.size()])))
						{
							return true;
						}
					}
					return false;
				};
				if (ok && end + 1 < lines.size() && Trim(lines[end + 1]) == merge + " = OpLabel" &&
				    !referenced_outside("%" + std::string(then_label)) && !referenced_outside(merge))
				{
					out += body;
					i         = end + 1;
					converted = true;
				}
			}
		}
		if (!converted)
		{
			out += lines[i];
			if (i + 1 < lines.size())
			{
				out += "\n";
			}
		}
	}
	return out;
}

} // namespace

bool Spirv::EmitComputeWaveGenericInstruction(const RecompilerFunc* func, const ShaderInstruction& instruction, uint32_t index,
                                              String8* output)
{
	if (output == nullptr || func == nullptr || func->type != instruction.type || func->format != instruction.format ||
	    !UsesComputeWaveBanks() ||
	    !(ShaderComputeWaveGenericVectorSupported(instruction) || ShaderComputeWaveGenericLdsSupported(instruction) ||
	      ShaderComputeWaveVectorBufferAtomicUmaxSupported(instruction)))
	{
		return false;
	}
	String8 native;
	if (!func->func(index, m_code, &native, this, func->param, func->scc_check))
	{
		return false;
	}
	// Image templates store native VGPR names. Guard those stores while they
	// are still recognizable, then rewrite each guard to its bank's EXEC bit.
	native = GuardImageDestinationStores(native, instruction, index);
	const std::string text(native.c_str());
	const auto        defined = DefinedIds(text);
	const auto        zero    = GetConstantUint(0u);
	const auto        one     = GetConstantUint(1u);

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;

	String8 source;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const std::string bank_name = bank == ShaderWaveBank::Low ? "low" : "high";
		const std::string exec_bit  = "wave_generic_exec_" + std::to_string(index) + "_" + bank_name;
		if (!EmitComputeWaveMaskBit(exec, bank, String8((exec_bit + "_b").c_str()), &source))
		{
			return false;
		}
		source += String8::FromPrintf("%%%s = OpSelect %%uint %%%s_b %%%s %%%s\n", exec_bit.c_str(), exec_bit.c_str(), one.c_str(),
		                              zero.c_str());
		std::string rewritten;
		if (!RewriteForBank(text, defined, bank_name, exec_bit, zero.c_str(), &rewritten))
		{
			return false;
		}
		source += String8(IfConvertGuards(rewritten, exec_bit).c_str());
		source += "\n";
	}
	*output += source;
	return true;
}

bool Spirv::EmitComputeWaveGenericCompare(const RecompilerFunc* func, const ShaderInstruction& instruction, uint32_t index,
                                          String8* output)
{
	if (output == nullptr || func == nullptr || func->type != instruction.type || func->format != instruction.format ||
	    !UsesComputeWaveBanks() || !ShaderComputeWaveGenericCompareSupported(instruction))
	{
		return false;
	}
	// The native lowering is consulted only for its lane predicate.
	String8 native;
	if (!func->func(index, m_code, &native, this, func->param, func->scc_check))
	{
		return false;
	}
	const std::string text(native.c_str());
	const std::string result_id = "%t3_" + std::to_string(index);
	const auto        result_at = text.find(result_id + " =");
	if (result_at == std::string::npos)
	{
		return false;
	}
	const auto line_end = text.find('\n', result_at);
	const auto prefix   = text.substr(0, line_end == std::string::npos ? text.size() : line_end + 1);
	if (prefix.find("OpStore") != std::string::npos || prefix.find("OpBranch") != std::string::npos ||
	    prefix.find("OpLabel") != std::string::npos)
	{
		return false;
	}
	const auto defined = DefinedIds(prefix);
	const auto zero    = GetConstantUint(0u);
	const auto one     = GetConstantUint(1u);

	ShaderOperand exec {};
	exec.type = ShaderOperandType::ExecLo;
	exec.size = 2;

	const auto index_str = std::to_string(index);
	String8    source;
	String8    active[2];
	int        slot = 0;
	for (const auto bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const std::string bank_name = bank == ShaderWaveBank::Low ? "low" : "high";
		const std::string exec_bit  = "wave_gcmp_exec_" + index_str + "_" + bank_name;
		if (!EmitComputeWaveMaskBit(exec, bank, String8((exec_bit + "_b").c_str()), &source))
		{
			return false;
		}
		source += String8::FromPrintf("%%%s = OpSelect %%uint %%%s_b %%%s %%%s\n", exec_bit.c_str(), exec_bit.c_str(), one.c_str(),
		                              zero.c_str());
		std::string rewritten;
		if (!RewriteForBank(prefix, defined, bank_name, exec_bit, zero.c_str(), &rewritten))
		{
			return false;
		}
		source += String8(rewritten.c_str());
		const std::string lane   = "t3_" + index_str + "_" + bank_name;
		const std::string pred   = "wave_gcmp_pred_" + index_str + "_" + bank_name;
		const std::string live   = "wave_gcmp_active_" + index_str + "_" + bank_name;
		source += String8::FromPrintf("\n%%%s = OpINotEqual %%bool %%%s %%%s\n%%%s = OpLogicalAnd %%bool %%%s %%%s_b\n", pred.c_str(),
		                              lane.c_str(), zero.c_str(), live.c_str(), pred.c_str(), exec_bit.c_str());
		active[slot++] = String8(live.c_str());
	}
	const String8 mask_low  = String8::FromPrintf("wave_gcmp_mask_low_%u", index);
	const String8 mask_high = String8::FromPrintf("wave_gcmp_mask_high_%u", index);
	if (!EmitComputeWaveBallot(active[0], active[1], mask_low, mask_high, &source))
	{
		return false;
	}
	ShaderOperand destination = instruction.dst;
	if (Core::EnumName8(instruction.type).StartsWith("VCmpx"))
	{
		destination = exec;
	}
	const auto low  = GetComputeWaveRegister(destination, ShaderWaveBank::Low, 0);
	const auto high = GetComputeWaveRegister(destination, ShaderWaveBank::High, 1);
	if (low.type != SpirvType::Uint || high.type != SpirvType::Uint || low.value.IsEmpty() || high.value.IsEmpty())
	{
		return false;
	}
	source += String8::FromPrintf("               OpStore %%%s %%%s\n               OpStore %%%s %%%s\n", low.value.c_str(),
	                              mask_low.c_str(), high.value.c_str(), mask_high.c_str());
	*output += source;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
