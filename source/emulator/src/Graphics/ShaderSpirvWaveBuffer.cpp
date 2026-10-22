#include "Emulator/Graphics/ShaderComputeWaveVectorBuffer.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
namespace {

const char* WaveBankName(ShaderWaveBank bank)
{
	return bank == ShaderWaveBank::Low ? "low" : "high";
}

String8 WaveBufferId(const String8& tag, const char* name)
{
	return String8::FromPrintf("wave_buf_%s_%s", name, tag.c_str());
}

String8 WaveBufferId(const String8& tag, const char* name, uint32_t component)
{
	return WaveBufferId(tag, String8::FromPrintf("%s_%u", name, component).c_str());
}

} // namespace

// One admitted BUFFER_LOAD_DWORD{,X2,X3,X4} per guest lane-half. The shared
// raw-address helper consumes (index, imm+0, soffset, desc1, desc3). A V#
// that a binding describes holds the host storage slot in desc0 and desc2
// drives the empty-record guard; a V# that no binding describes holds a guest
// base and is read through guest device addressing with the RDNA2 range
// check. Every source, descriptor word, old destination component and EXEC
// bit is captured before the first store of each bank, so a VDATA==VADDR
// alias stays bank-local.
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

	const auto* vertex = GetVsInputInfo();
	const bool  guest  = m_bind != nullptr && UsesGuestDeviceAddress() &&
	                   ShaderVectorBufferUsesRuntimeDescriptor(*m_bind, instruction, vertex != nullptr && vertex->gs_prolog ? 8 : 0);
	if (!guest && (m_bind == nullptr || m_bind->storage_buffers.buffers_num == 0))
	{
		return false;
	}

	const uint32_t dwords     = static_cast<uint32_t>(instruction.dst.size);
	const auto     immediate  = GetConstantUint(instruction.buffer_imm_offset);
	const auto     zero_uint  = GetConstantUint(0u);
	const auto     zero_int   = GetConstantInt(0);
	const auto     zero_float = GetConstantFloat(0.0f);
	const auto     one_uint   = GetConstantUint(1u);
	const auto     size       = GetConstantUint(dwords * 4u);
	const auto     shift16    = GetConstantUint(16u);
	const auto     mask_14    = GetConstantUint(0x3fffu);
	const auto     mask_16    = GetConstantUint(0xffffu);
	if (immediate == "unknown_uint_constant" || zero_uint == "unknown_uint_constant" || zero_int == "unknown_int_constant" ||
	    zero_float == "unknown_float_constant")
	{
		return false;
	}
	if (guest && (one_uint == "unknown_uint_constant" || size == "unknown_uint_constant" || shift16 == "unknown_uint_constant" ||
	              mask_14 == "unknown_uint_constant" || mask_16 == "unknown_uint_constant"))
	{
		return false;
	}

	// Structured (stride != 0) accesses check the index against NUM_RECORDS,
	// raw ones the byte range; the in-range branch loads through the guest
	// base plus the raw address.
	const auto guest_access = [&](ShaderWaveBank bank, const String8& tag, String8* source)
	{
		*source += String8(R"(
%<stride_raw> = OpShiftRightLogical %uint %<d1> %<shift16>
%<stride> = OpBitwiseAnd %uint %<stride_raw> %<mask_14>
%<structured> = OpINotEqual %bool %<stride> %<zero_u>
%<in_index> = OpULessThan %bool %<index> %<d2>
%<room> = OpUGreaterThanEqual %bool %<d2> %<size>
%<last> = OpISub %uint %<d2> %<size>
%<fits> = OpULessThanEqual %bool %<addr> %<last>
%<in_raw> = OpLogicalAnd %bool %<room> %<fits>
%<access> = OpSelect %bool %<structured> %<in_index> %<in_raw>
%<lo> = OpIAdd %uint %<d0> %<addr>
%<carry_b> = OpULessThan %bool %<lo> %<d0>
%<carry> = OpSelect %uint %<carry_b> %<one> %<zero_u>
%<base_hi> = OpBitwiseAnd %uint %<d1> %<mask_16>
%<hi> = OpIAdd %uint %<base_hi> %<carry>
               OpSelectionMerge %<merge> None
               OpBranchConditional %<access> %<then> %<oob>
%<then> = OpLabel
)")
		               .ReplaceStr("<stride_raw>", WaveBufferId(tag, "stride_raw"))
		               .ReplaceStr("<structured>", WaveBufferId(tag, "structured"))
		               .ReplaceStr("<stride>", WaveBufferId(tag, "stride"))
		               .ReplaceStr("<in_index>", WaveBufferId(tag, "in_index"))
		               .ReplaceStr("<in_raw>", WaveBufferId(tag, "in_raw"))
		               .ReplaceStr("<room>", WaveBufferId(tag, "room"))
		               .ReplaceStr("<last>", WaveBufferId(tag, "last"))
		               .ReplaceStr("<fits>", WaveBufferId(tag, "fits"))
		               .ReplaceStr("<access>", WaveBufferId(tag, "access"))
		               .ReplaceStr("<carry_b>", WaveBufferId(tag, "carry_b"))
		               .ReplaceStr("<carry>", WaveBufferId(tag, "carry"))
		               .ReplaceStr("<base_hi>", WaveBufferId(tag, "base_hi"))
		               .ReplaceStr("<lo>", WaveBufferId(tag, "lo"))
		               .ReplaceStr("<hi>", WaveBufferId(tag, "hi"))
		               .ReplaceStr("<addr>", WaveBufferId(tag, "addr"))
		               .ReplaceStr("<index>", WaveBufferId(tag, "index"))
		               .ReplaceStr("<d0>", WaveBufferId(tag, "desc0"))
		               .ReplaceStr("<d1>", WaveBufferId(tag, "desc1"))
		               .ReplaceStr("<d2>", WaveBufferId(tag, "desc2"))
		               .ReplaceStr("<shift16>", shift16)
		               .ReplaceStr("<mask_14>", mask_14)
		               .ReplaceStr("<mask_16>", mask_16)
		               .ReplaceStr("<one>", one_uint)
		               .ReplaceStr("<size>", size)
		               .ReplaceStr("<zero_u>", zero_uint)
		               .ReplaceStr("<merge>", WaveBufferId(tag, "merge"))
		               .ReplaceStr("<then>", WaveBufferId(tag, "then"))
		               .ReplaceStr("<oob>", WaveBufferId(tag, "oob"));
		const auto words = WaveBufferId(tag, "g");
		if (!EmitGuestLoad(WaveBufferId(tag, "lo"), WaveBufferId(tag, "hi"), static_cast<int>(dwords), words, source))
		{
			return false;
		}
		for (uint32_t component = 0; component < dwords; ++component)
		{
			const auto dst = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			*source += String8(R"(
%<val> = OpBitcast %float %<word>
%<sel> = OpSelect %float %<exec> %<val> %<old>
               OpStore %<dst> %<sel>
)")
			               .ReplaceStr("<val>", WaveBufferId(tag, "val", component))
			               .ReplaceStr("<word>", String8::FromPrintf("%s_d%u", words.c_str(), component))
			               .ReplaceStr("<sel>", WaveBufferId(tag, "sel", component))
			               .ReplaceStr("<exec>", WaveBufferId(tag, "exec"))
			               .ReplaceStr("<old>", WaveBufferId(tag, "old", component))
			               .ReplaceStr("<dst>", dst.value);
		}
		return true;
	};

	const auto storage_access = [&](ShaderWaveBank bank, const String8& tag, String8* source)
	{
		*source += String8(R"(
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
		               .ReplaceStr("<addr>", WaveBufferId(tag, "addr"))
		               .ReplaceStr("<empty>", WaveBufferId(tag, "empty"))
		               .ReplaceStr("<d2>", WaveBufferId(tag, "desc2"))
		               .ReplaceStr("<zero_u>", zero_uint)
		               .ReplaceStr("<access>", WaveBufferId(tag, "access"))
		               .ReplaceStr("<addr_i>", WaveBufferId(tag, "addr_i"))
		               .ReplaceStr("<slot_i>", WaveBufferId(tag, "slot_i"))
		               .ReplaceStr("<d0>", WaveBufferId(tag, "desc0"))
		               .ReplaceStr("<zero_i>", zero_int)
		               .ReplaceStr("<merge>", WaveBufferId(tag, "merge"))
		               .ReplaceStr("<then>", WaveBufferId(tag, "then"))
		               .ReplaceStr("<oob>", WaveBufferId(tag, "oob"));

		for (uint32_t component = 0; component < dwords; ++component)
		{
			if (component != 0)
			{
				const auto component_offset = GetConstantUint(component * 4u);
				if (component_offset == "unknown_uint_constant")
				{
					return false;
				}
				*source += String8(R"(
%<off> = OpIAdd %uint %<addr> %<component_offset>
%<off_i> = OpBitcast %int %<off>
               OpStore %temp_int_2 %<off_i>
)")
				               .ReplaceStr("<off>", WaveBufferId(tag, "off", component))
				               .ReplaceStr("<addr>", WaveBufferId(tag, "addr"))
				               .ReplaceStr("<component_offset>", component_offset)
				               .ReplaceStr("<off_i>", WaveBufferId(tag, "off_i", component));
			}
			const auto dst = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			*source += String8(R"(
%<call> = OpFunctionCall %void %buffer_load_float1 %temp_float %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
%<val> = OpLoad %float %temp_float
%<sel> = OpSelect %float %<exec> %<val> %<old>
               OpStore %<dst> %<sel>
)")
			               .ReplaceStr("<call>", WaveBufferId(tag, "call", component))
			               .ReplaceStr("<val>", WaveBufferId(tag, "val", component))
			               .ReplaceStr("<sel>", WaveBufferId(tag, "sel", component))
			               .ReplaceStr("<exec>", WaveBufferId(tag, "exec"))
			               .ReplaceStr("<old>", WaveBufferId(tag, "old", component))
			               .ReplaceStr("<dst>", dst.value);
		}
		return true;
	};

	for (const ShaderWaveBank bank: {ShaderWaveBank::Low, ShaderWaveBank::High})
	{
		const auto tag = String8::FromPrintf("%u_%s", index, WaveBankName(bank));
		const auto id  = [&tag](const char* name) { return WaveBufferId(tag, name); };

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
			source += String8::FromPrintf("%%%s = OpLoad %%float %%%s\n", WaveBufferId(tag, "old", component).c_str(), old.value.c_str());
		}

		ShaderOperand exec {};
		exec.type = ShaderOperandType::ExecLo;
		exec.size = 2;
		if (!EmitComputeWaveMaskBit(exec, bank, id("exec"), &source))
		{
			return false;
		}

		source += String8("%<addr> = OpFunctionCall %uint %buffer_raw_address %<index> %<imm> %<soffset> %<d1> %<d3>\n")
		              .ReplaceStr("<addr>", id("addr"))
		              .ReplaceStr("<index>", id("index"))
		              .ReplaceStr("<imm>", immediate)
		              .ReplaceStr("<soffset>", id("soffset"))
		              .ReplaceStr("<d1>", id("desc1"))
		              .ReplaceStr("<d3>", id("desc3"));
		if (!(guest ? guest_access(bank, tag, &source) : storage_access(bank, tag, &source)))
		{
			return false;
		}

		source += String8::FromPrintf("               OpBranch %%%s\n%%%s = OpLabel\n", id("merge").c_str(), id("oob").c_str());
		for (uint32_t component = 0; component < dwords; ++component)
		{
			const auto dst = GetComputeWaveRegister(instruction.dst, bank, static_cast<int>(component));
			source += String8(R"(
%<zsel> = OpSelect %float %<exec> %<zero_f> %<old>
               OpStore %<dst> %<zsel>
)")
			              .ReplaceStr("<zsel>", WaveBufferId(tag, "zsel", component))
			              .ReplaceStr("<exec>", id("exec"))
			              .ReplaceStr("<zero_f>", zero_float)
			              .ReplaceStr("<old>", WaveBufferId(tag, "old", component))
			              .ReplaceStr("<dst>", dst.value);
		}
		source += String8::FromPrintf("               OpBranch %%%s\n%%%s = OpLabel\n", id("merge").c_str(), id("merge").c_str());

		*output += source;
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
