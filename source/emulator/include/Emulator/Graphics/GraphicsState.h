#ifndef EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
#define EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED

#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::State {

struct ScissorRect
{
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

struct DepthStencilUsage
{
	bool target_active      = false;
	bool depth_write_enable = false;
};

enum class StencilPlaneValidation
{
	Inactive,
	Valid,
	MissingReadBase,
	MissingWriteBase,
	MismatchedBases,
};

struct ViewportDepthRange
{
	float min_depth = 0.0f;
	float max_depth = 1.0f;
};

struct ViewportXy
{
	float x      = 0.0f;
	float y      = 0.0f;
	float width  = 0.0f;
	float height = 0.0f;
};

[[nodiscard]] ViewportXy ResolveViewportXy(float xscale, float xoffset, float yscale, float yoffset);

// Register DEPTH_CLEAR_ENABLE suppresses shader Z writes. HTILE clear metadata only
// means the surface reads as cleared and needs a Vulkan load-clear; it must not
// suppress depth writes on an otherwise normal draw.
struct DepthClearActions
{
	bool vulkan_clear         = false;
	bool suppress_depth_write = false;
};

void SetGenericScissorTl(HW::Context& context, uint32_t value);
void SetGenericScissorBr(HW::Context& context, uint32_t value);
void SetScreenScissorTl(HW::Context& context, uint32_t value);
void SetScreenScissorBr(HW::Context& context, uint32_t value);
void SetRenderControl(HW::Context& context, uint32_t value);
void SetDepthControl(HW::Context& context, uint32_t value);
void ApplyDepthStencilPlaneRegisters(HW::DepthRenderTarget& target, uint32_t stencil_info,
                                     uint32_t stencil_read_base, uint32_t stencil_write_base);
void ApplyDepthStencilPlaneRegisters(HW::Context& context, uint32_t stencil_info, uint32_t stencil_read_base,
                                     uint32_t stencil_write_base);
void SetStencilControl(HW::Context& context, uint32_t value);
void SetStencilRefMask(HW::Context& context, uint32_t value);
void SetStencilRefMaskBf(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);
[[nodiscard]] DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                         const HW::DepthControl& depth_control);
[[nodiscard]] StencilPlaneValidation ValidateStencilPlane(const HW::DepthRenderTarget& target,
                                                          const HW::RenderControl& render_control,
                                                          const HW::DepthControl& depth_control);

// AMD VTE window Z: OpenGL clip ([-W,+W]) uses zoffset±zscale; DX clip ([0,+W]) uses [zoffset, zoffset+zscale].
// Without VK_EXT_depth_range_unrestricted, clamp to [0,1] and pair with negativeOneToOne for OpenGL clip.
[[nodiscard]] ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted);

[[nodiscard]] DepthClearActions ResolveDepthClearActions(bool register_depth_clear, bool htile_meta_clear);

// CB_TARGET_MASK / CB_SHADER_MASK: four bits per MRT (RGBA). The one-argument
// form validates all eight slots; the bounded form ignores mask bits for slots
// whose CB_COLORn_BASE is not configured by the current render pass.
enum class ColorTargetLayoutError
{
	None,
	Gapped,
	PartialChannel,
};

struct ColorTargetLayout
{
	static constexpr uint32_t kMaxTargets = 8;
	uint32_t                  count       = 0;
	uint8_t                   nibbles[kMaxTargets] {};
	ColorTargetLayoutError    error       = ColorTargetLayoutError::None;
};

[[nodiscard]] ColorTargetLayout ResolveColorTargetLayout(uint32_t mask);
[[nodiscard]] ColorTargetLayout ResolveColorTargetLayout(uint32_t mask, uint32_t configured_target_count);

// A sampled surface may reuse a render target or storage texture when
// FindRenderTexture / FindStorageTexture found a live object (Equals, non-exact
// IsContainedWithin, or Contains). Matching dimensions alone do not establish
// identity.
enum class Gen5SampleBacking
{
	ExactRenderTarget,
	GuestMemoryTexture,
	Unsupported,
};

// exact_render_target_found: true when a live RT or StorageTexture alias was
// found (exact Equals, non-exact IsContainedWithin, or Contains).
[[nodiscard]] Gen5SampleBacking ResolveGen5SampleBacking(uint32_t fmt, uint32_t tile, bool exact_render_target_found);

enum class ImageSampleOperation
{
	Regular,
	DepthReference,
};

enum class SamplerAddressMode
{
	Repeat,
	MirroredRepeat,
	ClampToEdge,
	ClampToBorder,
};

struct SamplerComparison
{
	bool    enabled  = false;
	uint8_t function = 0;
};

[[nodiscard]] SamplerAddressMode ResolveSamplerAddressMode(uint8_t sq_tex_clamp);
// Vulkan requires sampler comparison state to agree with the SPIR-V image instruction.
[[nodiscard]] SamplerComparison ResolveSamplerComparison(uint8_t depth_compare_function, ImageSampleOperation operation);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
