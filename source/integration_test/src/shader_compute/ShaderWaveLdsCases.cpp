#include "ShaderWaveLdsCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "paired LDS numerical failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunLdsCase(VulkanComputeProbe& probe, bool only_upper_lane)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 128u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num                        = 1;
	input.lds_dwords                            = 1;
	const ShaderComputeWaveRequest request {{128, 1, 1}, {1, 1, 1}, 0x41, 1, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("two-wave shared LDS layout unavailable");
	}
	// Guest instructions initialize counter only from logical lane0, restore
	// full EXEC and synchronize both waves. No test-only LDS initialization or
	// substituted atomic/barrier lowering is involved.
	std::vector<uint32_t>                   words = {0x7e020280u,                        // v_mov_b32 v1,0 (byte address)
	                                                   0x7e040281u,                      // v_mov_b32 v2,1 (increment)
	                                                   0x7e0602aau,                      // v_mov_b32 v3,42 (inactive result)
	                                                   0xd4c2006au, 256u | (128u << 9u), // v_cmp_eq_u32 vcc,v0,0
	                                                   0xbefe046au,                      // s_mov_b64 exec,vcc
	                                                   0xd8340000u, 0x00000101u,         // ds_write_b32 v1,v1
	                                                   0xbefe04c1u,                      // s_mov_b64 exec,-1
	                                                   0xbf8a0000u};                     // s_barrier
	if (only_upper_lane)
	{
		words.insert(words.end(), {0xd4c2006au, 256u | (191u << 9u), 0xbefe046au}); // select logical lane63
	}
	words.insert(words.end(), {0xd8800000u, 0x03000201u, // ds_add_rtn_u32 v3,v1,v2
	                          0xbefe04c1u, 0xbf8a0000u, // restore full EXEC; s_barrier
	                          0xd8d80000u, 0x04000001u, // ds_read_b32 v4,v1
	                          0xbf810000u});
	const std::vector<WaveProbeObservation> observations {{"v3_low", "v3_high", true}, {"v4_low", "v4_high", true}};
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
	constexpr uint32_t    sentinel     = 0xa5c37e19u;
	constexpr size_t      output_words = 128u * 2u;
	std::vector<uint32_t> initial(output_words * 2u, sentinel), result(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message);
	}
	std::vector<uint32_t> old_values;
	const uint32_t expected_counter = only_upper_lane ? 1u : 128u;
	for (size_t lane = 0; lane < 128u; ++lane)
	{
		old_values.push_back(result[lane * 2u]);
		if (result[lane * 2u + 1u] != expected_counter)
		{
			Fail(String8::FromPrintf("lane%zu counter=%u expected%u", lane, result[lane * 2u + 1u], expected_counter).c_str());
		}
		if (only_upper_lane && result[lane * 2u] != (lane == 63u ? 0u : 42u))
		{
			Fail("inactive atomic destination changed or active upper-bank return is wrong");
		}
	}
	std::sort(old_values.begin(), old_values.end());
	for (size_t value = 0; value < old_values.size(); ++value)
	{
		if (!only_upper_lane && old_values[value] != value)
		{
			Fail("atomic returns are not a permutation of0..127");
		}
	}
	if (!std::all_of(result.begin() + output_words, result.end(), [](uint32_t value) { return value == sentinel; }))
	{
		Fail("shared LDS output canary overwritten");
	}
	std::puts(only_upper_lane ? "PairedLdsAtomicUpperLaneInactivePreservation PASS" : "PairedLdsAtomicReturnTwoWaves128 PASS");
}

} // namespace

void RunWaveLdsCases(VulkanComputeProbe& probe)
{
	RunLdsCase(probe, false);
	RunLdsCase(probe, true);
}

} // namespace Kyty::Libs::Graphics
