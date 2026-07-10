#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(EmulatorGraphicsState);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsState, DecodesGenericScissorHalves)
{
	HW::Context context;

	State::SetGenericScissorTl(context, 0x80020001u);
	State::SetGenericScissorBr(context, 0x00140010u);

	const auto& viewport = context.GetScreenViewport();
	EXPECT_EQ(viewport.generic_scissor_left, 1);
	EXPECT_EQ(viewport.generic_scissor_top, 2);
	EXPECT_EQ(viewport.generic_scissor_right, 16);
	EXPECT_EQ(viewport.generic_scissor_bottom, 20);
	EXPECT_FALSE(viewport.generic_scissor_window_offset_enable);
}

TEST(EmulatorGraphicsState, DecodesModeControl)
{
	HW::Context context;

	const uint32_t value = (1u << Pm4::PA_SU_SC_MODE_CNTL_CULL_FRONT_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_CULL_BACK_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_FACE_SHIFT) |
	                       (2u << Pm4::PA_SU_SC_MODE_CNTL_POLY_MODE_SHIFT) |
	                       (3u << Pm4::PA_SU_SC_MODE_CNTL_POLYMODE_FRONT_PTYPE_SHIFT) |
	                       (4u << Pm4::PA_SU_SC_MODE_CNTL_POLYMODE_BACK_PTYPE_SHIFT) |
	                       (1u << Pm4::PA_SU_SC_MODE_CNTL_PROVOKING_VTX_LAST_SHIFT);

	State::SetModeControl(context, value);

	const auto& mode = context.GetModeControl();
	EXPECT_TRUE(mode.cull_front);
	EXPECT_TRUE(mode.cull_back);
	EXPECT_TRUE(mode.face);
	EXPECT_EQ(mode.poly_mode, 2);
	EXPECT_EQ(mode.polymode_front_ptype, 3);
	EXPECT_EQ(mode.polymode_back_ptype, 4);
	EXPECT_TRUE(mode.provoking_vtx_last);
}

TEST(EmulatorGraphicsState, DecodesBlendControl)
{
	HW::Context context;

	const uint32_t value = (5u << Pm4::CB_BLEND0_CONTROL_COLOR_SRCBLEND_SHIFT) |
	                       (3u << Pm4::CB_BLEND0_CONTROL_COLOR_COMB_FCN_SHIFT) |
	                       (6u << Pm4::CB_BLEND0_CONTROL_COLOR_DESTBLEND_SHIFT) |
	                       (7u << Pm4::CB_BLEND0_CONTROL_ALPHA_SRCBLEND_SHIFT) |
	                       (2u << Pm4::CB_BLEND0_CONTROL_ALPHA_COMB_FCN_SHIFT) |
	                       (8u << Pm4::CB_BLEND0_CONTROL_ALPHA_DESTBLEND_SHIFT) |
	                       (1u << Pm4::CB_BLEND0_CONTROL_SEPARATE_ALPHA_BLEND_SHIFT) |
	                       (1u << Pm4::CB_BLEND0_CONTROL_ENABLE_SHIFT);

	State::SetBlendControl(context, 0, value);

	const auto& blend = context.GetBlendControl(0);
	EXPECT_EQ(blend.color_srcblend, 5);
	EXPECT_EQ(blend.color_comb_fcn, 3);
	EXPECT_EQ(blend.color_destblend, 6);
	EXPECT_EQ(blend.alpha_srcblend, 7);
	EXPECT_EQ(blend.alpha_comb_fcn, 2);
	EXPECT_EQ(blend.alpha_destblend, 8);
	EXPECT_TRUE(blend.separate_alpha_blend);
	EXPECT_TRUE(blend.enable);
}

UT_END();
