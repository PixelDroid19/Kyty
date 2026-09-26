#include "ShaderWaveScalarCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const char* message)
{
	std::fprintf(stderr, "paired scalar mask failure: %s\n", message);
	std::_Exit(EXIT_FAILURE);
}

void RunCase(VulkanComputeProbe& probe, uint32_t opcode, uint32_t destination, bool save = false, bool zero = false)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired layout unavailable");
	}
	using Pair              = std::array<uint32_t, 2>;
	const Pair            a = save ? Pair {0u, zero ? 0x40000000u : 0x80000000u} : Pair {1u, 0x80000000u};
	const Pair            b = save ? Pair {1u, 0x80000000u} : Pair {0u, 0x80000000u};
	std::vector<uint32_t> words {0x7e020280u, // v1=0 under initial full EXEC
	                             0xbe8403ffu, a[0], 0xbe8503ffu, a[1], 0xbe8603ffu, b[0], 0xbe8703ffu, b[1]};
	Pair                  result {}, exec {0xffffffffu, 0xffffffffu};
	if (save)
	{
		words.insert(words.end(), {0xbefe0406u, 0xbe842404u}); // EXEC=s[6:7]; s_and_saveexec_b64 s[4:5],s[4:5]
		result = b;
		exec   = {a[0] & b[0], a[1] & b[1]};
	} else
	{
		if (destination != 4u)
		{
			words.push_back(0xbe800400u | (destination << 16u) | 4u);
		}
		words.push_back(0x80000000u | (opcode << 23u) | (destination << 16u) | ((zero ? destination : 6u) << 8u) | destination);
		for (size_t half = 0; half < 2; ++half)
		{
			const auto rhs = zero ? a[half] : b[half];
			result[half]   = opcode == 0x0fu ? a[half] & rhs : opcode == 0x11u ? a[half] | rhs : a[half] ^ rhs;
		}
		if (destination == 126u)
		{
			exec = result;
		}
	}
	words.insert(words.end(), {0x7e0202aau, 0xbf810000u}); // v1=42 under resulting EXEC
	const std::string                       low  = destination == 4u ? "s4" : destination == 106u ? "vcc_lo" : "exec_lo";
	const std::string                       high = destination == 4u ? "s5" : destination == 106u ? "vcc_hi" : "exec_hi";
	const std::vector<WaveProbeObservation> observations {
	    {low, low},     {high, high},       {"exec_lo", "exec_lo"},     {"exec_hi", "exec_hi"},
	    {"scc", "scc"}, {"execz", "execz"}, {"v1_low", "v1_high", true}};
	String8 source, error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, {}, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}
	constexpr uint32_t    sentinel     = 0xa5c37e19u;
	const size_t          output_words = 64u * observations.size();
	std::vector<uint32_t> initial(output_words * 2u, sentinel), actual(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &actual, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message.c_str());
	}
	const auto& scc_result = save ? exec : result;
	for (size_t lane = 0; lane < 64u; ++lane)
	{
		const std::array<uint32_t, 7> expected {result[0],
		                                        result[1],
		                                        exec[0],
		                                        exec[1],
		                                        (scc_result[0] | scc_result[1]) != 0u,
		                                        (exec[0] | exec[1]) == 0u,
		                                        ((exec[lane / 32u] >> (lane % 32u)) & 1u) ? 42u : 0u};
		for (size_t slot = 0; slot < expected.size(); ++slot)
		{
			if (actual[lane * expected.size() + slot] != expected[slot])
			{
				Fail(String8::FromPrintf("op%x dst%u save%u zero%u lane%zu slot%zu: %u != %u", opcode, destination, save, zero, lane, slot,
				                         actual[lane * expected.size() + slot], expected[slot])
				         .c_str());
			}
		}
	}
	for (size_t word = output_words; word < actual.size(); ++word)
	{
		if (actual[word] != sentinel)
		{
			Fail("output canary overwritten");
		}
	}
	std::printf("PairedScalarMask op%x dst%u save%u zero%u PASS\n", opcode, destination, save, zero);
}

} // namespace

void RunWaveScalarCases(VulkanComputeProbe& probe)
{
	for (uint32_t opcode: {0x0fu, 0x11u, 0x13u})
	{
		for (uint32_t destination: {4u, 106u, 126u})
		{
			RunCase(probe, opcode, destination);
		}
	}
	RunCase(probe, 0x13u, 126u, false, true);
	RunCase(probe, 0x24u, 4u, true);
	RunCase(probe, 0x24u, 4u, true, true);
}

} // namespace Kyty::Libs::Graphics
