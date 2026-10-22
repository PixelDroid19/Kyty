#include "Emulator/Graphics/ShaderBufferFormat.h"

#include "Emulator/Config.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kFloatOneBits = 0x3f800000u;

// Writes the Gen5 BUFFER_LOAD_FORMAT functions. With a registrar it also
// registers every constant it names, so FindConstants and WriteFunctions
// share one list.
class FormatWriter
{
public:
	FormatWriter(const Spirv& spirv, Spirv* registrar): m_spirv(spirv), m_registrar(registrar) {}

	String8 Uint(uint32_t value)
	{
		if (m_registrar != nullptr)
		{
			m_registrar->AddConstantUint(value);
		}
		return "%" + m_spirv.GetConstantUint(value);
	}

	String8 Float(float value)
	{
		if (m_registrar != nullptr)
		{
			m_registrar->AddConstantFloat(value);
		}
		return "%" + m_spirv.GetConstantFloat(value);
	}

	String8 Bytes();
	String8 Align();
	String8 LoadWords();
	String8 Decode();

private:
	String8 Channel(const ShaderBufferFormat& format, uint32_t channel, String8* source);

	const Spirv& m_spirv;
	Spirv*       m_registrar;
};

// uint gen5_buffer_format_bytes(uint format): the element size, 0 for a
// format that does not decode.
String8 FormatWriter::Bytes()
{
	uint32_t   count   = 0;
	const auto formats = ShaderGen5BufferFormats(&count);
	String8    cases;
	String8    phi;
	for (uint32_t size: {1u, 2u, 4u, 8u, 12u, 16u})
	{
		cases += String8::FromPrintf("%%gbfb_b%u = OpLabel\nOpBranch %%gbfb_merge\n", size);
		phi += String8::FromPrintf(" %s %%gbfb_b%u", Uint(size).c_str(), size);
	}
	String8 targets;
	for (uint32_t i = 0; i < count; i++)
	{
		targets += String8::FromPrintf(" %u %%gbfb_b%u", formats[i].format, formats[i].bytes);
	}
	return String8::FromPrintf(R"(
%%gen5_buffer_format_bytes = OpFunction %%uint None %%function_gen5_buffer_format_bytes
%%gbfb_format = OpFunctionParameter %%uint
%%gbfb_entry = OpLabel
OpSelectionMerge %%gbfb_merge None
OpSwitch %%gbfb_format %%gbfb_merge%s
%s%%gbfb_merge = OpLabel
%%gbfb_bytes = OpPhi %%uint%s %s %%gbfb_entry
OpReturnValue %%gbfb_bytes
OpFunctionEnd
)",
	                           targets.c_str(), cases.c_str(), phi.c_str(), Uint(0).c_str());
}

// v4uint gen5_buffer_align_words(v4uint words, uint next, uint lead): the four
// dwords starting `lead` bytes into `words`, `next` following them.
String8 FormatWriter::Align()
{
	String8 source = String8::FromPrintf(R"(
%%gen5_buffer_align_words = OpFunction %%v4uint None %%function_gen5_buffer_element
%%gbaw_words = OpFunctionParameter %%v4uint
%%gbaw_w4 = OpFunctionParameter %%uint
%%gbaw_lead = OpFunctionParameter %%uint
%%gbaw_entry = OpLabel
%%gbaw_shift = OpShiftLeftLogical %%uint %%gbaw_lead %s
%%gbaw_aligned = OpIEqual %%bool %%gbaw_shift %s
%%gbaw_rest = OpISub %%uint %s %%gbaw_shift
)",
	                                     Uint(3).c_str(), Uint(0).c_str(), Uint(32).c_str());
	for (uint32_t k = 0; k < 4; k++)
	{
		source += String8::FromPrintf("%%gbaw_w%u = OpCompositeExtract %%uint %%gbaw_words %u\n", k, k);
	}
	// A shift by 32 is undefined; the aligned case selects the dword as read.
	for (uint32_t k = 0; k < 4; k++)
	{
		source += String8::FromPrintf(R"(%%gbaw_lo%u = OpShiftRightLogical %%uint %%gbaw_w%u %%gbaw_shift
%%gbaw_hi%u = OpShiftLeftLogical %%uint %%gbaw_w%u %%gbaw_rest
%%gbaw_or%u = OpBitwiseOr %%uint %%gbaw_lo%u %%gbaw_hi%u
%%gbaw_e%u = OpSelect %%uint %%gbaw_aligned %%gbaw_w%u %%gbaw_or%u
)",
		                              k, k, k, k + 1, k, k, k, k, k, k);
	}
	source += "%gbaw_result = OpCompositeConstruct %v4uint %gbaw_e0 %gbaw_e1 %gbaw_e2 %gbaw_e3\n"
	          "OpReturnValue %gbaw_result\nOpFunctionEnd\n";
	return source;
}

