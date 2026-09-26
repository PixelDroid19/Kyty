#include "ShaderWaveShiftCases.h"

#include "Emulator/Graphics/ShaderComputeWaveAnalysis.h"
#include "Emulator/Graphics/ShaderParse.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

struct ShiftCase
{
	const char* name;
	uint32_t    seed_s14;
	uint32_t    seed_scc;
	uint32_t    expected_vcc_hi;
	uint32_t    expected_scc;
};

constexpr uint32_t kCanary       = 0xa5c37e19u;
constexpr uint32_t kExecLow      = 0x80000001u;
constexpr uint32_t kExecHigh     = 0x80000000u;
constexpr uint32_t kVccLow       = 0x13579bdfu;
constexpr uint32_t kInitialVccHi = 0x2468ace0u;

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "paired SLshlB32 numeric failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, const ShiftCase& test)
{
	// A scalar VCC copy declares both packed VCC words without changing them.
	// The tested SOP2 shifts s14 into VccHi; s_endpgm terminates the fixture.
	// Keep these encoded guest words local to this test: there is no game fixture.
	const std::vector<uint32_t> words {0xbe94046au, 0x8f6b860eu, 0xbf810000u};
	ShaderCode                    code;
	code.SetType(ShaderType::Compute);
	if (!ShaderTryParseBounded(words.data(), words.size() * sizeof(uint32_t), &code))
	{
		Fail("bounded parser rejected the synthetic SOP2 shift fixture");
	}
	const auto admission = ShaderAnalyzeComputeWaveCode(code, input);
	if (!admission.supported)
	{
		Fail(String8::FromPrintf("SLshlB32 to VccHi is not admitted: %s", admission.reason.c_str()).c_str());
	}

	const std::vector<WaveProbeObservation> observations {{"vcc_hi", "vcc_hi"}, {"vcc_lo", "vcc_lo"}, {"scc", "scc"},
	                                                      {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}};
	const std::vector<WaveProbeSeed> seeds {{"s14", test.seed_s14}, {"exec_lo", kExecLow}, {"exec_hi", kExecHigh},
	                                        {"vcc_lo", kVccLow}, {"vcc_hi", kInitialVccHi}, {"scc", test.seed_scc}};
	String8                          source, error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, seeds, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}

	const uint32_t logical_count = input.wave_layout.waves * 64u;
	if (input.wave_layout.banks != 2u || input.wave_layout.waves != 2u || logical_count != 128u)
	{
		Fail("fixture requires two paired banks and two logical waves");
	}
	const size_t            stride = observations.size();
	std::vector<uint32_t> initial((logical_count + 64u) * stride, kCanary), actual(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1u, 1u, 1u}, initial, &actual, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message);
	}

	for (uint32_t lane = 0; lane < logical_count; ++lane)
	{
		const uint32_t expected[] = {test.expected_vcc_hi, kVccLow, test.expected_scc, kExecLow, kExecHigh};
		for (size_t field = 0; field < stride; ++field)
		{
			if (actual[lane * stride + field] != expected[field])
			{
				Fail(String8::FromPrintf("%s: logical_lane=%u field=%zu actual=%08x expected=%08x", test.name, lane, field,
				                         actual[lane * stride + field], expected[field])
				         .c_str());
			}
		}
	}
	for (size_t word = logical_count * stride; word < actual.size(); ++word)
	{
		if (actual[word] != kCanary)
		{
			Fail(String8::FromPrintf("%s: canary word=%zu actual=%08x expected=%08x", test.name, word, actual[word], kCanary).c_str());
		}
	}
	std::printf("%s PASS\n", test.name);
}

} // namespace

void RunWaveShiftCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0]    = 128u;
	input.threads_num[1]    = 1u;
	input.threads_num[2]    = 1u;
	input.thread_ids_num    = 1;
	input.workgroup_register = 14;
	const ShaderComputeWaveRequest request {{128u, 1u, 1u}, {1u, 1u, 1u}, 0x41u, 0u, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("two-wave paired layout unavailable");
	}

	// The scalar seed overrides s14 for deterministic arithmetic; the values
	// exercise both a nonzero shifted result and a result truncated to zero.
	const ShiftCase cases[] = {{"PairedSop2LshlB32VccHiNonzero", 0x01234567u, 0u, 0x48d159c0u, 1u},
	                           {"PairedSop2LshlB32VccHiZero", 0x40000000u, 1u, 0u, 0u}};
	for (const auto& test: cases)
	{
		RunCase(probe, input, test);
	}
}

} // namespace Kyty::Libs::Graphics
