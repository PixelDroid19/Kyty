#include "Emulator/Graphics/ShaderBufferFormat.h"

#include "Emulator/Config.h"

#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr uint32_t kFloatOneBits = 0x3f800000u;

// Writes the Gen5 BUFFER_LOAD_FORMAT and BUFFER_STORE_FORMAT functions. With a registrar it also
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
	String8 Encode();
	String8 StoreWords();

private:
	String8 Channel(const ShaderBufferFormat& format, uint32_t channel, String8* source);
	String8 Pack(const ShaderBufferFormat& format, uint32_t channel, String8* source);

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

// Converts channel `channel` of the store data in %gbfe_v0..v3 to the
// format's bits and names it %gbfe_<format>_<channel>, already shifted to its
// place in the element dword.
String8 FormatWriter::Pack(const ShaderBufferFormat& format, uint32_t channel, String8* source)
{
	const auto     name   = String8::FromPrintf("gbfe_%u_%u", format.format, channel);
	const auto     n      = name.c_str();
	const auto     layout = format.layout[channel];
	const uint32_t shift  = layout.offset % 32u;
	const uint32_t bits   = layout.bits;

	// The table's 32-bit channels are integer or float bits, stored as given.
	if (bits == 32u)
	{
		*source += String8::FromPrintf("%%%s = OpCopyObject %%uint %%gbfe_v%u\n", n, channel);
		return name;
	}
	const auto unsigned_max = Float(static_cast<float>((1ull << bits) - 1u));
	const auto signed_max   = Float(static_cast<float>((1ull << (bits - 1u)) - 1u));
	*source += String8::FromPrintf("%%%s_f = OpBitcast %%float %%gbfe_v%u\n", n, channel);
	switch (format.number)
	{
		case ShaderBufferNumber::Unorm:
			*source += String8::FromPrintf("%%%s_c = OpExtInst %%float %%GLSL_std_450 FClamp %%%s_f %s %s\n"
			                               "%%%s_s = OpFMul %%float %%%s_c %s\n%%%s_r = OpExtInst %%float %%GLSL_std_450 RoundEven %%%s_s\n"
			                               "%%%s_u = OpConvertFToU %%uint %%%s_r\n",
			                               n, n, Float(0.0f).c_str(), Float(1.0f).c_str(), n, n, unsigned_max.c_str(), n, n, n, n);
			break;
		case ShaderBufferNumber::Snorm:
			*source += String8::FromPrintf("%%%s_c = OpExtInst %%float %%GLSL_std_450 FClamp %%%s_f %s %s\n"
			                               "%%%s_s = OpFMul %%float %%%s_c %s\n%%%s_r = OpExtInst %%float %%GLSL_std_450 RoundEven %%%s_s\n"
			                               "%%%s_i = OpConvertFToS %%int %%%s_r\n%%%s_u = OpBitcast %%uint %%%s_i\n",
			                               n, n, Float(-1.0f).c_str(), Float(1.0f).c_str(), n, n, signed_max.c_str(), n, n, n, n, n, n);
			break;
		case ShaderBufferNumber::Uscaled:
			*source += String8::FromPrintf("%%%s_c = OpExtInst %%float %%GLSL_std_450 FClamp %%%s_f %s %s\n"
			                               "%%%s_u = OpConvertFToU %%uint %%%s_c\n",
			                               n, n, Float(0.0f).c_str(), unsigned_max.c_str(), n, n);
			break;
		case ShaderBufferNumber::Sscaled:
			*source += String8::FromPrintf("%%%s_c = OpExtInst %%float %%GLSL_std_450 FClamp %%%s_f %s %s\n"
			                               "%%%s_i = OpConvertFToS %%int %%%s_c\n%%%s_u = OpBitcast %%uint %%%s_i\n",
			                               n, n, Float(-static_cast<float>(1ull << (bits - 1u))).c_str(), signed_max.c_str(), n, n,
			                               n, n);
			break;
		case ShaderBufferNumber::Uint:
		case ShaderBufferNumber::Sint:
			// The low bits, as BUFFER_STORE_BYTE and SHORT store with their
			// derived 8- and 16-bit integer formats.
			*source += String8::FromPrintf("%%%s_u = OpCopyObject %%uint %%gbfe_v%u\n", n, channel);
			break;
		case ShaderBufferNumber::Float:
		{
			// The inverse of the load: 11- and 10-bit floats are halves without
			// sign and low mantissa bits, so negatives store zero and the
			// mantissa is truncated.
			const uint32_t to_half = bits == 16u ? 0u : 15u - bits;
			const auto     input   = String8::FromPrintf("%%%s_%s", n, to_half == 0u ? "f" : "p");
			if (to_half != 0u)
			{
				*source += String8::FromPrintf("%%%s_p = OpExtInst %%float %%GLSL_std_450 FMax %%%s_f %s\n", n, n, Float(0.0f).c_str());
			}
			*source += String8::FromPrintf("%%%s_v = OpCompositeConstruct %%v2float %s %s\n"
			                               "%%%s_h = OpExtInst %%uint %%GLSL_std_450 PackHalf2x16 %%%s_v\n"
			                               "%%%s_u = OpShiftRightLogical %%uint %%%s_h %s\n",
			                               n, input.c_str(), Float(0.0f).c_str(), n, n, n, n, Uint(to_half).c_str());
			break;
		}
	}
	*source += String8::FromPrintf("%%%s_m = OpBitwiseAnd %%uint %%%s_u %s\n%%%s = OpShiftLeftLogical %%uint %%%s_m %s\n", n, n,
	                               Uint(static_cast<uint32_t>((1ull << bits) - 1u)).c_str(), n, n, Uint(shift).c_str());
	return name;
}

