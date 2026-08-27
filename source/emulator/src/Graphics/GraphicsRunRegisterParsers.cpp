#include "GraphicsRunInternal.h"

#include "GraphicsComputeRegisters.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Log.h"

#include <cinttypes>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

KYTY_HW_CTX_PARSER(hw_ctx_set_aa_config)
{
	if (cmd_id != 0xc0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_AA_CONFIG) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_AA_CONFIG condition ignored (continuing)\n"); }

	trace_aa_register_write("direct", "PA_SC_AA_CONFIG", buffer[0]);
	HW::AaConfig r;

	r.msaa_num_samples      = KYTY_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MSAA_NUM_SAMPLES);
	r.aa_mask_centroid_dtmn = KYTY_PM4_GET(buffer[0], PA_SC_AA_CONFIG, AA_MASK_CENTROID_DTMN) != 0;
	r.max_sample_dist       = KYTY_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MAX_SAMPLE_DIST);
	r.msaa_exposed_samples  = KYTY_PM4_GET(buffer[0], PA_SC_AA_CONFIG, MSAA_EXPOSED_SAMPLES);

	cp->GetCtx()->SetAaConfig(r);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_aa_sample_control)
{
	if (cmd_id != 0xc0106900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0106900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 condition ignored (continuing)\n"); }

	uint32_t count = 1;

	if (dw >= 20 && buffer[16] == 0xc0026900 && buffer[17] == Pm4::PA_SC_CENTROID_PRIORITY_0)
	{
		count = 20;

		HW::AaSampleControl r;

		memcpy(r.locations, buffer, static_cast<size_t>(16) * 4);

		r.centroid_priority = static_cast<uint64_t>(buffer[18]) | (static_cast<uint64_t>(buffer[19]) << 32u);

		cp->GetCtx()->SetAaSampleControl(r);
	} else
	{
		KYTY_NOT_IMPLEMENTED;
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_blend_color)
{
	if (cmd_id != 0xc0046900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0046900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::CB_BLEND_RED) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::CB_BLEND_RED condition ignored (continuing)\n"); }

	HW::BlendColor r;

	r.red   = *reinterpret_cast<const float*>(&buffer[0]);
	r.green = *reinterpret_cast<const float*>(&buffer[1]);
	r.blue  = *reinterpret_cast<const float*>(&buffer[2]);
	r.alpha = *reinterpret_cast<const float*>(&buffer[3]);

	cp->GetCtx()->SetBlendColor(r);

	return 4;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_blend_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }

	uint32_t param = (cmd_offset - Pm4::CB_BLEND0_CONTROL) / 1;

	State::SetBlendControl(*cp->GetCtx(), param, buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_clip_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_CL_CLIP_CNTL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_CL_CLIP_CNTL condition ignored (continuing)\n"); }

	HW::ClipControl r;

	//	r.user_clip_planes                    = buffer[0] & 0x3fu;
	//	r.user_clip_plane_mode                = (buffer[0] >> 14u) & 0x3u;
	//	r.clip_space                          = (buffer[0] >> 19u) & 0x1u;
	//	r.vertex_kill_mode                    = (buffer[0] >> 21u) & 0x1u;
	//	r.min_z_clip_enable                   = (buffer[0] >> 26u) & 0x1u;
	//	r.max_z_clip_enable                   = (buffer[0] >> 27u) & 0x1u;
	//	r.user_clip_plane_negate_y            = (buffer[0] & 0x00002000u) != 0;
	//	r.clip_enable                         = (buffer[0] & 0x00010000u) != 0;
	//	r.user_clip_plane_cull_only           = (buffer[0] & 0x00020000u) != 0;
	//	r.cull_on_clipping_error_disable      = (buffer[0] & 0x00100000u) != 0;
	//	r.linear_attribute_clip_enable        = (buffer[0] & 0x01000000u) != 0;
	//	r.force_viewport_index_from_vs_enable = (buffer[0] & 0x02000000u) != 0;

	r.user_clip_planes                    = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, UCP_ENA);
	r.user_clip_plane_mode                = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, PS_UCP_MODE);
	r.dx_clip_space                       = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
	r.vertex_kill_any                     = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
	r.min_z_clip_disable                  = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
	r.max_z_clip_disable                  = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
	r.user_clip_plane_negate_y            = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
	r.clip_disable                        = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
	r.user_clip_plane_cull_only           = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
	r.cull_on_clipping_error_disable      = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
	r.linear_attribute_clip_enable        = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
	r.force_viewport_index_from_vs_enable = KYTY_PM4_GET(buffer[0], PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;

	cp->GetCtx()->SetClipControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_color_control)
{
	if (cmd_id != 0xc0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::CB_COLOR_CONTROL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::CB_COLOR_CONTROL condition ignored (continuing)\n"); }

	trace_aa_register_write("direct", "CB_COLOR_CONTROL", buffer[0]);
	HW::ColorControl r;

	r.mode = KYTY_PM4_GET(buffer[0], CB_COLOR_CONTROL, MODE);
	r.op   = KYTY_PM4_GET(buffer[0], CB_COLOR_CONTROL, ROP3);

	cp->GetCtx()->SetColorControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_color_info)
{
	if (cmd_id != 0xc0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0016900 condition ignored (continuing)\n"); }

	uint32_t param = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;
	cp->GetCtx()->SetColorInfo(param, State::DecodeColorInfo(buffer[0], Config::IsNextGen()));

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_depth_clear)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_DEPTH_CLEAR) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_DEPTH_CLEAR condition ignored (continuing)\n"); }

	cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(buffer));

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_depth_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_DEPTH_CONTROL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_DEPTH_CONTROL condition ignored (continuing)\n"); }

	State::SetDepthControl(*cp->GetCtx(), buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_depth_render_target)
{
	if (cmd_offset != Pm4::DB_Z_INFO) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_Z_INFO condition ignored (continuing)\n"); }

	uint32_t count = 1;

	if (cmd_id == 0xC0016900)
	{
		cp->GetCtx()->SetDepthZInfo(State::DecodeDepthZInfo(buffer[0]));
	} else if (cmd_id == 0xC0086900)
	{
		if (dw >= 22 && buffer[8] == 0xC0016900 && buffer[9] == Pm4::DB_DEPTH_INFO && buffer[11] == 0xC0016900 &&
		    buffer[12] == Pm4::DB_DEPTH_VIEW && buffer[14] == 0xC0016900 && buffer[15] == Pm4::DB_HTILE_DATA_BASE &&
		    buffer[17] == 0xC0016900 && buffer[18] == Pm4::DB_HTILE_SURFACE && buffer[20] == 0xC0001000)
		{
			count = 22;

			HW::DepthRenderTarget z;

			z.z_info            = State::DecodeDepthZInfo(buffer[0]);
			z.z_read_base_addr  = static_cast<uint64_t>(buffer[2]) << 8u;
			z.z_write_base_addr = static_cast<uint64_t>(buffer[4]) << 8u;
			State::ApplyDepthStencilPlaneRegisters(z, buffer[1], buffer[3], buffer[5]);

			// DB_DEPTH_SIZE
			z.pitch_div8_minus1  = (buffer[6] >> Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SIZE_PITCH_TILE_MAX_MASK;
			z.height_div8_minus1 = (buffer[6] >> Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SIZE_HEIGHT_TILE_MAX_MASK;

			// DB_DEPTH_SLICE
			z.slice_div64_minus1 = (buffer[7] >> Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_SHIFT) & Pm4::DB_DEPTH_SLICE_SLICE_TILE_MAX_MASK;

			z.depth_info.addr5_swizzle_mask = KYTY_PM4_GET(buffer[10], DB_DEPTH_INFO, ADDR5_SWIZZLE_MASK);
			z.depth_info.array_mode         = (buffer[10] >> Pm4::DB_DEPTH_INFO_ARRAY_MODE_SHIFT) & Pm4::DB_DEPTH_INFO_ARRAY_MODE_MASK;
			z.depth_info.pipe_config        = (buffer[10] >> Pm4::DB_DEPTH_INFO_PIPE_CONFIG_SHIFT) & Pm4::DB_DEPTH_INFO_PIPE_CONFIG_MASK;
			z.depth_info.bank_width         = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_WIDTH_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_WIDTH_MASK;
			z.depth_info.bank_height        = (buffer[10] >> Pm4::DB_DEPTH_INFO_BANK_HEIGHT_SHIFT) & Pm4::DB_DEPTH_INFO_BANK_HEIGHT_MASK;
			z.depth_info.macro_tile_aspect  = KYTY_PM4_GET(buffer[10], DB_DEPTH_INFO, MACRO_TILE_ASPECT);
			z.depth_info.num_banks          = (buffer[10] >> Pm4::DB_DEPTH_INFO_NUM_BANKS_SHIFT) & Pm4::DB_DEPTH_INFO_NUM_BANKS_MASK;

			// z.depth_view.slice_start = (buffer[13] >> Pm4::DB_DEPTH_VIEW_SLICE_START_SHIFT) & Pm4::DB_DEPTH_VIEW_SLICE_START_MASK;
			// z.depth_view.slice_max   = (buffer[13] >> Pm4::DB_DEPTH_VIEW_SLICE_MAX_SHIFT) & Pm4::DB_DEPTH_VIEW_SLICE_MAX_MASK;

			z.depth_view.slice_start =
			    KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START) + (KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
			z.depth_view.slice_max =
			    KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX) + (KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
			z.depth_view.depth_write_disable   = KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
			z.depth_view.stencil_write_disable = KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
			z.depth_view.current_mip_level     = KYTY_PM4_GET(buffer[13], DB_DEPTH_VIEW, MIPID);

			z.htile_data_base_addr = static_cast<uint64_t>(buffer[16]) << 8u;

			z.htile_surface.linear     = (buffer[19] >> Pm4::DB_HTILE_SURFACE_LINEAR_SHIFT) & Pm4::DB_HTILE_SURFACE_LINEAR_MASK;
			z.htile_surface.full_cache = (buffer[19] >> Pm4::DB_HTILE_SURFACE_FULL_CACHE_SHIFT) & Pm4::DB_HTILE_SURFACE_FULL_CACHE_MASK;
			z.htile_surface.htile_uses_preload_win = KYTY_PM4_GET(buffer[19], DB_HTILE_SURFACE, HTILE_USES_PRELOAD_WIN);
			z.htile_surface.preload         = (buffer[19] >> Pm4::DB_HTILE_SURFACE_PRELOAD_SHIFT) & Pm4::DB_HTILE_SURFACE_PRELOAD_MASK;
			z.htile_surface.prefetch_width  = KYTY_PM4_GET(buffer[19], DB_HTILE_SURFACE, PREFETCH_WIDTH);
			z.htile_surface.prefetch_height = KYTY_PM4_GET(buffer[19], DB_HTILE_SURFACE, PREFETCH_HEIGHT);
			z.htile_surface.dst_outside_zero_to_one = KYTY_PM4_GET(buffer[19], DB_HTILE_SURFACE, DST_OUTSIDE_ZERO_TO_ONE);

			z.width  = (buffer[21] >> 0u) & 0xffffu;
			z.height = (buffer[21] >> 16u) & 0xffffu;

			cp->GetCtx()->SetDepthRenderTarget(z);
		} else
		{
			KYTY_NOT_IMPLEMENTED;
		}
	} else
	{
		KYTY_NOT_IMPLEMENTED;
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_eqaa_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_EQAA) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_EQAA condition ignored (continuing)\n"); }

	trace_aa_register_write("direct", "DB_EQAA", buffer[0]);
	HW::EqaaControl r;

	r.max_anchor_samples         = KYTY_PM4_GET(buffer[0], DB_EQAA, MAX_ANCHOR_SAMPLES);
	r.ps_iter_samples            = KYTY_PM4_GET(buffer[0], DB_EQAA, PS_ITER_SAMPLES);
	r.mask_export_num_samples    = KYTY_PM4_GET(buffer[0], DB_EQAA, MASK_EXPORT_NUM_SAMPLES);
	r.alpha_to_mask_num_samples  = KYTY_PM4_GET(buffer[0], DB_EQAA, ALPHA_TO_MASK_NUM_SAMPLES);
	r.high_quality_intersections = KYTY_PM4_GET(buffer[0], DB_EQAA, HIGH_QUALITY_INTERSECTIONS) != 0;
	r.incoherent_eqaa_reads      = KYTY_PM4_GET(buffer[0], DB_EQAA, INCOHERENT_EQAA_READS) != 0;
	r.interpolate_comp_z         = KYTY_PM4_GET(buffer[0], DB_EQAA, INTERPOLATE_COMP_Z) != 0;
	r.static_anchor_associations = KYTY_PM4_GET(buffer[0], DB_EQAA, STATIC_ANCHOR_ASSOCIATIONS) != 0;

	cp->GetCtx()->SetEqaaControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_generic_scissor)
{
	if (cmd_id != 0xC0026900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0026900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_GENERIC_SCISSOR_TL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_GENERIC_SCISSOR_TL condition ignored (continuing)\n"); }

	State::SetGenericScissorTl(*cp->GetCtx(), buffer[0]);
	State::SetGenericScissorBr(*cp->GetCtx(), buffer[1]);

	return 2;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_guard_bands)
{
	if (cmd_id != 0xC0046900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0046900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_CL_GB_VERT_CLIP_ADJ) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_CL_GB_VERT_CLIP_ADJ condition ignored (continuing)\n"); }

	auto vert_clip    = *reinterpret_cast<const float*>(&buffer[0]); // PA_CL_GB_VERT_CLIP_ADJ
	auto vert_discard = *reinterpret_cast<const float*>(&buffer[1]); // PA_CL_GB_VERT_DISC_ADJ
	auto horz_clip    = *reinterpret_cast<const float*>(&buffer[2]); // PA_CL_GB_HORZ_CLIP_ADJ
	auto horz_discard = *reinterpret_cast<const float*>(&buffer[3]); // PA_CL_GB_HORZ_DISC_ADJ

	cp->GetCtx()->SetGuardBands(horz_clip, vert_clip, horz_discard, vert_discard);

	return 4;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_hardware_screen_offset)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SU_HARDWARE_SCREEN_OFFSET) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SU_HARDWARE_SCREEN_OFFSET condition ignored (continuing)\n"); }

	// uint32_t x = static_cast<uint16_t>(buffer[0] & 0xffffu);
	// uint32_t y = static_cast<uint16_t>((buffer[0] >> 16u) & 0xffffu);

	uint32_t x = KYTY_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
	uint32_t y = KYTY_PM4_GET(buffer[0], PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);

	cp->GetCtx()->SetHardwareScreenOffset(x, y);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_window_offset)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_WINDOW_OFFSET) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_WINDOW_OFFSET condition ignored (continuing)\n"); }
	State::SetWindowOffset(*cp->GetCtx(), buffer[0]);
	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_line_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SU_LINE_CNTL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SU_LINE_CNTL condition ignored (continuing)\n"); }

	auto line_width = KYTY_PM4_GET(buffer[0], PA_SU_LINE_CNTL, WIDTH);

	if (line_width == 8)
	{
		cp->GetCtx()->SetLineWidth(1.0f);
	} else
	{
		cp->GetCtx()->SetLineWidth(static_cast<float>(line_width) / 8.0f);
	}

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_mode_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SU_SC_MODE_CNTL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SU_SC_MODE_CNTL condition ignored (continuing)\n"); }

	State::SetModeControl(*cp->GetCtx(), buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_polygon_offset)
{
	const uint32_t count = (cmd_id >> 16u) & 0x3fffu;
	if (count == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: count == 0 condition ignored (continuing)\n"); }
	if (cmd_offset < Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL || cmd_offset + count > Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET + 1u) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset < Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL || cmd_offset + count > Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET + 1u condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < count; i++)
	{
		State::SetPolygonOffsetRegister(*cp->GetCtx(), cmd_offset + i, buffer[i]);
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_ps_input)
{
	if (cmd_offset != Pm4::SPI_PS_INPUT_CNTL_0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::SPI_PS_INPUT_CNTL_0 condition ignored (continuing)\n"); }

	uint32_t count = (cmd_id >> 16u) & 0x3fffu;

	if (count == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: count == 0 condition ignored (continuing)\n"); }
	if (count > 32) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: count > 32 condition ignored (continuing)\n"); }

	static const bool dump_ps_input_writes = std::getenv("KYTY_DUMP_PS_INPUT_WRITES") != nullptr;
	for (uint32_t i = 0; i < count; i++)
	{
		if (dump_ps_input_writes)
		{
			KYTY_LOG_DEBUG( "KYTY_PS_INPUT_WRITE direct slot=%u value=0x%08" PRIx32 " count=%u\n", i, buffer[i], count);
		}
		cp->GetCtx()->SetPsInputSettings(i, buffer[i]);
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_render_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_RENDER_CONTROL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_RENDER_CONTROL condition ignored (continuing)\n"); }

	State::SetRenderControl(*cp->GetCtx(), buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_render_target)
{
	if (cmd_id != 0xC00E6900 && cmd_id != 0xC00B6900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC00E6900 && cmd_id != 0xC00B6900 condition ignored (continuing)\n"); }

	uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;

	uint32_t count = 11;

	auto* ctx = cp->GetCtx();

	HW::ColorBase       base;
	HW::ColorPitch      pitch;
	HW::ColorSlice      slice;
	HW::ColorView       view;
	HW::ColorInfo       info;
	HW::ColorAttrib     attrib;
	HW::ColorDccControl dcc;
	HW::ColorCmask      cmask;
	HW::ColorCmaskSlice cmask_slice;
	HW::ColorFmask      fmask;
	HW::ColorFmaskSlice fmask_slice;

	base.addr                     = static_cast<uint64_t>(buffer[0]) << 8u;
	pitch.pitch_div8_minus1       = buffer[1] & 0x7ffu;
	pitch.fmask_pitch_div8_minus1 = (buffer[1] >> 20u) & 0x7ffu;
	slice.slice_div64_minus1      = buffer[2] & 0x3fffffu;

	view.base_array_slice_index = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, SLICE_START);
	view.last_array_slice_index = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, SLICE_MAX);
	view.current_mip_level      = KYTY_PM4_GET(buffer[3], CB_COLOR0_VIEW, MIP_LEVEL);
	info                        = State::DecodeColorInfo(buffer[4], Config::IsNextGen());

	//	attrib.force_dest_alpha_to_one  = (buffer[5] & 0x20000u) != 0;
	//	attrib.tile_mode                = buffer[5] & 0x1fu;
	//	attrib.fmask_tile_mode          = (buffer[5] >> 5u) & 0x1fu;
	//	attrib.num_samples              = (buffer[5] >> 12u) & 0x7u;
	//	attrib.num_fragments            = (buffer[5] >> 15u) & 0x3u;
	attrib.force_dest_alpha_to_one = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
	attrib.tile_mode               = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
	attrib.fmask_tile_mode         = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
	attrib.num_samples             = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, NUM_SAMPLES);
	attrib.num_fragments           = KYTY_PM4_GET(buffer[5], CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
	if (slot == 0)
	{
		trace_aa_register_write("direct", "CB_COLOR0_ATTRIB", buffer[5]);
	}

	//	dcc.max_uncompressed_block_size = (buffer[6] >> 2u) & 0x3u;
	//	dcc.max_compressed_block_size   = (buffer[6] >> 5u) & 0x3u;
	//	dcc.min_compressed_block_size   = (buffer[6] >> 4u) & 0x1u;
	//	dcc.color_transform             = (buffer[6] >> 7u) & 0x3u;
	//	dcc.overwrite_combiner_disable   = (buffer[6] & 0x1u) != 0;
	//	dcc.independent_64b_blocks    = (buffer[6] & 0x200u) != 0;
	dcc.overwrite_combiner_disable     = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
	dcc.dcc_clear_key_enable           = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
	dcc.max_uncompressed_block_size    = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
	dcc.min_compressed_block_size      = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
	dcc.max_compressed_block_size      = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
	dcc.color_transform                = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
	dcc.independent_64b_blocks         = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
	dcc.data_write_on_dcc_clear_to_reg = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
	dcc.independent_128b_blocks        = KYTY_PM4_GET(buffer[6], CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;

	cmask.addr               = static_cast<uint64_t>(buffer[7]) << 8u;
	cmask_slice.slice_minus1 = buffer[8] & 0x3fffu;

	fmask.addr               = static_cast<uint64_t>(buffer[9]) << 8u;
	fmask_slice.slice_minus1 = buffer[10] & 0x3fffffu;

	ctx->SetColorBase(slot, base);
	ctx->SetColorPitch(slot, pitch);
	ctx->SetColorSlice(slot, slice);
	ctx->SetColorView(slot, view);
	ctx->SetColorInfo(slot, info);
	ctx->SetColorAttrib(slot, attrib);
	ctx->SetColorDccControl(slot, dcc);
	ctx->SetColorCmask(slot, cmask);
	ctx->SetColorCmaskSlice(slot, cmask_slice);
	ctx->SetColorFmask(slot, fmask);
	ctx->SetColorFmaskSlice(slot, fmask_slice);

	if (cmd_id == 0xC00E6900)
	{
		count = 14;

		HW::ColorClearWord0 clear_word0;
		HW::ColorClearWord1 clear_word1;
		HW::ColorDccAddr    dcc_addr;

		clear_word0.word0 = buffer[11];
		clear_word1.word1 = buffer[12];
		dcc_addr.addr     = static_cast<uint64_t>(buffer[13]) << 8u;

		ctx->SetColorClearWord0(slot, clear_word0);
		ctx->SetColorClearWord1(slot, clear_word1);
		ctx->SetColorDccAddr(slot, dcc_addr);
	}

	if (dw >= count + 2 && buffer[count] == 0xC0001000)
	{
		HW::ColorSize size;

		size.width  = (buffer[count + 1] >> 0u) & 0xffffu;
		size.height = (buffer[count + 1] >> 16u) & 0xffffu;

		ctx->SetColorSize(slot, size);

		count += 2;
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_render_target_mask)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::CB_TARGET_MASK) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::CB_TARGET_MASK condition ignored (continuing)\n"); }

	cp->GetCtx()->SetRenderTargetMask(*buffer);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_scan_mode_control)
{
	if (cmd_id != 0xc0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_MODE_CNTL_0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_MODE_CNTL_0 condition ignored (continuing)\n"); }

	trace_aa_register_write("direct", "PA_SC_MODE_CNTL_0", buffer[0]);
	HW::ScanModeControl r;

	r.msaa_enable          = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
	r.vport_scissor_enable = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
	r.line_stipple_enable  = KYTY_PM4_GET(buffer[0], PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;

	cp->GetCtx()->SetScanModeControl(r);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_screen_scissor)
{
	if (cmd_id != 0xC0026900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0026900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_SC_SCREEN_SCISSOR_TL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_SC_SCREEN_SCISSOR_TL condition ignored (continuing)\n"); }

	int left   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(buffer[0], PA_SC_SCREEN_SCISSOR_TL, TL_X)));
	int top    = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(buffer[0], PA_SC_SCREEN_SCISSOR_TL, TL_Y)));
	int right  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(buffer[1], PA_SC_SCREEN_SCISSOR_BR, BR_X)));
	int bottom = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(buffer[1], PA_SC_SCREEN_SCISSOR_BR, BR_Y)));

	cp->GetCtx()->SetScreenScissor(left, top, right, bottom);

	return 2;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_shader_stages)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::VGT_SHADER_STAGES_EN) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::VGT_SHADER_STAGES_EN condition ignored (continuing)\n"); }

	cp->GetCtx()->SetShaderStages(buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_clear)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_STENCIL_CLEAR) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_STENCIL_CLEAR condition ignored (continuing)\n"); }

	cp->GetCtx()->SetStencilClearValue(KYTY_PM4_GET(buffer[0], DB_STENCIL_CLEAR, CLEAR));

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_STENCIL_CONTROL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_STENCIL_CONTROL condition ignored (continuing)\n"); }

	State::SetStencilControl(*cp->GetCtx(), buffer[0]);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_info)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_STENCIL_INFO) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_STENCIL_INFO condition ignored (continuing)\n"); }
	cp->GetCtx()->SetDepthStencilInfo(State::DecodeDepthStencilInfo(buffer[0]));

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_stencil_mask)
{
	if (cmd_id != 0xc0026900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0026900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::DB_STENCILREFMASK) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::DB_STENCILREFMASK condition ignored (continuing)\n"); }

	State::SetStencilRefMask(*cp->GetCtx(), buffer[0]);
	State::SetStencilRefMaskBf(*cp->GetCtx(), buffer[1]);

	return 2;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_scale_offset)
{
	const uint32_t count = (cmd_id >> 16u) & 0x3fffu;
	if (count == 0 || count > 6) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: count == 0 || count > 6 condition ignored (continuing)\n"); }
	if (cmd_offset < Pm4::PA_CL_VPORT_XSCALE || cmd_offset > Pm4::PA_CL_VPORT_ZOFFSET_15) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset < Pm4::PA_CL_VPORT_XSCALE || cmd_offset > Pm4::PA_CL_VPORT_ZOFFSET_15 condition ignored (continuing)\n"); }

	const uint32_t relative  = cmd_offset - Pm4::PA_CL_VPORT_XSCALE;
	const uint32_t viewport  = relative / 6;
	const uint32_t component = relative % 6;
	if (component + count > 6) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: component + count > 6 condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < count; i++)
	{
		const float value = *reinterpret_cast<const float*>(buffer + i);
		switch (component + i)
		{
			case 0: cp->GetCtx()->SetViewportXScale(viewport, value); break;
			case 1: cp->GetCtx()->SetViewportXOffset(viewport, value); break;
			case 2: cp->GetCtx()->SetViewportYScale(viewport, value); break;
			case 3: cp->GetCtx()->SetViewportYOffset(viewport, value); break;
			case 4: cp->GetCtx()->SetViewportZScale(viewport, value); break;
			case 5: cp->GetCtx()->SetViewportZOffset(viewport, value); break;
			default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: invalid viewport component (continuing)\n"); break;
		}
	}

	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_ignore)
{
	const uint32_t count = (cmd_id >> 16u) & 0x3fffu;
	if (count == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: count == 0 condition ignored (continuing)\n"); }
	(void)cp;
	(void)cmd_offset;
	(void)buffer;
	return count;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_transform_control)
{
	if (cmd_id != 0xC0016900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0016900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::PA_CL_VTE_CNTL) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::PA_CL_VTE_CNTL condition ignored (continuing)\n"); }

	cp->GetCtx()->SetViewportTransformControl(*buffer);

	return 1;
}

KYTY_HW_CTX_PARSER(hw_ctx_set_viewport_z)
{
	if (cmd_id != 0xC0026900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0026900 condition ignored (continuing)\n"); }

	uint32_t param = (cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2;

	if (param != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: param != 0 condition ignored (continuing)\n"); }

	cp->GetCtx()->SetViewportZ(param, *reinterpret_cast<const float*>(buffer), *reinterpret_cast<const float*>(buffer + 1));

	return 2;
}

KYTY_HW_SH_PARSER(hw_sh_set_cs_shader)
{
	if (cmd_id != 0xC017101C) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC017101C condition ignored (continuing)\n"); }

	auto shader_modifier = buffer[0];

	HW::CsStageRegisters r {};

	r.data_addr = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2]) << 40u);
	decode_compute_pgm_rsrc1(r, buffer[3]);
	decode_compute_pgm_rsrc2(r, buffer[4]);
	r.num_thread_x = buffer[5];
	r.num_thread_y = buffer[6];
	r.num_thread_z = buffer[7];

	cp->GetShCtx()->SetCsShader(r, shader_modifier);

	return 24;
}

// COMPUTE_PGM_LO/HI: shader code address, written as individual registers.
KYTY_HW_SH_PARSER(hw_sh_set_cs_pgm)
{
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;
	if (cmd_offset != Pm4::COMPUTE_PGM_LO || reg_num != 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::COMPUTE_PGM_LO || reg_num != 2 condition ignored (continuing)\n"); }

	auto& r     = cp->GetShCtx()->CsRegs();
	r.data_addr = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1]) << 40u);

	return reg_num;
}

