#include "ShaderWaveAluCases.h"

#include "../../../emulator/src/Graphics/ShaderSpirvToolchain.h"
#include "ShaderWaveProbeSource.h"
#include "VulkanComputeProbe.h"

#include <cstdio>
#include <cstdlib>

namespace Kyty::Libs::Graphics {
namespace {

[[noreturn]] void Fail(const std::string& message)
{
	std::fprintf(stderr, "paired ALU numerical failure: %s\n", message.c_str());
	std::fflush(stderr);
	std::_Exit(EXIT_FAILURE);
}

uint32_t Expected(uint32_t opcode, uint32_t left, uint32_t right)
{
	switch (opcode)
	{
		case 0x1b: return left & right;
		case 0x1c: return left | right;
		case 0x1d: return left ^ right;
		case 0x25: return left + right;
		case 0x26: return left - right;
		default: Fail("invalid test opcode");
	}
}

void RunCase(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, uint32_t opcode, bool destination_is_src0)
{
	// LLVM gfx1030 independently decodes these E32 operations. Each writes
	// v0 while reading its old value; a literal tests wraparound and bit patterns.
	const uint32_t other = 0xfffffff1u;
	const uint32_t alu = (opcode << 25u) | (destination_is_src0 ? (1u << 9u) | 256u : 255u);
	std::vector<uint32_t> words {0x7e0202ffu, other, alu}; // v_mov v1,literal
	if (!destination_is_src0) { words.push_back(other); }
	// A real scalar read makes the otherwise-unused VCC pair architectural
	// program state, so the observer never seeds an undeclared variable.
	words.push_back(0xbe84046au); // s_mov_b64 s[4:5],vcc
	words.push_back(0xbf810000u);
	const std::vector<WaveProbeObservation> observations {
	    {"v0_low", "v0_high", true}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"},
	    {"vcc_lo", "vcc_lo"}, {"vcc_hi", "vcc_hi"}, {"scc", "scc"}};
	const std::vector<WaveProbeSeed> seeds {
	    {"exec_lo", 0x80000001u}, {"exec_hi", 0x80000000u}, {"vcc_lo", 0x13579bdfu}, {"vcc_hi", 0x2468ace0u}, {"scc", 1u}};
	String8 source, error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, seeds, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error)) { Fail(error.c_str()); }
	constexpr uint32_t sentinel = 0xa5c37e19u;
	std::vector<uint32_t> initial(64u * observations.size() * 2u, sentinel), result(initial.size());
	std::string message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success) { Fail(message); }
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		const bool active = lane == 0u || lane == 31u || lane == 63u;
		const auto value = active ? Expected(opcode, destination_is_src0 ? lane : other, destination_is_src0 ? other : lane) : lane;
		const uint32_t expected[] = {value, 0x80000001u, 0x80000000u, 0x13579bdfu, 0x2468ace0u, 1u};
		for (size_t field = 0; field < observations.size(); ++field)
		{
			if (result[lane * observations.size() + field] != expected[field])
			{
				Fail(String8::FromPrintf("opcode%02x alias%u lane%u field%zu got%08x expected%08x", opcode,
				                        destination_is_src0, lane, field, result[lane * observations.size() + field], expected[field]).c_str());
			}
		}
	}
	for (size_t word = 64u * observations.size(); word < result.size(); ++word)
	{
		if (result[word] != sentinel) { Fail("output canary overwritten"); }
	}
	std::printf("PairedAluOpcode%02xAlias%u PASS\n", opcode, destination_is_src0);
}

// v_add3_u32 v0, vcc_lo|vcc_hi, vcc_hi|literal, v0: VCC words are scalar
// data, the destination aliases src2, and inactive lanes keep their old value.
void RunAdd3Case(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, bool literal)
{
	constexpr uint32_t vcc_lo = 0x13579bdfu;
	constexpr uint32_t vcc_hi = 0x2468ace0u;
	constexpr uint32_t other  = 0xfffffff1u;
	std::vector<uint32_t> words {0xd76d0000u, literal ? 0x0401fe6bu : 0x0400d66au};
	if (literal) { words.push_back(other); }
	words.push_back(0xbe84046au); // s_mov_b64 s[4:5],vcc keeps VCC architectural
	words.push_back(0xbf810000u);
	const std::vector<WaveProbeObservation> observations {{"v0_low", "v0_high", true}, {"vcc_lo", "vcc_lo"}, {"vcc_hi", "vcc_hi"}};
	const std::vector<WaveProbeSeed> seeds {{"exec_lo", 0x80000001u}, {"exec_hi", 0x80000000u}, {"vcc_lo", vcc_lo}, {"vcc_hi", vcc_hi}};
	String8 source, error;
	if (!BuildWaveProbeSource(words.data(), words.size() * sizeof(uint32_t), input, observations, seeds, &source, &error))
	{
		Fail(error.c_str());
	}
	Vector<uint32_t> binary;
	if (!ShaderToolchain::Run(source, &binary, &error)) { Fail(error.c_str()); }
	constexpr uint32_t sentinel = 0xa5c37e19u;
	std::vector<uint32_t> initial(64u * observations.size() * 2u, sentinel), result(initial.size());
	std::string message;
	if (probe.DispatchWave(binary.GetDataConst(), binary.Size(), input.wave_layout, {1, 1, 1}, initial, &result, &message) !=
	    VulkanComputeProbe::Result::Success) { Fail(message); }
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		const bool     active     = lane == 0u || lane == 31u || lane == 63u;
		const uint32_t sum        = literal ? vcc_hi + other + lane : vcc_lo + vcc_hi + lane;
		const uint32_t expected[] = {active ? sum : lane, vcc_lo, vcc_hi};
		for (size_t field = 0; field < observations.size(); ++field)
		{
			if (result[lane * observations.size() + field] != expected[field])
			{
				Fail(String8::FromPrintf("add3 literal%u lane%u field%zu got%08x expected%08x", literal, lane, field,
				                        result[lane * observations.size() + field], expected[field]).c_str());
			}
		}
	}
	for (size_t word = 64u * observations.size(); word < result.size(); ++word)
	{
		if (result[word] != sentinel) { Fail("output canary overwritten"); }
	}
	std::printf("PairedAluAdd3Literal%u PASS\n", literal);
}

} // namespace

void RunWaveAluCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num = 1;
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildPairedComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) != ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired ALU layout unavailable");
	}
	for (const uint32_t opcode: {0x1bu, 0x1cu, 0x1du, 0x25u, 0x26u})
	{
		RunCase(probe, input, opcode, false);
		RunCase(probe, input, opcode, true);
	}
	RunAdd3Case(probe, input, false);
	RunAdd3Case(probe, input, true);
}

} // namespace Kyty::Libs::Graphics