// v4uint gen5_buffer_format_encode(v4uint values, uint format, uint desc3):
// the element dwords BUFFER_STORE_FORMAT writes for the store data. RDNA2 ISA
// table 31 gives this store the resource DST_SEL: the inverse of the load
// puts data component c in memory channel DST_SEL[c] - 4, and a channel no
// select names stores zero, as do channels the instruction does not supply.
// An unknown format encodes zero and gen5_buffer_format_bytes gives it no
// size.
String8 FormatWriter::Encode()
{
	uint32_t   count   = 0;
	const auto formats = ShaderGen5BufferFormats(&count);
	const auto zero    = Uint(0);
	String8    source  = R"(
%gen5_buffer_format_encode = OpFunction %v4uint None %function_gen5_buffer_element
%gbfe_values = OpFunctionParameter %v4uint
%gbfe_format = OpFunctionParameter %uint
%gbfe_desc3 = OpFunctionParameter %uint
%gbfe_entry = OpLabel
)";
	for (uint32_t c = 0; c < 4; c++)
	{
		source += String8::FromPrintf("%%gbfe_r%u = OpCompositeExtract %%uint %%gbfe_values %u\n"
		                              "%%gbfe_s%u_r = OpShiftRightLogical %%uint %%gbfe_desc3 %s\n%%gbfe_s%u = OpBitwiseAnd %%uint %%gbfe_s%u_r %s\n",
		                              c, c, c, Uint(3u * c).c_str(), c, c, Uint(7).c_str());
	}
	for (uint32_t m = 0; m < 4; m++)
	{
		String8 previous = zero;
		for (uint32_t c = 0; c < 4; c++)
		{
			source += String8::FromPrintf("%%gbfe_t%u_%u = OpIEqual %%bool %%gbfe_s%u %s\n"
			                              "%%gbfe_p%u_%u = OpSelect %%uint %%gbfe_t%u_%u %%gbfe_r%u %s\n",
			                              m, c, c, Uint(4u + m).c_str(), m, c, m, c, c, previous.c_str());
			previous = String8::FromPrintf("%%gbfe_p%u_%u", m, c);
		}
		source += String8::FromPrintf("%%gbfe_v%u = OpCopyObject %%uint %s\n", m, previous.c_str());
	}
	source += R"(OpSelectionMerge %gbfe_merge None
OpSwitch %gbfe_format %gbfe_unknown)";
	for (uint32_t i = 0; i < count; i++)
	{
		source += String8::FromPrintf(" %u %%gbfe_f%u", formats[i].format, formats[i].format);
	}
	source += "\n";
	String8 phis[4];
	for (uint32_t i = 0; i < count; i++)
	{
		const auto& format = formats[i];
		source += String8::FromPrintf("%%gbfe_f%u = OpLabel\n", format.format);
		String8 words[4] = {zero, zero, zero, zero};
		for (uint32_t c = 0; c < format.channels; c++)
		{
			const uint32_t word   = format.layout[c].offset / 32u;
			const auto     packed = "%" + Pack(format, c, &source);
			if (words[word] == zero)
			{
				words[word] = packed;
				continue;
			}
			const auto merged = String8::FromPrintf("%%gbfe_%u_w%u_%u", format.format, word, c);
			source += String8::FromPrintf("%s = OpBitwiseOr %%uint %s %s\n", merged.c_str(), words[word].c_str(), packed.c_str());
			words[word] = merged;
		}
		for (uint32_t k = 0; k < 4; k++)
		{
			phis[k] += String8::FromPrintf(" %s %%gbfe_f%u", words[k].c_str(), format.format);
		}
		source += "OpBranch %gbfe_merge\n";
	}
	source += "%gbfe_unknown = OpLabel\nOpBranch %gbfe_merge\n%gbfe_merge = OpLabel\n";
	for (uint32_t k = 0; k < 4; k++)
	{
		source += String8::FromPrintf("%%gbfe_w%u = OpPhi %%uint%s %s %%gbfe_unknown\n", k, phis[k].c_str(), zero.c_str());
	}
	source += "%gbfe_result = OpCompositeConstruct %v4uint %gbfe_w0 %gbfe_w1 %gbfe_w2 %gbfe_w3\n"
	          "OpReturnValue %gbfe_result\nOpFunctionEnd\n";
	return source;
}

