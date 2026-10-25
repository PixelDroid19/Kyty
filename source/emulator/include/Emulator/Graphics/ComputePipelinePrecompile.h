#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTEPIPELINEPRECOMPILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTEPIPELINEPRECOMPILE_H_

#include "Emulator/Common.h"
#include "Emulator/Graphics/ShaderDescriptorLayoutPlan.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Everything vkCreateComputePipelines consumes for one compute pipeline, so a
// pipeline seen in an earlier session can be compiled again before its first
// dispatch with a driver cache key equal to the one the dispatch will use.
struct ComputePipelineRecipe
{
	uint32_t                   vendor_id              = 0;
	uint32_t                   device_id              = 0;
	uint32_t                   stage_flags            = 0;
	// 0 when the stage chains no required subgroup size.
	uint32_t                   required_subgroup_size = 0;
	uint32_t                   push_constant_offset   = 0;
	// 0 when the layout has no push-constant range.
	uint32_t                   push_constant_size     = 0;
	bool                       has_set_layout         = false;
	ShaderDescriptorLayoutPlan plan;
	std::vector<uint32_t>      spirv;
};

inline constexpr size_t kComputePipelineRecipeMaxSpirvWords = 4u * 1024u * 1024u;

[[nodiscard]] std::vector<uint32_t> ComputePipelineRecipeEncode(const ComputePipelineRecipe& recipe);
// Rejects anything that is not one complete recipe of this schema: wrong magic or
// version, a size that disagrees with the header, too many bindings, an unknown
// descriptor type, or a payload that is not a SPIR-V module.
[[nodiscard]] bool ComputePipelineRecipeDecode(const uint32_t* words, size_t count, ComputePipelineRecipe* recipe);
[[nodiscard]] uint64_t ComputePipelineRecipeHash(const ComputePipelineRecipe& recipe);

// Creates the pipeline described by the recipe and destroys it again; the
// driver's pipeline caches keep the compiled result.
using ComputePipelinePrecompileCreate = bool (*)(const ComputePipelineRecipe& recipe);

// Loads the recipes recorded for this title under `directory` and compiles the
// ones matching this device on background threads, largest module first. The
// title's stores of other translator versions are removed first. Only the first
// call has an effect.
void ComputePipelinePrecompileStart(const std::filesystem::path& directory, uint32_t vendor_id, uint32_t device_id,
                                    ComputePipelinePrecompileCreate create);
// Called by the dispatching thread right before it creates `recipe` itself.
// Records a recipe not seen before, keeps the background thread from starting
// it, and waits for a background compile of it that is already running.
void ComputePipelinePrecompileClaim(const ComputePipelineRecipe& recipe);
// The recipe directory of this title and translator version under the SPIR-V
// cache root, or an empty path when the title is unknown.
[[nodiscard]] std::filesystem::path ComputePipelinePrecompileDirectory();

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_COMPUTEPIPELINEPRECOMPILE_H_ */
