#include "ShaderProbeSource.h"

#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"

#include <algorithm>
#include <limits>
#include <string>

namespace Kyty::Libs::Graphics {

namespace {

// Scalar operations execute even with an empty EXEC mask. These legal status
// seeds make that fact observable without inventing arbitrary wave-mask state.
constexpr uint32_t kStatusScc     = 1u;
constexpr uint32_t kStatusExecLo  = 0u;
constexpr uint32_t kStatusExecHi  = 0u;

bool InsertBefore(std::string& text, const std::string& anchor, const std::string& insertion, Kyty::Core::String8* error)
{
	const size_t position = text.find(anchor);
	if (position == std::string::npos)
	{
		*error = Kyty::Core::String8::FromPrintf("generated SPIR-V source is missing anchor: %s", anchor.c_str());
		return false;
	}
	text.insert(position, insertion);
	return true;
}

bool InsertAfter(std::string& text, const std::string& anchor, const std::string& insertion, Kyty::Core::String8* error)
{
	const size_t position = text.find(anchor);
	if (position == std::string::npos)
	{
		*error = Kyty::Core::String8::FromPrintf("generated SPIR-V source is missing anchor: %s", anchor.c_str());
		return false;
	}
	text.insert(position + anchor.size(), insertion);
	return true;
}

} // namespace

bool BuildScalarProbeSource(const uint32_t* shader_words, size_t shader_word_count,
                            const std::vector<ScalarRegisterSeed>& register_seeds, int result_register,
                            Kyty::Core::String8* source, Kyty::Core::String8* error)
{
	ShaderComputeInputInfo input_info {};
	input_info.threads_num[0] = 1;
	input_info.threads_num[1] = 1;
	input_info.threads_num[2] = 1;
	return BuildScalarProbeSource(shader_words, shader_word_count, register_seeds, result_register, -1, input_info, source, error);
}

bool BuildScalarProbeSource(const uint32_t* shader_words, size_t shader_word_count,
	                            const std::vector<ScalarRegisterSeed>& register_seeds, int result_register,
	                            int result_register_high, const ShaderComputeInputInfo& input_info,
	                            Kyty::Core::String8* source, Kyty::Core::String8* error, bool nonempty_exec)
{
	if (shader_words == nullptr || shader_word_count == 0 || shader_word_count > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t) ||
	    source == nullptr || error == nullptr || result_register > 102 || result_register < -1 || result_register_high > 102 ||
	    result_register_high < -1 || (result_register_high >= 0 && (result_register < 0 || result_register_high == result_register)) ||
	    (input_info.bind.vsharp_uniform_buffer && input_info.bind.vsharp_binding_index < 0))
	{
		if (error != nullptr) { *error = "invalid shader probe input"; }
		return false;
	}

