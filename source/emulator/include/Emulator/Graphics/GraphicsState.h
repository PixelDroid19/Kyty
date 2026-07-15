#ifndef EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
#define EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED

#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
struct GraphicContext;
}

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

// Record guest color-target layouts already observed this session (base-keyed).
// Matching width/height/size alone is not enough: unrelated allocations can share
// dimensions. The sample promote path consults the same base.
void RecordGen5RenderTargetSize(GraphicContext* ctx, uint64_t base, uint32_t width, uint32_t height, uint64_t size);
[[nodiscard]] bool HasGen5RenderTargetSize(const GraphicContext* ctx, uint64_t base, uint32_t width, uint32_t height, uint64_t size);

// Gen5 tile-27 sample that misses FindRenderTexture: prefer a GPU-owned RT when
// this guest base was already observed as a color target. Captured: 642x362
// samples detiled as pure CPU before their RT existed (diagonal garbage →
// present stripes), while peers already had RTs. Pure CPU atlases never appear
// in the color-target registry and stay on the Texture path.
[[nodiscard]] bool Gen5Tile27SamplePrefersRenderTarget(uint32_t fmt, uint32_t tile, bool seen_as_color_target_size);

// A sampled surface may reuse a render target only when GpuMemory found the
// exact live object, or when promote registers the observed color-target base.
// Matching dimensions alone do not establish identity or content.
enum class Gen5SampleBacking
{
	ExactRenderTarget,
	GuestMemoryTexture,
	Unsupported,
};

[[nodiscard]] Gen5SampleBacking ResolveGen5SampleBacking(uint32_t fmt, uint32_t tile, bool exact_render_target_found);

enum class ImageSampleOperation
{
	Regular,
	DepthReference,
};

struct SamplerComparison
{
	bool    enabled  = false;
	uint8_t function = 0;
};

// Vulkan requires sampler comparison state to agree with the SPIR-V image instruction.
[[nodiscard]] SamplerComparison ResolveSamplerComparison(uint8_t depth_compare_function, ImageSampleOperation operation);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
