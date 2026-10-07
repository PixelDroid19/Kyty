#include "Emulator/Graphics/FragmentTransportCapture.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include <array>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

#include "host_shaders/fragment_wave_capture_asm.inc"

struct CaptureSource
{
	String8 interface;
	String8 annotations;
	String8 declarations;
	String8 constants;
	String8 cases;
	String8 blocks;
};

using InterpolationFields = std::array<uint32_t, 32>;

uint32_t InterpolationField(const ShaderPixelInputInfo& pixel, uint32_t reg)
{
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((pixel.system_input_enable & (1u << field)) != 0u && ShaderPixelSystemInputRegister(pixel, field) == reg)
		{
			return field;
		}
	}
	return UINT32_MAX;
}

bool InputsAvailable(const ShaderCode& code, const ShaderPixelInputInfo& pixel, const ShaderFragmentComputeInfo& transport,
                     InterpolationFields* fields)
{
	constexpr uint32_t supported = 0x1f66u;
	if (!pixel.custom_interpolation.Enabled() || pixel.input_num > 32u || (pixel.system_input_enable & ~supported) != 0u ||
	    (pixel.system_input_enable & ~pixel.system_input_address) != 0u)
	{
		return false;
	}
	for (uint32_t attribute = 0; attribute < pixel.input_num; ++attribute)
	{
		ShaderPixelInterpolator decoded {};
		if (ShaderPixelInputActive(pixel, attribute) && !ShaderDecodePixelInterpolator(pixel.interpolator_settings[attribute], &decoded))
		{
			return false;
		}
	}
	for (uint32_t field = 0; field < 13u; ++field)
	{
		const uint32_t width = field < 7u ? 2u : 1u;
		if ((pixel.system_input_enable & (1u << field)) != 0u &&
		    ShaderPixelSystemInputRegister(pixel, field) + width > transport.initial_vgpr_count)
		{
			return false;
		}
	}
	fields->fill(UINT32_MAX);
	for (uint32_t index = 0; index < code.GetInstructions().Size(); ++index)
	{
		const auto& instruction = code.GetInstructions().At(index);
		const bool  move        = instruction.type == ShaderInstructionType::VInterpMovF32;
		const bool  pair =
		    instruction.type == ShaderInstructionType::VInterpP1F32 || instruction.type == ShaderInstructionType::VInterpP2F32;
		if (!move && !pair)
		{
			continue;
		}
		const auto              attribute = instruction.src[1].constant.u;
		ShaderPixelInterpolator decoded {};
		if (attribute >= pixel.input_num || !ShaderPixelInputActive(pixel, attribute) ||
		    !ShaderDecodePixelInterpolator(pixel.interpolator_settings[attribute], &decoded))
		{
			return false;
		}
		if (move && decoded.source == ShaderPixelInterpolatorSource::Parameter)
		{
			return false;
		}
		if (!pair)
		{
			continue;
		}
		if (decoded.source == ShaderPixelInterpolatorSource::PerVertex || !ShaderFragmentInterpolationPairSupported(code, index, pixel))
		{
			return false;
		}
		if (instruction.type != ShaderInstructionType::VInterpP1F32 || decoded.flat)
		{
			continue;
		}
		const uint32_t field     = InterpolationField(pixel, static_cast<uint32_t>(instruction.src[0].register_id));
		const uint32_t canonical = ShaderPixelCanonicalInterpolator(pixel, attribute);
		if (field == UINT32_MAX || ((*fields)[canonical] != UINT32_MAX && (*fields)[canonical] != field))
		{
			return false;
		}
		(*fields)[canonical] = field;
	}
	return true;
}

