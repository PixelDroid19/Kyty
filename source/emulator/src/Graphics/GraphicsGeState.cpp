#include "Emulator/Graphics/GraphicsGeState.h"

#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"

#include "GraphicsComputeRegisters.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

GraphicsGeStageState GraphicsDecodeGeStages(GraphicsGeRawRegister raw, GraphicsGeGeneration generation)
{
	GraphicsGeStageState state;
	state.raw        = raw;
	state.generation = generation;
	if (!raw.known)
	{
		return state;
	}
	// GFX10 register layout. GFX10.3 additionally defines PASSTHRU_NO_MSG;
	// other generations must not inherit this interpretation implicitly.
	if (generation != GraphicsGeGeneration::Gfx10 && generation != GraphicsGeGeneration::Gfx103)
	{
		state.raw_unknown_bits = raw.value;
		return state;
	}
	const uint32_t value          = raw.value;
	const uint32_t known_mask     = generation == GraphicsGeGeneration::Gfx103 ? 0x07ffffffu : 0x03ffffffu;
	state.raw_unknown_bits        = value & ~known_mask;
	state.ls_en                   = static_cast<uint8_t>(value & 3u);
	state.hs_en                   = (value & (1u << 2u)) != 0;
	state.es_en                   = static_cast<uint8_t>((value >> 3u) & 3u);
	state.gs_en                   = (value & (1u << 5u)) != 0;
	state.vs_en                   = static_cast<uint8_t>((value >> 6u) & 3u);
	state.dynamic_hs              = (value & (1u << 8u)) != 0;
	state.dispatch_draw_en        = (value & (1u << 9u)) != 0;
	state.dis_dealloc_accum_0     = (value & (1u << 10u)) != 0;
	state.dis_dealloc_accum_1     = (value & (1u << 11u)) != 0;
	state.vs_wave_id_en           = (value & (1u << 12u)) != 0;
	state.primgen_en              = (value & (1u << 13u)) != 0;
	state.ordered_id_mode         = (value & (1u << 14u)) != 0;
	state.max_primgrp_in_wave     = static_cast<uint8_t>((value >> 15u) & 15u);
	state.gs_fast_launch          = static_cast<uint8_t>((value >> 19u) & 3u);
	state.hs_w32_en               = (value & (1u << 21u)) != 0;
	state.gs_w32_en               = (value & (1u << 22u)) != 0;
	state.vs_w32_en               = (value & (1u << 23u)) != 0;
	state.ngg_wave_id_en          = (value & (1u << 24u)) != 0;
	state.primgen_passthru_en     = (value & (1u << 25u)) != 0;
	state.primgen_passthru_no_msg = generation == GraphicsGeGeneration::Gfx103 && (value & (1u << 26u)) != 0;
	state.gs_wave_lanes           = state.gs_w32_en ? 32u : 64u;

	state.kind = GraphicsGeStageKind::Other;
	if (state.ls_en == 0 && !state.hs_en && state.vs_en == 0)
	{
		if (state.primgen_en && !state.primgen_passthru_en && state.es_en == 2 && state.gs_en)
		{
			state.kind = GraphicsGeStageKind::MergedEsGs;
		} else if (state.es_en == 0 && !state.gs_en)
		{
			if (state.primgen_en && state.primgen_passthru_en)
			{
				state.kind = GraphicsGeStageKind::NggPassthrough;
			} else if (!state.primgen_en && !state.primgen_passthru_en)
			{
				state.kind = GraphicsGeStageKind::LegacyVs;
			}
		}
	}
	return state;
}

GraphicsGeNggSubgroupControl GraphicsDecodeGeNggSubgroupControl(uint32_t raw)
{
	GraphicsGeNggSubgroupControl state;
	state.raw                            = raw;
	state.raw_unknown_bits               = raw & 0xfffc0000u;
	state.primitive_amplification_factor = static_cast<uint16_t>(raw & 0x1ffu);
	state.threads_per_subgroup           = static_cast<uint16_t>((raw >> 9u) & 0x1ffu);
	return state;
}