// COMPUTE_PGM_RSRC1/RSRC2: compute program resource descriptors.
KYTY_HW_SH_PARSER(hw_sh_set_cs_rsrc)
{
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	auto& r = cp->GetShCtx()->CsRegs();
	for (uint32_t i = 0; i < reg_num; i++)
	{
		const uint32_t offset = cmd_offset + i;
		switch (offset)
		{
			case Pm4::COMPUTE_PGM_RSRC1: decode_compute_pgm_rsrc1(r, buffer[i]); break;
			case Pm4::COMPUTE_PGM_RSRC2: decode_compute_pgm_rsrc2(r, buffer[i]); break;
			case Pm4::COMPUTE_PGM_RSRC3: r.rsrc3 = buffer[i]; break;
			case Pm4::COMPUTE_SHADER_CHKSUM: r.chksum = (r.chksum & 0xffffffff00000000ull) | static_cast<uint64_t>(buffer[i]); break;
			case Pm4::COMPUTE_SHADER_CHKSUM_HI:
				r.chksum = (r.chksum & 0x00000000ffffffffull) | (static_cast<uint64_t>(buffer[i]) << 32u);
				break;
			default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unexpected compute rsrc register (continuing)\n"); break;
		}
	}

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_cs_num_thread)
{
	// COMPUTE_NUM_THREAD_X/Y/Z, written as a run of individual registers by a
	// guest-built command buffer. Decode each component into the compute state.
	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	for (uint32_t i = 0; i < reg_num; i++)
	{
		const uint32_t offset = cmd_offset + i;
		const uint32_t value  = buffer[i];
		switch (offset)
		{
			case Pm4::COMPUTE_NUM_THREAD_X: cp->GetShCtx()->SetCsNumThreadX(value); break;
			case Pm4::COMPUTE_NUM_THREAD_Y: cp->GetShCtx()->SetCsNumThreadY(value); break;
			case Pm4::COMPUTE_NUM_THREAD_Z: cp->GetShCtx()->SetCsNumThreadZ(value); break;
			default: KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: unexpected compute thread register (continuing)\n"); break;
		}
	}

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_cs_resource_limits)
{
	const auto reg_num = (cmd_id >> 16u) & 0x3fffu;
	if (!GraphicsDecodeComputeResourceLimits(&cp->GetShCtx()->CsRegs(), cmd_offset, buffer, reg_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !GraphicsDecodeComputeResourceLimits(&cp->GetShCtx()->CsRegs(), cmd_offset, buffer, reg_num) condition ignored (continuing)\n"); }

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_cs_user_sgpr)
{
	if (!(cmd_offset >= Pm4::COMPUTE_USER_DATA_0 && cmd_offset <= Pm4::COMPUTE_USER_DATA_15)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(cmd_offset >= Pm4::COMPUTE_USER_DATA_0 && cmd_offset <= Pm4::COMPUTE_USER_DATA_15) condition ignored (continuing)\n"); }

	uint32_t slot = (cmd_offset - Pm4::COMPUTE_USER_DATA_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	if (!HW::UserSgprInfo::WriteRangeValid(slot, reg_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !HW::UserSgprInfo::WriteRangeValid(slot, reg_num) condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < reg_num; i++)
	{
		cp->GetShCtx()->SetCsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_ps_embedded)
{
	if (cmd_id != 0xc0261038) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0261038 condition ignored (continuing)\n"); }

	auto id = buffer[0];

	cp->GetShCtx()->SetPsEmbedded(id);

	return 39;
}

KYTY_HW_SH_PARSER(hw_sh_set_ps_shader)
{
	if (cmd_id != 0xC0261008) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0261008 condition ignored (continuing)\n"); }

	// HW::PsStageRegisters r {};
	HW::PsShaderResource1 r1;
	HW::PsShaderResource2 r2;

	//	r.data_addr   = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1]) << 40u);
	//	r.vgprs       = (buffer[2] >> Pm4::SPI_SHADER_PGM_RSRC1_PS_VGPRS_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC1_PS_VGPRS_MASK;
	//	r.sgprs       = (buffer[2] >> Pm4::SPI_SHADER_PGM_RSRC1_PS_SGPRS_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC1_PS_SGPRS_MASK;
	//	r.scratch_en  = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_SCRATCH_EN_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_SCRATCH_EN_MASK;
	//	r.user_sgpr   = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_USER_SGPR_MASK;
	//	r.wave_cnt_en = (buffer[3] >> Pm4::SPI_SHADER_PGM_RSRC2_PS_WAVE_CNT_EN_SHIFT) & Pm4::SPI_SHADER_PGM_RSRC2_PS_WAVE_CNT_EN_MASK;

	uint64_t addr = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1] & 0xffu) << 40u);

	r1.vgprs                    = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, VGPRS);
	r1.sgprs                    = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, SGPRS);
	r1.priority                 = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
	r1.float_mode               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
	r1.dx10_clamp               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
	r1.debug_mode               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
	r1.ieee_mode                = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
	r1.cu_group_disable         = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
	r1.require_forward_progress = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;

	r2.scratch_en             = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
	r2.user_sgpr              = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) +
	                            (KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
	r2.wave_cnt_en            = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
	r2.extra_lds_size         = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
	r2.raster_ordered_shading = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, RASTER_ORDERED_SHADING);
	r2.shared_vgprs           = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);

	cp->GetCtx()->SetShaderZFormat(buffer[4]);

	for (uint32_t i = 0; i < 8; i++)
	{
		cp->GetCtx()->SetTargetOutputMode(i, (buffer[5] >> (i * 4)) & 0xFu);
	}

	cp->GetCtx()->SetPsInputEna(buffer[6]);
	cp->GetCtx()->SetPsInputAddr(buffer[7]);
	cp->GetCtx()->SetPsInControl(buffer[8]);
	cp->GetCtx()->SetBarycCntl(buffer[9]);

	HW::DepthShaderControl db_shader_control {};

	//	db_shader_control.conservative_z_export_value = (buffer[10] >> 13u) & 0x3u;
	//	db_shader_control.shader_z_behavior           = (buffer[10] >> 4u) & 0x3u;
	//	db_shader_control.shader_kill_enable          = (buffer[10] & 0x40u) != 0;
	//	db_shader_control.shader_z_export_enable      = (buffer[10] & 0x1u) != 0;
	//	db_shader_control.shader_execute_on_noop      = (buffer[10] & 0x400u) != 0;
	db_shader_control.other_bits                  = buffer[10] & 0xFFFF9B8Eu;
	db_shader_control.conservative_z_export_value = KYTY_PM4_GET(buffer[10], DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
	db_shader_control.shader_z_behavior           = KYTY_PM4_GET(buffer[10], DB_SHADER_CONTROL, Z_ORDER);
	db_shader_control.shader_kill_enable          = KYTY_PM4_GET(buffer[10], DB_SHADER_CONTROL, KILL_ENABLE) != 0;
	db_shader_control.shader_z_export_enable      = KYTY_PM4_GET(buffer[10], DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
	db_shader_control.shader_execute_on_noop      = KYTY_PM4_GET(buffer[10], DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;

	cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	cp->GetCtx()->SetShaderMask(buffer[11]);

	// cp->GetShCtx()->SetPsShader(r);
	cp->GetShCtx()->SetPsShaderBase(addr);
	cp->GetShCtx()->SetPsShaderResource1(r1);
	cp->GetShCtx()->SetPsShaderResource2(r2);

	return 39;
}

KYTY_HW_SH_PARSER(hw_sh_set_ps_user_sgpr)
{
	if (!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_PS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_PS_31)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_PS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_PS_31) condition ignored (continuing)\n"); }

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_PS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	if (!HW::UserSgprInfo::WriteRangeValid(slot, reg_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !HW::UserSgprInfo::WriteRangeValid(slot, reg_num) condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < reg_num; i++)
	{
		cp->GetShCtx()->SetPsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_vs_embedded)
{
	if (cmd_id != 0xc01b1034) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc01b1034 condition ignored (continuing)\n"); }

	auto shader_modifier = buffer[0];
	auto id              = buffer[1];

	cp->GetShCtx()->SetVsEmbedded(id, shader_modifier);

	return 28;
}

KYTY_HW_SH_PARSER(hw_sh_set_vs_shader)
{
	if (cmd_id != 0xC01B1004) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC01B1004 condition ignored (continuing)\n"); }

	auto shader_modifier = buffer[0];

	// HW::VsStageRegisters r {};

	uint64_t addr = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2] & 0xffu) << 40u);

	HW::VsShaderResource1 r1;
	HW::VsShaderResource2 r2;

	// r.m_spiShaderPgmLoVs    = buffer[1];
	// r.m_spiShaderPgmHiVs    = buffer[2];
	// r.m_spiShaderPgmRsrc1Vs = buffer[3];
	// r.m_spiShaderPgmRsrc2Vs = buffer[4];

	r1.vgprs                    = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPRS);
	r1.sgprs                    = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, SGPRS);
	r1.priority                 = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
	r1.float_mode               = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
	r1.dx10_clamp               = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
	r1.ieee_mode                = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
	r1.vgpr_component_count     = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
	r1.cu_group_enable          = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
	r1.require_forward_progress = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;

	r2.scratch_en        = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
	r2.user_sgpr         = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
	                       (KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
	r2.offchip_lds       = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
	r2.streamout_enabled = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
	r2.shared_vgprs      = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);

	uint32_t m_spi_vs_out_config     = buffer[5];
	uint32_t m_spi_shader_pos_format = buffer[6];
	uint32_t m_pa_cl_vs_out_cntl     = buffer[7];

	// cp->GetShCtx()->SetVsShader(r, shader_modifier);
	cp->GetShCtx()->SetVsShaderBase(addr);
	cp->GetShCtx()->SetVsShaderModifier(shader_modifier);
	cp->GetShCtx()->SetVsShaderResource1(r1);
	cp->GetShCtx()->SetVsShaderResource2(r2);

	cp->GetCtx()->SetVsOutConfig(m_spi_vs_out_config);
	cp->GetCtx()->SetShaderPosFormat(m_spi_shader_pos_format);
	cp->GetCtx()->SetClVsOutCntl(m_pa_cl_vs_out_cntl);

	return 28;
}

