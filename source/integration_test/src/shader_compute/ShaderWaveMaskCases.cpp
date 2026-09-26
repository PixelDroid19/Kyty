#include "ShaderWaveMaskCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveMaskSource.h"
#include "VulkanComputeProbe.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

struct MaskCase
{
	const char*             name;
	uint32_t                opcode;
	uint32_t                value;
	std::array<uint32_t, 2> expected;
	bool                    seed_exec = false;
	std::array<uint32_t, 2> exec {0xffffffffu, 0xffffffffu};
	uint32_t                destination = 106u;
};

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "wave mask numerical failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunMaskCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, const MaskCase& test, uint32_t source_vgpr = 0u,
                 const std::vector<std::array<uint32_t, 2>>& wave_masks = {})
{
	String8 source;
	String8 error;
	if (!BuildWaveMaskProbeSource(test.opcode, test.value, input, test.seed_exec ? &test.exec : nullptr, &source, &error, test.destination,
	                              source_vgpr))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}

	// The extra wave-sized tail is a canary: it safely catches the old guest
	// LocalSize=64 path running two native32 subgroups instead of one paired wave.
	constexpr uint32_t    sentinel      = 0xa5c37e19u;
	const uint32_t        logical_count = input.wave_layout.waves * 64u;
	std::vector<uint32_t> initial((logical_count + 64u) * 5u, sentinel);
	std::vector<uint32_t> results(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &results, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message);
	}
	for (uint32_t lane = 0; lane < logical_count; ++lane)
	{
		const auto&    mask       = wave_masks.empty() ? test.expected : wave_masks.at(lane / 64u);
		const uint32_t expected[] = {mask[0], mask[1], 1u, test.exec[0], test.exec[1]};
		for (uint32_t word = 0; word < 5u; ++word)
		{
			if (results[lane * 5u + word] != expected[word])
			{
				Fail(String8::FromPrintf("%s: logical_lane=%u word=%u actual=%08x expected=%08x", test.name, lane, word,
				                         results[lane * 5u + word], expected[word])
				         .c_str());
			}
		}
	}
	for (size_t word = logical_count * 5u; word < results.size(); ++word)
	{
		if (results[word] != sentinel)
		{
			Fail(String8::FromPrintf("%s: canary word=%zu actual=%08x expected=%08x logical_count=%u", test.name, word, results[word],
			                         sentinel, logical_count)
			         .c_str());
		}
	}
	std::printf("%s PASS\n", test.name);
}

} // namespace

void RunWaveMaskCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired layout must be supported after wave initialization");
	}
	const MaskCase cases[] = {
	    {"PairedLane63PackedMask", 0xc2u, 63u, {0u, 0x80000000u}},
	    {"PairedZeroPredicate", 0xc2u, 64u, {0u, 0u}},
	    {"PairedLowHalfMask", 0xc1u, 32u, {0xffffffffu, 0u}},
	    {"PairedBothHalvesMask", 0xc1u, 64u, {0xffffffffu, 0xffffffffu}},
	    {"PairedUpperOnlyExec", 0xc1u, 64u, {0u, 0xffffffffu}, true, {0u, 0xffffffffu}},
	    {"PairedPartialExec", 0xc1u, 64u, {0x80000001u, 0x81008001u}, true, {0x80000001u, 0x81008001u}},
	    {"PairedEmptyExec", 0xc1u, 64u, {0u, 0u}, true, {0u, 0u}},
	    {"PairedOrdinaryScalarMaskPair", 0xc2u, 63u, {0u, 0x80000000u}, false, {0xffffffffu, 0xffffffffu}, 4u},
	};
	for (const auto& test: cases)
	{
		RunMaskCase(probe, input, test);
	}

	// Two waves and all three guest axes: the oracle is computed from linear
	// guest indices, independently of the physical workgroup coordinates.
	const ShaderComputeWaveRequest xyz_request {{8, 4, 4}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	input.threads_num[0] = 8;
	input.threads_num[1] = input.threads_num[2] = 4;
	input.thread_ids_num                        = 3;
	if (ShaderBuildPairedComputeWaveLayout(xyz_request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("multidimensional paired layout rejected");
	}
	for (uint32_t axis = 0; axis < 3u; ++axis)
	{
		const uint32_t                       value = input.threads_num[axis] - 1u;
		std::vector<std::array<uint32_t, 2>> masks(2u, {0u, 0u});
		for (uint32_t logical = 0; logical < 128u; ++logical)
		{
			const uint32_t coords[] = {logical % 8u, (logical / 8u) % 4u, logical / 32u};
			if (coords[axis] == value)
			{
				masks[logical / 64u][(logical % 64u) / 32u] |= 1u << (logical % 32u);
			}
		}
		const auto name = String8::FromPrintf("PairedLogicalCoordinateAxis%u", axis);
		RunMaskCase(probe, input, {name.c_str(), 0xc2u, value, {0u, 0u}}, axis, masks);
	}
}

} // namespace Kyty::Libs::Graphics
