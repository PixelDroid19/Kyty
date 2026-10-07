#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GraphicsGeState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"

UT_BEGIN(EmulatorGeState);

using namespace Libs::Graphics;

static GraphicsGeStageState DecodeStages(uint32_t value, GraphicsGeGeneration generation = GraphicsGeGeneration::Gfx10)
{
	HW::Context ctx;
	ctx.SetShaderStages(value);
	return GraphicsDecodeGeStages(ctx.GetShaderStagesRaw(), generation);
}

static void ExpectRaw(const GraphicsGeRawRegister& raw, uint32_t value, bool written, bool known)
{
	EXPECT_EQ(raw.value, value);
	EXPECT_EQ(raw.written, written);
	EXPECT_EQ(raw.known, known);
}

TEST(EmulatorGeState, StageShapeAndGuestWidthAreIndependent)
{
	for (uint32_t width_bit: {0u, 0x00400000u})
	{
		const auto merged = DecodeStages(0x00002030u | width_bit);
		EXPECT_EQ(merged.kind, GraphicsGeStageKind::MergedEsGs);
		EXPECT_EQ(merged.ls_en, 0u);
		EXPECT_FALSE(merged.hs_en);
		EXPECT_EQ(merged.es_en, 2u);
		EXPECT_TRUE(merged.gs_en);
		EXPECT_EQ(merged.vs_en, 0u);
		EXPECT_TRUE(merged.primgen_en);
		EXPECT_FALSE(merged.primgen_passthru_en);
		EXPECT_EQ(merged.gs_wave_lanes, width_bit == 0 ? 64u : 32u);
		EXPECT_EQ(merged.raw_unknown_bits, 0u);
		ExpectRaw(merged.raw, 0x00002030u | width_bit, true, true);

		const auto passthrough = DecodeStages(0x02002000u | width_bit);
		EXPECT_EQ(passthrough.kind, GraphicsGeStageKind::NggPassthrough);
		EXPECT_EQ(passthrough.es_en, 0u);
		EXPECT_FALSE(passthrough.gs_en);
		EXPECT_TRUE(passthrough.primgen_en);
		EXPECT_TRUE(passthrough.primgen_passthru_en);
		EXPECT_EQ(passthrough.gs_wave_lanes, merged.gs_wave_lanes);
		EXPECT_EQ(DecodeStages(width_bit).kind, GraphicsGeStageKind::LegacyVs);
	}
}

TEST(EmulatorGeState, IndependentStageEnableChangesDoNotBecomePassthrough)
{
	struct Case
	{
		uint32_t            value;
		GraphicsGeStageKind kind;
	};
	const Case cases[] = {
	    {0x00002030u, GraphicsGeStageKind::MergedEsGs},
	    {0x00000030u, GraphicsGeStageKind::Other}, // PRIMGEN disabled
	    {0x00002010u, GraphicsGeStageKind::Other}, // GS disabled
	    {0x00002020u, GraphicsGeStageKind::Other}, // ES disabled
	    {0x00002028u, GraphicsGeStageKind::Other}, // ES=1
	    {0x00002038u, GraphicsGeStageKind::Other}, // ES=3
	    {0x00002031u, GraphicsGeStageKind::Other}, // LS enabled
	    {0x00002034u, GraphicsGeStageKind::Other}, // HS enabled
	    {0x00002070u, GraphicsGeStageKind::Other}, // VS=1
	    {0x02002030u, GraphicsGeStageKind::Other}, // Conflicting merged/passthrough shape
	    {0x02002000u, GraphicsGeStageKind::NggPassthrough},
	    {0x02000000u, GraphicsGeStageKind::Other}, // Passthrough without PRIMGEN
	    {0x00002000u, GraphicsGeStageKind::Other}, // PRIMGEN without either modeled shape
	    {0x02002020u, GraphicsGeStageKind::Other},
	    {0x02002010u, GraphicsGeStageKind::Other},
	    {0x00000000u, GraphicsGeStageKind::LegacyVs},
	};
	for (const auto& test: cases)
	{
		SCOPED_TRACE(test.value);
		EXPECT_EQ(DecodeStages(test.value).kind, test.kind);
		EXPECT_EQ(DecodeStages(test.value | 0x00400000u).kind, test.kind);
	}
}