KYTY_HW_SH_PARSER(hw_sh_set_vs_user_sgpr)
{
	if (!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_VS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_VS_15)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_VS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_VS_15) condition ignored (continuing)\n"); }

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_VS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	if (!HW::UserSgprInfo::WriteRangeValid(slot, reg_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !HW::UserSgprInfo::WriteRangeValid(slot, reg_num) condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < reg_num; i++)
	{
		cp->GetShCtx()->SetVsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_set_gs_user_sgpr)
{
	if (!(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_GS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_GS_15)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !(cmd_offset >= Pm4::SPI_SHADER_USER_DATA_GS_0 && cmd_offset <= Pm4::SPI_SHADER_USER_DATA_GS_15) condition ignored (continuing)\n"); }

	uint32_t slot = (cmd_offset - Pm4::SPI_SHADER_USER_DATA_GS_0) / 1;

	auto reg_num = (cmd_id >> 16u) & 0x3fffu;

	if (!HW::UserSgprInfo::WriteRangeValid(slot, reg_num)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !HW::UserSgprInfo::WriteRangeValid(slot, reg_num) condition ignored (continuing)\n"); }

	for (uint32_t i = 0; i < reg_num; i++)
	{
		cp->GetShCtx()->SetGsUserSgpr(slot + i, buffer[i], cp->GetUserDataMarker());
	}
	cp->SetUserDataMarker(HW::UserSgprType::Unknown);

	return reg_num;
}

KYTY_HW_SH_PARSER(hw_sh_update_ps_shader)
{
	if (cmd_id != 0xc0261040) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc0261040 condition ignored (continuing)\n"); }

	HW::PsShaderResource1 r1;
	HW::PsShaderResource2 r2;

	uint64_t addr = (static_cast<uint64_t>(buffer[0]) << 8u) | (static_cast<uint64_t>(buffer[1] & 0xffu) << 40u);

	r1.vgprs                    = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, VGPRS);
	r1.sgprs                    = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, SGPRS);
	r1.priority                 = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
	r1.float_mode               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
	r1.dx10_clamp               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
	r1.debug_mode               = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
	r1.ieee_mode                = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
	r1.cu_group_disable         = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
	r1.require_forward_progress = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = KYTY_PM4_GET(buffer[2], SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;

	r2.scratch_en             = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
	r2.user_sgpr              = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) +
	                            (KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
	r2.wave_cnt_en            = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
	r2.extra_lds_size         = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
	r2.raster_ordered_shading = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, RASTER_ORDERED_SHADING);
	r2.shared_vgprs           = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);

	cp->GetCtx()->SetShaderZFormat(buffer[4]);

	// cp->GetShCtx()->UpdatePsShader(r);
	cp->GetShCtx()->SetPsShaderBase(addr);
	cp->GetShCtx()->SetPsShaderResource1(r1);
	cp->GetShCtx()->SetPsShaderResource2(r2);

	return 39;
}

