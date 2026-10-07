#ifndef KYTY_INTEGRATION_TEST_SHADER_COMPUTE_SHADER_PROBE_SOURCE_H_
#define KYTY_INTEGRATION_TEST_SHADER_COMPUTE_SHADER_PROBE_SOURCE_H_

#include "Kyty/Core/String8.h"
#include "Emulator/Graphics/Shader.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

struct ScalarRegisterSeed
{
	uint32_t reg   = 0;
	uint32_t value = 0;
};

// Produces the real Kyty compute translation with a test-only SSBO export.
// result_register < 0 stores zero in output word 0; words 1..3 always export
// SCC, EXEC_LO, and EXEC_HI respectively.
[[nodiscard]] bool BuildScalarProbeSource(const uint32_t* shader_words, size_t shader_word_count,
	                                          const std::vector<ScalarRegisterSeed>& register_seeds, int result_register,
	                                          Kyty::Core::String8* source, Kyty::Core::String8* error);

// Builds a probe from already-configured production input metadata. In
// particular, callers must pass the final push-constant/UBO layout unchanged;
// this helper does not recalculate binding indices. When result_register_high
// is nonnegative, output words 0..4 are low, high, SCC, EXEC_LO, EXEC_HI.
[[nodiscard]] bool BuildScalarProbeSource(const uint32_t* shader_words, size_t shader_word_count,
	                                          const std::vector<ScalarRegisterSeed>& register_seeds,
	                                          int result_register, int result_register_high,
	                                          const ShaderComputeInputInfo& input_info,
	                                          Kyty::Core::String8* source, Kyty::Core::String8* error,
	                                          bool nonempty_exec = false);

} // namespace Kyty::Libs::Graphics

#endif