TEST(EmulatorGeState, StageBooleanFieldsHaveIndependentBitPositions)
{
	struct Flag
	{
		uint32_t value;
		bool GraphicsGeStageState::* field;
	};
	const Flag flags[] = {
	    {0x00000004u, &GraphicsGeStageState::hs_en},
	    {0x00000020u, &GraphicsGeStageState::gs_en},
	    {0x00000100u, &GraphicsGeStageState::dynamic_hs},
	    {0x00000200u, &GraphicsGeStageState::dispatch_draw_en},
	    {0x00000400u, &GraphicsGeStageState::dis_dealloc_accum_0},
	    {0x00000800u, &GraphicsGeStageState::dis_dealloc_accum_1},
	    {0x00001000u, &GraphicsGeStageState::vs_wave_id_en},
	    {0x00002000u, &GraphicsGeStageState::primgen_en},
	    {0x00004000u, &GraphicsGeStageState::ordered_id_mode},
	    {0x00200000u, &GraphicsGeStageState::hs_w32_en},
	    {0x00400000u, &GraphicsGeStageState::gs_w32_en},
	    {0x00800000u, &GraphicsGeStageState::vs_w32_en},
	    {0x01000000u, &GraphicsGeStageState::ngg_wave_id_en},
	    {0x02000000u, &GraphicsGeStageState::primgen_passthru_en},
	    {0x04000000u, &GraphicsGeStageState::primgen_passthru_no_msg},
	};
	for (const auto& flag: flags)
	{
		SCOPED_TRACE(flag.value);
		const auto state = DecodeStages(flag.value, GraphicsGeGeneration::Gfx103);
		for (const auto& other: flags)
		{
			EXPECT_EQ(state.*(other.field), flag.value == other.value);
		}
		EXPECT_EQ(state.ls_en, 0u);
		EXPECT_EQ(state.es_en, 0u);
		EXPECT_EQ(state.vs_en, 0u);
		EXPECT_EQ(state.max_primgrp_in_wave, 0u);
		EXPECT_EQ(state.gs_fast_launch, 0u);
		EXPECT_EQ(state.gs_wave_lanes, flag.value == 0x00400000u ? 32u : 64u);
		EXPECT_EQ(state.raw_unknown_bits, 0u);
	}
}

TEST(EmulatorGeState, StageMultiBitFieldsAndUnknownBitsAreLossless)
{
	EXPECT_EQ(DecodeStages(0x00000003u).ls_en, 3u);
	EXPECT_EQ(DecodeStages(0x00000018u).es_en, 3u);
	EXPECT_EQ(DecodeStages(0x000000c0u).vs_en, 3u);
	EXPECT_EQ(DecodeStages(0x00078000u).max_primgrp_in_wave, 15u);
	EXPECT_EQ(DecodeStages(0x00180000u).gs_fast_launch, 3u);
	const auto all_gfx10 = DecodeStages(0xffffffffu);
	EXPECT_EQ(all_gfx10.raw_unknown_bits, 0xfc000000u);
	EXPECT_FALSE(all_gfx10.primgen_passthru_no_msg);
	const auto all_gfx103 = DecodeStages(0xffffffffu, GraphicsGeGeneration::Gfx103);
	EXPECT_EQ(all_gfx103.raw_unknown_bits, 0xf8000000u);
	EXPECT_TRUE(all_gfx103.primgen_passthru_no_msg);
	const auto unknown_generation = DecodeStages(0x00002030u, GraphicsGeGeneration::Unknown);
	EXPECT_EQ(unknown_generation.kind, GraphicsGeStageKind::Unknown);
	EXPECT_EQ(unknown_generation.raw_unknown_bits, 0x00002030u);
	EXPECT_EQ(unknown_generation.gs_wave_lanes, 0u);
	// Classification is only shape; the scheduling/unknown bits still need an
	// execution contract even when that shape is recognized.
	const auto extra = DecodeStages(0x801fa330u);
	EXPECT_EQ(extra.kind, GraphicsGeStageKind::MergedEsGs);
	EXPECT_EQ(extra.raw_unknown_bits, 0x80000000u);
	EXPECT_TRUE(extra.dynamic_hs);
	EXPECT_TRUE(extra.dispatch_draw_en);
	EXPECT_EQ(extra.max_primgrp_in_wave, 15u);
	EXPECT_EQ(extra.gs_fast_launch, 3u);
}