KYTY_HW_SH_PARSER(hw_sh_update_vs_shader)
{
	if (cmd_id != 0xc01b103c) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xc01b103c condition ignored (continuing)\n"); }

	auto shader_modifier = buffer[0];

	uint64_t addr = (static_cast<uint64_t>(buffer[1]) << 8u) | (static_cast<uint64_t>(buffer[2] & 0xffu) << 40u);

	HW::VsShaderResource1 r1;
	HW::VsShaderResource2 r2;

	r1.vgprs                    = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPRS);
	r1.sgprs                    = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, SGPRS);
	r1.priority                 = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, PRIORITY);
	r1.float_mode               = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FLOAT_MODE);
	r1.dx10_clamp               = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, DX10_CLAMP) != 0;
	r1.ieee_mode                = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, IEEE_MODE) != 0;
	r1.vgpr_component_count     = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, VGPR_COMP_CNT);
	r1.cu_group_enable          = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, CU_GROUP_ENABLE) != 0;
	r1.require_forward_progress = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FWD_PROGRESS) != 0;
	r1.fp16_overflow            = KYTY_PM4_GET(buffer[3], SPI_SHADER_PGM_RSRC1_VS, FP16_OVFL) != 0;

	r2.scratch_en        = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SCRATCH_EN) != 0;
	r2.user_sgpr         = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR) +
	                       (KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, USER_SGPR_MSB) << 5u);
	r2.offchip_lds       = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, OC_LDS_EN) != 0;
	r2.streamout_enabled = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SO_EN) != 0;
	r2.shared_vgprs      = KYTY_PM4_GET(buffer[4], SPI_SHADER_PGM_RSRC2_VS, SHARED_VGPR_CNT);

	cp->GetShCtx()->SetVsShaderBase(addr);
	cp->GetShCtx()->SetVsShaderModifier(shader_modifier);
	cp->GetShCtx()->SetVsShaderResource1(r1);
	cp->GetShCtx()->SetVsShaderResource2(r2);

	return 28;
}

KYTY_HW_UC_PARSER(hw_uc_set_primitive_type)
{
	if (cmd_id != 0xC0017900) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_id != 0xC0017900 condition ignored (continuing)\n"); }
	if (cmd_offset != Pm4::VGT_PRIMITIVE_TYPE) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: cmd_offset != Pm4::VGT_PRIMITIVE_TYPE condition ignored (continuing)\n"); }

	uint32_t prim_type = KYTY_PM4_GET(buffer[0], VGT_PRIMITIVE_TYPE, PRIM_TYPE);

	cp->GetUcfg()->SetPrimitiveType(prim_type);

	return 1;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
