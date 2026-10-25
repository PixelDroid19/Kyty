#include "ShaderWaveProbeSource.h"

#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include <string>

namespace Kyty::Libs::Graphics {
namespace {

bool Insert(std::string* text, const char* anchor, const std::string& value, String8* error, bool after = false)
{
	const auto pos = text->find(anchor);
	if (pos == std::string::npos)
	{
		*error = String8::FromPrintf("wave observation anchor missing: %s", anchor);
		return false;
	}
	text->insert(pos + (after ? std::char_traits<char>::length(anchor) : 0u), value);
	return true;
}

} // namespace

bool BuildWaveProbeSource(const uint32_t* words, size_t byte_count, const ShaderComputeInputInfo& input,
                          const std::vector<WaveProbeObservation>& observations, const std::vector<WaveProbeSeed>& seeds, String8* source,
                          String8* error, uint32_t output_binding)
{
	if (source == nullptr || error == nullptr || words == nullptr || byte_count == 0 || observations.empty() || observations.size() > 16u ||
	    seeds.size() > 16u || input.bind.descriptor_set_slot != 0 ||
	    (input.bind.storage_buffers.buffers_num > 0 &&
	     input.bind.storage_buffers.binding_index >= 0 &&
	     static_cast<uint32_t>(input.bind.storage_buffers.binding_index) == output_binding) ||
	    (input.bind.vsharp_uniform_buffer && input.bind.vsharp_binding_index >= 0 &&
	     static_cast<uint32_t>(input.bind.vsharp_binding_index) == output_binding))
	{
		if (error != nullptr)
		{
			*error = "invalid wave observation fixture or colliding probe output binding";
		}
		return false;
	}
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words, byte_count, &code))
	{
		*error = "bounded parser rejected wave observation fixture";
		return false;
	}
	const auto        generated = SpirvGenerateSource(code, nullptr, nullptr, &input);
	std::string       text(generated.GetDataConst(), generated.Size());
	// Split programs keep the guest lane in %gl_SubgroupInvocationID and read the host lane from %split_host_lane.
	const bool        has_split = text.find("OpDecorate %split_host_lane BuiltIn SubgroupLocalInvocationId") != std::string::npos;
	const bool        has_lane  = has_split || text.find("OpDecorate %gl_SubgroupInvocationID BuiltIn SubgroupLocalInvocationId") != std::string::npos;
	const bool        has_wave  = text.find("OpDecorate %gl_SubgroupID BuiltIn SubgroupId") != std::string::npos;
	const std::string lane      = has_split ? "%split_host_lane" : has_lane ? "%gl_SubgroupInvocationID" : "%mask_probe_lane";
	const std::string wave     = has_wave ? "%gl_SubgroupID" : "%mask_probe_wave";
	if (text.find("OpCapability GroupNonUniform\n") == std::string::npos &&
	    !Insert(&text, "OpCapability Shader", "\nOpCapability GroupNonUniform\n", error, true))
	{
		return false;
	}
	std::string annotations = "OpDecorate %mask_probe_words ArrayStride 4\nOpDecorate %mask_probe_block Block\n"
	                          "OpMemberDecorate %mask_probe_block 0 Offset 0\n"
	                          "OpDecorate %mask_probe_buffer DescriptorSet 0\n";
	annotations += String8::FromPrintf("OpDecorate %%mask_probe_buffer Binding %u\n", output_binding).GetDataConst();
	if (!has_lane)
	{
		annotations += "OpDecorate " + lane + " BuiltIn SubgroupLocalInvocationId\n";
	}
	if (!has_wave)
	{
		annotations += "OpDecorate " + wave + " BuiltIn SubgroupId\n";
	}
	if (!Insert(&text, "%void = OpTypeVoid", annotations, error))
	{
		return false;
	}
	if (!Insert(&text, "%function_void = OpTypeFunction %void",
	            "%mask_probe_words = OpTypeRuntimeArray %uint\n%mask_probe_block = OpTypeStruct %mask_probe_words\n"
	            "%mask_probe_ptr = OpTypePointer StorageBuffer %mask_probe_block\n",
	            error))
	{
		return false;
	}
	std::string constants;
	for (const uint32_t value: {0u, 1u, 2u, 3u, 4u, 5u, 32u})
	{
		constants += String8::FromPrintf("%%mask_probe_c%u = OpConstant %%uint %u\n", value, value).GetDataConst();
	}
	constants += String8::FromPrintf("%%mask_probe_stride = OpConstant %%uint %zu\n", observations.size()).c_str();
	for (size_t seed = 0; seed < seeds.size(); ++seed)
	{
		constants += String8::FromPrintf("%%mask_probe_seed_%zu = OpConstant %%uint %u\n", seed, seeds[seed].value).c_str();
	}
	if (!Insert(&text, "%true = OpConstantTrue %bool", constants, error))
	{
		return false;
	}
	std::string interfaces = " %mask_probe_buffer";
	std::string variables  = "%mask_probe_buffer = OpVariable %mask_probe_ptr StorageBuffer\n";
	if (!has_lane)
	{
		interfaces += " " + lane;
		variables += lane + " = OpVariable %_ptr_Input_uint Input\n";
	}
	if (!has_wave)
	{
		interfaces += " " + wave;
		variables += wave + " = OpVariable %_ptr_Input_uint Input\n";
	}
	if (!Insert(&text, "OpEntryPoint GLCompute %main \"main\"", interfaces, error, true) ||
	    !Insert(&text, ";Variables\n", variables, error, true))
	{
		return false;
	}
	std::string stores;
	for (size_t seed = 0; seed < seeds.size(); ++seed)
	{
		stores += String8::FromPrintf("OpStore %%%s %%mask_probe_seed_%zu\n", seeds[seed].name.c_str(), seed).c_str();
	}
	// After the prolog publishes the initial EXEC words and before the lane bits are derived from them.
	if (!Insert(&text, "%mask_exec_initial_lane = ", stores, error))
	{
		return false;
	}
	// One record per host invocation: subgroup * 32 + host lane is the guest lane index in both layouts.
	std::string exports = "%mask_probe_lane_id = OpLoad %uint " + lane + "\n%mask_probe_wave_id = OpLoad %uint " + wave +
	                      "\n"
	                      "%mask_probe_base = OpIMul %uint %mask_probe_wave_id %mask_probe_c32\n"
	                      "%mask_probe_logical = OpIAdd %uint %mask_probe_base %mask_probe_lane_id\n"
	                      "%mask_probe_record = OpIMul %uint %mask_probe_logical %mask_probe_stride\n";
	for (size_t slot = 0; slot < observations.size(); ++slot)
	{
		const auto& observation = observations[slot];
		constants               = String8::FromPrintf("%%mask_probe_slot_%zu = OpConstant %%uint %zu\n", slot, slot).c_str();
		if (!Insert(&text, "%true = OpConstantTrue %bool", constants, error))
		{
			return false;
		}
		exports += String8::FromPrintf("%%mask_probe_loaded_%zu = OpLoad %%%s %%%s\n"
		                               "%%mask_probe_value_%zu = OpBitcast %%uint %%mask_probe_loaded_%zu\n"
		                               "%%mask_probe_index_%zu = OpIAdd %%uint %%mask_probe_record %%mask_probe_slot_%zu\n"
		                               "%%mask_probe_address_%zu = OpAccessChain %%_ptr_StorageBuffer_uint %%mask_probe_buffer "
		                               "%%mask_probe_c0 %%mask_probe_index_%zu\n"
		                               "OpStore %%mask_probe_address_%zu %%mask_probe_value_%zu\n",
		                               slot, observation.floating ? "float" : "uint", observation.variable.c_str(), slot, slot, slot,
		                               slot, slot, slot, slot, slot)
		               .c_str();
	}
	if (!Insert(&text, "OpReturn", exports, error))
	{
		return false;
	}
	*source = text.c_str();
	error->Clear();
	return true;
}

} // namespace Kyty::Libs::Graphics
