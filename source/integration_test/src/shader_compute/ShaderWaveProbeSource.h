#pragma once

#include "Emulator/Graphics/Shader.h"

#include <cstddef>
#include <string>
#include <vector>

namespace Kyty::Libs::Graphics {

struct WaveProbeObservation
{
	std::string low;
	std::string high;
	bool        floating = false;
};

struct WaveProbeSeed
{
	std::string name;
	uint32_t    value;
};

// Test-only instrumentation around real bounded parsing and lowering.
// Seeds scalar inputs and observes registers; never substitutes guest operations.
[[nodiscard]] bool BuildWaveProbeSource(const uint32_t* words, size_t byte_count, const ShaderComputeInputInfo& input,
                                        const std::vector<WaveProbeObservation>& observations, const std::vector<WaveProbeSeed>& seeds,
                                        String8* source, String8* error, uint32_t output_binding = 0);

} // namespace Kyty::Libs::Graphics