TEST(EmulatorGeState, UnwrittenStagesAreNotAnObservedZeroWord)
{
	HW::Context ctx;
	ExpectRaw(ctx.GetShaderStagesRaw(), 0u, false, false);
	EXPECT_EQ(GraphicsDecodeGeStages(ctx.GetShaderStagesRaw(), GraphicsGeGeneration::Gfx10).kind, GraphicsGeStageKind::Unknown);
	ctx.SetShaderStages(0u);
	ExpectRaw(ctx.GetShaderStagesRaw(), 0u, true, true);
	EXPECT_EQ(GraphicsDecodeGeStages(ctx.GetShaderStagesRaw(), GraphicsGeGeneration::Gfx10).kind, GraphicsGeStageKind::LegacyVs);
	const HW::Context copied = ctx;
	ctx.Reset();
	ExpectRaw(ctx.GetShaderStagesRaw(), 0u, false, false);
	ExpectRaw(copied.GetShaderStagesRaw(), 0u, true, true);
}

static void ExpectResourceFixture(const HW::Shader& shader)
{
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0xcab969cdu, true, true);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0x584e9563u, true, true);
	const auto& r1 = shader.GetVs().gs_regs.rsrc1;
	EXPECT_EQ(r1.vgprs, 13u);
	EXPECT_EQ(r1.sgprs, 7u);
	EXPECT_EQ(r1.priority, 2u);
	EXPECT_EQ(r1.float_mode, 0x96u);
	EXPECT_TRUE(r1.dx10_clamp);
	EXPECT_FALSE(r1.debug_mode);
	EXPECT_TRUE(r1.ieee_mode);
	EXPECT_FALSE(r1.cu_group_enable);
	EXPECT_FALSE(r1.require_forward_progress);
	EXPECT_TRUE(r1.lds_configuration);
	EXPECT_EQ(r1.gs_vgpr_component_count, 2u);
	EXPECT_TRUE(r1.fp16_overflow);
	EXPECT_TRUE(r1.priv);
	EXPECT_TRUE(r1.mem_ordered);
	EXPECT_FALSE(r1.cdbg_user);
	const auto& r2 = shader.GetVs().gs_regs.rsrc2;
	EXPECT_TRUE(r2.scratch_en);
	EXPECT_EQ(r2.user_sgpr, 49u);
	EXPECT_EQ(r2.es_vgpr_component_count, 2u);
	EXPECT_TRUE(r2.offchip_lds);
	EXPECT_EQ(r2.lds_size, 9u);
	EXPECT_EQ(r2.shared_vgprs, 5u);
	EXPECT_TRUE(r2.trap_present);
	EXPECT_EQ(r2.exception_en, 0x12au);
	EXPECT_EQ(r2.GetLdsSizeDwords(), 1152u);
	EXPECT_EQ(r2.GetLdsSizeBytes(), 4608u);
}

