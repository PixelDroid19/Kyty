#include "ShaderWaveHintCases.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const char* message)
{
	std::fprintf(stderr, "paired S_INST_PREFETCH numerical failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunCase(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                        = 1u;
	const ShaderComputeWaveRequest request {{64u, 1u, 1u}, {1u, 1u, 1u}, 0x41u, 0u, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired wave64 layout unavailable");
	}

	const std::array<uint32_t, 4> words {0xbfa00003u, 0x7e0202ffu, 0x12345678u,
	                                     0xbf810000u}; // s_inst_prefetch m3; v_mov_b32 v1, literal; s_endpgm
	ShaderCode                    code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), sizeof(words), &code))
	{
		Fail("bounded parser rejected instruction prefetch fixture");
	}
	const auto admission = ShaderAnalyzeComputeWaveCode(code, input);
	if (!admission.supported)
	{
		Fail(admission.reason.c_str());
	}
	const std::vector<WaveProbeObservation> observations {{"v1_low", "v1_high", true}};
	String8                                 source, error;
	if (!BuildWaveProbeSource(words.data(), sizeof(words), input, observations, {}, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}

	constexpr uint32_t    sentinel = 0xa5c37e19u;
	std::vector<uint32_t> initial(128u, sentinel), actual(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1u, 1u, 1u}, initial, &actual, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message.c_str());
	}
	if (actual.size() != initial.size())
	{
		Fail("unexpected wave-probe output length");
	}
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		if (actual[lane] != 0x12345678u)
		{
			Fail("V_MOV_B32 result differs in a guest wave bank");
		}
	}
	for (size_t word = 64u; word < actual.size(); ++word)
	{
		if (actual[word] != sentinel)
		{
			Fail("output canary overwritten");
		}
	}
	std::printf("PairedWaveSInstPrefetchMode3 PASS\n");
}

} // namespace

void RunWaveHintCases(VulkanComputeProbe& probe)
{
	RunCase(probe);
}

} // namespace Kyty::Libs::Graphics