void DeclareInputs(const ShaderPixelInputInfo& pixel, const InterpolationFields& fields, CaptureSource* source)
{
	source->declarations = "%capture_v4uint = OpTypeVector %uint 4\n"
	                       "%capture_three = OpConstant %uint 3\n"
	                       "%capture_triangle = OpTypeArray %capture_v4uint %capture_three\n"
	                       "%capture_raw_ptr = OpTypePointer Input %capture_triangle\n"
	                       "%capture_bary_ptr = OpTypePointer Input %v2float\n";
	for (uint32_t attribute = 0; attribute < pixel.input_num; ++attribute)
	{
		if (!ShaderPixelInputActive(pixel, attribute) || ShaderPixelCanonicalInterpolator(pixel, attribute) != attribute)
		{
			continue;
		}
		ShaderPixelInterpolator decoded {};
		if (!ShaderDecodePixelInterpolator(pixel.interpolator_settings[attribute], &decoded) ||
		    decoded.source == ShaderPixelInterpolatorSource::Default)
		{
			continue;
		}
		const bool raw = decoded.source == ShaderPixelInterpolatorSource::PerVertex;
		source->interface += String8::FromPrintf(" %%capture_attr%u", attribute);
		source->annotations +=
		    String8::FromPrintf("OpDecorate %%capture_attr%u Location %u\n", attribute, pixel.custom_interpolation.locations[attribute]);
		if (raw || decoded.flat)
		{
			source->annotations += String8::FromPrintf("OpDecorate %%capture_attr%u Flat\n", attribute);
		}
		const uint32_t field = fields[attribute];
		if (field == 5u || field == 6u)
		{
			source->annotations += String8::FromPrintf("OpDecorate %%capture_attr%u NoPerspective\n", attribute);
		}
		if (field == 2u || field == 6u)
		{
			source->annotations += String8::FromPrintf("OpDecorate %%capture_attr%u Centroid\n", attribute);
		}
		source->declarations +=
		    String8::FromPrintf("%%capture_attr%u = OpVariable %%%s Input\n", attribute, raw ? "capture_raw_ptr" : "_ptr_Input_v4float");
	}
	for (uint32_t field: {1u, 2u, 5u, 6u})
	{
		if ((pixel.system_input_enable & (1u << field)) == 0u)
		{
			continue;
		}
		source->interface += String8::FromPrintf(" %%capture_bary%u", field);
		source->annotations += String8::FromPrintf("OpDecorate %%capture_bary%u Location %u\n", field,
		                                           pixel.custom_interpolation.barycentric_locations[field]);
		if (field >= 5u)
		{
			source->annotations += String8::FromPrintf("OpDecorate %%capture_bary%u NoPerspective\n", field);
		}
		if (field == 2u || field == 6u)
		{
			source->annotations += String8::FromPrintf("OpDecorate %%capture_bary%u Centroid\n", field);
		}
		source->declarations += String8::FromPrintf("%%capture_bary%u = OpVariable %%capture_bary_ptr Input\n", field);
	}
}

String8 LoadFloat(uint32_t word, const String8& pointer)
{
	return pointer + String8::FromPrintf("%%capture_float%u = OpLoad %%float %%capture_pointer%u\n"
	                                     "%%capture_value%u = OpBitcast %%uint %%capture_float%u\n",
	                                     word, word, word, word);
}

String8 InputPointer(uint32_t word, const char* type, const String8& input, uint32_t component)
{
	return String8::FromPrintf("%%capture_pointer%u = OpAccessChain %%%s %%%s %%capture_constant%u\n", word, type, input.c_str(),
	                           component);
}

String8 SystemWord(const ShaderPixelInputInfo& pixel, uint32_t reg, uint32_t word)
{
	for (uint32_t field = 0; field < 13u; ++field)
	{
		if ((pixel.system_input_enable & (1u << field)) == 0u)
		{
			continue;
		}
		const uint32_t first = ShaderPixelSystemInputRegister(pixel, field);
		const uint32_t width = field < 7u ? 2u : 1u;
		if (reg < first || reg - first >= width)
		{
			continue;
		}
		if (field < 7u)
		{
			return LoadFloat(word, InputPointer(word, "_ptr_Input_float", String8::FromPrintf("capture_bary%u", field), reg - first));
		}
		if (field == 12u)
		{
			return String8::FromPrintf("%%capture_front%u = OpLoad %%bool %%gl_FrontFacing\n"
			                           "%%capture_value%u = OpSelect %%uint %%capture_front%u %%capture_front_bits %%capture_back_bits\n",
			                           word, word, word);
		}
		return LoadFloat(word, InputPointer(word, "_ptr_Input_float", "gl_FragCoord", field - 8u));
	}
	return String8::FromPrintf("%%capture_value%u = OpCopyObject %%uint %%capture_constant0\n", word);
}

String8 AttributeWord(const ShaderPixelInputInfo& pixel, uint32_t attribute, uint32_t component, uint32_t word)
{
	if (!ShaderPixelInputActive(pixel, attribute))
	{
		return String8::FromPrintf("%%capture_value%u = OpCopyObject %%uint %%capture_constant0\n", word);
	}
	ShaderPixelInterpolator decoded {};
	if (!ShaderDecodePixelInterpolator(pixel.interpolator_settings[attribute], &decoded))
	{
		return {};
	}
	if (decoded.source == ShaderPixelInterpolatorSource::Default)
	{
		const uint32_t bits = ShaderPixelInterpolatorDefaultComponent(decoded, component & 3u) != 0.0f ? 0x3f800000u : 0u;
		return String8::FromPrintf("%%capture_value%u = OpCopyObject %%uint %%%s\n", word,
		                           bits != 0u ? "capture_float_one_bits" : "capture_constant0");
	}
	const uint32_t canonical = ShaderPixelCanonicalInterpolator(pixel, attribute);
	if (decoded.source == ShaderPixelInterpolatorSource::PerVertex && component >= 4u)
	{
		const uint32_t vertex = ((component - 4u) / 4u + 1u) % 3u;
		return String8::FromPrintf(
		    "%%capture_pointer%u = OpAccessChain %%_ptr_Input_uint %%capture_attr%u %%capture_constant%u %%capture_constant%u\n"
		    "%%capture_value%u = OpLoad %%uint %%capture_pointer%u\n",
		    word, canonical, vertex, component & 3u, word, word);
	}
	if (decoded.source == ShaderPixelInterpolatorSource::Parameter && component < 4u)
	{
		return LoadFloat(word, InputPointer(word, "_ptr_Input_float", String8::FromPrintf("capture_attr%u", canonical), component));
	}
	// Admission proves these parameter-cache slots are unobserved. They are
	// transport padding; they must not become guessed interpolation inputs.
	return String8::FromPrintf("%%capture_value%u = OpCopyObject %%uint %%capture_constant0\n", word);
}