// v4uint gen5_buffer_load_words(uint buffer, uint address, uint bytes): the
// element's dwords from a bound storage buffer, reading only the dwords the
// element covers.
String8 FormatWriter::LoadWords()
{
	String8 source = String8::FromPrintf(R"(
%%gen5_buffer_load_words = OpFunction %%v4uint None %%function_gen5_buffer_load_words
%%gblw_buffer = OpFunctionParameter %%uint
%%gblw_address = OpFunctionParameter %%uint
%%gblw_bytes = OpFunctionParameter %%uint
%%gblw_entry = OpLabel
%%gblw_word = OpShiftRightLogical %%uint %%gblw_address %s
%%gblw_lead = OpBitwiseAnd %%uint %%gblw_address %s
%%gblw_span = OpIAdd %%uint %%gblw_lead %%gblw_bytes
%%gblw_span_up = OpIAdd %%uint %%gblw_span %s
%%gblw_count = OpShiftRightLogical %%uint %%gblw_span_up %s
%%gblw_p0 = OpAccessChain %%_ptr_StorageBuffer_float %%buf %%gblw_buffer %%int_0 %%gblw_word
%%gblw_f0 = OpLoad %%float %%gblw_p0
%%gblw_w0 = OpBitcast %%uint %%gblw_f0
)",
	                                     Uint(2).c_str(), Uint(3).c_str(), Uint(3).c_str(), Uint(2).c_str());
	String8 previous = "%gblw_entry";
	for (uint32_t k = 1; k <= 4; k++)
	{
		source += String8::FromPrintf(R"(%%gblw_c%u = OpULessThan %%bool %s %%gblw_count
OpSelectionMerge %%gblw_m%u None
OpBranchConditional %%gblw_c%u %%gblw_l%u %%gblw_m%u
%%gblw_l%u = OpLabel
%%gblw_a%u = OpIAdd %%uint %%gblw_word %s
%%gblw_p%u = OpAccessChain %%_ptr_StorageBuffer_float %%buf %%gblw_buffer %%int_0 %%gblw_a%u
%%gblw_f%u = OpLoad %%float %%gblw_p%u
%%gblw_v%u = OpBitcast %%uint %%gblw_f%u
OpBranch %%gblw_m%u
%%gblw_m%u = OpLabel
%%gblw_w%u = OpPhi %%uint %%gblw_v%u %%gblw_l%u %s %s
)",
		                              k, Uint(k).c_str(), k, k, k, k, k, k, Uint(k).c_str(), k, k, k, k, k, k, k, k, k, k, k,
		                              Uint(0).c_str(), previous.c_str());
		previous = String8::FromPrintf("%%gblw_m%u", k);
	}
	source += "%gblw_words = OpCompositeConstruct %v4uint %gblw_w0 %gblw_w1 %gblw_w2 %gblw_w3\n"
	          "%gblw_result = OpFunctionCall %v4uint %gen5_buffer_align_words %gblw_words %gblw_w4 %gblw_lead\n"
	          "OpReturnValue %gblw_result\nOpFunctionEnd\n";
	return source;
}

