#include "Emulator/Graphics/GraphicsState.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Pm4.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
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
	const auto combine = [&](bool apply_window_offset)
	{
		bool has_scissor = false;
		resolved         = {};

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
		const auto with_window_offset = [&viewport, apply_window_offset](ScissorRect scissor, bool enabled)
		{
			if (enabled && apply_window_offset)
			{
				scissor.left += viewport.window_offset_x;
				scissor.top += viewport.window_offset_y;
				scissor.right += viewport.window_offset_x;
				scissor.bottom += viewport.window_offset_y;
			}
			return scissor;
		};

		include({viewport.screen_scissor_left, viewport.screen_scissor_top, viewport.screen_scissor_right, viewport.screen_scissor_bottom});
		include(with_window_offset(
		    {viewport.generic_scissor_left, viewport.generic_scissor_top, viewport.generic_scissor_right, viewport.generic_scissor_bottom},
		    viewport.generic_scissor_window_offset_enable));

		if (mode.vport_scissor_enable)
		{
			const auto& vport = viewport.viewports[viewport_id];
			include(with_window_offset(
			    {vport.viewport_scissor_left, vport.viewport_scissor_top, vport.viewport_scissor_right, vport.viewport_scissor_bottom},
			    vport.viewport_scissor_window_offset_enable));
		}

		if (has_scissor)
		{
			resolved.right  = std::max(resolved.left, resolved.right);
			resolved.bottom = std::max(resolved.top, resolved.bottom);
		}
	};

	combine(true);
	if ((viewport.window_offset_x != 0 || viewport.window_offset_y != 0) &&
	    (resolved.right <= resolved.left || resolved.bottom <= resolved.top))
	{
		combine(false);
	}

	return resolved;
}

ScissorRect ClampScissorToExtent(ScissorRect scissor, uint32_t width, uint32_t height)
{
	const int maximum_x = static_cast<int>(width);
	const int maximum_y = static_cast<int>(height);
	scissor.left        = std::clamp(scissor.left, 0, maximum_x);
	scissor.top         = std::clamp(scissor.top, 0, maximum_y);
	scissor.right       = std::clamp(scissor.right, scissor.left, maximum_x);
	scissor.bottom      = std::clamp(scissor.bottom, scissor.top, maximum_y);
	return scissor;
}

void SetWindowOffset(HW::Context& context, uint32_t value)
{
	const int offset_x = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_OFFSET, WINDOW_X)));
	const int offset_y = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_WINDOW_OFFSET, WINDOW_Y)));
	context.SetWindowOffset(offset_x, offset_y);
}

DepthStencilUsage ResolveDepthStencilUsage(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                           const HW::DepthControl& depth_control)
{
	const bool decompress =
	    target.z_info.tile_surface_enable && (render_control.depth_compress_disable || render_control.stencil_compress_disable);
	// Resummarization updates only Prospero's depth metadata. Vulkan owns the
	// equivalent compression state, but the depth attachment must still be
	// admitted so its logical contents remain synchronized with the draw.
	const bool resummarize = render_control.resummarize_enable;

	DepthStencilUsage usage;
	usage.target_active      = depth_control.z_enable || depth_control.stencil_enable || decompress || resummarize ||
	                           render_control.depth_copy || render_control.stencil_copy;
	usage.depth_write_enable = depth_control.z_enable && depth_control.z_write_enable;
	return usage;
}

