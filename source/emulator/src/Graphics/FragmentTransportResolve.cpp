#include "Emulator/Graphics/FragmentTransportResolve.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::FragmentTransport {
namespace {

#include "host_shaders/fragment_wave_resolve_asm.inc"

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

String8 GenerateResolveSource(uint32_t descriptor_set, uint32_t color_write_mask)
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
	writer += "OpReturn\nOpFunctionEnd\n";
	String8 source(kFragmentResolveCore);
	source = source.ReplaceStr("<resolve_interface>", interface);
	source = source.ReplaceStr("<resolve_annotations>", annotations);
	source = source.ReplaceStr("<resolve_declarations>", declarations);
	source = source.ReplaceStr("<resolve_writer>", writer);
	source = source.ReplaceStr("<resolve_mask>", String8::FromPrintf("%u", color_write_mask));
	for (const char* buffer: {"record_buffer", "control_buffer", "reference_buffer", "result_buffer"})
	{
		source = source.ReplaceStr(String8::FromPrintf("OpDecorate %%%s DescriptorSet 0", buffer),
		                           String8::FromPrintf("OpDecorate %%%s DescriptorSet %u", buffer, descriptor_set));
	}
	return source;
}

} // namespace Kyty::Libs::Graphics::FragmentTransport

#endif
