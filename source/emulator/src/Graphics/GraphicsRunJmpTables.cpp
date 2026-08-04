#include "GraphicsRunInternal.h"

#include "GraphicsComputeRegisters.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Utils.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {
static void graphics_init_jmp_tables_cx_indirect()
{
	for (auto& func: g_hw_ctx_indirect_func)
	{
		func = nullptr;
	}

	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetGenericScissorTl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_GENERIC_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetGenericScissorBr(*cp->GetCtx(), value); };
	// Screen scissor is handled as a TL+BR pair on the direct SET_CONTEXT_REG path;
	// Gen5 CX-indirect emits the halves separately.
	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_TL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetScreenScissorTl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_SCREEN_SCISSOR_BR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ State::SetScreenScissorBr(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SU_SC_MODE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetModeControl(*cp->GetCtx(), value); };
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; reg++)
	{
		g_hw_ctx_indirect_func[reg] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetPolygonOffsetRegister(*cp->GetCtx(), cmd_offset, value); };
	}
	// Gen5 CX-indirect emits DB_RENDER_CONTROL (offset 0) as a lone pair; direct
	// SET_CONTEXT_REG already uses the same decoder.
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_CONTROL]  = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetRenderControl(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilControl(*cp->GetCtx(), value); };
	// Direct SET_CONTEXT_REG writes REFMASK+REFMASK_BF as a 2-dword pair; Gen5
	// CX-indirect emits each half alone.
	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilRefMask(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCILREFMASK_BF] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetStencilRefMaskBf(*cp->GetCtx(), value); };
	// Direct path registers CB_BLEND0..7; indirect must share the same decoder
	// (captured: CB_BLEND1_CONTROL = 0x1e1 after post-menu load).
	for (uint32_t slot = 0; slot < 8; slot++)
	{
		const uint32_t blend_reg          = Pm4::CB_BLEND0_CONTROL + slot;
		g_hw_ctx_indirect_func[blend_reg] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			const uint32_t blend_slot = cmd_offset - Pm4::CB_BLEND0_CONTROL;
			State::SetBlendControl(*cp->GetCtx(), blend_slot, value);
		};
	}

	for (auto cmd_offset = Pm4::SPI_PS_INPUT_CNTL_0; cmd_offset <= Pm4::SPI_PS_INPUT_CNTL_31; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = cmd_offset - Pm4::SPI_PS_INPUT_CNTL_0;
			cp->GetCtx()->SetPsInputSettings(slot, value);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE; cmd_offset <= Pm4::CB_COLOR7_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).base;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorBase(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_VIEW; cmd_offset <= Pm4::CB_COLOR7_VIEW; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t      slot = (cmd_offset - Pm4::CB_COLOR0_VIEW) / 15;
			HW::ColorView view;
			view.base_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_START);
			view.last_array_slice_index = KYTY_PM4_GET(value, CB_COLOR0_VIEW, SLICE_MAX);
			view.current_mip_level      = KYTY_PM4_GET(value, CB_COLOR0_VIEW, MIP_LEVEL);
			cp->GetCtx()->SetColorView(slot, view);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_INFO; cmd_offset <= Pm4::CB_COLOR7_INFO; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			const uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_INFO) / 15;
			cp->GetCtx()->SetColorInfo(slot, State::DecodeColorInfo(value, Config::IsNextGen()));
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB; cmd_offset <= Pm4::CB_COLOR7_ATTRIB; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t        slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB) / 15;
			if (slot == 0)
			{
				trace_aa_register_write("indirect", "CB_COLOR0_ATTRIB", value);
			}
			HW::ColorAttrib attrib;
			attrib.force_dest_alpha_to_one = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FORCE_DST_ALPHA_1) != 0;
			attrib.tile_mode               = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, TILE_MODE_INDEX);
			attrib.fmask_tile_mode         = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, FMASK_TILE_MODE_INDEX);
			attrib.num_samples             = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_SAMPLES);
			attrib.num_fragments           = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB, NUM_FRAGMENTS);
			cp->GetCtx()->SetColorAttrib(slot, attrib);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_CONTROL; cmd_offset <= Pm4::CB_COLOR7_DCC_CONTROL; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t            slot = (cmd_offset - Pm4::CB_COLOR0_DCC_CONTROL) / 15;
			HW::ColorDccControl dcc;
			dcc.overwrite_combiner_disable     = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, OVERWRITE_COMBINER_DISABLE) != 0;
			dcc.dcc_clear_key_enable           = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, KEY_CLEAR_ENABLE) != 0;
			dcc.max_uncompressed_block_size    = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_UNCOMPRESSED_BLOCK_SIZE);
			dcc.min_compressed_block_size      = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MIN_COMPRESSED_BLOCK_SIZE);
			dcc.max_compressed_block_size      = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, MAX_COMPRESSED_BLOCK_SIZE);
			dcc.color_transform                = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, COLOR_TRANSFORM);
			dcc.independent_64b_blocks         = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_64B_BLOCKS) != 0;
			dcc.data_write_on_dcc_clear_to_reg = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, ENABLE_CONSTANT_ENCODE_REG_WRITE) != 0;
			dcc.independent_128b_blocks        = KYTY_PM4_GET(value, CB_COLOR0_DCC_CONTROL, INDEPENDENT_128B_BLOCKS) != 0;
			cp->GetCtx()->SetColorDccControl(slot, dcc);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK; cmd_offset <= Pm4::CB_COLOR7_CMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_CMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_CMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).cmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorCmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK; cmd_offset <= Pm4::CB_COLOR7_FMASK; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_FMASK_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_FMASK_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_FMASK_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).fmask;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorFmask(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD0; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD0; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord0 clear_word0;
			clear_word0.word0 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD0) / 15;
			cp->GetCtx()->SetColorClearWord0(slot, clear_word0);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_CLEAR_WORD1; cmd_offset <= Pm4::CB_COLOR7_CLEAR_WORD1; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			HW::ColorClearWord1 clear_word1;
			clear_word1.word1 = value;
			uint32_t slot     = (cmd_offset - Pm4::CB_COLOR0_CLEAR_WORD1) / 15;
			cp->GetCtx()->SetColorClearWord1(slot, clear_word1);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE; cmd_offset += 15)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE) / 15;
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFFFF00000000FFull;
			base.addr |= static_cast<uint64_t>(value) << 8u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_DCC_BASE_EXT; cmd_offset <= Pm4::CB_COLOR7_DCC_BASE_EXT; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t slot = (cmd_offset - Pm4::CB_COLOR0_DCC_BASE_EXT);
			auto     base = cp->GetCtx()->GetRenderTarget(slot).dcc_addr;
			base.addr &= 0xFFFF00FFFFFFFFFFull;
			base.addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
			cp->GetCtx()->SetColorDccAddr(slot, base);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB2; cmd_offset <= Pm4::CB_COLOR7_ATTRIB2; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB2);
			HW::ColorAttrib2 attrib2;
			attrib2.height         = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_HEIGHT);
			attrib2.width          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MIP0_WIDTH);
			attrib2.num_mip_levels = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB2, MAX_MIP);
			cp->GetCtx()->SetColorAttrib2(slot, attrib2);
		};
	}

	for (auto cmd_offset = Pm4::CB_COLOR0_ATTRIB3; cmd_offset <= Pm4::CB_COLOR7_ATTRIB3; cmd_offset++)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			uint32_t         slot = (cmd_offset - Pm4::CB_COLOR0_ATTRIB3);
			HW::ColorAttrib3 attrib3;
			attrib3.depth              = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, MIP0_DEPTH);
			attrib3.tile_mode          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, COLOR_SW_MODE);
			attrib3.dimension          = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, RESOURCE_TYPE);
			attrib3.cmask_pipe_aligned = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, CMASK_PIPE_ALIGNED);
			attrib3.dcc_pipe_aligned   = KYTY_PM4_GET(value, CB_COLOR0_ATTRIB3, DCC_PIPE_ALIGNED);
			cp->GetCtx()->SetColorAttrib3(slot, attrib3);
		};
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XSCALE; cmd_offset <= Pm4::PA_CL_VPORT_XSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXScale((cmd_offset - Pm4::PA_CL_VPORT_XSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_XOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_XOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportXOffset((cmd_offset - Pm4::PA_CL_VPORT_XOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YSCALE; cmd_offset <= Pm4::PA_CL_VPORT_YSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYScale((cmd_offset - Pm4::PA_CL_VPORT_YSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_YOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_YOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportYOffset((cmd_offset - Pm4::PA_CL_VPORT_YOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZSCALE; cmd_offset <= Pm4::PA_CL_VPORT_ZSCALE_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZScale((cmd_offset - Pm4::PA_CL_VPORT_ZSCALE) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_CL_VPORT_ZOFFSET; cmd_offset <= Pm4::PA_CL_VPORT_ZOFFSET_15; cmd_offset += 6)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZOffset((cmd_offset - Pm4::PA_CL_VPORT_ZOFFSET) / 6, *reinterpret_cast<const float*>(&value)); };
	}

	// Guard-band adj floats written one-at-a-time via IT_SET_CONTEXT_REG
	// indirect (Gen5 AGC). Bulk four-dword form is handled by hw_ctx_set_guard_bands.
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, *reinterpret_cast<const float*>(&value), vp.guard_band_horz_discard,
		                            vp.guard_band_vert_discard);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_VERT_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip, vp.guard_band_horz_discard,
		                            *reinterpret_cast<const float*>(&value));
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_CLIP_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(*reinterpret_cast<const float*>(&value), vp.guard_band_vert_clip, vp.guard_band_horz_discard,
		                            vp.guard_band_vert_discard);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_GB_HORZ_DISC_ADJ] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto vp = cp->GetCtx()->GetScreenViewport();
		cp->GetCtx()->SetGuardBands(vp.guard_band_horz_clip, vp.guard_band_vert_clip, *reinterpret_cast<const float*>(&value),
		                            vp.guard_band_vert_discard);
	};

	// Single-dword forms of bulk CX parsers (Gen5 indirect set path).
	g_hw_ctx_indirect_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const uint32_t x = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_X);
		const uint32_t y = KYTY_PM4_GET(value, PA_SU_HARDWARE_SCREEN_OFFSET, HW_SCREEN_OFFSET_Y);
		cp->GetCtx()->SetHardwareScreenOffset(x, y);
	};
	g_hw_ctx_indirect_func[Pm4::PA_CL_CLIP_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::ClipControl r;
		r.user_clip_planes                    = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_ENA);
		r.user_clip_plane_mode                = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_MODE);
		r.dx_clip_space                       = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_CLIP_SPACE_DEF) != 0;
		r.vertex_kill_any                     = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTX_KILL_OR) != 0;
		r.min_z_clip_disable                  = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_NEAR_DISABLE) != 0;
		r.max_z_clip_disable                  = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, ZCLIP_FAR_DISABLE) != 0;
		r.user_clip_plane_negate_y            = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, PS_UCP_Y_SCALE_NEG) != 0;
		r.clip_disable                        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, CLIP_DISABLE) != 0;
		r.user_clip_plane_cull_only           = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, UCP_CULL_ONLY_ENA) != 0;
		r.cull_on_clipping_error_disable      = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DIS_CLIP_ERR_DETECT) != 0;
		r.linear_attribute_clip_enable        = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, DX_LINEAR_ATTR_CLIP_ENA) != 0;
		r.force_viewport_index_from_vs_enable = KYTY_PM4_GET(value, PA_CL_CLIP_CNTL, VTE_VPORT_PROVOKE_DISABLE) != 0;
		cp->GetCtx()->SetClipControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::CB_COLOR_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "CB_COLOR_CONTROL", value);
		HW::ColorControl r;
		r.mode = KYTY_PM4_GET(value, CB_COLOR_CONTROL, MODE);
		r.op   = KYTY_PM4_GET(value, CB_COLOR_CONTROL, ROP3);
		cp->GetCtx()->SetColorControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SU_LINE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		const auto line_width = KYTY_PM4_GET(value, PA_SU_LINE_CNTL, WIDTH);
		cp->GetCtx()->SetLineWidth(line_width == 8 ? 1.0f : static_cast<float>(line_width) / 8.0f);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "PA_SC_AA_CONFIG", value);
		HW::AaConfig r;
		r.msaa_num_samples      = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_NUM_SAMPLES);
		r.aa_mask_centroid_dtmn = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, AA_MASK_CENTROID_DTMN) != 0;
		r.max_sample_dist       = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MAX_SAMPLE_DIST);
		r.msaa_exposed_samples  = KYTY_PM4_GET(value, PA_SC_AA_CONFIG, MSAA_EXPOSED_SAMPLES);
		cp->GetCtx()->SetAaConfig(r);
	};
	g_hw_ctx_indirect_func[Pm4::DB_EQAA] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "DB_EQAA", value);
		HW::EqaaControl r;
		r.max_anchor_samples         = KYTY_PM4_GET(value, DB_EQAA, MAX_ANCHOR_SAMPLES);
		r.ps_iter_samples            = KYTY_PM4_GET(value, DB_EQAA, PS_ITER_SAMPLES);
		r.mask_export_num_samples    = KYTY_PM4_GET(value, DB_EQAA, MASK_EXPORT_NUM_SAMPLES);
		r.alpha_to_mask_num_samples  = KYTY_PM4_GET(value, DB_EQAA, ALPHA_TO_MASK_NUM_SAMPLES);
		r.high_quality_intersections = KYTY_PM4_GET(value, DB_EQAA, HIGH_QUALITY_INTERSECTIONS) != 0;
		r.incoherent_eqaa_reads      = KYTY_PM4_GET(value, DB_EQAA, INCOHERENT_EQAA_READS) != 0;
		r.interpolate_comp_z         = KYTY_PM4_GET(value, DB_EQAA, INTERPOLATE_COMP_Z) != 0;
		r.static_anchor_associations = KYTY_PM4_GET(value, DB_EQAA, STATIC_ANCHOR_ASSOCIATIONS) != 0;
		cp->GetCtx()->SetEqaaControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_RED] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color = cp->GetCtx()->GetBlendColor();
		color.red  = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_GREEN] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color  = cp->GetCtx()->GetBlendColor();
		color.green = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_BLUE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color = cp->GetCtx()->GetBlendColor();
		color.blue = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};
	g_hw_ctx_indirect_func[Pm4::CB_BLEND_ALPHA] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto color  = cp->GetCtx()->GetBlendColor();
		color.alpha = *reinterpret_cast<const float*>(&value);
		cp->GetCtx()->SetBlendColor(color);
	};

	for (uint32_t sample = 0; sample < 16u; sample++)
	{
		g_hw_ctx_indirect_func[Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0 + sample] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			auto control                                                           = cp->GetCtx()->GetAaSampleControl();
			control.locations[cmd_offset - Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = value;
			cp->GetCtx()->SetAaSampleControl(control);
		};
	}

	// Host-irrelevant GPU metadata / modes that Kyty accepts without state
	// (no guest-visible Vulkan mapping yet). Accept to keep PM4 streams moving.
	const auto ignore_cx = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		(void)cp;
		(void)cmd_offset;
		(void)value;
	};
	g_hw_ctx_indirect_func[Pm4::CB_DCC_CONTROL]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_COUNT_CONTROL]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RENDER_OVERRIDE2]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DFSM_CONTROL]              = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_RMI_L2_CACHE_CONTROL]      = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::CB_RMI_GL2_CACHE_CONTROL]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR]              = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::TA_BC_BASE_ADDR_HI]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_SIZE]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_POINT_MINMAX]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::SPI_TMPRING_SIZE]             = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_MULTI_PRIM_IB_RESET_INDX] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_DRAW_PAYLOAD_CNTL]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_RESET]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_CL_OBJPRIM_ID_CNTL]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_LR]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_FOV_WINDOW_TB]          = ignore_cx;
	// PA_SC_FSR_ENABLE / FSR_RECURSIONS* use host-only fake offsets
	// (0x800003FC..) outside the CX table; bulk path only.
	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_1]                     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1]               = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SU_VTX_CNTL]                        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK]       = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_0]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_BINNER_CNTL_1]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_NGG_MODE_CNTL]                   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_ALPHA_TO_MASK]                      = ignore_cx;
	// Window scissor/offset and tessellation stage regs need full Context
	// fields (Kyty). Accept values for now so Gen5 bootstreams proceed;
	// geometry that depends on them will need the proper setters later.
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_OFFSET]     = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetWindowOffset(*cp->GetCtx(), value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_TL] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_WINDOW_SCISSOR_BR] = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MAX_TESS_LEVEL]  = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_HOS_MIN_TESS_LEVEL]  = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_PRIMITIVEID_EN]      = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_REUSE_OFF]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_TESS_DISTRIBUTION]   = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_LS_HS_CONFIG]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::VGT_TF_PARAM]            = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MIN]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_BOUNDS_MAX]     = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_HTILE_SURFACE]        = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_INFO]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE]           = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SLICE]          = ignore_cx;
	// Legacy EPITCH fields. GFX10 Vulkan depth resources derive their geometry
	// from the image descriptor rather than these GFX9-era context registers.
	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO2]                = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO2]          = ignore_cx;
	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_0] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto r              = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority = (r.centroid_priority & 0xffffffff00000000ull) | value;
		cp->GetCtx()->SetAaSampleControl(r);
	};
	g_hw_ctx_indirect_func[Pm4::PA_SC_CENTROID_PRIORITY_1] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto r              = cp->GetCtx()->GetAaSampleControl();
		r.centroid_priority = (r.centroid_priority & 0x00000000ffffffffull) | (static_cast<uint64_t>(value) << 32u);
		cp->GetCtx()->SetAaSampleControl(r);
	};

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_TL; cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_TL; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			int  left                  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_X)));
			int  top                   = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, TL_Y)));
			bool window_offset_disable = KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_TL, WINDOW_OFFSET_DISABLE) != 0;
			cp->GetCtx()->SetViewportScissorTL((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_TL) / 2, left, top, !window_offset_disable);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_SCISSOR_0_BR; cmd_offset <= Pm4::PA_SC_VPORT_SCISSOR_15_BR; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{
			int right  = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_X)));
			int bottom = static_cast<int16_t>(static_cast<uint16_t>(KYTY_PM4_GET(value, PA_SC_VPORT_SCISSOR_0_BR, BR_Y)));
			cp->GetCtx()->SetViewportScissorBR((cmd_offset - Pm4::PA_SC_VPORT_SCISSOR_0_BR) / 2, right, bottom);
		};
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMIN_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMIN_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMin((cmd_offset - Pm4::PA_SC_VPORT_ZMIN_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	for (auto cmd_offset = Pm4::PA_SC_VPORT_ZMAX_0; cmd_offset <= Pm4::PA_SC_VPORT_ZMAX_15; cmd_offset += 2)
	{
		g_hw_ctx_indirect_func[cmd_offset] = [](KYTY_HW_CTX_INDIRECT_ARGS)
		{ cp->GetCtx()->SetViewportZMax((cmd_offset - Pm4::PA_SC_VPORT_ZMAX_0) / 2, *reinterpret_cast<const float*>(&value)); };
	}

	g_hw_ctx_indirect_func[Pm4::SPI_VS_OUT_CONFIG] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetVsOutConfig(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_POS_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderPosFormat(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_IDX_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderIdxFormat(value); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VS_OUT_CNTL]     = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetClVsOutCntl(value); };
	g_hw_ctx_indirect_func[Pm4::GE_NGG_SUBGRP_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetNggSubgrpCntl(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_INSTANCE_CNT]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsInstanceCnt(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_ONCHIP_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOnchipCntl(value); };

	g_hw_ctx_indirect_func[Pm4::GE_MAX_OUTPUT_PER_SUBGROUP] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetMaxOutputPerSubgroup(value); };

	g_hw_ctx_indirect_func[Pm4::VGT_ESGS_RING_ITEMSIZE] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetEsgsRingItemsize(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_MAX_VERT_OUT]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsMaxVertOut(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_SHADER_STAGES_EN]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderStages(value); };
	g_hw_ctx_indirect_func[Pm4::VGT_GS_OUT_PRIM_TYPE]   = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetGsOutPrimType(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_Z_FORMAT]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderZFormat(value); };

	g_hw_ctx_indirect_func[Pm4::SPI_SHADER_COL_FORMAT] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		for (uint32_t i = 0; i < 8; i++)
		{
			cp->GetCtx()->SetTargetOutputMode(i, (value >> (i * 4)) & 0xFu);
		}
	};

	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ENA]  = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputEna(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_INPUT_ADDR] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInputAddr(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_PS_IN_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetPsInControl(value); };
	g_hw_ctx_indirect_func[Pm4::SPI_BARYC_CNTL]    = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetBarycCntl(value); };

	g_hw_ctx_indirect_func[Pm4::DB_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthShaderControl db_shader_control {};
		db_shader_control.other_bits                  = value & 0xFFFF9B8Eu;
		db_shader_control.conservative_z_export_value = KYTY_PM4_GET(value, DB_SHADER_CONTROL, CONSERVATIVE_Z_EXPORT);
		db_shader_control.shader_z_behavior           = KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_ORDER);
		db_shader_control.shader_kill_enable          = KYTY_PM4_GET(value, DB_SHADER_CONTROL, KILL_ENABLE) != 0;
		db_shader_control.shader_z_export_enable      = KYTY_PM4_GET(value, DB_SHADER_CONTROL, Z_EXPORT_ENABLE) != 0;
		db_shader_control.shader_execute_on_noop      = KYTY_PM4_GET(value, DB_SHADER_CONTROL, EXEC_ON_NOOP) != 0;
		cp->GetCtx()->SetDepthShaderControl(db_shader_control);
	};

	g_hw_ctx_indirect_func[Pm4::CB_SHADER_MASK]       = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetShaderMask(value); };
	g_hw_ctx_indirect_func[Pm4::PA_SC_SHADER_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetScShaderControl(value); };
	g_hw_ctx_indirect_func[Pm4::CB_TARGET_MASK]       = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetRenderTargetMask(value); };

	g_hw_ctx_indirect_func[Pm4::DB_Z_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetDepthZInfo(State::DecodeDepthZInfo(value)); };

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_INFO] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetDepthStencilInfo(State::DecodeDepthStencilInfo(value)); };

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_READ_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_read_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilReadBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_Z_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().z_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthZWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_WRITE_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().stencil_write_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthStencilWriteBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_HTILE_DATA_BASE_HI] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		auto base = cp->GetCtx()->GetDepthRenderTarget().htile_data_base_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetCtx()->SetDepthHTileDataBase(base);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_VIEW] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthView r;
		r.slice_start = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START) + (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_START_HI) << 11u);
		r.slice_max   = KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX) + (KYTY_PM4_GET(value, DB_DEPTH_VIEW, SLICE_MAX_HI) << 11u);
		r.depth_write_disable   = KYTY_PM4_GET(value, DB_DEPTH_VIEW, Z_READ_ONLY) != 0;
		r.stencil_write_disable = KYTY_PM4_GET(value, DB_DEPTH_VIEW, STENCIL_READ_ONLY) != 0;
		r.current_mip_level     = KYTY_PM4_GET(value, DB_DEPTH_VIEW, MIPID);
		cp->GetCtx()->SetDepthDepthView(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_SIZE_XY] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		HW::DepthDepthSizeXY r;
		r.x_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, X_MAX);
		r.y_max = KYTY_PM4_GET(value, DB_DEPTH_SIZE_XY, Y_MAX);
		r.valid = true;
		cp->GetCtx()->SetDepthDepthSizeXY(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetDepthClearValue(*reinterpret_cast<const float*>(&value)); };
	g_hw_ctx_indirect_func[Pm4::DB_STENCIL_CLEAR] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{ cp->GetCtx()->SetStencilClearValue(KYTY_PM4_GET(value, DB_STENCIL_CLEAR, CLEAR)); };
	g_hw_ctx_indirect_func[Pm4::PA_CL_VTE_CNTL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { cp->GetCtx()->SetViewportTransformControl(value); };

	g_hw_ctx_indirect_func[Pm4::PA_SC_MODE_CNTL_0] = [](KYTY_HW_CTX_INDIRECT_ARGS)
	{
		trace_aa_register_write("indirect", "PA_SC_MODE_CNTL_0", value);
		HW::ScanModeControl r;
		r.msaa_enable          = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, MSAA_ENABLE) != 0;
		r.vport_scissor_enable = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, VPORT_SCISSOR_ENABLE) != 0;
		r.line_stipple_enable  = KYTY_PM4_GET(value, PA_SC_MODE_CNTL_0, LINE_STIPPLE_ENABLE) != 0;
		cp->GetCtx()->SetScanModeControl(r);
	};

	g_hw_ctx_indirect_func[Pm4::DB_DEPTH_CONTROL] = [](KYTY_HW_CTX_INDIRECT_ARGS) { State::SetDepthControl(*cp->GetCtx(), value); };
}