StencilPlaneValidation ValidateStencilPlane(const HW::DepthRenderTarget& target, const HW::RenderControl& render_control,
                                            const HW::DepthControl& depth_control)
{
	const bool decompress  = target.z_info.tile_surface_enable && render_control.stencil_compress_disable;
	const bool plane_declared = target.stencil_info.format != 0 || target.stencil_read_base_addr != 0 ||
	                            target.stencil_write_base_addr != 0;
	const bool sample_selection = render_control.copy_centroid || render_control.copy_sample != 0;
	// Render-control metadata operates on an existing depth/stencil target. It
	// does not declare a stencil plane when the guest supplied neither format nor
	// base address.
	if (!plane_declared)
	{
		return depth_control.stencil_enable ? StencilPlaneValidation::MissingReadBase : StencilPlaneValidation::Inactive;
	}
	const bool needs_read  = depth_control.stencil_enable || decompress || render_control.resummarize_enable ||
	                         render_control.stencil_copy || sample_selection;
	const bool needs_write = render_control.stencil_clear_enable ||
	                         (depth_control.stencil_enable && !target.depth_view.stencil_write_disable) || decompress ||
	                         render_control.resummarize_enable || render_control.stencil_copy || sample_selection;

	if (!needs_read && !needs_write)
	{
		return StencilPlaneValidation::Inactive;
	}
	if (needs_read && target.stencil_read_base_addr == 0)
	{
		return StencilPlaneValidation::MissingReadBase;
	}
	if (needs_write && target.stencil_write_base_addr == 0)
	{
		return StencilPlaneValidation::MissingWriteBase;
	}
	if (needs_read && needs_write && target.stencil_read_base_addr != target.stencil_write_base_addr)
	{
		return StencilPlaneValidation::MismatchedBases;
	}
	return StencilPlaneValidation::Valid;
}

HW::DepthRenderTarget ResolveDepthStencilBasePairs(const HW::DepthRenderTarget& target)
{
	auto       resolved          = target;
	const auto recover_lone_zero = [](uint64_t* read_base, uint64_t* write_base)
	{
		if (*read_base == 0 && *write_base != 0)
		{
			*read_base = *write_base;
		} else if (*write_base == 0 && *read_base != 0)
		{
			*write_base = *read_base;
		}
	};
	recover_lone_zero(&resolved.z_read_base_addr, &resolved.z_write_base_addr);
	recover_lone_zero(&resolved.stencil_read_base_addr, &resolved.stencil_write_base_addr);
	return resolved;
}

DepthTargetExtent ResolveDepthTargetExtent(const HW::DepthRenderTarget& target, bool next_gen)
{
	if (next_gen)
	{
		if (!target.size.valid)
		{
			return {};
		}

		return {static_cast<uint32_t>(target.size.x_max) + 1, static_cast<uint32_t>(target.size.y_max) + 1, true};
	}

	if (target.width == 0 || target.height == 0)
	{
		return {};
	}

	return {target.width, target.height, true};
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

ViewportDepthRange ResolveViewportDepth(float zscale, float zoffset, bool dx_clip_space, bool depth_range_unrestricted, float clamp_min,
                                        float clamp_max)
{
	auto range = ResolveViewportDepth(zscale, zoffset, dx_clip_space, depth_range_unrestricted);

	// PA_SC_VPORT_ZMIN/ZMAX clamp the transformed depth. The window transform is
	// monotonic, so clamping both endpoints of the Vulkan range reproduces it,
	// including a reversed range where min_depth > max_depth.
	const float clamp_low  = std::min(clamp_min, clamp_max);
	const float clamp_high = std::max(clamp_min, clamp_max);
	range.min_depth        = std::clamp(range.min_depth, clamp_low, clamp_high);
	range.max_depth        = std::clamp(range.max_depth, clamp_low, clamp_high);
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
	if (configured_target_count > ColorTargetLayout::kMaxTargets) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: configured_target_count > ColorTargetLayout::kMaxTargets condition ignored (continuing)\n"); }
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

uint8_t ResolveColorWriteMask(uint32_t target_mask, uint32_t shader_mask, uint32_t target_index)
{
	if (target_index >= ColorTargetLayout::kMaxTargets) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: target_index >= ColorTargetLayout::kMaxTargets condition ignored (continuing)\n"); }
	const uint32_t shift = target_index * 4u;
	return static_cast<uint8_t>(((target_mask >> shift) & (shader_mask >> shift)) & 0xFu);
}

bool PixelShaderStageRequired(uint32_t target_mask, const HW::ShaderRegisters& shader, const HW::DepthControl& depth)
{
	if ((target_mask & shader.m_cbShaderMask) != 0 || depth.z_enable || depth.stencil_enable || shader.shader_z_format != 0)
	{
		return true;
	}
	for (const auto output_mode: shader.target_output_mode)
	{
		if (output_mode != 0)
		{
			return true;
		}
	}
	const auto& control = shader.db_shader_control;
	return control.shader_kill_enable || control.shader_z_export_enable || control.shader_execute_on_noop || control.other_bits != 0;
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
	// tile 9 (kStandard64KB): ufmt 56/71; MayGuestUpload when uncovered
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
		if (fmt == 56u || fmt == 71u)
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
	return {operation == ImageSampleOperation::DepthReference, ResolveSamplerCompareOp(depth_compare_function)};
}

SamplerCompareOp ResolveSamplerCompareOp(uint8_t depth_compare_function)
{
	switch (depth_compare_function)
	{
		case 0: return SamplerCompareOp::Never;
		case 1: return SamplerCompareOp::Less;
		case 2: return SamplerCompareOp::Equal;
		case 3: return SamplerCompareOp::LessOrEqual;
		case 4: return SamplerCompareOp::Greater;
		case 5: return SamplerCompareOp::NotEqual;
		case 6: return SamplerCompareOp::GreaterOrEqual;
		case 7: return SamplerCompareOp::Always;
		default: EXIT("unknown sampler depth compare function: %u\n", depth_compare_function);
	}
	return SamplerCompareOp::Never;
}

UnnormalizedSamplerPolicy ResolveUnnormalizedSamplerPolicy(bool force_unnormalized_coordinates)
{
	if (!force_unnormalized_coordinates)
	{
		return {};
	}
	return {.enabled            = true,
	        .address_mode       = SamplerAddressMode::ClampToEdge,
	        .force_base_mip     = true,
	        .disable_anisotropy = true,
	        .disable_comparison = true,
	        .reset_lod_bias     = true};
}

void SetGenericScissorTl(HW::Context& context, uint32_t value)
{
	const auto& viewport              = context.GetScreenViewport();
	const int   left                  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_X)));
	const int   top                   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, TL_Y)));
	const bool  window_offset_disable = KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_TL, WINDOW_OFFSET_DISABLE) != 0;

	context.SetGenericScissor(left, top, viewport.generic_scissor_right, viewport.generic_scissor_bottom, !window_offset_disable);
}

