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

struct ViewportDepthRange
{
	float min_depth = 0.0f;
	float max_depth = 1.0f;
};

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
void SetModeControl(HW::Context& context, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);
[[nodiscard]] DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                         const HW::DepthControl& depth_control);

// AMD VTE window Z: OpenGL clip ([-W,+W]) uses zoffset±zscale; DX clip ([0,+W]) uses [zoffset, zoffset+zscale].
// Without VK_EXT_depth_range_unrestricted, clamp to [0,1] and pair with negativeOneToOne for OpenGL clip.
[[nodiscard]] ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted);

[[nodiscard]] DepthClearActions ResolveDepthClearActions(bool register_depth_clear, bool htile_meta_clear);

// CB_TARGET_MASK / CB_SHADER_MASK: four bits per MRT (RGBA). Observed Gen5
// contract accepts only contiguous full-channel (0xf) nibbles from RT0.
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

// A sampled surface may reuse a render target only when GpuMemory found the
// exact live object. Matching dimensions do not establish identity or content.
enum class Gen5SampleBacking
{
	ExactRenderTarget,
	GuestMemoryTexture,
	Unsupported,
};

[[nodiscard]] Gen5SampleBacking ResolveGen5SampleBacking(uint32_t fmt, uint32_t tile, bool exact_render_target_found);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
