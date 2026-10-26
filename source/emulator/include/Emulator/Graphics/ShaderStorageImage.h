#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_

#include "Emulator/Common.h"
#include "Emulator/Graphics/Shader.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Every writable descriptor is accessed through a typed view of the single
// storage binding: its shape (2D, 2D array or 3D) and either the formatless
// float type or its exact uint format. Each distinct view declares one aliased
// SPIR-V variable over that binding; the view of the first writable descriptor
// keeps the primary names. Sampled (read-only) descriptors never participate.
struct ShaderStorageImagePlan
{
	static constexpr int SHAPES = 3;
	static constexpr int TYPES = 8; // formatless float, then the exact uint formats
	static constexpr int VIEWS = SHAPES * TYPES;

	bool supported = true;
	bool has_storage = false;
	bool formatless = false;
	bool extended_formats = false;
	int primary_view = -1;
	bool view_used[VIEWS] = {};
	int8_t descriptor_view[ShaderTextureResources::RES_MAX] = {};
	int descriptor_index = -1;
	uint16_t guest_format = 0;
	uint32_t instruction_pc = 0;
	const char* reason = "supported";
};

[[nodiscard]] ShaderStorageImagePlan ShaderPlanStorageImages(const ShaderCode& code, const ShaderBindResources* bind);

// The SPIR-V names, shape and element type of one view of the plan.
struct ShaderStorageImageView
{
	String8 image_type; // OpTypeImage id, also the stem of its array and pointer types
	String8 variable;
	ShaderGen5SampledTextureShape shape = ShaderGen5SampledTextureShape::TwoDimensional;
	bool unsigned_integer = false;
	const char* image_format = "Unknown"; // exact SPIR-V Image Format enumerant
};

[[nodiscard]] ShaderStorageImageView ShaderStorageImageViewOf(const ShaderStorageImagePlan& plan, int view);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSTORAGEIMAGE_H_