static void graphics_init_jmp_tables_sh_indirect()
{
	for (auto& func: g_hw_sh_indirect_func)
	{
		func = nullptr;
	}

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_ES] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_ES] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetVs().es_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetEsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_GS] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetGsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_GS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource1 r1;
		r1.vgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, VGPRS);
		r1.sgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, SGPRS);
		r1.priority                 = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, PRIORITY);
		r1.float_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE);
		r1.dx10_clamp               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) != 0;
		r1.debug_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE) != 0;
		r1.ieee_mode                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, IEEE_MODE) != 0;
		r1.cu_group_enable          = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, CU_GROUP_ENABLE) != 0;
		r1.require_forward_progress = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FWD_PROGRESS) != 0;
		r1.lds_configuration        = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE) != 0;
		r1.gs_vgpr_component_count  = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT);
		r1.fp16_overflow            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_GS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetGsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_GS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::GsShaderResource2 r2;
		r2.scratch_en = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SCRATCH_EN) != 0;
		r2.user_sgpr =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR) + (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB) << 5u);
		r2.es_vgpr_component_count = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT);
		r2.offchip_lds             = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN) != 0;
		r2.lds_size                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE);
		r2.shared_vgprs            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_GS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetGsShaderResource2(r2);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_LO_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFFFF00000000FFull;
		base |= static_cast<uint64_t>(value) << 8u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_HI_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto base = cp->GetShCtx()->GetPs().ps_regs.data_addr;
		base &= 0xFFFF00FFFFFFFFFFull;
		base |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
		cp->GetShCtx()->SetPsShaderBase(base);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_CHKSUM_PS] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetPsShaderChksum(value); };

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource1 r1;
		r1.vgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, VGPRS);
		r1.sgprs                    = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, SGPRS);
		r1.priority                 = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, PRIORITY);
		r1.float_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FLOAT_MODE);
		r1.dx10_clamp               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DX10_CLAMP) != 0;
		r1.debug_mode               = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, DEBUG_MODE) != 0;
		r1.ieee_mode                = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, IEEE_MODE) != 0;
		r1.cu_group_disable         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, CU_GROUP_DISABLE) != 0;
		r1.require_forward_progress = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FWD_PROGRESS) != 0;
		r1.fp16_overflow            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC1_PS, FP16_OVFL) != 0;
		cp->GetShCtx()->SetPsShaderResource1(r1);
	};

	g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC2_PS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		HW::PsShaderResource2 r2;
		r2.scratch_en = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SCRATCH_EN);
		r2.user_sgpr =
		    KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR) + (KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, USER_SGPR_MSB) << 5u);
		r2.wave_cnt_en            = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, WAVE_CNT_EN);
		r2.extra_lds_size         = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, EXTRA_LDS_SIZE);
		r2.raster_ordered_shading = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, LOAD_INTRAWAVE_COLLISION);
		r2.shared_vgprs           = KYTY_PM4_GET(value, SPI_SHADER_PGM_RSRC2_PS, SHARED_VGPR_CNT);
		cp->GetShCtx()->SetPsShaderResource2(r2);
	};

	// Gen5 SH-indirect emits COMPUTE_* as lone offset/value pairs. Mirror the
	// direct SET_SH_REG decoders.
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_LO] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r = cp->GetShCtx()->CsRegs();
		r.data_addr &= 0xFFFFFF00000000FFull;
		r.data_addr |= static_cast<uint64_t>(value) << 8u;
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_HI] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r = cp->GetShCtx()->CsRegs();
		r.data_addr &= 0xFFFF00FFFFFFFFFFull;
		r.data_addr |= (static_cast<uint64_t>(value) & 0xffu) << 40u;
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC1] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ decode_compute_pgm_rsrc1(cp->GetShCtx()->CsRegs(), value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC2] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ decode_compute_pgm_rsrc2(cp->GetShCtx()->CsRegs(), value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_RESOURCE_LIMITS] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{ EXIT_NOT_IMPLEMENTED(!GraphicsDecodeComputeResourceLimits(&cp->GetShCtx()->CsRegs(), cmd_offset, &value, 1)); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_PGM_RSRC3]     = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->CsRegs().rsrc3 = value; };
	g_hw_sh_indirect_func[Pm4::COMPUTE_SHADER_CHKSUM] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r  = cp->GetShCtx()->CsRegs();
		r.chksum = (r.chksum & 0xffffffff00000000ull) | static_cast<uint64_t>(value);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_SHADER_CHKSUM_HI] = [](KYTY_HW_SH_INDIRECT_ARGS)
	{
		auto& r  = cp->GetShCtx()->CsRegs();
		r.chksum = (r.chksum & 0x00000000ffffffffull) | (static_cast<uint64_t>(value) << 32u);
	};
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_X] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadX(value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Y] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadY(value); };
	g_hw_sh_indirect_func[Pm4::COMPUTE_NUM_THREAD_Z] = [](KYTY_HW_SH_INDIRECT_ARGS) { cp->GetShCtx()->SetCsNumThreadZ(value); };
	for (uint32_t slot = 0; slot < 16; slot++)
	{
		g_hw_sh_indirect_func[Pm4::COMPUTE_USER_DATA_0 + slot] = [](KYTY_HW_SH_INDIRECT_ARGS)
		{
			const uint32_t id = cmd_offset - Pm4::COMPUTE_USER_DATA_0;
			cp->GetShCtx()->SetCsUserSgpr(id, value, cp->GetUserDataMarker());
			cp->SetUserDataMarker(HW::UserSgprType::Unknown);
		};
	}
}

