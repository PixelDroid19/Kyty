#include "ShaderWaveMaskSource.h"

#include "ShaderWaveProbeSource.h"

namespace Kyty::Libs::Graphics {

bool BuildWaveMaskProbeSource(uint32_t compare_opcode, uint32_t compare_value, const ShaderComputeInputInfo& input,
                              const std::array<uint32_t, 2>* exec_seed, String8* source, String8* error, uint32_t destination,
                              uint32_t source_vgpr)
{
	if (source == nullptr || error == nullptr || compare_value > 64u || (compare_opcode != 0xc1u && compare_opcode != 0xc2u) ||
	    (destination != 106u && destination > 102u) || source_vgpr > 2u)
	{
		if (error != nullptr)
		{
			*error = "invalid wave comparison fixture";
		}
		return false;
	}
	const uint32_t compare_word = (0xd4u << 24u) | (compare_opcode << 16u) | destination;
	const uint32_t sources_word = (256u + source_vgpr) | ((128u + compare_value) << 9u);
	// Repeat the operation to detect per-instruction temporary-ID collisions.
	const uint32_t                          words[] = {compare_word, sources_word, compare_word, sources_word, 0xbf810000u};
	const std::string                       low     = destination == 106u ? "vcc_lo" : "s" + std::to_string(destination);
	const std::string                       high    = destination == 106u ? "vcc_hi" : "s" + std::to_string(destination + 1u);
	const std::vector<WaveProbeObservation> observations {
	    {low, low}, {high, high}, {"scc", "scc"}, {"exec_lo", "exec_lo"}, {"exec_hi", "exec_hi"}};
	std::vector<WaveProbeSeed> seeds {{"scc", 1u}};
	if (exec_seed != nullptr)
	{
		seeds.push_back({"exec_lo", (*exec_seed)[0]});
		seeds.push_back({"exec_hi", (*exec_seed)[1]});
	}
	return BuildWaveProbeSource(words, sizeof(words), input, observations, seeds, source, error);
}

} // namespace Kyty::Libs::Graphics