TEST(EmulatorGeState, DirectRangeAndIndirectWritesRetainTheSameGsState)
{
	HW::Shader direct;
	HW::Shader indirect;
	direct.SetEsShaderBase(0x111122223300ull);
	indirect.SetEsShaderBase(0x111122223300ull);
	// Synthetic words: RSRC3, program LO/HI, RSRC1, RSRC2. High program bits
	// outside [7:0] are not address bits. No mapping or shader execution occurs.
	const uint32_t words[] = {0xa5a55a5au, 0x12345678u, 0xa50000abu, 0xcab969cdu, 0x584e9563u};
	ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_RSRC3_GS, words, 5u));
	for (uint32_t i = 0; i < 5u; ++i)
	{
		ASSERT_TRUE(GraphicsDecodeGeShaderRegister(indirect, Pm4::SPI_SHADER_PGM_RSRC3_GS + i, words[i]));
	}
	for (const auto* shader: {&direct, &indirect})
	{
		ExpectResourceFixture(*shader);
		EXPECT_EQ(shader->GetGsBackBase(), 0xab1234567800ull);
		EXPECT_EQ(shader->GetVs().gs_back_addr, 0xab1234567800ull);
		EXPECT_EQ(shader->GetVs().gs_regs.data_addr, 0u);
		EXPECT_EQ(shader->GetVs().es_regs.data_addr, 0x111122223300ull);
		EXPECT_EQ(shader->GetVs().gs_regs.rsrc3, 0xa5a55a5au);
		ExpectRaw(shader->GetGsRsrc3Raw(), 0xa5a55a5au, true, true);
	}
}

TEST(EmulatorGeState, BackProgramHalvesCanBeWrittenInEitherOrderAndReplaced)
{
	HW::Shader     direct;
	HW::Shader     indirect;
	const uint32_t lo = 0x01020304u;
	const uint32_t hi = 0xffff0044u;
	ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_LO_GS, &lo, 1u));
	ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_HI_GS, &hi, 1u));
	ASSERT_TRUE(GraphicsDecodeGeShaderRegister(indirect, Pm4::SPI_SHADER_PGM_HI_GS, hi));
	ASSERT_TRUE(GraphicsDecodeGeShaderRegister(indirect, Pm4::SPI_SHADER_PGM_LO_GS, lo));
	EXPECT_EQ(direct.GetGsBackBase(), 0x440102030400ull);
	EXPECT_EQ(indirect.GetGsBackBase(), direct.GetGsBackBase());
	const uint32_t zero = 0;
	ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_LO_GS, &zero, 1u));
	ASSERT_TRUE(GraphicsDecodeGeShaderRegister(indirect, Pm4::SPI_SHADER_PGM_LO_GS, zero));
	EXPECT_EQ(direct.GetGsBackBase(), 0x440000000000ull);
	EXPECT_EQ(indirect.GetGsBackBase(), direct.GetGsBackBase());
	ASSERT_TRUE(GraphicsDecodeGeShaderRegisters(&direct, Pm4::SPI_SHADER_PGM_HI_GS, &zero, 1u));
	EXPECT_EQ(direct.GetGsBackBase(), 0u);
	EXPECT_EQ(direct.GetVs().gs_regs.data_addr, 0u);
	EXPECT_EQ(indirect.GetVs().gs_regs.data_addr, 0u);
}

