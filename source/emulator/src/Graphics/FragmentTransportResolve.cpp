#include "Emulator/Graphics/FragmentTransportResolve.h"

#include "Emulator/Graphics/FragmentTransportLayout.h"

#include "Kyty/Core/DbgAssert.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

#include "host_shaders/fragment_wave_resolve_asm.inc"

// The kernels declare their parameters at byte zero. The guest vertex stage
// pushes its user data from byte zero too, so the parameters move to the
// pixel-stage slot that follows it.
String8 PlaceKernelParameters(String8 source, uint32_t offset)
{
	for (uint32_t member = 0; member < KERNEL_PARAMETER_BYTES / 4u; ++member)
	{
		const auto declared = String8::FromPrintf("OpMemberDecorate %%TransportParameters %u Offset %u\n", member, member * 4u);
		EXIT_IF(!source.ContainsStr(declared));
		source = source.ReplaceStr(declared, String8::FromPrintf("OpMemberDecorate %%TransportParameters %u Offset %u\n", member,
		                                                         offset + member * 4u));
	}
	return source;
}

String8 WriteTarget(uint32_t target)
{
	String8 code;
	for (uint32_t component = 0; component < 4u; ++component)
	{
		code += String8::FromPrintf(
		    "%%resolve_index%u_%u = OpIAdd %%uint %%resolve_base %%resolve_offset%u_%u\n"
		    "%%resolve_pointer%u_%u = OpAccessChain %%_ptr_StorageBuffer_uint %%result_buffer %%int_0 %%resolve_index%u_%u\n"
		    "%%resolve_word%u_%u = OpLoad %%uint %%resolve_pointer%u_%u\n"
		    "%%resolve_float%u_%u = OpBitcast %%float %%resolve_word%u_%u\n",
		    target, component, target, component, target, component, target, component, target, component, target, component, target,
		    component, target, component);
	}
	code += String8::FromPrintf("%%resolve_color%u = OpCompositeConstruct %%v4float %%resolve_float%u_0 %%resolve_float%u_1 "
	                            "%%resolve_float%u_2 %%resolve_float%u_3\nOpStore %%resolve_output%u %%resolve_color%u\n",
	                            target, target, target, target, target, target, target);
	return code;
}

} // namespace

String8 GenerateResolveSource(uint32_t descriptor_set, uint32_t parameter_offset, uint32_t color_write_mask, bool depth_export)
{
	String8 interface;
	String8 annotations;
	String8 declarations;
	String8 writer(kFragmentResolveWriterSignature);
	writer += "%resolve_writer_entry = OpLabel\n%resolve_base = OpLoad %uint %base\n";
	for (uint32_t target = 0; target < 8u; ++target)
	{
		if ((color_write_mask & (15u << (target * 4u))) == 0u)
		{
			continue;
		}
		interface += String8::FromPrintf(" %%resolve_output%u", target);
		annotations += String8::FromPrintf("OpDecorate %%resolve_output%u Location %u\n", target, target);
		declarations += String8::FromPrintf("%%resolve_output%u = OpVariable %%_ptr_Output_v4float Output\n", target);
		for (uint32_t component = 0; component < 4u; ++component)
		{
			declarations +=
			    String8::FromPrintf("%%resolve_offset%u_%u = OpConstant %%uint %u\n", target, component, 2u + target * 4u + component);
		}
		writer += WriteTarget(target);
	}
	if (depth_export)
	{
		interface += " %resolve_depth";
		annotations += "OpDecorate %resolve_depth BuiltIn FragDepth\n";
		declarations += String8::FromPrintf("%%resolve_ptr_output_float = OpTypePointer Output %%float\n"
		                                    "%%resolve_depth = OpVariable %%resolve_ptr_output_float Output\n"
		                                    "%%resolve_depth_offset = OpConstant %%uint %u\n",
		                                    OUTPUT_LANE_WORDS - 1u);
		writer += "%resolve_depth_index = OpIAdd %uint %resolve_base %resolve_depth_offset\n"
		          "%resolve_depth_pointer = OpAccessChain %_ptr_StorageBuffer_uint %result_buffer %int_0 %resolve_depth_index\n"
		          "%resolve_depth_word = OpLoad %uint %resolve_depth_pointer\n"
		          "%resolve_depth_float = OpBitcast %float %resolve_depth_word\n"
		          "OpStore %resolve_depth %resolve_depth_float\n";
	}
	writer += "OpReturn\nOpFunctionEnd\n";
	String8 source(kFragmentResolveCore);
	source = source.ReplaceStr("<resolve_interface>", interface);
	source = source.ReplaceStr("<resolve_annotations>", annotations);
	source = source.ReplaceStr("<resolve_declarations>", declarations);
	source = source.ReplaceStr("<resolve_writer>", writer);
	if (depth_export)
	{
		source = source.ReplaceStr("OpExecutionMode %main OriginUpperLeft", "OpExecutionMode %main OriginUpperLeft\nOpExecutionMode %main DepthReplacing");
	}
	source = source.ReplaceStr("<resolve_mask>", String8::FromPrintf("%u", color_write_mask));
	for (const char* buffer: {"record_buffer", "control_buffer", "reference_buffer", "result_buffer"})
	{
		source = source.ReplaceStr(String8::FromPrintf("OpDecorate %%%s DescriptorSet 0", buffer),
		                           String8::FromPrintf("OpDecorate %%%s DescriptorSet %u", buffer, descriptor_set));
	}
	return PlaceKernelParameters(source, parameter_offset);
}

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