// void gen5_buffer_store_words(uint buffer, uint address, uint bytes, v4uint
// words): writes the `bytes`-byte element at a byte address of a bound
// storage buffer. Dwords it covers whole are stored; a dword it covers in part
// is merged with atomics so neighbouring bytes other lanes write survive.
String8 FormatWriter::StoreWords()
{
	String8 source = String8::FromPrintf(R"(
%%gen5_buffer_store_words = OpFunction %%void None %%function_gen5_buffer_store_words
%%gbsw_buffer = OpFunctionParameter %%uint
%%gbsw_address = OpFunctionParameter %%uint
%%gbsw_bytes = OpFunctionParameter %%uint
%%gbsw_words = OpFunctionParameter %%v4uint
%%gbsw_entry = OpLabel
%%gbsw_word = OpShiftRightLogical %%uint %%gbsw_address %s
%%gbsw_lead = OpBitwiseAnd %%uint %%gbsw_address %s
%%gbsw_shift = OpShiftLeftLogical %%uint %%gbsw_lead %s
%%gbsw_aligned = OpIEqual %%bool %%gbsw_shift %s
%%gbsw_rest = OpISub %%uint %s %%gbsw_shift
)",
	                                     Uint(2).c_str(), Uint(3).c_str(), Uint(3).c_str(), Uint(0).c_str(), Uint(32).c_str());
	const auto all  = Uint(0xffffffffu);
	const auto zero = Uint(0);
	// The element's dwords and the byte mask of each.
	for (uint32_t k = 0; k < 4; k++)
	{
		source += String8::FromPrintf(R"(%%gbsw_e%u = OpCompositeExtract %%uint %%gbsw_words %u
%%gbsw_has%u = OpUGreaterThan %%bool %%gbsw_bytes %s
%%gbsw_left%u = OpISub %%uint %%gbsw_bytes %s
%%gbsw_cov%u = OpExtInst %%uint %%GLSL_std_450 UMin %%gbsw_left%u %s
%%gbsw_bits%u = OpShiftLeftLogical %%uint %%gbsw_cov%u %s
%%gbsw_low%u = OpShiftLeftLogical %%uint %s %%gbsw_bits%u
%%gbsw_part%u = OpISub %%uint %%gbsw_low%u %s
%%gbsw_whole%u = OpIEqual %%bool %%gbsw_cov%u %s
%%gbsw_cm%u = OpSelect %%uint %%gbsw_whole%u %s %%gbsw_part%u
%%gbsw_em%u = OpSelect %%uint %%gbsw_has%u %%gbsw_cm%u %s
)",
		                              k, k, k, Uint(4u * k).c_str(), k, Uint(4u * k).c_str(), k, k, Uint(4).c_str(), k, k,
		                              Uint(3).c_str(), k, Uint(1).c_str(), k, k, k, Uint(1).c_str(), k, k, Uint(4).c_str(), k, k,
		                              all.c_str(), k, k, k, k, zero.c_str());
	}
	// Shifted by the lead the element spans up to five dwords. A shift by 32
	// is undefined; the aligned case selects the dwords as given.
	for (uint32_t k = 0; k < 5; k++)
	{
		for (const char* part: {"e", "em"})
		{
			String8 combined;
			if (k < 4)
			{
				source += String8::FromPrintf("%%gbsw_%s_lo%u = OpShiftLeftLogical %%uint %%gbsw_%s%u %%gbsw_shift\n", part, k, part, k);
				combined = String8::FromPrintf("%%gbsw_%s_lo%u", part, k);
			}
			if (k > 0)
			{
				source += String8::FromPrintf("%%gbsw_%s_hi%u = OpShiftRightLogical %%uint %%gbsw_%s%u %%gbsw_rest\n", part, k, part, k - 1);
				const auto hi = String8::FromPrintf("%%gbsw_%s_hi%u", part, k);
				if (k < 4)
				{
					source += String8::FromPrintf("%%gbsw_%s_or%u = OpBitwiseOr %%uint %s %s\n", part, k, combined.c_str(), hi.c_str());
					combined = String8::FromPrintf("%%gbsw_%s_or%u", part, k);
				} else
				{
					combined = hi;
				}
			}
			const auto as_given = k < 4 ? String8::FromPrintf("%%gbsw_%s%u", part, k) : zero;
			source += String8::FromPrintf("%%gbsw_%s_s%u = OpSelect %%uint %%gbsw_aligned %s %s\n", part, k, as_given.c_str(),
			                              combined.c_str());
		}
	}
	const auto scope   = Uint(1); // Device
	const auto relaxed = Uint(0);
	for (uint32_t k = 0; k < 5; k++)
	{
		source += String8(R"(%gbsw_a<k> = OpIAdd %uint %gbsw_word <offset>
%gbsw_full<k> = OpIEqual %bool %gbsw_em_s<k> <all>
%gbsw_any<k> = OpINotEqual %bool %gbsw_em_s<k> <zero>
OpSelectionMerge %gbsw_m<k> None
OpBranchConditional %gbsw_full<k> %gbsw_store<k> %gbsw_check<k>
%gbsw_store<k> = OpLabel
%gbsw_fp<k> = OpAccessChain %_ptr_StorageBuffer_float %buf %gbsw_buffer %int_0 %gbsw_a<k>
%gbsw_fv<k> = OpBitcast %float %gbsw_e_s<k>
OpStore %gbsw_fp<k> %gbsw_fv<k>
OpBranch %gbsw_m<k>
%gbsw_check<k> = OpLabel
OpSelectionMerge %gbsw_pm<k> None
OpBranchConditional %gbsw_any<k> %gbsw_merge<k> %gbsw_pm<k>
%gbsw_merge<k> = OpLabel
%gbsw_up<k> = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint %gbsw_buffer %int_0 %gbsw_a<k>
%gbsw_keep<k> = OpNot %uint %gbsw_em_s<k>
%gbsw_new<k> = OpBitwiseAnd %uint %gbsw_e_s<k> %gbsw_em_s<k>
%gbsw_and<k> = OpAtomicAnd %uint %gbsw_up<k> <scope> <relaxed> %gbsw_keep<k>
%gbsw_or<k> = OpAtomicOr %uint %gbsw_up<k> <scope> <relaxed> %gbsw_new<k>
OpBranch %gbsw_pm<k>
%gbsw_pm<k> = OpLabel
OpBranch %gbsw_m<k>
%gbsw_m<k> = OpLabel
)")
		              .ReplaceStr("<k>", String8::FromPrintf("%u", k))
		              .ReplaceStr("<offset>", Uint(k))
		              .ReplaceStr("<all>", all)
		              .ReplaceStr("<zero>", zero)
		              .ReplaceStr("<scope>", scope)
		              .ReplaceStr("<relaxed>", relaxed);
	}
	source += "OpReturn\nOpFunctionEnd\n";
	return source;
}

} // namespace