TEST(EmulatorGeState, ResourceProvenanceSurvivesCopyAndTypedWritesInvalidateIt)
{
	HW::Shader shader;
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0u, false, false);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0u, false, false);
	ExpectRaw(shader.GetGsRsrc3Raw(), 0u, false, false);
	HW::GsShaderResource1 r1;
	r1.vgprs = 3;
	HW::GsShaderResource2 r2;
	r2.lds_size = 9;
	shader.SetGsShaderResource1(r1);
	shader.SetGsShaderResource2(r2);
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0u, true, false);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0u, true, false);
	shader.SetGsShaderResource1Raw(0xffffffffu);
	shader.SetGsShaderResource2Raw(0xffffffffu);
	shader.SetGsRsrc3(0xffffffffu);
	const HW::Shader copied = shader;
	EXPECT_TRUE(copied.GetVs().gs_regs.rsrc1.cdbg_user);
	EXPECT_EQ(copied.GetVs().gs_regs.rsrc2.user_sgpr, 63u);
	EXPECT_EQ(copied.GetVs().gs_regs.rsrc2.exception_en, 511u);
	EXPECT_EQ(copied.GetVs().gs_regs.rsrc2.GetLdsSizeBytes(), 130560u);
	shader.SetGsShaderResource1(copied.GetVs().gs_regs.rsrc1);
	shader.SetGsShaderResource2(copied.GetVs().gs_regs.rsrc2);
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0u, true, false);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0u, true, false);
	shader.Reset();
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0u, false, false);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0u, false, false);
	ExpectRaw(shader.GetGsRsrc3Raw(), 0u, false, false);
	ExpectRaw(copied.GetGsShaderResource1Raw(), 0xffffffffu, true, true);
	ExpectRaw(copied.GetGsShaderResource2Raw(), 0xffffffffu, true, true);
	ExpectRaw(copied.GetGsRsrc3Raw(), 0xffffffffu, true, true);
	shader.SetGsShaderResource1Raw(0u);
	shader.SetGsShaderResource2Raw(0u);
	shader.SetGsRsrc3(0u);
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0u, true, true);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0u, true, true);
	ExpectRaw(shader.GetGsRsrc3Raw(), 0u, true, true);
}

TEST(EmulatorGeState, ResourceSystemInputFieldsDoNotOverlap)
{
	struct Case
	{
		uint32_t value;
		bool     scratch;
		uint8_t  user_sgprs;
		uint8_t  es_components;
		bool     offchip;
		uint8_t  lds_size;
		uint8_t  shared_vgprs;
		bool     trap;
		uint16_t exceptions;
	};
	const Case cases[] = {
	    {0x00000001u, true,  0, 0, false,   0,  0, false,   0},
	    {0x0000003eu, false, 31, 0, false,   0,  0, false,   0},
	    {0x08000000u, false, 32, 0, false,   0,  0, false,   0},
	    {0x00000040u, false,  0, 0, false,   0,  0, true,    0},
	    {0x0000ff80u, false,  0, 0, false,   0,  0, false, 511},
	    {0x00030000u, false,  0, 3, false,   0,  0, false,   0},
	    {0x00040000u, false,  0, 0, true,    0,  0, false,   0},
	    {0x07f80000u, false,  0, 0, false, 255,  0, false,   0},
	    {0xf0000000u, false,  0, 0, false,   0, 15, false,   0},
	};
	HW::Shader shader;
	for (const auto& test: cases)
	{
		SCOPED_TRACE(test.value);
		shader.SetGsShaderResource2Raw(test.value);
		const auto& r = shader.GetVs().gs_regs.rsrc2;
		ExpectRaw(shader.GetGsShaderResource2Raw(), test.value, true, true);
		EXPECT_EQ(r.scratch_en, test.scratch);
		EXPECT_EQ(r.user_sgpr, test.user_sgprs);
		EXPECT_EQ(r.es_vgpr_component_count, test.es_components);
		EXPECT_EQ(r.offchip_lds, test.offchip);
		EXPECT_EQ(r.lds_size, test.lds_size);
		EXPECT_EQ(r.shared_vgprs, test.shared_vgprs);
		EXPECT_EQ(r.trap_present, test.trap);
		EXPECT_EQ(r.exception_en, test.exceptions);
	}
}