void SetGenericScissorBr(HW::Context& context, uint32_t value)
{
	const auto& viewport = context.GetScreenViewport();
	const int   right    = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_X)));
	const int   bottom   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_GENERIC_SCISSOR_BR, BR_Y)));

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
	const bool        gen5 = Config::IsNextGen();

	r.depth_clear_enable       = KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_CLEAR_ENABLE) != 0;
	r.stencil_clear_enable     = KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_CLEAR_ENABLE) != 0;
	r.depth_copy               = !gen5 && KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_COPY) != 0;
	r.stencil_copy             = !gen5 && KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_COPY) != 0;
	r.resummarize_enable       = KYTY_PM4_GET(value, DB_RENDER_CONTROL, RESUMMARIZE_ENABLE) != 0;
	r.stencil_compress_disable = KYTY_PM4_GET(value, DB_RENDER_CONTROL, STENCIL_COMPRESS_DISABLE) != 0;
	r.depth_compress_disable   = KYTY_PM4_GET(value, DB_RENDER_CONTROL, DEPTH_COMPRESS_DISABLE) != 0;
	r.copy_centroid            = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_CENTROID) != 0;
	r.copy_sample              = KYTY_PM4_GET(value, DB_RENDER_CONTROL, COPY_SAMPLE);

	context.SetRenderControl(r);
}

void SetDepthControl(HW::Context& context, uint32_t value)
{
	HW::DepthControl r;
	r.stencil_enable                     = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCIL_ENABLE) != 0;
	r.z_enable                           = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, Z_ENABLE) != 0;
	r.z_write_enable                     = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, Z_WRITE_ENABLE) != 0;
	r.depth_bounds_enable                = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, DEPTH_BOUNDS_ENABLE) != 0;
	r.zfunc                              = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, ZFUNC);
	r.backface_enable                    = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, BACKFACE_ENABLE) != 0;
	r.stencilfunc                        = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC);
	r.stencilfunc_bf                     = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, STENCILFUNC_BF);
	r.color_writes_on_depth_fail_enable  = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, ENABLE_COLOR_WRITES_ON_DEPTH_FAIL) != 0;
	r.color_writes_on_depth_pass_disable = KYTY_PM4_GET(value, DB_DEPTH_CONTROL, DISABLE_COLOR_WRITES_ON_DEPTH_PASS) != 0;
	context.SetDepthControl(r);
}