static bool uses_gen5_format_stores(const ShaderCode& code)
{
	return code.HasAnyOf(
	    {ShaderInstructionType::BufferStoreFormatX, ShaderInstructionType::BufferStoreFormatXy, ShaderInstructionType::BufferStoreFormatXyzw});
}

bool SpirvUsesGen5BufferFormats(const ShaderCode& code)
{
	return Config::IsNextGen() &&
	       (uses_gen5_format_stores(code) ||
	        code.HasAnyOf({ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferLoadFormatXy,
	                       ShaderInstructionType::BufferLoadFormatXyz, ShaderInstructionType::BufferLoadFormatXyzw}));
}

bool SpirvUsesGen5BufferFormatStores(const ShaderCode& code)
{
	return Config::IsNextGen() && uses_gen5_format_stores(code);
}

void SpirvAddGen5BufferFormatConstants(Spirv* spirv)
{
	FormatWriter writer(*spirv, spirv);
	(void)writer.Bytes();
	(void)writer.Align();
	(void)writer.LoadWords();
	(void)writer.Decode();
	(void)writer.Encode();
	(void)writer.StoreWords();
}

String8 SpirvGen5BufferFormatFunctions(const Spirv& spirv, const ShaderCode& code, bool bound_buffers)
{
	FormatWriter writer(spirv, nullptr);
	String8      source = writer.Bytes() + writer.Align() + writer.Decode();
	if (bound_buffers)
	{
		source += writer.LoadWords();
	}
	// Stores reach only bound buffers; their partial dwords need %buf_uint.
	if (bound_buffers && SpirvUsesGen5BufferFormatStores(code))
	{
		source += writer.Encode() + writer.StoreWords();
	}
	return source;
}

} // namespace Kyty::Libs::Graphics

#endif