TEST(EmulatorGeState, GeControlPreservesFlagsAndUnknownBitsInBothPaths)
{
	HW::UserConfig direct;
	HW::UserConfig indirect;
	const uint32_t control = 0xa5fc3003u;
	const uint32_t user    = 0xfffffffdu;
	ASSERT_TRUE(GraphicsDecodeGeUserConfigRegisters(&direct, Pm4::GE_CNTL, &control, 1u));
	ASSERT_TRUE(GraphicsDecodeGeUserConfigRegisters(&direct, Pm4::GE_USER_VGPR_EN, &user, 1u));
	ASSERT_TRUE(GraphicsDecodeGeUserConfigRegister(indirect, Pm4::GE_CNTL, control));
	ASSERT_TRUE(GraphicsDecodeGeUserConfigRegister(indirect, Pm4::GE_USER_VGPR_EN, user));
	for (const auto* ucfg: {&direct, &indirect})
	{
		ExpectRaw(ucfg->GetGeControlRaw(), control, true, true);
		ExpectRaw(ucfg->GetGeUserVgprEnRaw(), user, true, true);
		EXPECT_EQ(ucfg->GetGeControl().primitive_group_size, 3u);
		EXPECT_EQ(ucfg->GetGeControl().vertex_group_size, 24u);
		EXPECT_TRUE(ucfg->GetGeControl().break_wave_at_eoi);
		EXPECT_TRUE(ucfg->GetGeControl().packet_to_one_pa);
		EXPECT_EQ(ucfg->GetGeControl().raw_unknown_bits, 0xa5f00000u);
		EXPECT_TRUE(ucfg->GetGeUserVgprEn().vgpr1);
		EXPECT_FALSE(ucfg->GetGeUserVgprEn().vgpr2);
		EXPECT_TRUE(ucfg->GetGeUserVgprEn().vgpr3);
		EXPECT_EQ(ucfg->GetGeUserVgprEn().raw_unknown_bits, 0xfffffff8u);
	}
}

TEST(EmulatorGeState, GeGroupFieldsDoNotConsumeControlFlags)
{
	const auto prim = GraphicsDecodeGeControl(0x000001ffu);
	EXPECT_EQ(prim.primitive_group_size, 511u);
	EXPECT_EQ(prim.vertex_group_size, 0u);
	const auto vert = GraphicsDecodeGeControl(0x0003fe00u);
	EXPECT_EQ(vert.primitive_group_size, 0u);
	EXPECT_EQ(vert.vertex_group_size, 511u);
	for (uint32_t flag: {0x00040000u, 0x00080000u})
	{
		const auto control = GraphicsDecodeGeControl(flag);
		EXPECT_EQ(control.primitive_group_size, 0u);
		EXPECT_EQ(control.vertex_group_size, 0u);
		EXPECT_EQ(control.break_wave_at_eoi, flag == 0x00040000u);
		EXPECT_EQ(control.packet_to_one_pa, flag == 0x00080000u);
		EXPECT_EQ(control.raw_unknown_bits, 0u);
	}
	for (uint32_t flag: {1u, 2u, 4u})
	{
		const auto en = GraphicsDecodeGeUserVgprEn(flag);
		EXPECT_EQ(en.vgpr1, flag == 1u);
		EXPECT_EQ(en.vgpr2, flag == 2u);
		EXPECT_EQ(en.vgpr3, flag == 4u);
	}
}

TEST(EmulatorGeState, TypedGeAssignmentsCannotFabricateRawWords)
{
	HW::UserConfig ucfg;
	ExpectRaw(ucfg.GetGeControlRaw(), 0u, false, false);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, false, false);
	HW::GeControl control;
	control.primitive_group_size = 3;
	control.vertex_group_size    = 24;
	HW::GeUserVgprEn user;
	user.vgpr2 = true;
	ucfg.SetGeControl(control);
	ucfg.SetGeUserVgprEn(user);
	ExpectRaw(ucfg.GetGeControlRaw(), 0u, true, false);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, true, false);
	ucfg.SetGeControlRaw(0xffffffffu);
	ucfg.SetGeUserVgprEnRaw(0xffffffffu);
	const HW::UserConfig copied = ucfg;
	ucfg.SetGeControl(copied.GetGeControl());
	ucfg.SetGeUserVgprEn(copied.GetGeUserVgprEn());
	ExpectRaw(ucfg.GetGeControlRaw(), 0u, true, false);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, true, false);
	ucfg.Reset();
	ExpectRaw(ucfg.GetGeControlRaw(), 0u, false, false);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, false, false);
	ExpectRaw(copied.GetGeControlRaw(), 0xffffffffu, true, true);
	ExpectRaw(copied.GetGeUserVgprEnRaw(), 0xffffffffu, true, true);
	ucfg.SetGeControlRaw(0u);
	ucfg.SetGeUserVgprEnRaw(0u);
	ExpectRaw(ucfg.GetGeControlRaw(), 0u, true, true);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, true, true);
}