HW::ColorInfo DecodeColorInfo(uint32_t value, bool next_gen)
{
	HW::ColorInfo info;
	info.format                         = KYTY_PM4_GET(value, CB_COLOR0_INFO, FORMAT);
	info.channel_type                   = KYTY_PM4_GET(value, CB_COLOR0_INFO, NUMBER_TYPE);
	info.channel_order                  = KYTY_PM4_GET(value, CB_COLOR0_INFO, COMP_SWAP);
	info.cmask_fast_clear_enable        = KYTY_PM4_GET(value, CB_COLOR0_INFO, FAST_CLEAR) != 0;
	info.fmask_compression_enable       = KYTY_PM4_GET(value, CB_COLOR0_INFO, COMPRESSION) != 0;
	info.blend_clamp                    = KYTY_PM4_GET(value, CB_COLOR0_INFO, BLEND_CLAMP) != 0;
	info.blend_bypass                   = KYTY_PM4_GET(value, CB_COLOR0_INFO, BLEND_BYPASS) != 0;
	info.round_mode                     = KYTY_PM4_GET(value, CB_COLOR0_INFO, ROUND_MODE) != 0;
	info.cmask_tile_mode                = KYTY_PM4_GET(value, CB_COLOR0_INFO, CMASK_IS_LINEAR);
	info.fmask_data_compression_disable = KYTY_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESSION_DISABLE) != 0;
	info.fmask_one_frag_mode            = KYTY_PM4_GET(value, CB_COLOR0_INFO, FMASK_COMPRESS_1FRAG_ONLY) != 0;
	info.dcc_compression_enable         = KYTY_PM4_GET(value, CB_COLOR0_INFO, DCC_ENABLE) != 0;
	info.cmask_tile_mode_neo            = KYTY_PM4_GET(value, CB_COLOR0_INFO, CMASK_ADDR_TYPE);
	info.neo_mode                       = KYTY_PM4_GET(value, CB_COLOR0_INFO, ALT_TILE_MODE) != 0 || next_gen;
	return info;
}

HW::DepthZInfo DecodeDepthZInfo(uint32_t value)
{
	HW::DepthZInfo info;
	info.format                    = KYTY_PM4_GET(value, DB_Z_INFO, FORMAT);
	info.num_samples               = KYTY_PM4_GET(value, DB_Z_INFO, NUM_SAMPLES);
	info.embedded_sample_locations = KYTY_PM4_GET(value, DB_Z_INFO, ITERATE_FLUSH) != 0;
	info.partially_resident        = KYTY_PM4_GET(value, DB_Z_INFO, PARTIALLY_RESIDENT) != 0;
	info.num_mip_levels            = KYTY_PM4_GET(value, DB_Z_INFO, MAXMIP);
	info.tile_mode_index           = KYTY_PM4_GET(value, DB_Z_INFO, TILE_MODE_INDEX);
	info.plane_compression         = KYTY_PM4_GET(value, DB_Z_INFO, DECOMPRESS_ON_N_ZPLANES);
	info.expclear_enabled          = KYTY_PM4_GET(value, DB_Z_INFO, ALLOW_EXPCLEAR) != 0;
	info.tile_surface_enable       = KYTY_PM4_GET(value, DB_Z_INFO, TILE_SURFACE_ENABLE) != 0;
	info.zrange_precision          = KYTY_PM4_GET(value, DB_Z_INFO, ZRANGE_PRECISION);
	return info;
}

HW::DepthStencilInfo DecodeDepthStencilInfo(uint32_t value)
{
	HW::DepthStencilInfo info;
	info.format                     = KYTY_PM4_GET(value, DB_STENCIL_INFO, FORMAT);
	info.texture_compatible_stencil = KYTY_PM4_GET(value, DB_STENCIL_INFO, ITERATE_FLUSH) != 0;
	info.partially_resident         = KYTY_PM4_GET(value, DB_STENCIL_INFO, PARTIALLY_RESIDENT) != 0;
	info.tile_split                 = KYTY_PM4_GET(value, DB_STENCIL_INFO, RESERVED_FIELD_1);
	info.tile_mode_index            = KYTY_PM4_GET(value, DB_STENCIL_INFO, TILE_MODE_INDEX);
	info.expclear_enabled           = KYTY_PM4_GET(value, DB_STENCIL_INFO, ALLOW_EXPCLEAR) != 0;
	info.tile_stencil_disable       = KYTY_PM4_GET(value, DB_STENCIL_INFO, TILE_STENCIL_DISABLE) != 0;
	return info;
}

