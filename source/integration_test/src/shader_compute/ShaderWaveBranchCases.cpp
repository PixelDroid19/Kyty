#include "ShaderWaveBranchCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {
namespace {

struct ExecBranchCase
{
	const char* name;
	uint32_t    branch_word;
	uint32_t    fallthrough_marker;
	uint32_t    alternate_marker;
	uint32_t    nonempty_marker;
	uint32_t    empty_marker;
};

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "wave EXEC branch numerical failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

void RunExecBranchCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, const ExecBranchCase& test)
{
	// SOPP offsets are signed dword offsets from PC+4. This is a forward
	// diamond: the compare/EXEC setup ends at PC 16; s_cbranch_exec{z,nz}
	// targets PC 32 from PC 16 (+3 dwords). s_branch at PC 28 targets the
	// shared join at PC 40 (+2 dwords). Each s_mov_b32 s8 literal is two dwords.
	const uint32_t                          words[] = {0xbf8a0000u, // s_barrier
	                                                   0xd4c2006au, // v_cmp_eq_u32 vcc,v0,63
	                                                   256u | (191u << 9u),
	                                                   0xbefe046au, // s_mov_b64 exec,vcc
	                                                   test.branch_word,
	                                                   0xbe8803ffu, // s_mov_b32 s8, fallthrough_marker
	                                                   test.fallthrough_marker,
	                                                   0xbf820002u, // s_branch join (PC 40)
	                                                   0xbe8803ffu, // alternate arm: s_mov_b32 s8, alternate_marker
	                                                   test.alternate_marker,
	                                                   0xbf810000u}; // s_endpgm (shared join)
	const std::vector<WaveProbeObservation> observations {{"s8", "s8"}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}, {"execz", "execz"}};

	String8 source;
	String8 error;
	if (!BuildWaveProbeSource(words, sizeof(words), input, observations, {}, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error) || binary.IsEmpty())
	{
		Fail(error.c_str());
	}

	constexpr uint32_t sentinel      = 0xa5c37e19u;
	constexpr uint32_t stride        = 4u;
	const uint32_t     logical_count = input.wave_layout.waves * 64u;
	if (input.wave_layout.waves != 2u || logical_count != 128u)
	{
		Fail(String8::FromPrintf("%s: fixture requires exactly two guest waves, got %u", test.name, input.wave_layout.waves).c_str());
	}
	std::vector<uint32_t> initial((logical_count + 64u) * stride, sentinel);
	std::vector<uint32_t> actual(initial.size());
	std::string           message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &actual, &message) !=
	    VulkanComputeProbe::Result::Success)
	{
		Fail(message);
	}

	for (uint32_t lane = 0; lane < logical_count; ++lane)
	{
		const bool     wave0      = lane < 64u;
		const uint32_t expected[] = {wave0 ? test.nonempty_marker : test.empty_marker, 0u, wave0 ? 0x80000000u : 0u, wave0 ? 0u : 1u};
		for (uint32_t slot = 0; slot < stride; ++slot)
		{
			if (actual[lane * stride + slot] != expected[slot])
			{
				Fail(String8::FromPrintf("%s: logical_lane=%u word=%u actual=%08x expected=%08x", test.name, lane, slot,
				                         actual[lane * stride + slot], expected[slot])
				         .c_str());
			}
		}
	}
	for (size_t word = logical_count * stride; word < actual.size(); ++word)
	{
		if (actual[word] != sentinel)
		{
			Fail(
			    String8::FromPrintf("%s: tail canary word=%zu actual=%08x expected=%08x", test.name, word, actual[word], sentinel).c_str());
		}
	}
	std::printf("%s PASS\n", test.name);
}

} // namespace

void RunWaveBranchCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 128;
	input.threads_num[1] = input.threads_num[2] = 1;
	input.thread_ids_num                        = 1;
	const ShaderComputeWaveRequest request {{128, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) !=
	    ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("two-wave paired layout must be supported after wave initialization");
	}

	const ExecBranchCase cases[] = {
	    {"PairedExeczUpperOnlyVersusEmpty", 0xbf880003u, 0xec000001u, 0xec000000u, 0xec000001u, 0xec000000u},
	    {"PairedExecnzUpperOnlyVersusEmpty", 0xbf890003u, 0xec000000u, 0xec000001u, 0xec000001u, 0xec000000u},
	};
	for (const auto& test: cases)
	{
		RunExecBranchCase(probe, input, test);
	}
}

} // namespace Kyty::Libs::Graphics