TEST(EmulatorGeState, EncodedZeroThreadFieldIsKeptSeparateFromAmplification)
{
	const auto state = GraphicsDecodeGeNggSubgroupControl(0x46u);
	EXPECT_EQ(state.raw, 0x46u);
	EXPECT_EQ(state.primitive_amplification_factor, 70u);
	EXPECT_EQ(state.threads_per_subgroup, 0u);
	EXPECT_EQ(state.raw_unknown_bits, 0u);
	const auto all = GraphicsDecodeGeNggSubgroupControl(0xffffffffu);
	EXPECT_EQ(all.primitive_amplification_factor, 511u);
	EXPECT_EQ(all.threads_per_subgroup, 511u);
	EXPECT_EQ(all.raw_unknown_bits, 0xfffc0000u);
}

TEST(EmulatorGeState, MalformedDirectRangesDoNotPartiallyChangeState)
{
	HW::Shader shader;
	shader.SetGsShaderResource1Raw(0x12345678u);
	shader.SetGsShaderResource2Raw(0x23456789u);
	const uint32_t values[] = {0u, 0u};
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(nullptr, Pm4::SPI_SHADER_PGM_RSRC1_GS, values, 1u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, Pm4::SPI_SHADER_PGM_RSRC1_GS, nullptr, 1u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, Pm4::SPI_SHADER_PGM_RSRC1_GS, values, 0u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, Pm4::SPI_SHADER_PGM_RSRC1_GS, values, 3u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, Pm4::SPI_SHADER_PGM_RSRC2_GS, values, 2u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, Pm4::SPI_SHADER_PGM_RSRC3_GS, values, 0xffffffffu));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegisters(&shader, 0xffffffffu, values, 1u));
	EXPECT_FALSE(GraphicsDecodeGeShaderRegister(shader, Pm4::SPI_SHADER_PGM_RSRC1_ES, values[0]));
	ExpectRaw(shader.GetGsShaderResource1Raw(), 0x12345678u, true, true);
	ExpectRaw(shader.GetGsShaderResource2Raw(), 0x23456789u, true, true);
	ExpectRaw(shader.GetGsRsrc3Raw(), 0u, false, false);
	EXPECT_EQ(shader.GetGsBackBase(), 0u);

	HW::UserConfig ucfg;
	ucfg.SetGeControlRaw(0x12345678u);
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegisters(nullptr, Pm4::GE_CNTL, values, 1u));
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegisters(&ucfg, Pm4::GE_CNTL, nullptr, 1u));
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegisters(&ucfg, Pm4::GE_CNTL, values, 0u));
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegisters(&ucfg, Pm4::GE_CNTL, values, 2u));
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegisters(&ucfg, 0xffffffffu, values, 1u));
	EXPECT_FALSE(GraphicsDecodeGeUserConfigRegister(ucfg, Pm4::GE_USER_VGPR1, values[0]));
	ExpectRaw(ucfg.GetGeControlRaw(), 0x12345678u, true, true);
	ExpectRaw(ucfg.GetGeUserVgprEnRaw(), 0u, false, false);
}

UT_END();