void ApplyDepthStencilPlaneRegisters(HW::DepthRenderTarget& target, uint32_t stencil_info, uint32_t stencil_read_base,
                                     uint32_t stencil_write_base)
{
	target.stencil_info            = DecodeDepthStencilInfo(stencil_info);
	target.stencil_read_base_addr  = static_cast<uint64_t>(stencil_read_base) << 8u;
	target.stencil_write_base_addr = static_cast<uint64_t>(stencil_write_base) << 8u;
}

void ApplyDepthStencilPlaneRegisters(HW::Context& context, uint32_t stencil_info, uint32_t stencil_read_base, uint32_t stencil_write_base)
{
	auto target = context.GetDepthRenderTarget();
	ApplyDepthStencilPlaneRegisters(target, stencil_info, stencil_read_base, stencil_write_base);
	context.SetDepthRenderTarget(target);
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

void SetPolygonOffsetRegister(HW::Context& context, uint32_t reg, uint32_t value)
{
	auto  offset      = context.GetPolygonOffset();
	float float_value = 0.0f;
	std::memcpy(&float_value, &value, sizeof(float_value));

	switch (reg)
	{
		case Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL: offset.db_format_control = value; break;
		case Pm4::PA_SU_POLY_OFFSET_CLAMP: offset.clamp = float_value; break;
		case Pm4::PA_SU_POLY_OFFSET_FRONT_SCALE: offset.front_scale = float_value; break;
		case Pm4::PA_SU_POLY_OFFSET_FRONT_OFFSET: offset.front_offset = float_value; break;
		case Pm4::PA_SU_POLY_OFFSET_BACK_SCALE: offset.back_scale = float_value; break;
		case Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET: offset.back_offset = float_value; break;
		default: EXIT("invalid polygon offset register: 0x%08" PRIx32 "\n", reg);
	}

	context.SetPolygonOffset(offset);
}

DepthBias ResolveDepthBias(const HW::ModeControl& mode, const HW::PolygonOffset& offset)
{
	DepthBias bias;
	bias.enabled = mode.poly_offset_front_enable || mode.poly_offset_back_enable;
	if (!bias.enabled)
	{
		return bias;
	}

	const bool front     = mode.poly_offset_front_enable;
	bias.constant_factor = front ? offset.front_offset : offset.back_offset;
	bias.clamp           = offset.clamp;
	bias.slope_factor    = (front ? offset.front_scale : offset.back_scale) / 16.0f;
	return bias;
}

bool RenderControlSampleSelectionIsNoOp(const HW::RenderControl& control, uint8_t num_samples)
{
	if (!control.copy_centroid && control.copy_sample == 0)
	{
		return true;
	}
	return control.copy_centroid && num_samples == 0;
}

void SetBlendControl(HW::Context& context, uint32_t slot, uint32_t value)
{
	EXIT_IF(slot >= 8);

	HW::BlendControl blend;

	blend.color_srcblend       = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_SRCBLEND);
	blend.color_comb_fcn       = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_COMB_FCN);
	blend.color_destblend      = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, COLOR_DESTBLEND);
	blend.alpha_srcblend       = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_SRCBLEND);
	blend.alpha_comb_fcn       = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_COMB_FCN);
	blend.alpha_destblend      = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ALPHA_DESTBLEND);
	blend.separate_alpha_blend = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, SEPARATE_ALPHA_BLEND) != 0;
	blend.enable               = KYTY_PM4_GET(value, CB_BLEND0_CONTROL, ENABLE) != 0;

	context.SetBlendControl(slot, blend);
}

} // namespace Kyty::Libs::Graphics::State

#endif // KYTY_EMU_ENABLED
