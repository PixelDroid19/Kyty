#include "ShaderWaveLaneCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <cstdio>
#include <cstdlib>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "wave lane numerical failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, const char* name, const std::vector<uint32_t>& words,
             const std::vector<WaveProbeObservation>& observations, const std::array<uint32_t, 2>& exec,
             const std::vector<uint32_t>& expected, bool native32 = false)
{
	String8 source, error;
	auto    source_input = input;
	if (native32)
	{
		// The native shader has 32 physical lanes. Repeated scalar observations
		// in the second record slot do not imply 64 native guest lanes.
		source_input.wave_layout    = {};
		source_input.threads_num[0] = 32u;
	}
	const std::vector<WaveProbeSeed> seeds {{"exec_lo", exec[0]}, {"exec_hi", exec[1]}, {"scc", 1u}};
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), source_input, observations, seeds, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}
	constexpr uint32_t sentinel = 0xa5c37e19u;
	if (expected.size() != 64u * observations.size())
	{
		Fail("invalid lane oracle size");
	}
	std::vector<uint32_t> initial(expected.size() + 64u * observations.size(), sentinel), result(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message);
	}
	for (size_t word = 0; word < result.size(); ++word)
	{
		const auto wanted = word < expected.size() ? expected[word] : sentinel;
		if (result[word] != wanted)
		{
			Fail(String8::FromPrintf("%s: word=%zu actual=%08x expected=%08x", name, word, result[word], wanted).c_str());
		}
	}
	std::printf("%s PASS\n", name);
}

void ReadLaneCases(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	const std::array<uint32_t, 2> cases[] = {{31u, 31u}, {32u, 32u}, {63u, 63u}, {64u, 0u}};
	for (const auto& test: cases)
	{
		// Scalar selector followed by repeated readlane catches temporary-ID collisions.
		const std::vector<uint32_t>             words {0xbe8403ffu, test[0],           0xd7600005u, 256u | (4u << 9u),
		                                               0xd7600005u, 256u | (4u << 9u), 0xbf810000u};
		const std::vector<WaveProbeObservation> observations {{"s5", "s5"}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}, {"scc", "scc"}};
		std::vector<uint32_t>                   expected;
		for (uint32_t lane = 0; lane < 64u; ++lane)
		{
			expected.insert(expected.end(), {test[1], 0u, 0u, 1u});
		}
		const auto name = String8::FromPrintf("PairedReadLane%uEmptyExec", test[0]);
		RunCase(probe, input, name.c_str(), words, observations, {0u, 0u}, expected);
	}
}

void WriteLaneCases(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	const std::array<uint32_t, 2> cases[] = {{31u, 31u}, {32u, 32u}, {63u, 63u}, {64u, 0u}};
	for (const auto& test: cases)
	{
		// Write 42 into exactly one logical lane despite EXEC=0, then read it.
		const std::vector<uint32_t>             words {0xbe8403ffu, test[0],           0xd7610000u, 170u | (4u << 9u),
		                                               0xd7600005u, 256u | (4u << 9u), 0xbf810000u};
		const std::vector<WaveProbeObservation> observations {
		    {"v0_low", "v0_high", true}, {"s5", "s5"}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}};
		std::vector<uint32_t> expected;
		for (uint32_t lane = 0; lane < 64u; ++lane)
		{
			expected.insert(expected.end(), {lane == test[1] ? 42u : lane, 42u, 0u, 0u});
		}
		const auto name = String8::FromPrintf("PairedWriteLane%uEmptyExec", test[0]);
		RunCase(probe, input, name.c_str(), words, observations, {0u, 0u}, expected);
	}
}

void ReadFirstCases(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	struct FirstCase
	{
		const char*             name;
		std::array<uint32_t, 2> exec;
		uint32_t                expected;
	};
	const FirstCase cases[] = {
	    {"PairedReadFirstLane63", {0u, 0x80000000u}, 63u},
	    {"PairedReadFirstLane32", {0u, 1u}, 32u},
	    {"PairedReadFirstLowBeforeHigh", {4u, 1u}, 2u},
	    {"PairedReadFirstEmptyReadsNonzeroLane0", {0u, 0u}, 42u},
	};
	// Establish a nonzero lane0 through a real writelane, independently of EXEC.
	const std::vector<uint32_t>             words {0xbe840380u, 0xd7610000u, 170u | (4u << 9u), 0xd5820005u, 256u, 0xbf810000u};
	const std::vector<WaveProbeObservation> observations {{"s5", "s5"}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}, {"scc", "scc"}};
	for (const auto& test: cases)
	{
		std::vector<uint32_t> expected;
		for (uint32_t lane = 0; lane < 64u; ++lane)
		{
			expected.insert(expected.end(), {test.expected, test.exec[0], test.exec[1], 1u});
		}
		RunCase(probe, input, test.name, words, observations, test.exec, expected);
	}
}

void VectorMoveCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	// S_MOV s4,19; V_MOV v0,s4; V_MOV v0,v0. Both snapshots and inactive
	// destination preservation are observed through all 64 original lane values.
	const std::vector<uint32_t>             words {0xbe840393u, 0x7e000204u, 0x7e000300u, 0xbf810000u};
	const std::vector<WaveProbeObservation> observations {
	    {"v0_low", "v0_high", true}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}, {"scc", "scc"}};
	std::vector<uint32_t> expected;
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		expected.insert(expected.end(), {(lane == 0u || lane == 63u) ? 19u : lane, 1u, 0x80000000u, 1u});
	}
	RunCase(probe, input, "PairedMovePreservesInactiveAndAliasedSources", words, observations, {1u, 0x80000000u}, expected);
}

void NativeEmptyFirstCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	const std::vector<uint32_t>             words {0xbe840380u, 0xd7610000u, 170u | (4u << 9u), 0xd5820005u, 256u, 0xbf810000u};
	const std::vector<WaveProbeObservation> observations {{"s5", "s5"}};
	RunCase(probe, input, "Native32ReadFirstEmptyReadsNonzeroLane0", words, observations, {0u, 0u}, std::vector<uint32_t>(64u, 42u), true);
}

void ScalarExecCopyCases(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input)
{
	// Compare -> full VCC mask -> EXEC -> ordinary SGPR pair. Only lane63
	// changes, then restoring EXEC=-1 must restore both words and EXECZ.
	const std::vector<uint32_t>             words {0xd4c2006au, 256u | (191u << 9u), 0xbefe046au, 0x7e0002aau,
	                                               0xbe84047eu, 0xbefe04c1u,         0xbe86047eu, 0xbf810000u};
	const std::vector<WaveProbeObservation> observations {
	    {"v0_low", "v0_high", true}, {"s4", "s4"}, {"s5", "s5"}, {"s6", "s6"}, {"s7", "s7"}, {"execz", "execz"}, {"scc", "scc"}};
	std::vector<uint32_t> expected;
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		expected.insert(expected.end(), {lane == 63u ? 42u : lane, 0u, 0x80000000u, 0xffffffffu, 0xffffffffu, 0u, 1u});
	}
	RunCase(probe, input, "PairedScalarExecCopyAndRestore", words, observations, {0xffffffffu, 0xffffffffu}, expected);

	const std::vector<uint32_t>             empty_words {0xbefe0480u, 0x7e0002aau, 0xbf810000u};
	const std::vector<WaveProbeObservation> empty_observations {
	    {"v0_low", "v0_high", true}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}, {"execz", "execz"}, {"scc", "scc"}};
	expected.clear();
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		expected.insert(expected.end(), {lane, 0u, 0u, 1u, 1u});
	}
	RunCase(probe, input, "PairedScalarExecCopyZeroPreservesInactive", empty_words, empty_observations, {0xffffffffu, 0xffffffffu},
	        expected);

	// Independent words in a SGPR pair must survive SGPR -> VCC -> EXEC.
	const std::vector<uint32_t> pair_words {0xbe840381u, 0xbe8503ffu, 0x80000000u, 0xbeea0404u, 0xbefe046au,
	                                       0x7e0002aau, 0xbf810000u};
	expected.clear();
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		expected.insert(expected.end(), {(lane == 0u || lane == 63u) ? 42u : lane, 1u, 0x80000000u, 0u, 1u});
	}
	RunCase(probe, input, "PairedScalarSgprToVccToExec", pair_words, empty_observations, {0xffffffffu, 0xffffffffu}, expected);
}

} // namespace

void RunWaveLaneCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                        = 1;
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired lane layout unavailable after initialization");
	}
	NativeEmptyFirstCase(probe, input);
	ReadLaneCases(probe, input);
	WriteLaneCases(probe, input);
	ReadFirstCases(probe, input);
	VectorMoveCase(probe, input);
	ScalarExecCopyCases(probe, input);
}

} // namespace Kyty::Libs::Graphics