String8 ReaderBody(const ShaderPixelInputInfo& pixel, const ShaderFragmentComputeInfo& transport, CaptureSource* source)
{
	const uint32_t words = 1u + transport.initial_vgpr_count + pixel.input_num * 16u;
	for (uint32_t value = 0; value < 4u; ++value)
	{
		source->constants += String8::FromPrintf("%%capture_constant%u = OpConstant %%uint %u\n", value, value);
	}
	source->constants += "%capture_flag_covered = OpConstant %uint 7\n"
	                     "%capture_float_one_bits = OpConstant %uint 1065353216\n";
	for (uint32_t word = 0; word < words; ++word)
	{
		source->cases += String8::FromPrintf(" %u %%capture_case%u", word, word);
		source->blocks += String8::FromPrintf("%%capture_case%u = OpLabel\n", word);
		String8 load;
		if (word == 0u)
		{
			load = "%capture_helper = OpLoad %bool %gl_HelperInvocation\n"
			       "%capture_value0 = OpSelect %uint %capture_helper %capture_constant1 %capture_flag_covered\n";
		} else if (word <= transport.initial_vgpr_count)
		{
			load = SystemWord(pixel, word - 1u, word);
		} else
		{
			const uint32_t offset = word - 1u - transport.initial_vgpr_count;
			load                  = AttributeWord(pixel, offset / 16u, offset % 16u, word);
		}
		source->blocks += load + String8::FromPrintf("OpReturnValue %%capture_value%u\n", word);
	}
	return String8(kFragmentCaptureReaderSignature) +
	       "%capture_reader_entry = OpLabel\n"
	       "%capture_word = OpLoad %uint %word\nOpSelectionMerge %capture_reader_end None\n"
	       "OpSwitch %capture_word %capture_reader_default" +
	       source->cases + "\n" + source->blocks +
	       "%capture_reader_default = OpLabel\nOpReturnValue %capture_constant0\n"
	       "%capture_reader_end = OpLabel\nOpUnreachable\nOpFunctionEnd\n";
}

} // namespace

String8 GenerateCaptureSource(const ShaderCode& code, const ShaderPixelInputInfo& pixel, const ShaderFragmentComputeInfo& transport)
{
	InterpolationFields fields;
	if (code.GetType() != ShaderType::Pixel || !InputsAvailable(code, pixel, transport, &fields) || transport.initial_vgpr_count > 256u ||
	    !pixel.host_to_guest_scale.IsIdentity())
	{
		return "OpKytyFragmentCaptureInputsRejected\n";
	}
	CaptureSource generated;
	DeclareInputs(pixel, fields, &generated);
	if (pixel.FrontFaceEnabled())
	{
		generated.interface += " %gl_FrontFacing";
		generated.annotations += "OpDecorate %gl_FrontFacing BuiltIn FrontFacing\n";
		generated.declarations += "%gl_FrontFacing = OpVariable %_ptr_Input_bool Input\n";
	}
	generated.constants += String8::FromPrintf("%%capture_front_bits = OpConstant %%uint %u\n%%capture_back_bits = OpConstant %%uint %u\n",
	                                           pixel.front_face_all_bits ? 1u : 0x3f800000u, pixel.front_face_all_bits ? 0u : 0xbf800000u);
	const auto reader = ReaderBody(pixel, transport, &generated);
	String8    source(kFragmentCaptureCore);
	source = source.ReplaceStr("<capture_interface>", generated.interface);
	source = source.ReplaceStr("<capture_annotations>", generated.annotations);
	source = source.ReplaceStr("<capture_declarations>", generated.declarations + generated.constants);
	source = source.ReplaceStr("<capture_reader>", reader);
	for (const char* buffer: {"record_buffer", "control_buffer", "reference_buffer"})
	{
		source = source.ReplaceStr(String8::FromPrintf("OpDecorate %%%s DescriptorSet 0", buffer),
		                           String8::FromPrintf("OpDecorate %%%s DescriptorSet %u", buffer, transport.descriptor_set));
	}
	return source;
}

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
