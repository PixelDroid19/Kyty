#include "Emulator/Graphics/GraphicsState.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Graphics/Pm4.h"

#include <algorithm>
#include <iterator>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics::State {

namespace {

[[nodiscard]] bool IsActive(const ScissorRect& scissor)
{
	return scissor.left != 0 || scissor.top != 0 || scissor.right != 0 || scissor.bottom != 0;
}

} // namespace

ScissorRect ResolveScissor(const HW::ScreenViewport& viewport, const HW::ScanModeControl& mode, uint32_t viewport_id)
{
	EXIT_IF(viewport_id >= std::size(viewport.viewports));

	ScissorRect resolved {};
	bool        has_scissor = false;

	const auto include = [&resolved, &has_scissor](const ScissorRect& scissor)
	{
		if (!IsActive(scissor))
		{
			return;
		}

		if (!has_scissor)
		{
			resolved    = scissor;
			has_scissor = true;
			return;
		}

		resolved.left   = std::max(resolved.left, scissor.left);
		resolved.top    = std::max(resolved.top, scissor.top);
		resolved.right  = std::min(resolved.right, scissor.right);
		resolved.bottom = std::min(resolved.bottom, scissor.bottom);
	};

	include({viewport.screen_scissor_left, viewport.screen_scissor_top, viewport.screen_scissor_right, viewport.screen_scissor_bottom});
	include({viewport.generic_scissor_left, viewport.generic_scissor_top, viewport.generic_scissor_right, viewport.generic_scissor_bottom});

	if (mode.vport_scissor_enable)
	{
		const auto& vport = viewport.viewports[viewport_id];
		include({vport.viewport_scissor_left, vport.viewport_scissor_top, vport.viewport_scissor_right, vport.viewport_scissor_bottom});
	}

	if (has_scissor)
	{
		resolved.right  = std::max(resolved.left, resolved.right);
		resolved.bottom = std::max(resolved.top, resolved.bottom);
	}

	return resolved;
}

DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                           const HW::DepthControl& depth_control)
{
	const bool decompress =
	    target.z_info.tile_surface_enable && (render_control.depth_compress_disable || render_control.stencil_compress_disable);

	DepthStencilUsage usage;
	usage.target_active      = depth_control.z_enable || depth_control.stencil_enable || decompress;
	usage.depth_write_enable = depth_control.z_enable && depth_control.z_write_enable;
	return usage;
}

ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted)
{
	ViewportDepthRange range {};
	if (dx_clip_space)
	{
		range.min_depth = zoffset;
		range.max_depth = zoffset + zscale;
	} else
	{
		range.min_depth = zoffset - zscale;
		range.max_depth = zoffset + zscale;
	}

	if (!depth_range_unrestricted)
	{
		range.min_depth = std::max(range.min_depth, 0.0f);
		range.max_depth = std::min(range.max_depth, 1.0f);
	}

	return range;
}

DepthClearActions ResolveDepthClearActions(bool register_depth_clear, bool htile_meta_clear)
{
	DepthClearActions actions {};
	actions.vulkan_clear         = register_depth_clear || htile_meta_clear;
	actions.suppress_depth_write = register_depth_clear;
	return actions;
}

ColorTargetLayout ResolveColorTargetLayout(uint32_t mask)
{
	ColorTargetLayout layout {};
	if (mask == 0)
	{
		return layout;
	}

	// Scan RT0..RT7: accept only contiguous full-channel (0xf) nibbles from RT0.
	// A partial nibble is PartialChannel. A nonzero nibble after a zero hole is Gapped.
	bool saw_zero = false;
	for (uint32_t slot = 0; slot < ColorTargetLayout::kMaxTargets; slot++)
	{
		const uint8_t nibble = static_cast<uint8_t>((mask >> (slot * 4u)) & 0xFu);
		if (nibble == 0)
		{
			saw_zero = true;
			continue;
		}
		if (saw_zero)
		{
			layout.count = 0;
			layout.error = ColorTargetLayoutError::Gapped;
			return layout;
		}
		if (nibble != 0xFu)
		{
			layout.count = 0;
			layout.error = ColorTargetLayoutError::PartialChannel;
			return layout;
		}
		layout.nibbles[layout.count] = nibble;
		layout.count++;
	}

	// Trailing zeros after a contiguous full prefix are OK (count stops at first zero).
	layout.error = ColorTargetLayoutError::None;
	return layout;
}

Gen5SampleBacking ResolveGen5SampleBacking(uint32_t fmt, uint32_t tile, bool exact_render_target_found)
{
	if (exact_render_target_found)
	{
		return Gen5SampleBacking::ExactRenderTarget;
	}

	// The guest-memory tile-27 uploader currently has an evidenced 4-Bpp
	// detile path only (fmt 56). Other formats require an exact live RT until
	// their byte-width-specific layout is implemented.
	if (tile == 27u && fmt != 56u)
	{
		return Gen5SampleBacking::Unsupported;
	}

	return Gen5SampleBacking::GuestMemoryTexture;
}

SamplerComparison ResolveSamplerComparison(uint8_t depth_compare_function, ImageSampleOperation operation)
{
	return {operation == ImageSampleOperation::DepthReference, depth_compare_function};
}

void SetGenericScissorTl(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int left = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_X)));
	const int top  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_Y)));
	const bool window_offset_disable = KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;

	context.SetGenericScissor(left, top, viewport.generic_scissor_right, viewport.generic_scissor_bottom, !window_offset_disable);
}

void SetGenericScissorBr(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int right  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_X)));
	const int bottom = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_Y)));

	context.SetGenericScissor(viewport.generic_scissor_left, viewport.generic_scissor_top, right, bottom,
	                          viewport.generic_scissor_window_offset_enable);
}

void SetModeControl(HW::Context& context, uint32_t value)
{
	HW::ModeControl mode;

	mode.cull_front               = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_FRONT) != 0;
	mode.cull_back                = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, CULL_BACK) != 0;
	mode.face                     = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, FACE) != 0;
	mode.poly_mode                = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_MODE);
	mode.polymode_front_ptype     = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_FRONT_PTYPE);
	mode.polymode_back_ptype      = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLYMODE_BACK_PTYPE);
	mode.poly_offset_front_enable = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_FRONT_ENABLE) != 0;
	mode.poly_offset_back_enable  = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, POLY_OFFSET_BACK_ENABLE) != 0;
	mode.vtx_window_offset_enable = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE) != 0;
	mode.provoking_vtx_last       = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PROVOKING_VTX_LAST) != 0;
	mode.persp_corr_dis           = KYTY_PM4_GET(value, PA_SU_SC_MODE_CNTL, PERSP_CORR_DIS) != 0;

	context.SetModeControl(mode);
}

void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value)
{
	EXIT_IF(slot >= 8);

	HW::BlendControl blend;

	blend.color_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_SRCBLEND);
	blend.color_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_COMB_FCN);
	blend.color_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_DESTBLEND);
	blend.alpha_srcblend  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_SRCBLEND);
	blend.alpha_comb_fcn  = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_COMB_FCN);
	blend.alpha_destblend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_DESTBLEND);
	blend.separate_alpha_blend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, SEPARATE_ALPHA_BLEND) != 0;
	blend.enable               = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ENABLE) != 0;

	context.SetBlendControl(slot, blend);
}

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED
