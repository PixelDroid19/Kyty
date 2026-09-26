#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

const char* WaveBankName(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low ? "low" : "high";
}

} // namespace

// One admitted BUFFER_LOAD_DWORD{,X2,X3,X4} per guest lane-half. The shared
// raw-address helper consumes (index, imm+0, soffset, desc1, desc3); desc0 is
// the host storage slot and desc2 drives the empty-record guard. Every source,
// descriptor word, old destination component and EXEC bit is captured before
// the first store of each bank, so a VDATA==VADDR alias stays bank-local.
bool Spirv::EmitComputeWaveBufferLoadInstruction(const ShaderInstruction& instruction, uint32_t index, String8* output) const
{
	if (output == nullptr || !UsesComputeWaveBanks() || !ShaderComputeWaveVectorBufferLoadSupported(instruction))
	{
		return false;
	}

	const auto& descriptor = instruction.src[1];
	const auto  desc0      = GetComputeWaveRegister(descriptor, ShaderWaveBank::Low, 0);
	const auto  desc1      = GetComputeWaveRegister(descriptor, ShaderWaveBank::Low, 1);
	const auto  desc2      = GetComputeWaveRegister(descriptor, ShaderWaveBank::Low, 2);
	const auto  desc3      = GetComputeWaveRegister(descriptor, ShaderWaveBank::Low, 3);
	if (desc0.type != SpirvType::Uint || desc1.type != SpirvType::Uint || desc2.type != SpirvType::Uint ||
	    desc3.type != SpirvType::Uint || desc0.value.IsEmpty() || desc1.value.IsEmpty() || desc2.value.IsEmpty() ||
	    desc3.value.IsEmpty())
	{
		return false;
	}

	const auto immediate   = GetConstantUint(instruction.buffer_imm_offset);
	const auto zero_uint   = GetConstantUint(0u);
	const auto zero_int    = GetConstantInt(0);
	const auto zero_float  = GetConstantFloat(0.0f);
	if (immediate == "unknown_uint_constant" || zero_uint == "unknown_uint_constant" ||
	    zero_int == "unknown_int_constant" || zero_float == "unknown_float_constant")
	{
		return false;
	}

	const uint32_t dwords = static_cast<uint32_t>(instruction.dst.size);
	for (const ShaderWaveBank bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto bank_name = WaveBankName(bank);
		const auto tag       = String8::FromPrintf("%u_%s", index, bank_name);
		const auto id        = [&tag](const char* name) { return String8::FromPrintf("wave_buf_%s_%s", name, tag.c_str()); };

		String8 source;
		// Capture the per-bank index, the scalar offset constant, the shared
		// descriptor words, every old destination component and the EXEC bit
		// before the branch below stores anything.
		if (!EmitComputeWaveOperandUint(instruction.src[0], bank, id("index"), &source) ||
		    !EmitComputeWaveOperandUint(instruction.src[2], bank, id("soffset"), &source))
		{
			return false;
		}
		source += String8(R"(
%<d0> = OpLoad %uint %<desc0>
%<d1> = OpLoad %uint %<desc1>
%<d2> = OpLoad %uint %<desc2>
%<d3> = OpLoad %uint %<desc3>
)")
		              .ReplaceStr("<d0>", id("desc0"))
		              .ReplaceStr("<d1>", id("desc1"))
		              .ReplaceStr("<d2>", id("desc2"))
		              .ReplaceStr("<d3>", id("desc3"))
		              .ReplaceStr("<desc0>", desc0.value)
		              .ReplaceStr("<desc1>", desc1.value)
		              .ReplaceStr("<desc2>", desc2.value)
		              .ReplaceStr("<desc3>", desc3.value);

		for (uint32_t component = 0; component < dwords; ++component)
		{
			const auto old = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			if (old.type != SpirvType::Float || old.value.IsEmpty())
			{
				return false;
			}
			source += String8::FromPrintf("%%%s = OpLoad %%float %%%s\n", id(String8::FromPrintf("old_%u", component).c_str()).c_str(),
			                              old.value.c_str());
		}

		ShaderOperand exec {};
		exec.type = ShaderOperandType::ExecLo;
		exec.size = 2;
		if (!EmitComputeWaveMaskBit(exec, bank, id("exec"), &source))
		{
			return false;
		}

		source += String8(R"(
%<addr> = OpFunctionCall %uint %buffer_raw_address %<index> %<imm> %<soffset> %<d1> %<d3>
%<empty> = OpIEqual %bool %<d2> %<zero_u>
%<access> = OpLogicalNot %bool %<empty>
%<addr_i> = OpBitcast %int %<addr>
%<slot_i> = OpBitcast %int %<d0>
               OpStore %temp_int_1 %<zero_i>
               OpStore %temp_int_2 %<addr_i>
               OpStore %temp_int_3 %<zero_i>
               OpStore %temp_int_4 %<slot_i>
               OpSelectionMerge %<merge> None
               OpBranchConditional %<access> %<then> %<oob>
%<then> = OpLabel
)")
		              .ReplaceStr("<addr>", id("addr"))
		              .ReplaceStr("<index>", id("index"))
		              .ReplaceStr("<imm>", immediate)
		              .ReplaceStr("<soffset>", id("soffset"))
		              .ReplaceStr("<d1>", id("desc1"))
		              .ReplaceStr("<d3>", id("desc3"))
		              .ReplaceStr("<empty>", id("empty"))
		              .ReplaceStr("<d2>", id("desc2"))
		              .ReplaceStr("<zero_u>", zero_uint)
		              .ReplaceStr("<access>", id("access"))
		              .ReplaceStr("<addr_i>", id("addr_i"))
		              .ReplaceStr("<slot_i>", id("slot_i"))
		              .ReplaceStr("<d0>", id("desc0"))
		              .ReplaceStr("<zero_i>", zero_int)
		              .ReplaceStr("<merge>", id("merge"))
		              .ReplaceStr("<then>", id("then"))
		              .ReplaceStr("<oob>", id("oob"));

		for (uint32_t component = 0; component < dwords; ++component)
		{
			if (component != 0)
			{
				const auto component_offset = GetConstantUint(component * 4u);
				if (component_offset == "unknown_uint_constant")
				{
					return false;
				}
				source += String8(R"(
%<off> = OpIAdd %uint %<addr> %<component_offset>
%<off_i> = OpBitcast %int %<off>
               OpStore %temp_int_2 %<off_i>
)")
				              .ReplaceStr("<off>", id(String8::FromPrintf("off_%u", component).c_str()))
				              .ReplaceStr("<addr>", id("addr"))
				              .ReplaceStr("<component_offset>", component_offset)
				              .ReplaceStr("<off_i>", id(String8::FromPrintf("off_i_%u", component).c_str()));
			}
			const auto dst = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			source += String8(R"(
%<call> = OpFunctionCall %void %buffer_load_float1 %temp_float %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
%<val> = OpLoad %float %temp_float
%<sel> = OpSelect %float %<exec> %<val> %<old>
               OpStore %<dst> %<sel>
)")
			              .ReplaceStr("<call>", id(String8::FromPrintf("call_%u", component).c_str()))
			              .ReplaceStr("<val>", id(String8::FromPrintf("val_%u", component).c_str()))
			              .ReplaceStr("<sel>", id(String8::FromPrintf("sel_%u", component).c_str()))
			              .ReplaceStr("<exec>", id("exec"))
			              .ReplaceStr("<old>", id(String8::FromPrintf("old_%u", component).c_str()))
			              .ReplaceStr("<dst>", dst.value);
		}
		source += String8::FromPrintf("               OpBranch %%%s\n%%%s = OpLabel\n", id("merge").c_str(), id("oob").c_str());
		for (uint32_t component = 0; component < dwords; ++component)
		{
			const auto dst = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			source += String8(R"(
%<zsel> = OpSelect %float %<exec> %<zero_f> %<old>
               OpStore %<dst> %<zsel>
)")
			              .ReplaceStr("<zsel>", id(String8::FromPrintf("zsel_%u", component).c_str()))
			              .ReplaceStr("<exec>", id("exec"))
			              .ReplaceStr("<zero_f>", zero_float)
			              .ReplaceStr("<old>", id(String8::FromPrintf("old_%u", component).c_str()))
			              .ReplaceStr("<dst>", dst.value);
		}
		source += String8::FromPrintf("               OpBranch %%%s\n%%%s = OpLabel\n", id("merge").c_str(), id("merge").c_str());

		*output += source;
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