HW::GsShaderResource1 GraphicsDecodeGsShaderResource1(uint32_t raw)
{
	HW::GsShaderResource1 r;
	r.vgprs                    = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, VGPRS);
	r.sgprs                    = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, SGPRS);
	r.priority                 = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, PRIORITY);
	r.float_mode               = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, FLOAT_MODE);
	r.priv                     = (raw & (1u << 20u)) != 0;
	r.dx10_clamp               = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, DX10_CLAMP) != 0;
	r.debug_mode               = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, DEBUG_MODE) != 0;
	r.ieee_mode                = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, IEEE_MODE) != 0;
	r.cu_group_enable          = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, CU_GROUP_ENABLE) != 0;
	r.mem_ordered              = (raw & (1u << 25u)) != 0;
	r.require_forward_progress = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, FWD_PROGRESS) != 0;
	r.lds_configuration        = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, WGP_MODE) != 0;
	r.cdbg_user                = (raw & (1u << 28u)) != 0;
	r.gs_vgpr_component_count  = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC1_GS, GS_VGPR_COMP_CNT);
	const auto overflow   = GraphicsDecodeFp16Overflow(raw, GraphicsFp16OverflowStage::Geometry);
	r.fp16_overflow       = overflow.enabled;
	r.fp16_overflow_known = overflow.known;
	return r;
}

HW::GsShaderResource2 GraphicsDecodeGsShaderResource2(uint32_t raw)
{
	HW::GsShaderResource2 r;
	r.scratch_en = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, SCRATCH_EN) != 0;
	r.user_sgpr = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR) | (KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, USER_SGPR_MSB) << 5u);
	r.trap_present            = (raw & (1u << 6u)) != 0;
	r.exception_en            = static_cast<uint16_t>((raw >> 7u) & 0x1ffu);
	r.es_vgpr_component_count = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, ES_VGPR_COMP_CNT);
	r.offchip_lds             = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, OC_LDS_EN) != 0;
	r.lds_size                = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, LDS_SIZE);
	r.shared_vgprs            = KYTY_PM4_GET(raw, SPI_SHADER_PGM_RSRC2_GS, SHARED_VGPR_CNT);
	return r;
}

HW::GeControl GraphicsDecodeGeControl(uint32_t raw)
{
	HW::GeControl r;
	r.primitive_group_size = KYTY_PM4_GET(raw, GE_CNTL, PRIM_GRP_SIZE);
	r.vertex_group_size    = KYTY_PM4_GET(raw, GE_CNTL, VERT_GRP_SIZE);
	r.break_wave_at_eoi    = (raw & (1u << 18u)) != 0;
	r.packet_to_one_pa     = (raw & (1u << 19u)) != 0;
	r.raw_unknown_bits     = raw & 0xfff00000u;
	return r;
}

HW::GeUserVgprEn GraphicsDecodeGeUserVgprEn(uint32_t raw)
{
	HW::GeUserVgprEn r;
	r.vgpr1            = KYTY_PM4_GET(raw, GE_USER_VGPR_EN, EN_USER_VGPR1) != 0;
	r.vgpr2            = KYTY_PM4_GET(raw, GE_USER_VGPR_EN, EN_USER_VGPR2) != 0;
	r.vgpr3            = KYTY_PM4_GET(raw, GE_USER_VGPR_EN, EN_USER_VGPR3) != 0;
	r.raw_unknown_bits = raw & 0xfffffff8u;
	return r;
}

bool GraphicsDecodeGeShaderRegister(HW::Shader& shader, uint32_t offset, uint32_t value)
{
	switch (offset)
	{
		case Pm4::SPI_SHADER_PGM_LO_GS:
			shader.SetGsBackBase((shader.GetGsBackBase() & 0xffffff00000000ffull) | (static_cast<uint64_t>(value) << 8u));
			break;
		case Pm4::SPI_SHADER_PGM_HI_GS:
			shader.SetGsBackBase((shader.GetGsBackBase() & 0xffff00ffffffffffull) | (static_cast<uint64_t>(value & 0xffu) << 40u));
			break;
		case Pm4::SPI_SHADER_PGM_RSRC1_GS: shader.SetGsShaderResource1Raw(value); break;
		case Pm4::SPI_SHADER_PGM_RSRC2_GS: shader.SetGsShaderResource2Raw(value); break;
		case Pm4::SPI_SHADER_PGM_RSRC3_GS: shader.SetGsRsrc3(value); break;
		default: return false;
	}
	return true;
}

bool GraphicsDecodeGeUserConfigRegister(HW::UserConfig& ucfg, uint32_t offset, uint32_t value)
{
	switch (offset)
	{
		case Pm4::GE_CNTL: ucfg.SetGeControlRaw(value); break;
		case Pm4::GE_USER_VGPR_EN: ucfg.SetGeUserVgprEnRaw(value); break;
		default: return false;
	}
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
