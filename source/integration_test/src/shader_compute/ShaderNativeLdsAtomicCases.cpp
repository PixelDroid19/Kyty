#include "ShaderNativeLdsAtomicCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const char* message)
{
	std::fprintf(stderr, "native LDS atomic failure: %s\n", message);
	std::_Exit(EXIT_FAILURE);
}

void RunCase(VulkanComputeProbe& probe, uint32_t destination, bool active, uint32_t offset, bool contended = false)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 32;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	input.lds_dwords                            = 128;
	ShaderComputeWaveLayout        dispatch_layout {};
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 128, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &dispatch_layout) != ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("physical32 dispatch unavailable");
	}
	// Native32 lowering, one distinct LDS word per physical invocation. The
	// observer duplicates each native result into two slots; this is not wave64.
	std::vector<uint32_t> words {0x34020082u, // v_lshlrev_b32 v1,2,v0
	                             0x4a0a02ffu,
	                             offset, // v_add_u32 v5,offset,v1
	                             0x7e040287u,
	                             0x7e0602aau, // v2=7, v3=42
	                             0xd8340000u,
	                             0x00000305u,                        // ds_write_b32 v5,v3 (42)
	                             0xbf8a0000u,                        // s_barrier
	                             active ? 0xbefe04c1u : 0xbefe0480u, // EXEC=-1 or 0
	                             0xd8800000u | offset,
	                             (destination << 24u) | 0x201u,
	                             0xbefe04c1u,
	                             0xbf8a0000u, // restore EXEC; s_barrier
	                             0xd8d80000u,
	                             0x04000005u, // ds_read_b32 v4,v5
	                             0xbf810000u};
	if (contended)
	{
		// All words were initialized by distinct invocations and synchronized.
		// Now every atomic/read targets word0, initialized by invocation0 only.
		words.insert(words.begin() + 8, {0x7e020280u, 0x7e0a0280u}); // v1=0; v5=0
	}
	const auto                              register_name = "v" + std::to_string(destination);
	const std::vector<WaveProbeObservation> observations {{register_name, register_name, true}, {"v4", "v4", true}};
	String8                                 source, error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, {}, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}
	constexpr uint32_t    sentinel = 0xa5c37e19u;
	std::vector<uint32_t> initial(256u, sentinel), result(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), dispatch_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message.c_str());
	}
	std::vector<uint32_t> returned;
	for (size_t word = 0; word < result.size(); ++word)
	{
		const uint32_t lane            = (word / 2u) % 32u;
		const uint32_t old_destination = destination == 1u ? lane * 4u : destination == 2u ? 7u : 42u;
		if (contended && word < 128u && word % 2u == 0u)
		{
			if (word < 64u)
			{
				returned.push_back(result[word]);
			} else if (result[word] != result[word - 64u])
			{
				Fail("native observer duplicate differs");
			}
			continue;
		}
		const uint32_t counter  = contended ? 266u : active ? 49u : 42u;
		const uint32_t expected = word >= 128u ? sentinel : word % 2u == 0u ? (active ? 42u : old_destination) : counter;
		if (result[word] != expected)
		{
			Fail(String8::FromPrintf("dst%u active%u offset%u word%zu: %u != %u", destination, active, offset, word, result[word], expected)
			         .c_str());
		}
	}
	std::sort(returned.begin(), returned.end());
	for (size_t ordinal = 0; ordinal < returned.size(); ++ordinal)
	{
		if (returned[ordinal] != 42u + 7u * ordinal)
		{
			Fail("contended atomic old-value multiset differs");
		}
	}
	std::printf("NativeLdsAtomicReturn dst%u active%u offset%u contended%u PASS\n", destination, active, offset, contended);
}

} // namespace

void RunNativeLdsAtomicCases(VulkanComputeProbe& probe)
{
	for (uint32_t destination: {1u, 2u, 3u})
	{
		RunCase(probe, destination, true, 0u);
		RunCase(probe, destination, false, 0u);
	}
	RunCase(probe, 1u, true, 256u);
	RunCase(probe, 3u, true, 0u, true);
}

} // namespace Kyty::Libs::Graphics