static void graphics_init_jmp_tables_uc_indirect()
{
	for (auto& func: g_hw_uc_indirect_func)
	{
		func = nullptr;
	}

	g_hw_uc_indirect_func[Pm4::GE_CNTL] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		HW::GeControl r;
		r.primitive_group_size = KYTY_PM4_GET(value, GE_CNTL, PRIM_GRP_SIZE);
		r.vertex_group_size    = KYTY_PM4_GET(value, GE_CNTL, VERT_GRP_SIZE);
		cp->GetUcfg()->SetGeControl(r);
	};

	g_hw_uc_indirect_func[Pm4::GE_USER_VGPR_EN] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		HW::GeUserVgprEn r;
		r.vgpr1 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR1) != 0;
		r.vgpr2 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR2) != 0;
		r.vgpr3 = KYTY_PM4_GET(value, GE_USER_VGPR_EN, EN_USER_VGPR3) != 0;
		cp->GetUcfg()->SetGeUserVgprEn(r);
	};

	g_hw_uc_indirect_func[Pm4::VGT_PRIMITIVE_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		uint32_t prim_type = KYTY_PM4_GET(value, VGT_PRIMITIVE_TYPE, PRIM_TYPE);
		cp->GetUcfg()->SetPrimitiveType(prim_type);
	};

	// Index type via UCONFIG (same 2-bit size field as IT_INDEX_TYPE).
	g_hw_uc_indirect_func[Pm4::VGT_INDEX_TYPE] = [](KYTY_HW_UC_INDIRECT_ARGS) { cp->SetIndexType(value & 0x3u); };

	// Remaining UCONFIG regs accepted without host state until HardwareContext
	// gains matching setters.
	const auto ignore_uc = [](KYTY_HW_UC_INDIRECT_ARGS)
	{
		(void)cp;
		(void)cmd_offset;
		(void)value;
	};
	g_hw_uc_indirect_func[Pm4::GE_INDX_OFFSET]            = [](KYTY_HW_UC_INDIRECT_ARGS) { cp->GetUcfg()->SetIndexOffset(value); };
	g_hw_uc_indirect_func[Pm4::GE_MULTI_PRIM_IB_RESET_EN] = ignore_uc;
	g_hw_uc_indirect_func[Pm4::VGT_OBJECT_ID]             = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TEXTURE_GRADIENT_FACTORS]  = ignore_uc;
	g_hw_uc_indirect_func[Pm4::IA_MULTI_VGT_PARAM]        = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR]        = ignore_uc;
	g_hw_uc_indirect_func[Pm4::TA_CS_BC_BASE_ADDR_HI]     = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_CNTL]               = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_COUNTER]            = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GDS_OA_ADDRESS]            = ignore_uc;
	g_hw_uc_indirect_func[Pm4::GE_STEREO_CNTL]            = ignore_uc;
}