	std::vector<uint32_t> seeded_registers;
	seeded_registers.reserve(register_seeds.size());
	for (const auto& seed: register_seeds)
	{
		if (seed.reg > 102 || std::find(seeded_registers.begin(), seeded_registers.end(), seed.reg) != seeded_registers.end())
		{
			*error = "invalid or duplicate SGPR seed";
			return false;
		}
		seeded_registers.push_back(seed.reg);
	}

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(shader_words, static_cast<uint32_t>(shader_word_count * sizeof(uint32_t)), &code))
	{
		*error = "bounded parser rejected probe shader";
		return false;
	}

	const Kyty::Core::String8 translated = SpirvGenerateSource(code, nullptr, nullptr, &input_info);
	std::string text(translated.GetDataConst(), translated.Size());
	const uint32_t probe_binding = input_info.bind.vsharp_uniform_buffer ?
	                              static_cast<uint32_t>(input_info.bind.vsharp_binding_index + 1) : 0u;

	std::string annotations;
	annotations += "OpDecorate %probe_words ArrayStride 4\n";
	annotations += "OpDecorate %probe_block Block\n";
	annotations += "OpMemberDecorate %probe_block 0 Offset 0\n";
	annotations += Kyty::Core::String8::FromPrintf("OpDecorate %%probe_buffer DescriptorSet %u\n", input_info.bind.descriptor_set_slot)
	                  .GetDataConst();
	annotations += Kyty::Core::String8::FromPrintf("OpDecorate %%probe_buffer Binding %u\n", probe_binding).GetDataConst();
	if (!InsertBefore(text, "%void = OpTypeVoid", annotations, error)) { return false; }

	const std::string types =
	    "%probe_words = OpTypeRuntimeArray %uint\n"
	    "%probe_block = OpTypeStruct %probe_words\n"
	    "%probe_block_ptr = OpTypePointer StorageBuffer %probe_block\n";
	if (!InsertBefore(text, "%function_void = OpTypeFunction %void", types, error)) { return false; }

	std::string constants;
	constants += "%probe_index_0 = OpConstant %uint 0\n";
	constants += "%probe_index_1 = OpConstant %uint 1\n";
	constants += "%probe_index_2 = OpConstant %uint 2\n";
	constants += "%probe_index_3 = OpConstant %uint 3\n";
	constants += "%probe_index_4 = OpConstant %uint 4\n";
	constants += "%probe_index_5 = OpConstant %uint 5\n";
	constants += "%probe_index_6 = OpConstant %uint 6\n";
	constants += "%probe_index_7 = OpConstant %uint 7\n";
	constants += Kyty::Core::String8::FromPrintf("%%probe_seed_scc = OpConstant %%uint %u\n", kStatusScc).GetDataConst();
	constants += Kyty::Core::String8::FromPrintf("%%probe_seed_exec_lo = OpConstant %%uint %u\n", nonempty_exec ? 1u : kStatusExecLo).GetDataConst();
	constants += Kyty::Core::String8::FromPrintf("%%probe_seed_exec_hi = OpConstant %%uint %u\n", kStatusExecHi).GetDataConst();
	for (const auto& seed: register_seeds)
	{
		constants += Kyty::Core::String8::FromPrintf("%%probe_seed_sgpr_%u = OpConstant %%uint %u\n", seed.reg, seed.value)
		                 .GetDataConst();
	}
	if (!InsertBefore(text, "%true = OpConstantTrue %bool", constants, error)) { return false; }

	if (!InsertAfter(text, "OpEntryPoint GLCompute %main \"main\"", " %probe_buffer", error)) { return false; }
	if (!InsertBefore(text, ";Variables\n%gl_LocalInvocationID",
	                  "%probe_buffer = OpVariable %probe_block_ptr StorageBuffer\n", error))
	{
		return false;
	}

	const std::string main_anchor = "%main       = OpFunction %void None %function_void";
	const size_t       main_start  = text.find(main_anchor);
	if (main_start == std::string::npos)
	{
		*error = "generated SPIR-V source is missing main function";
		return false;
	}
	const std::string common_scc = "OpStore %scc %uint_0";
	const size_t       common_init = text.find(common_scc, main_start);
	if (common_init == std::string::npos)
	{
		*error = "generated SPIR-V source is missing common SCC initialization";
		return false;
	}
	const size_t seed_position = text.find('\n', common_init);
	if (seed_position == std::string::npos)
	{
		*error = "generated SPIR-V source has truncated common initialization";
		return false;
	}
	std::string seeds = "\nOpStore %scc %probe_seed_scc\nOpStore %exec_lo %probe_seed_exec_lo\nOpStore %exec_hi %probe_seed_exec_hi";
	for (const auto& seed: register_seeds)
	{
		seeds += Kyty::Core::String8::FromPrintf("\nOpStore %%s%u %%probe_seed_sgpr_%u", seed.reg, seed.reg).GetDataConst();
	}
	text.insert(seed_position + 1u, seeds);

	std::string exports;
	const uint32_t output_words = result_register_high >= 0 ? 8u : 4u;
	for (uint32_t slot = 0; slot < output_words; ++slot)
	{
		const int output_register = result_register_high >= 0 ?
		                            (slot == 0 ? result_register : slot == 1 ? result_register_high : -1) :
		                            (slot == 0 ? result_register : -1);
		const char* status_value = nullptr;
		if (result_register_high >= 0)
		{
			status_value = slot == 2 ? "%scc" : slot == 3 ? "%exec_lo" : slot == 4 ? "%exec_hi" : nullptr;
		} else
		{
			status_value = slot == 1 ? "%scc" : slot == 2 ? "%exec_lo" : slot == 3 ? "%exec_hi" : nullptr;
		}
		const uint32_t index = slot;
		exports += Kyty::Core::String8::FromPrintf(
		    "%%probe_ptr_%u = OpAccessChain %%_ptr_StorageBuffer_uint %%probe_buffer %%probe_index_0 %%probe_index_%u\n",
		    slot, index).GetDataConst();
		if (output_register >= 0)
		{
			exports += Kyty::Core::String8::FromPrintf("%%probe_value_%u = OpLoad %%uint %%s%d\n", slot, output_register).GetDataConst();
			exports += Kyty::Core::String8::FromPrintf("OpStore %%probe_ptr_%u %%probe_value_%u\n", slot, slot).GetDataConst();
		} else if (status_value != nullptr)
		{
			exports += Kyty::Core::String8::FromPrintf("%%probe_value_%u = OpLoad %%uint %s\n", slot, status_value).GetDataConst();
			exports += Kyty::Core::String8::FromPrintf("OpStore %%probe_ptr_%u %%probe_value_%u\n", slot, slot).GetDataConst();
		} else
		{
			exports += Kyty::Core::String8::FromPrintf("OpStore %%probe_ptr_%u %%uint_0\n", slot).GetDataConst();
		}
	}

	const size_t main_return = text.find("OpReturn", main_start);
	if (main_return == std::string::npos)
	{
		*error = "generated SPIR-V source is missing main return";
		return false;
	}
	text.insert(main_return, exports);

	*source = text.c_str();
	error->Clear();
	return true;
}

} // namespace Kyty::Libs::Graphics
