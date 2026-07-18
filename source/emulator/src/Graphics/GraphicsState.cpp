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

ViewportXy ResolveViewportXy(float xscale, float xoffset, float yscale, float yoffset)
{
	ViewportXy xy {};
	xy.x      = xoffset - xscale;
	xy.y      = yoffset - yscale;
	xy.width  = xscale * 2.0f;
	xy.height = yscale * 2.0f;
	return xy;
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
	return ResolveColorTargetLayout(mask, ColorTargetLayout::kMaxTargets);
}

ColorTargetLayout ResolveColorTargetLayout(uint32_t mask, uint32_t configured_target_count)
{
	ColorTargetLayout layout {};
	EXIT_NOT_IMPLEMENTED(configured_target_count > ColorTargetLayout::kMaxTargets);
	if (mask == 0)
	{
		return layout;
	}

	// Scan only physically configured RT slots. Higher CB_TARGET_MASK nibbles
	// have no attachment semantics until their CB_COLORn_BASE is configured.
	// The nibble is preserved so pipeline colorWriteMask can apply partial
	// channel writes. A nonzero nibble after a zero hole is Gapped.
	bool saw_zero = false;
	for (uint32_t slot = 0; slot < configured_target_count; slot++)
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

	// Texture-object path (not necessarily CPU detile). Whether guest pages are
	// detiled is decided only by Gen5SampleMayGuestUploadTiled — one behavior:
	//
	// tile 27 (kRenderTarget):
	//   - ufmt 133 (BC1): GuestMemoryTexture; MayGuestUpload may detile package data
	//   - ufmt 56 (RGBA8): GuestMemoryTexture; MayGuestUpload always false (skip_guest
	//     transparent clear — never detile GPU intermediates)
	//   - ufmt 71 (RGBA16F): requires live RT (Unsupported without alias)
	// tile 9 (kStandard64KB): ufmt 56 only; MayGuestUpload when uncovered
	//
	// Unsupported = no Texture object and no live alias → structured EXIT.
	if (tile == 27u)
	{
		if (fmt == 56u || fmt == 133u)
		{
			return Gen5SampleBacking::GuestMemoryTexture;
		}
		return Gen5SampleBacking::Unsupported;
	}
	if (tile == 9u)
	{
		if (fmt == 56u)
		{
			return Gen5SampleBacking::GuestMemoryTexture;
		}
		return Gen5SampleBacking::Unsupported;
	}

	// Linear tile 0 and other package modes: guest Texture object.
	return Gen5SampleBacking::GuestMemoryTexture;
}

SamplerAddressMode ResolveSamplerAddressMode(uint8_t sq_tex_clamp)
{
	switch (sq_tex_clamp)
	{
		case 0: return SamplerAddressMode::Repeat;
		case 1: return SamplerAddressMode::MirroredRepeat;
		case 2: return SamplerAddressMode::ClampToEdge;
		case 6: return SamplerAddressMode::ClampToBorder;
		// AMD SQ_TEX_MIRROR_ONCE_BORDER has no exact Vulkan address mode.
		// Prefer border behavior over enabling mirror-clamp-to-edge without a
		// checked device feature/extension.
		case 7: return SamplerAddressMode::ClampToBorder;
		default: EXIT("unknown clamp: %u\n", sq_tex_clamp);
	}
	return SamplerAddressMode::ClampToBorder;
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

void SetScreenScissorTl(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int   left     = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_TL, TL_X)));
	const int   top      = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_TL, TL_Y)));

	context.SetScreenScissor(left, top, viewport.screen_scissor_right, viewport.screen_scissor_bottom);
}

void SetScreenScissorBr(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int   right    = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_BR, BR_X)));
	const int   bottom   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_SCREEN_SCISSOR_BR, BR_Y)));

	context.SetScreenScissor(viewport.screen_scissor_left, viewport.screen_scissor_top, right, bottom);
}

void SetRenderControl(HW::Context& context, uint32_t value)
{
	HW::RenderControl r;

	r.depth_clear_enable       = KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_CLEAR_ENABLE) != 0;
	r.stencil_clear_enable     = KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_CLEAR_ENABLE) != 0;
	r.resummarize_enable       = KYTY_PM4_GET(value, DB_RENDER_CONTROL, RESUMMARIZE_ENABLE) != 0;
	r.stencil_compress_disable = KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_COMPRESS_DISABLE) != 0;
	r.depth_compress_disable   = KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_COMPRESS_DISABLE) != 0;
	r.copy_centroid            = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_CENTROID) != 0;
	r.copy_sample              = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_SAMPLE);

	context.SetRenderControl(r);
}

void SetStencilControl(HW::Context& context, uint32_t value)
{
	HW::StencilControl r;

	r.stencil_fail     = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILFAIL);
	r.stencil_zpass    = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZPASS);
	r.stencil_zfail    = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZFAIL);
	r.stencil_fail_bf  = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILFAIL_BF);
	r.stencil_zpass_bf = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZPASS_BF);
	r.stencil_zfail_bf = KYTY_PM4_GET(value, DB_STENCIL_CONTROL, STENCILZFAIL_BF);

	context.SetStencilControl(r);
}

void SetStencilRefMask(HW::Context& context, uint32_t value)
{
	auto r = context.GetStencilMask();

	r.stencil_testval   = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILTESTVAL);
	r.stencil_mask      = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILMASK);
	r.stencil_writemask = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILWRITEMASK);
	r.stencil_opval     = KYTY_PM4_GET(value, DB_STENCILREFMASK, STENCILOPVAL);

	context.SetStencilMask(r);
}

void SetStencilRefMaskBf(HW::Context& context, uint32_t value)
{
	auto r = context.GetStencilMask();

	r.stencil_testval_bf   = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILTESTVAL_BF);
	r.stencil_mask_bf      = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILMASK_BF);
	r.stencil_writemask_bf = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILWRITEMASK_BF);
	r.stencil_opval_bf     = KYTY_PM4_GET(value, DB_STENCILREFMASK_BF, STENCILOPVAL_BF);

	context.SetStencilMask(r);
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
