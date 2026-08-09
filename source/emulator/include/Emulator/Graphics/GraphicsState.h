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

// DB_DEPTH_SIZE_XY encodes inclusive maxima, so zero represents a 1x1
// attachment after the register has been written. Preserve register presence
// separately from its encoded value.
struct DepthTargetExtent
{
	uint32_t width  = 0;
	uint32_t height = 0;
	bool     valid  = false;
};

struct DepthBias
{
	bool  enabled         = false;
	float constant_factor = 0.0f;
	float clamp           = 0.0f;
	float slope_factor    = 0.0f;
};

// A null stencil plane with TILE_STENCIL_DISABLE is a depth-only target.
[[nodiscard]] inline uint32_t ResolveEffectiveStencilFormat(const HW::DepthRenderTarget& target)
{
	if (target.stencil_info.format == 0)
	{
		return 0;
	}
	if (target.stencil_read_base_addr != 0 || target.stencil_write_base_addr != 0)
	{
		return target.stencil_info.format;
	}
	if (target.stencil_info.tile_stencil_disable)
	{
		return 0;
	}
	return target.stencil_info.format;
}

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
void SetWindowOffset(HW::Context& context, uint32_t value);
void SetScreenScissorTl(HW::Context& context, uint32_t value);
void SetScreenScissorBr(HW::Context& context, uint32_t value);
void SetRenderControl(HW::Context& context, uint32_t value);
void SetDepthControl(HW::Context& context, uint32_t value);
// Canonical PM4 register decoders shared by direct, batched, and indirect register writes.
// next_gen makes the Gen5 color-tile interpretation explicit instead of hiding it at call sites.
[[nodiscard]] HW::ColorInfo        DecodeColorInfo(uint32_t value, bool next_gen);
[[nodiscard]] HW::DepthZInfo       DecodeDepthZInfo(uint32_t value);
[[nodiscard]] HW::DepthStencilInfo DecodeDepthStencilInfo(uint32_t value);
void ApplyDepthStencilPlaneRegisters(HW::DepthRenderTarget& target, uint32_t stencil_info, uint32_t stencil_read_base,
                                     uint32_t stencil_write_base);
void ApplyDepthStencilPlaneRegisters(HW::Context& context, uint32_t stencil_info, uint32_t stencil_read_base, uint32_t stencil_write_base);
void SetStencilControl(HW::Context& context, uint32_t value);
void SetStencilRefMask(HW::Context& context, uint32_t value);
void SetStencilRefMaskBf(HW::Context& context, uint32_t value);
void SetModeControl(HW::Context& context, uint32_t value);
void SetPolygonOffsetRegister(HW::Context& context, uint32_t reg, uint32_t value);
void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value);

[[nodiscard]] DepthBias ResolveDepthBias(const HW::ModeControl& mode, const HW::PolygonOffset& offset);

// COPY_CENTROID and COPY_SAMPLE only select a distinct sample on a multisample
// attachment. Callers that issue a depth or stencil copy still require an
// explicit copy implementation.
[[nodiscard]] bool RenderControlSampleSelectionIsNoOp(const HW::RenderControl& control, uint8_t num_samples);

// Guest top-left coordinates are inclusive, bottom-right coordinates are exclusive, and enabled rectangles intersect.
[[nodiscard]] ScissorRect       ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id);
// Vulkan framebuffer bounds are authoritative after all color and depth
// attachments have been combined. Clamp the resolved guest rectangle before
// issuing dynamic scissor state so it cannot address pixels outside them.
[[nodiscard]] ScissorRect       ClampScissorToExtent(ScissorRect scissor, uint32_t width, uint32_t height);
[[nodiscard]] DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                         const HW::DepthControl& depth_control);
[[nodiscard]] StencilPlaneValidation ValidateStencilPlane(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                                          const HW::DepthControl& depth_control);
[[nodiscard]] HW::DepthRenderTarget  ResolveDepthStencilBasePairs(const HW::DepthRenderTarget& target);
[[nodiscard]] DepthTargetExtent       ResolveDepthTargetExtent(const HW::DepthRenderTarget& target, bool next_gen);

// AMD VTE window Z: OpenGL clip ([-W,+W]) uses zoffset±zscale; DX clip ([0,+W]) uses [zoffset, zoffset+zscale].
// Without VK_EXT_depth_range_unrestricted, clamp to [0,1] and pair with negativeOneToOne for OpenGL clip.
[[nodiscard]] ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted);
// PA_SC_VPORT_ZMIN/ZMAX form the hardware depth clamp applied after the window
// transform; engines that render with a reversed or partial depth range program
// values other than [0,1].
[[nodiscard]] ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted,
                                                      float clamp_min, float clamp_max);

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
	ColorTargetLayoutError    error = ColorTargetLayoutError::None;
};

[[nodiscard]] ColorTargetLayout ResolveColorTargetLayout(uint32_t mask);
[[nodiscard]] ColorTargetLayout ResolveColorTargetLayout(uint32_t mask, uint32_t configured_target_count);
// CB_TARGET_MASK admits a render-target channel and CB_SHADER_MASK admits the
// corresponding pixel-shader export. Vulkan must receive their intersection.
[[nodiscard]] uint8_t ResolveColorWriteMask(uint32_t target_mask, uint32_t shader_mask, uint32_t target_index);

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
	Mixed,
};

enum class SamplerAddressMode
{
	Repeat,
	MirroredRepeat,
	ClampToEdge,
	ClampToBorder,
};

enum class SamplerCompareOp
{
	Never,
	Less,
	Equal,
	LessOrEqual,
	Greater,
	NotEqual,
	GreaterOrEqual,
	Always,
};

struct SamplerComparison
{
	bool             enabled  = false;
	SamplerCompareOp function = SamplerCompareOp::Never;
};

struct UnnormalizedSamplerPolicy
{
	bool               enabled            = false;
	SamplerAddressMode address_mode       = SamplerAddressMode::ClampToEdge;
	bool               force_base_mip     = false;
	bool               disable_anisotropy = false;
	bool               disable_comparison = false;
	bool               reset_lod_bias     = false;
};

[[nodiscard]] SamplerAddressMode ResolveSamplerAddressMode(uint8_t sq_tex_clamp);
[[nodiscard]] SamplerCompareOp    ResolveSamplerCompareOp(uint8_t depth_compare_function);
// Vulkan requires sampler comparison state to agree with the SPIR-V image instruction.
[[nodiscard]] SamplerComparison         ResolveSamplerComparison(uint8_t depth_compare_function, ImageSampleOperation operation);
[[nodiscard]] UnnormalizedSamplerPolicy ResolveUnnormalizedSamplerPolicy(bool force_unnormalized_coordinates);

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED

#endif // EMULATOR_GRAPHICS_GRAPHICS_STATE_H_INCLUDED
