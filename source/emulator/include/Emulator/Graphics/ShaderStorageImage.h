#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

class ShaderCode;
struct ShaderBindResources;

// The primary storage array has one SPIR-V sampled type, image format and image
// shape (2D, 2D array or 3D; mixed shapes need separate banks). The established
// atomic path additionally declares a 2D R32ui alias. Sampled (read-only)
// descriptors never participate in this plan.
struct ShaderStorageImagePlan
{
	bool supported = true;
	bool has_storage = false;
	bool unsigned_primary = false;
	bool formatless = false;
	bool extended_formats = false;
	bool three_dimensional = false;
	const char* image_format = "Unknown";
	int descriptor_index = -1;
	uint16_t guest_format = 0;
	uint32_t instruction_pc = 0;
	const char* reason = "supported";
};

[[nodiscard]] ShaderStorageImagePlan ShaderPlanStorageImages(const ShaderCode& code, const ShaderBindResources* bind);
// These enforce the same admission used by WriteHeader, before image types
// are declared. The returned token is an exact SPIR-V Image Format enumerant.
[[nodiscard]] const char* GetStorageImageFormat(const ShaderCode& code, const ShaderBindResources* bind);
[[nodiscard]] bool UsesExtendedStorageImageFormats(const ShaderCode& code, const ShaderBindResources* bind);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_