// Converts one channel of the element in %gbfd_e0..e3 to the 32-bit value
// BUFFER_LOAD_FORMAT returns and names it %gbfd_<format>_<channel>.
String8 FormatWriter::Channel(const ShaderBufferFormat& format, uint32_t channel, String8* source)
{
	const auto     name   = String8::FromPrintf("gbfd_%u_%u", format.format, channel);
	const auto     n      = name.c_str();
	const auto     layout = format.layout[channel];
	const uint32_t word   = layout.offset / 32u;
	const uint32_t shift  = layout.offset % 32u;
	const uint32_t bits   = layout.bits;
	const auto     number = format.number;
	const bool     is_signed = number == ShaderBufferNumber::Snorm || number == ShaderBufferNumber::Sscaled ||
	                       number == ShaderBufferNumber::Sint;

	if (bits == 32u && (number == ShaderBufferNumber::Uint || number == ShaderBufferNumber::Sint ||
	                    number == ShaderBufferNumber::Float))
	{
		*source += String8::FromPrintf("%%%s = OpCopyObject %%uint %%gbfd_e%u\n", n, word);
		return name;
	}
	if (is_signed)
	{
		*source += String8::FromPrintf("%%%s_i = OpBitcast %%int %%gbfd_e%u\n%%%s_s = OpBitFieldSExtract %%int %%%s_i %s %s\n", n, word, n,
		                               n, Uint(shift).c_str(), Uint(bits).c_str());
	} else
	{
		*source += String8::FromPrintf("%%%s_u = OpBitFieldUExtract %%uint %%gbfd_e%u %s %s\n", n, word, Uint(shift).c_str(),
		                               Uint(bits).c_str());
	}
	switch (number)
	{
		case ShaderBufferNumber::Unorm:
			*source += String8::FromPrintf("%%%s_f = OpConvertUToF %%float %%%s_u\n%%%s_n = OpFDiv %%float %%%s_f %s\n", n, n, n, n,
			                               Float(static_cast<float>((1ull << bits) - 1u)).c_str());
			break;
		case ShaderBufferNumber::Snorm:
			// The most negative code clamps to -1.
			*source += String8::FromPrintf("%%%s_f = OpConvertSToF %%float %%%s_s\n%%%s_d = OpFDiv %%float %%%s_f %s\n"
			                               "%%%s_n = OpExtInst %%float %%GLSL_std_450 FMax %%%s_d %s\n",
			                               n, n, n, n, Float(static_cast<float>((1ull << (bits - 1u)) - 1u)).c_str(), n, n,
			                               Float(-1.0f).c_str());
			break;
		case ShaderBufferNumber::Uscaled: *source += String8::FromPrintf("%%%s_n = OpConvertUToF %%float %%%s_u\n", n, n); break;
		case ShaderBufferNumber::Sscaled: *source += String8::FromPrintf("%%%s_n = OpConvertSToF %%float %%%s_s\n", n, n); break;
		case ShaderBufferNumber::Uint: *source += String8::FromPrintf("%%%s = OpCopyObject %%uint %%%s_u\n", n, n); return name;
		case ShaderBufferNumber::Sint: *source += String8::FromPrintf("%%%s = OpBitcast %%uint %%%s_s\n", n, n); return name;
		case ShaderBufferNumber::Float:
		{
			// 16-, 11- and 10-bit floats share the half-float exponent: the
			// narrow ones are halves without sign and low mantissa bits.
			const uint32_t to_half = bits == 16u ? 0u : 15u - bits;
			*source += String8::FromPrintf("%%%s_h = OpShiftLeftLogical %%uint %%%s_u %s\n"
			                               "%%%s_v = OpExtInst %%v2float %%GLSL_std_450 UnpackHalf2x16 %%%s_h\n"
			                               "%%%s_n = OpCompositeExtract %%float %%%s_v 0\n",
			                               n, n, Uint(to_half).c_str(), n, n, n, n);
			break;
		}
	}
	*source += String8::FromPrintf("%%%s = OpBitcast %%uint %%%s_n\n", n, n);
	return name;
}

