#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

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

TEST(EmulatorGraphicsState, IntersectsEnabledScissorRectangles)
{
	HW::Context context;
	context.SetScreenScissor(5, 15, 180, 200);
	context.SetGenericScissor(20, 5, 170, 190, false);
	context.SetViewportScissor(0, 10, 25, 160, 180, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = true;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 25);
	EXPECT_EQ(scissor.right, 160);
	EXPECT_EQ(scissor.bottom, 180);
}

TEST(EmulatorGraphicsState, IgnoresViewportScissorWhenDisabled)
{
	HW::Context context;
	context.SetScreenScissor(5, 15, 180, 200);
	context.SetGenericScissor(20, 5, 170, 190, false);
	context.SetViewportScissor(0, 40, 50, 100, 120, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = false;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 15);
	EXPECT_EQ(scissor.right, 170);
	EXPECT_EQ(scissor.bottom, 190);
}

TEST(EmulatorGraphicsState, RepresentsEmptyScissorIntersectionWithoutUnsignedWrap)
{
	HW::Context context;
	context.SetScreenScissor(0, 0, 10, 10);
	context.SetGenericScissor(20, 30, 40, 50, false);

	HW::ScanModeControl mode;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 20);
	EXPECT_EQ(scissor.top, 30);
	EXPECT_EQ(scissor.right, 20);
	EXPECT_EQ(scissor.bottom, 30);
}

TEST(EmulatorGraphicsState, ResolvesViewportAndGenericScissorWithoutScreenState)
{
	HW::Context context;
	context.SetGenericScissor(0, 0, 384, 216, false);
	context.SetViewportScissor(0, 0, 0, 384, 216, false);

	HW::ScanModeControl mode;
	mode.vport_scissor_enable = true;

	const auto scissor = State::ResolveScissor(context.GetScreenViewport(), mode, 0);
	EXPECT_EQ(scissor.left, 0);
	EXPECT_EQ(scissor.top, 0);
	EXPECT_EQ(scissor.right, 384);
	EXPECT_EQ(scissor.bottom, 216);
}

TEST(EmulatorGraphicsState, RequiresAnActiveDepthStencilOperationForTargetBinding)
{
	HW::DepthRenderTarget target;
	target.z_info.tile_surface_enable = true;

	HW::RenderControl render_control;
	HW::DepthControl  depth_control;
	depth_control.z_write_enable = true;

	auto usage = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_FALSE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control.z_enable = true;
	usage                  = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_TRUE(usage.depth_write_enable);

	depth_control.z_write_enable = false;
	usage                        = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control                = {};
	depth_control.stencil_enable = true;
	usage                        = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	depth_control                         = {};
	render_control.depth_compress_disable = true;
	usage                                 = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_TRUE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);

	target.z_info.tile_surface_enable = false;
	usage                             = State::ResolveDepthStencilUsage(target, render_control, depth_control);
	EXPECT_FALSE(usage.target_active);
	EXPECT_FALSE(usage.depth_write_enable);
}

TEST(EmulatorGraphicsState, ResolvesSharedVideoOutExportsForGen5Module)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libVideoOut_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"CBiu4mCE1DA";
	query.library              = U"VideoOut";
	query.library_version      = 1;
	query.module               = U"VideoOut";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	EXPECT_NE(symbols.Find(query), nullptr);
}

TEST(EmulatorGraphicsState, ReportsNoSystemServiceEventWithoutFabricatingOne)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libSystemService_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"656LMQSrg6U";
	query.library              = U"SystemService";
	query.library_version      = 1;
	query.module               = U"SystemService";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;

	const auto* record = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using ReceiveEventFunc = int (*)(void*);
	auto    receive_event  = reinterpret_cast<ReceiveEventFunc>(record->vaddr);
	uint8_t event[64]      = {};
	for (auto& value: event)
	{
		value = 0xa5;
	}

	EXPECT_EQ(receive_event(nullptr), Libs::SystemService::SYSTEM_SERVICE_ERROR_PARAMETER);
	EXPECT_EQ(receive_event(event), Libs::SystemService::SYSTEM_SERVICE_ERROR_NO_EVENT);
	for (const auto value: event)
	{
		EXPECT_EQ(value, 0xa5);
	}
}

UT_END();