void graphics_init_jmp_tables()
{
	for (auto& func: g_hw_ctx_func)
	{
		func = nullptr;
	}

	g_hw_ctx_func[Pm4::DB_RENDER_CONTROL]            = hw_ctx_set_render_control;
	g_hw_ctx_func[Pm4::DB_STENCIL_CLEAR]             = hw_ctx_set_stencil_clear;
	g_hw_ctx_func[Pm4::DB_DEPTH_CLEAR]               = hw_ctx_set_depth_clear;
	g_hw_ctx_func[Pm4::PA_SC_SCREEN_SCISSOR_TL]      = hw_ctx_set_screen_scissor;
	g_hw_ctx_func[Pm4::DB_Z_INFO]                    = hw_ctx_set_depth_render_target;
	g_hw_ctx_func[Pm4::DB_STENCIL_INFO]              = hw_ctx_set_stencil_info;
	g_hw_ctx_func[Pm4::PA_SU_HARDWARE_SCREEN_OFFSET] = hw_ctx_set_hardware_screen_offset;
	g_hw_ctx_func[Pm4::PA_SC_WINDOW_OFFSET]          = hw_ctx_set_window_offset;
	g_hw_ctx_func[Pm4::CB_TARGET_MASK]               = hw_ctx_set_render_target_mask;
	g_hw_ctx_func[Pm4::PA_SC_GENERIC_SCISSOR_TL]     = hw_ctx_set_generic_scissor;
	g_hw_ctx_func[Pm4::CB_BLEND_RED]                 = hw_ctx_set_blend_color;
	g_hw_ctx_func[Pm4::DB_STENCIL_CONTROL]           = hw_ctx_set_stencil_control;
	g_hw_ctx_func[Pm4::DB_STENCILREFMASK]            = hw_ctx_set_stencil_mask;
	g_hw_ctx_func[Pm4::SPI_PS_INPUT_CNTL_0]          = hw_ctx_set_ps_input;
	g_hw_ctx_func[Pm4::DB_DEPTH_CONTROL]             = hw_ctx_set_depth_control;
	g_hw_ctx_func[Pm4::DB_EQAA]                      = hw_ctx_set_eqaa_control;
	g_hw_ctx_func[Pm4::CB_COLOR_CONTROL]             = hw_ctx_set_color_control;
	g_hw_ctx_func[Pm4::PA_CL_CLIP_CNTL]              = hw_ctx_set_clip_control;
	g_hw_ctx_func[Pm4::PA_SU_SC_MODE_CNTL]           = hw_ctx_set_mode_control;
	for (uint32_t reg = Pm4::PA_SU_POLY_OFFSET_DB_FMT_CNTL; reg <= Pm4::PA_SU_POLY_OFFSET_BACK_OFFSET; reg++)
	{
		g_hw_ctx_func[reg] = hw_ctx_set_polygon_offset;
	}
	g_hw_ctx_func[Pm4::PA_CL_VTE_CNTL]                    = hw_ctx_set_viewport_transform_control;
	g_hw_ctx_func[Pm4::PA_SU_LINE_CNTL]                   = hw_ctx_set_line_control;
	g_hw_ctx_func[Pm4::PA_SC_MODE_CNTL_0]                 = hw_ctx_set_scan_mode_control;
	g_hw_ctx_func[Pm4::PA_SC_AA_CONFIG]                   = hw_ctx_set_aa_config;
	g_hw_ctx_func[Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0] = hw_ctx_set_aa_sample_control;
	// Sample masks are already intentionally ignored by the indirect register
	// path. Accept the direct form as well so both PM4 encodings agree.
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y0_X1Y0] = hw_ctx_ignore;
	g_hw_ctx_func[Pm4::PA_SC_AA_MASK_X0Y1_X1Y1] = hw_ctx_ignore;
	// Keep direct and indirect handling aligned for metadata-only registers.
	for (const uint32_t reg: {Pm4::CB_DCC_CONTROL,
	                          Pm4::DB_COUNT_CONTROL,
	                          Pm4::DB_RENDER_OVERRIDE,
	                          Pm4::DB_RENDER_OVERRIDE2,
	                          Pm4::DB_DFSM_CONTROL,
	                          Pm4::DB_RMI_L2_CACHE_CONTROL,
	                          Pm4::CB_RMI_GL2_CACHE_CONTROL,
	                          Pm4::TA_BC_BASE_ADDR,
	                          Pm4::TA_BC_BASE_ADDR_HI,
	                          Pm4::PA_SU_POINT_SIZE,
	                          Pm4::PA_SU_POINT_MINMAX,
	                          Pm4::SPI_TMPRING_SIZE,
	                          Pm4::VGT_MULTI_PRIM_IB_RESET_INDX,
	                          Pm4::VGT_DRAW_PAYLOAD_CNTL,
	                          Pm4::VGT_PRIMITIVEID_RESET,
	                          Pm4::PA_CL_OBJPRIM_ID_CNTL,
	                          Pm4::PA_SC_FOV_WINDOW_LR,
	                          Pm4::PA_SC_FOV_WINDOW_TB,
	                          Pm4::PA_SC_MODE_CNTL_1,
	                          Pm4::PA_SU_VTX_CNTL,
	                          Pm4::PS_SHADER_SAMPLE_EXCLUSION_MASK,
	                          Pm4::PA_SC_BINNER_CNTL_0,
	                          Pm4::PA_SC_BINNER_CNTL_1,
	                          Pm4::PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,
	                          Pm4::PA_SC_NGG_MODE_CNTL,
	                          Pm4::DB_ALPHA_TO_MASK,
	                          Pm4::PA_SC_WINDOW_SCISSOR_TL,
	                          Pm4::PA_SC_WINDOW_SCISSOR_BR,
	                          Pm4::VGT_HOS_MAX_TESS_LEVEL,
	                          Pm4::VGT_HOS_MIN_TESS_LEVEL,
	                          Pm4::VGT_PRIMITIVEID_EN,
	                          Pm4::VGT_REUSE_OFF,
	                          Pm4::VGT_TESS_DISTRIBUTION,
	                          Pm4::VGT_LS_HS_CONFIG,
	                          Pm4::VGT_TF_PARAM,
	                          Pm4::DB_DEPTH_BOUNDS_MIN,
	                          Pm4::DB_DEPTH_BOUNDS_MAX,
	                          Pm4::DB_HTILE_SURFACE,
	                          Pm4::DB_DEPTH_INFO,
	                          Pm4::DB_DEPTH_SIZE,
	                          Pm4::DB_DEPTH_SLICE})
	{
		g_hw_ctx_func[reg] = hw_ctx_ignore;
	}
	g_hw_ctx_func[Pm4::VGT_SHADER_STAGES_EN]   = hw_ctx_set_shader_stages;
	g_hw_ctx_func[Pm4::PA_CL_GB_VERT_CLIP_ADJ] = hw_ctx_set_guard_bands;

	for (uint32_t slot = 0; slot < 8; slot++)
	{
		g_hw_ctx_func[Pm4::CB_COLOR0_BASE + slot * 15] = hw_ctx_set_render_target;
		g_hw_ctx_func[Pm4::CB_COLOR0_INFO + slot * 15] = hw_ctx_set_color_info;

		g_hw_ctx_func[Pm4::CB_BLEND0_CONTROL + slot * 1] = hw_ctx_set_blend_control;
	}

	for (uint32_t viewport = 0; viewport < 16; viewport++)
	{
		g_hw_ctx_func[Pm4::PA_SC_VPORT_ZMIN_0 + viewport * 2] = hw_ctx_set_viewport_z;
		for (uint32_t component = 0; component < 6; component++)
		{
			g_hw_ctx_func[Pm4::PA_CL_VPORT_XSCALE + viewport * 6 + component] = hw_ctx_set_viewport_scale_offset;
		}
	}

	for (auto& func: g_hw_sh_func)
	{
		func = nullptr;
	}

	for (uint32_t slot = 0; slot < 16; slot++)
	{
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_VS_0 + slot * 1] = hw_sh_set_vs_user_sgpr;
		g_hw_sh_func[Pm4::COMPUTE_USER_DATA_0 + slot * 1]       = hw_sh_set_cs_user_sgpr;
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_GS_0 + slot * 1] = hw_sh_set_gs_user_sgpr;
	}
	// PS user data is 32 dwords on Gen5 (SPI_SHADER_USER_DATA_PS_0..31).
	for (uint32_t slot = 0; slot < 32; slot++)
	{
		g_hw_sh_func[Pm4::SPI_SHADER_USER_DATA_PS_0 + slot * 1] = hw_sh_set_ps_user_sgpr;
	}

	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_X]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Y]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_NUM_THREAD_Z]     = hw_sh_set_cs_num_thread;
	g_hw_sh_func[Pm4::COMPUTE_PGM_LO]           = hw_sh_set_cs_pgm;
	g_hw_sh_func[Pm4::COMPUTE_PGM_HI]           = hw_sh_set_cs_pgm;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC1]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC2]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_RESOURCE_LIMITS]  = hw_sh_set_cs_resource_limits;
	g_hw_sh_func[Pm4::COMPUTE_PGM_RSRC3]        = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_SHADER_CHKSUM]    = hw_sh_set_cs_rsrc;
	g_hw_sh_func[Pm4::COMPUTE_SHADER_CHKSUM_HI] = hw_sh_set_cs_rsrc;

	for (auto& func: g_hw_uc_func)
	{
		func = nullptr;
	}

	g_hw_uc_func[Pm4::VGT_PRIMITIVE_TYPE] = hw_uc_set_primitive_type;

	for (auto& func: g_hw_sh_custom_func)
	{
		func = nullptr;
	}

	g_hw_sh_custom_func[Pm4::R_VS]          = hw_sh_set_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS]          = hw_sh_set_ps_shader;
	g_hw_sh_custom_func[Pm4::R_CS]          = hw_sh_set_cs_shader;
	g_hw_sh_custom_func[Pm4::R_VS_EMBEDDED] = hw_sh_set_vs_embedded;
	g_hw_sh_custom_func[Pm4::R_PS_EMBEDDED] = hw_sh_set_ps_embedded;
	g_hw_sh_custom_func[Pm4::R_VS_UPDATE]   = hw_sh_update_vs_shader;
	g_hw_sh_custom_func[Pm4::R_PS_UPDATE]   = hw_sh_update_ps_shader;

	for (auto& func: g_cp_op_func)
	{
		func = nullptr;
	}

	g_cp_op_func[Pm4::IT_NOP]                     = cp_op_nop;
	g_cp_op_func[Pm4::IT_CLEAR_STATE]             = cp_op_clear_state;
	g_cp_op_func[Pm4::IT_SET_BASE]                = cp_op_set_base;
	g_cp_op_func[Pm4::IT_DISPATCH_INDIRECT]       = cp_op_dispatch_indirect;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_INDIRECT]     = cp_op_draw_index_indirect;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_2]            = cp_op_draw_index;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_OFFSET_2]     = cp_op_draw_index_offset;
	g_cp_op_func[Pm4::IT_INDEX_BASE]              = cp_op_index_base;
	g_cp_op_func[Pm4::IT_INDEX_BUFFER_SIZE]       = cp_op_index_buffer_size;
	g_cp_op_func[Pm4::IT_INDEX_TYPE]              = cp_op_index_type;
	g_cp_op_func[Pm4::IT_NUM_INSTANCES]           = cp_op_num_instances;
	g_cp_op_func[Pm4::IT_DRAW_INDEX_AUTO]         = cp_op_draw_index_auto;
	g_cp_op_func[Pm4::IT_WAIT_REG_MEM]            = cp_op_wait_reg_mem;
	g_cp_op_func[Pm4::IT_WRITE_DATA]              = cp_op_write_data;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER]         = cp_op_indirect_buffer;
	g_cp_op_func[Pm4::IT_INDIRECT_BUFFER_END]     = cp_op_indirect_buffer_end;
	g_cp_op_func[Pm4::IT_EVENT_WRITE]             = cp_op_event_write;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOP]         = cp_op_event_write_eop;
	g_cp_op_func[Pm4::IT_EVENT_WRITE_EOS]         = cp_op_event_write_eos;
	g_cp_op_func[Pm4::IT_RELEASE_MEM]             = cp_op_release_mem;
	g_cp_op_func[Pm4::IT_DMA_DATA]                = cp_op_dma_data;
	g_cp_op_func[Pm4::IT_ONE_REG_WRITE]           = cp_op_one_reg_write;
	g_cp_op_func[Pm4::IT_ACQUIRE_MEM]             = cp_op_acquire_mem;
	g_cp_op_func[Pm4::IT_SET_CONTEXT_REG]         = cp_op_set_context_reg;
	g_cp_op_func[Pm4::IT_SET_SH_REG]              = cp_op_set_shader_reg;
	g_cp_op_func[Pm4::IT_DISPATCH_DIRECT]         = cp_op_dispatch_direct;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG]         = cp_op_set_uconfig_reg;
	g_cp_op_func[Pm4::IT_SET_UCONFIG_REG_INDEX]   = cp_op_set_uconfig_reg_index;
	g_cp_op_func[Pm4::IT_WRITE_CONST_RAM]         = cp_op_write_const_ram;
	g_cp_op_func[Pm4::IT_DUMP_CONST_RAM]          = cp_op_dump_const_ram;
	g_cp_op_func[Pm4::IT_INCREMENT_CE_COUNTER]    = cp_op_increment_ce_counter;
	g_cp_op_func[Pm4::IT_INCREMENT_DE_COUNTER]    = cp_op_increment_de_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_CE_COUNTER]      = cp_op_wait_on_ce_counter;
	g_cp_op_func[Pm4::IT_WAIT_ON_DE_COUNTER_DIFF] = cp_op_wait_on_de_counter_diff;
	g_cp_op_func[Pm4::IT_GET_LOD_STATS]           = cp_op_get_lod_stats;

	for (auto& func: g_cp_op_custom_func)
	{
		func = nullptr;
	}

	g_cp_op_custom_func[Pm4::R_DRAW_INDEX]       = cp_op_draw_index;
	g_cp_op_custom_func[Pm4::R_DRAW_INDEX_AUTO]  = cp_op_draw_index_auto;
	g_cp_op_custom_func[Pm4::R_DISPATCH_DIRECT]  = cp_op_dispatch_direct;
	g_cp_op_custom_func[Pm4::R_DISPATCH_RESET]   = cp_op_dispatch_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_32]      = cp_op_wait_reg_mem_32;
	g_cp_op_custom_func[Pm4::R_DRAW_RESET]       = cp_op_draw_reset;
	g_cp_op_custom_func[Pm4::R_WAIT_FLIP_DONE]   = cp_op_wait_flip_done;
	g_cp_op_custom_func[Pm4::R_PUSH_MARKER]      = cp_op_push_marker;
	g_cp_op_custom_func[Pm4::R_POP_MARKER]       = cp_op_pop_marker;
	g_cp_op_custom_func[Pm4::R_CX_REGS_INDIRECT] = cp_op_indirect_cx_regs;
	g_cp_op_custom_func[Pm4::R_SH_REGS_INDIRECT] = cp_op_indirect_sh_regs;
	g_cp_op_custom_func[Pm4::R_UC_REGS_INDIRECT] = cp_op_indirect_uc_regs;
	g_cp_op_custom_func[Pm4::R_ACQUIRE_MEM]      = cp_op_acquire_mem;
	g_cp_op_custom_func[Pm4::R_WRITE_DATA]       = cp_op_write_data;
	g_cp_op_custom_func[Pm4::R_WAIT_MEM_64]      = cp_op_wait_reg_mem_64;
	g_cp_op_custom_func[Pm4::R_FLIP]             = cp_op_flip;
	g_cp_op_custom_func[Pm4::R_RELEASE_MEM]      = cp_op_release_mem;
	g_cp_op_custom_func[Pm4::R_DMA_DATA]         = cp_op_custom_dma_data;

	graphics_init_jmp_tables_cx_indirect();
	graphics_init_jmp_tables_sh_indirect();
	graphics_init_jmp_tables_uc_indirect();
}
} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