// v4uint gen5_buffer_format_decode(v4uint element, uint format, uint desc3):
// converts the element's channels and applies the V# DST_SEL (desc3[11:0]):
// 0 reads 0, 1 reads one (integer 1 for integer formats), 4..7 read X..W, and
// a channel the format lacks reads 0. An all-zero element decodes to zero in
// every format, which is what an out-of-range read returns.
String8 FormatWriter::Decode()
{
	uint32_t   count   = 0;
	const auto formats = ShaderGen5BufferFormats(&count);
	String8    source  = R"(
%gen5_buffer_format_decode = OpFunction %v4uint None %function_gen5_buffer_element
%gbfd_element = OpFunctionParameter %v4uint
%gbfd_format = OpFunctionParameter %uint
%gbfd_desc3 = OpFunctionParameter %uint
%gbfd_entry = OpLabel
%gbfd_e0 = OpCompositeExtract %uint %gbfd_element 0
%gbfd_e1 = OpCompositeExtract %uint %gbfd_element 1
%gbfd_e2 = OpCompositeExtract %uint %gbfd_element 2
%gbfd_e3 = OpCompositeExtract %uint %gbfd_element 3
OpSelectionMerge %gbfd_merge None
OpSwitch %gbfd_format %gbfd_unknown)";
	for (uint32_t i = 0; i < count; i++)
	{
		source += String8::FromPrintf(" %u %%gbfd_f%u", formats[i].format, formats[i].format);
	}
	source += "\n";
	const auto zero = Uint(0);
	String8    phis[5];
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& format = formats[i];
		source += String8::FromPrintf("%%gbfd_f%u = OpLabel\n", format.format);
		for (uint32_t c = 0; c < 4; c++)
		{
			const auto value = c < format.channels ? "%" + Channel(format, c, &source) : zero;
			phis[c] += String8::FromPrintf(" %s %%gbfd_f%u", value.c_str(), format.format);
		}
		const bool integer = format.number == ShaderBufferNumber::Uint || format.number == ShaderBufferNumber::Sint;
		phis[4] += String8::FromPrintf(" %s %%gbfd_f%u", Uint(integer ? 1u : kFloatOneBits).c_str(), format.format);
		source += "OpBranch %gbfd_merge\n";
	}
	source += "%gbfd_unknown = OpLabel\nOpBranch %gbfd_merge\n%gbfd_merge = OpLabel\n";
	const char* names[5] = {"x", "y", "z", "w", "one"};
	for (uint32_t c = 0; c < 5; c++)
	{
		const auto unknown = c < 4 ? zero : Uint(kFloatOneBits);
		source += String8::FromPrintf("%%gbfd_%s = OpPhi %%uint%s %s %%gbfd_unknown\n", names[c], phis[c].c_str(), unknown.c_str());
	}
	for (uint32_t c = 0; c < 4; c++)
	{
		source += String8::FromPrintf(R"(%%gbfd_s%u_r = OpShiftRightLogical %%uint %%gbfd_desc3 %s
%%gbfd_s%u = OpBitwiseAnd %%uint %%gbfd_s%u_r %s
%%gbfd_s%u_1 = OpIEqual %%bool %%gbfd_s%u %s
%%gbfd_v%u_0 = OpSelect %%uint %%gbfd_s%u_1 %%gbfd_one %s
)",
		                              c, Uint(3u * c).c_str(), c, c, Uint(7).c_str(), c, c, Uint(1).c_str(), c, c, zero.c_str());
		for (uint32_t k = 0; k < 4; k++)
		{
			source += String8::FromPrintf("%%gbfd_s%u_%u = OpIEqual %%bool %%gbfd_s%u %s\n"
			                              "%%gbfd_v%u_%u = OpSelect %%uint %%gbfd_s%u_%u %%gbfd_%s %%gbfd_v%u_%u\n",
			                              c, k + 4, c, Uint(k + 4).c_str(), c, k + 1, c, k + 4, names[k], c, k);
		}
	}
	source += "%gbfd_result = OpCompositeConstruct %v4uint %gbfd_v0_4 %gbfd_v1_4 %gbfd_v2_4 %gbfd_v3_4\n"
	          "OpReturnValue %gbfd_result\nOpFunctionEnd\n";
	return source;
}

} // namespace

bool SpirvUsesGen5BufferFormatLoads(const ShaderCode& code)
{
	return Config::IsNextGen() &&
	       code.HasAnyOf({ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferLoadFormatXy,
	                      ShaderInstructionType::BufferLoadFormatXyz, ShaderInstructionType::BufferLoadFormatXyzw});
}

void SpirvAddGen5BufferFormatConstants(Spirv* spirv)
{
	FormatWriter writer(*spirv, spirv);
	(void)writer.Bytes();
	(void)writer.Align();
	(void)writer.LoadWords();
	(void)writer.Decode();
}

String8 SpirvGen5BufferFormatFunctions(const Spirv& spirv, bool bound_buffers)
{
	FormatWriter writer(spirv, nullptr);
	String8      source = writer.Bytes() + writer.Align() + writer.Decode();
	if (bound_buffers)
	{
		source += writer.LoadWords();
	}
	return source;
}

} // namespace Kyty::Libs::Graphics

#endif
