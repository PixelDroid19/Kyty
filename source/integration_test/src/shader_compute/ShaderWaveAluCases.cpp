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

// Lowers the words through the real paired path, dispatches one 64-lane
// workgroup and returns one record of observations per lane.
std::vector<uint32_t> Dispatch(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, const std::vector<uint32_t>& words,
                               const std::vector<WaveProbeObservation>& observations, const std::vector<WaveProbeSeed>& seeds)
{
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
	for (size_t word = 64u * observations.size(); word < result.size(); ++word)
	{
		if (result[word] != sentinel) { Fail("output canary overwritten"); }
	}
	result.resize(64u * observations.size());
	return result;
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
	    {"v0", true}, {"exec_lo"}, {"exec_hi"},
	    {"vcc_lo"}, {"vcc_hi"}, {"scc"}};
	const std::vector<WaveProbeSeed> seeds {
	    {"exec_lo", 0x80000001u}, {"exec_hi", 0x80000000u}, {"vcc_lo", 0x13579bdfu}, {"vcc_hi", 0x2468ace0u}, {"scc", 1u}};
	const auto result = Dispatch(probe, input, words, observations, seeds);
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
	const std::vector<WaveProbeObservation> observations {{"v0", true}, {"vcc_lo"}, {"vcc_hi"}};
	const std::vector<WaveProbeSeed> seeds {{"exec_lo", 0x80000001u}, {"exec_hi", 0x80000000u}, {"vcc_lo", vcc_lo}, {"vcc_hi", vcc_hi}};
	const auto result = Dispatch(probe, input, words, observations, seeds);
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
	std::printf("PairedAluAdd3Literal%u PASS\n", literal);
}

// v_mad_u64_u32 v[0:1], vcc, literal, v0, src2: {carry, D} = S0 * S1 + S2.
// The destination aliases src1 (and src2 for the pair); a literal src2 is
// zero-extended and an inline integer sign-extended; VCC holds only active
// lanes' carries.
void RunMadU64Case(VulkanComputeProbe& probe, const ShaderComputeInputInfo& input, uint32_t src2)
{
	constexpr uint32_t multiplier = 0x92492492u;
	constexpr uint32_t high       = 0xfffffff1u;
	const std::vector<uint32_t> words {0x7e0202ffu, high, // v_mov_b32 v1, literal
	                                   0xd5766a00u, 0x000200ffu | (src2 << 18u), multiplier,
	                                   0xbe84046au, // s_mov_b64 s[4:5],vcc
	                                   0xbf810000u};
	const std::vector<WaveProbeObservation> observations {{"v0", true}, {"v1", true}, {"vcc_lo"}, {"vcc_hi"}};
	const std::vector<WaveProbeSeed> seeds {{"exec_lo", 0x80000001u}, {"exec_hi", 0x80000000u}, {"vcc_lo", 0x13579bdfu}, {"vcc_hi", 0x2468ace0u}};
	const auto result = Dispatch(probe, input, words, observations, seeds);
	uint32_t   vcc[2] = {};
	for (const uint32_t lane: {0u, 31u, 63u})
	{
		const uint64_t product = uint64_t {lane} * multiplier;
		const uint64_t addend  = src2 == 256u ? (uint64_t {high} << 32u) | lane : (src2 == 255u ? multiplier : ~uint64_t {0});
		const uint64_t sum     = product + addend;
		vcc[lane / 32u] |= (sum < product ? 1u : 0u) << (lane % 32u);
		const uint32_t expected[] = {static_cast<uint32_t>(sum), static_cast<uint32_t>(sum >> 32u)};
		for (size_t field = 0; field < 2u; ++field)
		{
			if (result[lane * observations.size() + field] != expected[field])
			{
				Fail(String8::FromPrintf("mad_u64 src2=%u lane%u field%zu got%08x expected%08x", src2, lane, field,
				                        result[lane * observations.size() + field], expected[field]).c_str());
			}
		}
	}
	for (uint32_t lane = 0; lane < 64u; ++lane)
	{
		const bool     active     = lane == 0u || lane == 31u || lane == 63u;
		const uint32_t expected[] = {lane, vcc[0], vcc[1]};
		const size_t   fields[]   = {0u, 2u, 3u};
		for (size_t i = active ? 1u : 0u; i < 3u; ++i)
		{
			if (result[lane * observations.size() + fields[i]] != expected[i])
			{
				Fail(String8::FromPrintf("mad_u64 src2=%u lane%u field%zu got%08x expected%08x", src2, lane, fields[i],
				                        result[lane * observations.size() + fields[i]], expected[i]).c_str());
			}
		}
	}
	std::printf("PairedAluMadU64Src2_%u PASS\n", src2);
}

} // namespace

void RunWaveAluCases(VulkanComputeProbe& probe)
{
	ShaderComputeInputInfo input {};
	input.threads_num[0] = 64u;
	input.threads_num[1] = input.threads_num[2] = 1u;
	input.thread_ids_num = 1;
	const ShaderComputeWaveRequest request {{64, 1, 1}, {1, 1, 1}, 0x41, 0, ShaderGuestLaneOrder::LinearXFirst};
	if (ShaderBuildSplitComputeWaveLayout(request, probe.WaveCapabilities(), &input.wave_layout) != ShaderComputeWaveLayoutStatus::Supported)
	{
		Fail("paired ALU layout unavailable");
	}
	// Production marks every split dispatch wave-sensitive with an exact 32-wide subgroup.
	input.native_wave_sensitive  = true;
	input.required_subgroup_size = 32u;
	for (const uint32_t opcode: {0x1bu, 0x1cu, 0x1du, 0x25u, 0x26u})
	{
		RunCase(probe, input, opcode, false);
		RunCase(probe, input, opcode, true);
	}
	RunAdd3Case(probe, input, false);
	RunAdd3Case(probe, input, true);
	for (const uint32_t src2: {256u, 255u, 193u}) // v[0:1], literal, inline -1
	{
		RunMadU64Case(probe, input, src2);
	}
}

} // namespace Kyty::Libs::Graphics
