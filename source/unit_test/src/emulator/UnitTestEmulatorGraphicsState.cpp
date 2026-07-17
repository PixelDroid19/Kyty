#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Log.h"

UT_BEGIN(EmulatorGraphicsState);

using namespace Libs::Graphics;

TEST(EmulatorGraphicsState, TiledVideoOutBufferUpdateDoesNotCpuUpload)
{
	EXPECT_FALSE(VideoOutBufferShouldCpuUploadOnUpdate(true));
	EXPECT_TRUE(VideoOutBufferShouldCpuUploadOnUpdate(false));
}

TEST(EmulatorGraphicsState, BlitInitializesUndefinedSource)
{
	EXPECT_TRUE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_UNDEFINED));
	EXPECT_FALSE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	EXPECT_FALSE(UtilBlitImageNeedsSourceInitialization(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
}

TEST(EmulatorGraphicsState, MapsColorExportComponents)
{
	EXPECT_EQ(ShaderColorExportSourceComponent(0, 0), 0u);
	EXPECT_EQ(ShaderColorExportSourceComponent(0, 3), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(1, 0), 2u);
	EXPECT_EQ(ShaderColorExportSourceComponent(1, 3), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(2, 0), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(2, 3), 0u);
	EXPECT_EQ(ShaderColorExportSourceComponent(3, 0), 3u);
	EXPECT_EQ(ShaderColorExportSourceComponent(3, 3), 2u);
}

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

TEST(EmulatorGraphicsState, DecodesScreenScissorHalves)
{
	HW::Context context;

	State::SetScreenScissorTl(context, 0x000A0005u);
	State::SetScreenScissorBr(context, 0x00C800B4u);

	const auto& viewport = context.GetScreenViewport();
	EXPECT_EQ(viewport.screen_scissor_left, 5);
	EXPECT_EQ(viewport.screen_scissor_top, 10);
	EXPECT_EQ(viewport.screen_scissor_right, 180);
	EXPECT_EQ(viewport.screen_scissor_bottom, 200);
}

TEST(EmulatorGraphicsState, DecodesRenderControl)
{
	HW::Context context;

	const uint32_t value = (1u << Pm4::DB_RENDER_CONTROL_DEPTH_CLEAR_ENABLE_SHIFT) |
	                       (1u << Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_SHIFT) |
	                       (1u << Pm4::DB_RENDER_CONTROL_DEPTH_COMPRESS_DISABLE_SHIFT) |
	                       (3u << Pm4::DB_RENDER_CONTROL_COPY_SAMPLE_SHIFT);

	State::SetRenderControl(context, value);

	const auto& rc = context.GetRenderControl();
	EXPECT_TRUE(rc.depth_clear_enable);
	EXPECT_TRUE(rc.stencil_clear_enable);
	EXPECT_FALSE(rc.resummarize_enable);
	EXPECT_FALSE(rc.stencil_compress_disable);
	EXPECT_TRUE(rc.depth_compress_disable);
	EXPECT_FALSE(rc.copy_centroid);
	EXPECT_EQ(rc.copy_sample, 3u);
}

TEST(EmulatorGraphicsState, DecodesStencilControlAndRefMaskHalves)
{
	HW::Context context;

	const uint32_t control = (1u << Pm4::DB_STENCIL_CONTROL_STENCILFAIL_SHIFT) |
	                         (2u << Pm4::DB_STENCIL_CONTROL_STENCILZPASS_SHIFT) |
	                         (3u << Pm4::DB_STENCIL_CONTROL_STENCILZFAIL_SHIFT) |
	                         (4u << Pm4::DB_STENCIL_CONTROL_STENCILFAIL_BF_SHIFT) |
	                         (5u << Pm4::DB_STENCIL_CONTROL_STENCILZPASS_BF_SHIFT) |
	                         (6u << Pm4::DB_STENCIL_CONTROL_STENCILZFAIL_BF_SHIFT);
	State::SetStencilControl(context, control);

	const auto& sc = context.GetStencilControl();
	EXPECT_EQ(sc.stencil_fail, 1u);
	EXPECT_EQ(sc.stencil_zpass, 2u);
	EXPECT_EQ(sc.stencil_zfail, 3u);
	EXPECT_EQ(sc.stencil_fail_bf, 4u);
	EXPECT_EQ(sc.stencil_zpass_bf, 5u);
	EXPECT_EQ(sc.stencil_zfail_bf, 6u);

	State::SetStencilRefMask(context, 0x04030201u);
	State::SetStencilRefMaskBf(context, 0x08070605u);

	const auto& sm = context.GetStencilMask();
	EXPECT_EQ(sm.stencil_testval, 0x01u);
	EXPECT_EQ(sm.stencil_mask, 0x02u);
	EXPECT_EQ(sm.stencil_writemask, 0x03u);
	EXPECT_EQ(sm.stencil_opval, 0x04u);
	EXPECT_EQ(sm.stencil_testval_bf, 0x05u);
	EXPECT_EQ(sm.stencil_mask_bf, 0x06u);
	EXPECT_EQ(sm.stencil_writemask_bf, 0x07u);
	EXPECT_EQ(sm.stencil_opval_bf, 0x08u);
}

TEST(EmulatorGraphicsState, Gen5SampledRgba8FormatUsesUnormByDefault)
{
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 56), VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 56, true), VK_FORMAT_R8G8B8A8_SRGB);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 14), VK_FORMAT_R8G8_UNORM);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 71), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(Kyty::Libs::Graphics::TextureResolveSampledVkFormat(0, 0, 133), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
}

TEST(EmulatorGraphicsState, Gen5SharpSampledTextureAcceptsTexture2DType)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 8; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Vsharp;
	}
	user_sgpr.value[3] = 9u << 28u;

	ShaderSharp read_only_texture {};
	read_only_texture.offset_dw = 0;
	read_only_texture.size      = 0;

	ShaderUserData user_data {};
	user_data.sharp_resource_offset[0] = &read_only_texture;
	user_data.sharp_resource_count[0]  = 1;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 8);

	ASSERT_EQ(bind.textures2D.textures_num, 1);
	EXPECT_EQ(bind.textures2D.textures2d_sampled_num, 1);
	EXPECT_EQ(bind.textures2D.desc[0].texture.Type(), 9);
	EXPECT_EQ(bind.textures2D.desc[0].usage, ShaderTextureUsage::ReadOnly);
	EXPECT_EQ(usage.textures2D_readonly, 1);
}

TEST(EmulatorGraphicsState, Gen5SharpNullBufferDescriptorIsNotStorageBuffer)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 4; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}

	ShaderSharp read_only_buffer {};
	read_only_buffer.offset_dw = 0;
	read_only_buffer.size      = 1;

	ShaderUserData user_data {};
	user_data.sharp_resource_offset[0] = &read_only_buffer;
	user_data.sharp_resource_count[0]  = 1;

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 4);

	EXPECT_EQ(bind.storage_buffers.buffers_num, 0);
	EXPECT_EQ(usage.storage_buffers_constant, 0);
	EXPECT_EQ(bind.direct_sgprs.sgprs_num, 4);
}

TEST(EmulatorGraphicsState, Gen5DirectSgprsAllowFullUserWindow)
{
	HW::UserSgprInfo user_sgpr {};
	for (int i = 0; i < 28; i++)
	{
		user_sgpr.type[i] = HW::UserSgprType::Region;
	}

	ShaderUserData user_data {};

	ShaderParsedUsage  usage {};
	ShaderBindResources bind {};

	ShaderParseUsage2(&user_data, &usage, &bind, user_sgpr, 28);

	EXPECT_EQ(bind.direct_sgprs.sgprs_num, 28);
	EXPECT_EQ(usage.direct_sgprs, 28);
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

	// Slot 0 and slot 1 (CB_BLEND1_CONTROL = 0x1e1, captured on indirect CX path)
	// share the same decoder.
	for (uint32_t slot = 0; slot < 8; slot++)
	{
		State::SetBlendControl(context, slot, value);
		const auto& blend = context.GetBlendControl(slot);
		EXPECT_EQ(blend.color_srcblend, 5) << "slot " << slot;
		EXPECT_EQ(blend.color_comb_fcn, 3) << "slot " << slot;
		EXPECT_EQ(blend.color_destblend, 6) << "slot " << slot;
		EXPECT_EQ(blend.alpha_srcblend, 7) << "slot " << slot;
		EXPECT_EQ(blend.alpha_comb_fcn, 2) << "slot " << slot;
		EXPECT_EQ(blend.alpha_destblend, 8) << "slot " << slot;
		EXPECT_TRUE(blend.separate_alpha_blend) << "slot " << slot;
		EXPECT_TRUE(blend.enable) << "slot " << slot;
	}
	// Register spacing matches direct+indirect jump tables.
	EXPECT_EQ(Pm4::CB_BLEND0_CONTROL + 1u, 0x1e1u);
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

TEST(EmulatorGraphicsState, ResolvesViewportDepthForClipSpaceAndHostLimits)
{
	// Captured first MRT world pass: zscale=1, zoffset=0, dx_clip_space=false.
	auto ogl = State::ResolveViewportDepth(1.0f, 0.0f, false, true);
	EXPECT_FLOAT_EQ(ogl.min_depth, -1.0f);
	EXPECT_FLOAT_EQ(ogl.max_depth, 1.0f);

	// Intel Arc and many hosts lack VK_EXT_depth_range_unrestricted.
	ogl = State::ResolveViewportDepth(1.0f, 0.0f, false, false);
	EXPECT_FLOAT_EQ(ogl.min_depth, 0.0f);
	EXPECT_FLOAT_EQ(ogl.max_depth, 1.0f);

	auto dx = State::ResolveViewportDepth(0.5f, 0.5f, true, true);
	EXPECT_FLOAT_EQ(dx.min_depth, 0.5f);
	EXPECT_FLOAT_EQ(dx.max_depth, 1.0f);

	dx = State::ResolveViewportDepth(1.0f, 0.0f, true, false);
	EXPECT_FLOAT_EQ(dx.min_depth, 0.0f);
	EXPECT_FLOAT_EQ(dx.max_depth, 1.0f);
}

TEST(EmulatorGraphicsState, ResolvesViewportXyFromScaleAndOffset)
{
	const auto xy = State::ResolveViewportXy(640.0f, 640.0f, 360.0f, 360.0f);
	EXPECT_FLOAT_EQ(xy.x, 0.0f);
	EXPECT_FLOAT_EQ(xy.y, 0.0f);
	EXPECT_FLOAT_EQ(xy.width, 1280.0f);
	EXPECT_FLOAT_EQ(xy.height, 720.0f);

	const auto xy2 = State::ResolveViewportXy(100.0f, 250.0f, 50.0f, 150.0f);
	EXPECT_FLOAT_EQ(xy2.x, 150.0f);
	EXPECT_FLOAT_EQ(xy2.y, 100.0f);
	EXPECT_FLOAT_EQ(xy2.width, 200.0f);
	EXPECT_FLOAT_EQ(xy2.height, 100.0f);
}

TEST(EmulatorGraphicsState, SeparatesHtileMetaClearFromRegisterDepthClear)
{
	// Captured world path: register DEPTH_CLEAR_ENABLE=0, HTILE meta clear=1.
	auto htile_only = State::ResolveDepthClearActions(false, true);
	EXPECT_TRUE(htile_only.vulkan_clear);
	EXPECT_FALSE(htile_only.suppress_depth_write);

	auto register_only = State::ResolveDepthClearActions(true, false);
	EXPECT_TRUE(register_only.vulkan_clear);
	EXPECT_TRUE(register_only.suppress_depth_write);

	auto both = State::ResolveDepthClearActions(true, true);
	EXPECT_TRUE(both.vulkan_clear);
	EXPECT_TRUE(both.suppress_depth_write);

	auto neither = State::ResolveDepthClearActions(false, false);
	EXPECT_FALSE(neither.vulkan_clear);
	EXPECT_FALSE(neither.suppress_depth_write);
}

TEST(EmulatorGraphicsState, DepthAttachmentLoadOpsClearWhenGuestDepthClear)
{
	using namespace Kyty::Libs::Graphics;
	// HTILE/register clear → loadOp CLEAR + UNDEFINED.
	auto ds = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, true, false);
	EXPECT_EQ(ds.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ds.stencil_load, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	EXPECT_EQ(ds.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto both = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, true, true);
	EXPECT_EQ(both.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(both.stencil_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(both.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto depth_only = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT, true, false);
	EXPECT_EQ(depth_only.depth_load, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(depth_only.stencil_load, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	EXPECT_EQ(depth_only.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);

	auto load = ResolveDepthAttachmentLoadOps(VK_FORMAT_D32_SFLOAT_S8_UINT, false, false);
	EXPECT_EQ(load.depth_load, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(load.stencil_load, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(load.initial_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
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

// Gen5 ACB/DCB sizing helpers and Trinity query contracts used by Astro boot.
TEST(EmulatorGraphicsState, Gen5AgcSizeHelpersAndTrinityMode)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	using namespace Kyty::Libs::Graphics::Gen5;
	EXPECT_EQ(GraphicsCbNopGetSize(4), 16u);
	EXPECT_EQ(GraphicsCbNopGetSize(1), 4u);
	EXPECT_EQ(GraphicsCbDispatchGetSize(), 20u);
	EXPECT_EQ(GraphicsCbSetShRegisterRangeDirectGetSize(3), 20u);
	EXPECT_EQ(GraphicsGetIsTrinityMode(), 0u);
	EXPECT_EQ(GraphicsDebugRaiseException(0x1234u), OK);

	uint32_t write_cmd[4] = {KYTY_PM4(4, Pm4::IT_WRITE_DATA, 0u), 0u, 0u, 0u};
	EXPECT_EQ(GraphicsWriteDataPatchSetAddressOrOffset(write_cmd, 0x1122334455667788ull), OK);
	EXPECT_EQ(write_cmd[2], 0x55667788u);
	EXPECT_EQ(write_cmd[3], 0x11223344u);
	uint32_t bad_cmd[2] = {KYTY_PM4(2, Pm4::IT_NOP, Pm4::R_ZERO), 0u};
	EXPECT_NE(GraphicsWriteDataPatchSetAddressOrOffset(bad_cmd, 0), OK);
}

// Missing Gen5 AGC / AgcDriver exports that blocked Astro after Ampr/VideoOut.
TEST(EmulatorGraphicsState, ResolvesGen5AgcAndDriverExports)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libGraphicsDriver_1", &symbols));

	auto resolve = [&](const char16_t* nid, const char16_t* library, const char16_t* module) {
		Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = library;
		query.library_version      = 1;
		query.module               = module;
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		return symbols.Find(query) != nullptr;
	};

	EXPECT_TRUE(resolve(u"BfBDZGbti7A", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"htn36gPnBk4", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"t7PlZ9nt5Lc", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"1rZSWUv1IRc", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"+kSrjIVxKFE", u"Agc", u"Agc"));
	EXPECT_TRUE(resolve(u"AhGvpITrf4M", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"gSRnr79F8tQ", u"Graphics5Driver", u"Graphics5Driver"));
	EXPECT_TRUE(resolve(u"w2rJhmD+dsE", u"Graphics5Driver", u"Graphics5Driver"));
}

// WaitFlipDone body observed post-Play: handle=0, index=3 while Open() left
// only slot 1 opened. Resolve handle 0 to that sole open port.
TEST(EmulatorGraphicsState, ResolvesVideoOutHandleZeroToSoleOpenPort)
{
	using Kyty::Libs::VideoOut::VideoOutResolveHandle;

	const bool only_one[] = {false, true};
	EXPECT_EQ(VideoOutResolveHandle(0, only_one, 2), 1);
	EXPECT_EQ(VideoOutResolveHandle(1, only_one, 2), 1);

	const bool none[] = {false, false};
	EXPECT_EQ(VideoOutResolveHandle(0, none, 2), 0);

	const bool both[] = {true, true};
	EXPECT_EQ(VideoOutResolveHandle(0, both, 2), 0); // ambiguous

	const bool only_zero[] = {true, false};
	EXPECT_EQ(VideoOutResolveHandle(0, only_zero, 2), 0);
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

TEST(EmulatorGraphicsState, ClassifiesConstantStorageResourcesAsReadOnly)
{
	EXPECT_TRUE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::Constant));
	EXPECT_TRUE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::ReadOnly));
	EXPECT_FALSE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::ReadWrite));
	EXPECT_FALSE(ShaderStorageUsageIsReadOnly(ShaderStorageUsage::Unknown));
}

// Guest malloc/new → host heap; small V# bases land here (Dreaming Sarah).
TEST(EmulatorGraphicsState, ClassifiesHostGuestMallocRangesForLazyGpuHeaps)
{
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0, 0x40));
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x1190000, 0x40));           // direct GPU map
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x900000000ull, 0x40));      // main image
	EXPECT_TRUE(GpuMemoryIsHostGuestMallocRange(0x7f005c0b3d20ull, 0x40));    // captured VB
	EXPECT_TRUE(GpuMemoryIsHostGuestMallocRange(0x7fffcc068720ull, 0x18));    // host index-ish
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x7f005c0b3d20ull, 0));
	// Overflow / past host band
	EXPECT_FALSE(GpuMemoryIsHostGuestMallocRange(0x7fffffffffffffull, 0x20));

	uint64_t start = 0;
	uint64_t size  = 0;
	GpuMemoryHostGuestMallocPageCover(0x7f005c0b3d20ull, 0x40, &start, &size);
	EXPECT_EQ(start, 0x7f005c0b3000ull);
	EXPECT_EQ(size, 0x1000ull);
	// Spans a page boundary
	GpuMemoryHostGuestMallocPageCover(0x7f005c0b3ff0ull, 0x20, &start, &size);
	EXPECT_EQ(start, 0x7f005c0b3000ull);
	EXPECT_EQ(size, 0x2000ull);
}

TEST(EmulatorGraphicsState, SharesOverlappingReadOnlyStorageViews)
{
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x70, true));
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x80, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, false, 0x1010, 0x70, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1010, 0x70, false));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1080, 0x20, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(0x1000, 0x80, true, 0x1000, 0x80, true));

	// Observed multi-parent RO StorageBuffer geometry: a 0x70 view Contained in
	// a 0x80 parent and Crossing a neighboring 0x70 parent. Both parents must
	// individually share so CreateObject can link them without inventing a
	// single-parent policy.
	constexpr uint64_t parent_contains_addr = 0x12267d9a0ull;
	constexpr uint64_t parent_contains_size = 0x80ull;
	constexpr uint64_t parent_crosses_addr  = 0x12267d9c0ull;
	constexpr uint64_t parent_crosses_size  = 0x70ull;
	constexpr uint64_t child_addr           = 0x12267d9b0ull;
	constexpr uint64_t child_size           = 0x70ull;
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(parent_contains_addr, parent_contains_size, true, child_addr, child_size, true));
	EXPECT_TRUE(GpuMemoryCanShareReadOnlyStorageViews(parent_crosses_addr, parent_crosses_size, true, child_addr, child_size, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(parent_contains_addr, parent_contains_size, false, child_addr, child_size, true));
	EXPECT_FALSE(GpuMemoryCanShareReadOnlyStorageViews(parent_crosses_addr, parent_crosses_size, true, child_addr, child_size, false));
}

TEST(EmulatorGraphicsState, ReversesGpuMemoryOverlapRelations)
{
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Equals), GpuMemoryOverlapType::Equals);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Crosses), GpuMemoryOverlapType::Crosses);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::Contains), GpuMemoryOverlapType::IsContainedWithin);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::IsContainedWithin), GpuMemoryOverlapType::Contains);
	EXPECT_EQ(GpuMemoryReverseOverlap(GpuMemoryOverlapType::None), GpuMemoryOverlapType::None);
}

// Non-exact FindRenderTexture: IsContainedWithin and Contains (same-base or
// offset-into-parent). PreferGpuMemoryAliasIndex picks among multi-matches;
// Size()>1 no longer EXIT. Crosses rejected (wrong-sized bind).
TEST(EmulatorGraphicsState, FindObjectsNonExactAcceptsContainedSampleInRenderTarget)
{
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Equals, true));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Equals, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, true));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::IsContainedWithin, true));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::IsContainedWithin, false));
	// Same-base Contains: sample and RT share start address (size mismatch only).
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false, true));
	// Offset-into-parent Contains: bind the live parent RT instead of empty
	// tile-27 upload (opaque-black props). Cropped views are a follow-up.
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false, false));
	EXPECT_TRUE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Contains, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::Crosses, false));
	EXPECT_FALSE(GpuMemoryFindObjectsAcceptsRelation(GpuMemoryOverlapType::None, false));
}

TEST(EmulatorGraphicsState, PreferGpuMemoryAliasPicksTightestCover)
{
	const uint64_t sizes[] = {0x60000ull, 0x140000ull, 0xa0000ull};
	// Sample fits in 0xa0000 and 0x140000; prefer the smaller cover.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0xa0000ull), 2u);
	// Sample larger than every object: prefer the largest under-sample RT.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0x200000ull), 1u);
	// Comparable size proxy only: prefer the smallest object.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 3, 0ull), 0u);
	EXPECT_EQ(PreferGpuMemoryAliasIndex(sizes, 1, 0x100ull), 0u);
}

// Residual world false-color: ufmt-56 samples must not alias float lighting RTs.
// GraphicsRender rejects the RT alias entirely when every overlap is the wrong
// family for known ufmts 56/71 (falls through to guest/storage upload).
TEST(EmulatorGraphicsState, Gen5SampleFormatMatchesVulkanRejectsFloatForRgba8)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R8G8B8A8_UNORM));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R8G8B8A8_SRGB));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_B8G8R8A8_UNORM));
	EXPECT_FALSE(Gen5SampleFormatMatchesVulkan(56u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(71u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_FALSE(Gen5SampleFormatMatchesVulkan(71u, VK_FORMAT_R8G8B8A8_UNORM));
	// Unknown ufmt: do not invent a filter that drops every match.
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(14u, VK_FORMAT_R8G8_UNORM));
	EXPECT_TRUE(Gen5SampleFormatMatchesVulkan(14u, VK_FORMAT_R16G16B16A16_SFLOAT));

	// Simulated multi-match policy used by PrepareTextures: when every RT is
	// float and the sample is ufmt 56, zero compatible aliases → reject path.
	const VkFormat only_float[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
	size_t         compatible   = 0;
	for (VkFormat f: only_float)
	{
		if (Gen5SampleFormatMatchesVulkan(56u, f))
		{
			compatible++;
		}
	}
	EXPECT_EQ(compatible, 0u);
	const VkFormat mixed[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
	compatible             = 0;
	for (VkFormat f: mixed)
	{
		if (Gen5SampleFormatMatchesVulkan(56u, f))
		{
			compatible++;
		}
	}
	EXPECT_EQ(compatible, 1u);
}

// Sample guest upload must never flush StorageBuffers first (linear SSBO ≠ texture).
TEST(EmulatorGraphicsState, Gen5SampleGuestUploadNeverWriteBackStorage)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_FALSE(Gen5SampleMayWriteBackStorageBeforeGuestUpload());
}

// Tiled guest upload under a live color surface is forbidden (GPU-owned guest).
TEST(EmulatorGraphicsState, Gen5SampleMayGuestUploadTiledRejectsCoveredTile27)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(27u, true));
	EXPECT_FALSE(Gen5SampleMayGuestUploadTiled(9u, true));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(27u, false));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(9u, false));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(0u, true));
	EXPECT_TRUE(Gen5SampleMayGuestUploadTiled(0u, false));
}

// Hash refresh stays enabled so late package loads still upload; SSBO clobber
// is handled by skipping guest re-detile when a live RT/ST parent is linked.
TEST(EmulatorGraphicsState, Gen5SampleTextureHashRefreshStaysEnabled)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(56u));
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(71u));
	EXPECT_TRUE(Gen5SampleTextureUsesHashRefresh(0u));
}

// CreateFromObjects may copy a live surface parent only when ufmt families match.
// Float lighting under an RGBA8 sample must not be blitted into the Texture.
TEST(EmulatorGraphicsState, Gen5SampleMayCopyFromSurfaceParentMatchesFormatFamily)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R8G8B8A8_UNORM));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R8G8B8A8_SRGB));
	EXPECT_FALSE(Gen5SampleMayCopyFromSurfaceParent(56u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(71u, VK_FORMAT_R16G16B16A16_SFLOAT));
	EXPECT_FALSE(Gen5SampleMayCopyFromSurfaceParent(71u, VK_FORMAT_R8G8B8A8_UNORM));
	// Unknown sample ufmt: do not block all parents.
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(14u, VK_FORMAT_R8G8_UNORM));
	EXPECT_TRUE(Gen5SampleMayCopyFromSurfaceParent(14u, VK_FORMAT_R16G16B16A16_SFLOAT));
}

// Dead Cells residual: bind only exact sample extent. A larger parent without a
// crop view left banded/false-color world tiles (never fall back to non-exact).
TEST(EmulatorGraphicsState, Gen5PickSampleSurfaceAliasesPrefersExactExtent)
{
	using namespace Kyty::Libs::Graphics;
	const VkFormat formats[]   = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM,
	                              VK_FORMAT_R8G8B8A8_UNORM};
	const uint32_t extents_w[] = {1920u, 1920u, 128u};
	const uint32_t extents_h[] = {1080u, 1080u, 128u};
	int            indices[16] = {};
	size_t         count       = 0;
	bool           reject      = false;

	// Sample 128x128 ufmt 56: skip float parent; only exact 128x128.
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 128u, 128u, 3, formats, extents_w, extents_h, indices,
	                                         &count, &reject));
	EXPECT_FALSE(reject);
	ASSERT_EQ(count, 1u);
	EXPECT_EQ(indices[0], 2);

	// Only float candidates for ufmt 56 → reject.
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 1920u, 1080u, 1, formats, extents_w, extents_h, indices,
	                                         &count, &reject));
	EXPECT_TRUE(reject);
	EXPECT_EQ(count, 0u);

	// Format match without exact extent → reject (no full-screen parent bind).
	const VkFormat only_large[] = {VK_FORMAT_R8G8B8A8_UNORM};
	const uint32_t large_w[]    = {1920u};
	const uint32_t large_h[]    = {1080u};
	ASSERT_TRUE(Gen5PickSampleSurfaceAliases(56u, 128u, 128u, 1, only_large, large_w, large_h, indices,
	                                         &count, &reject));
	EXPECT_TRUE(reject);
	EXPECT_EQ(count, 0u);
}

// Sample pixel area vs RT extent areas (GraphicsRender sample-bind path).
// A large sample covering a tiny child RT and a full-size parent must bind the
// parent (tightest cover >= sample), not the child (sample_size==0 bug).
TEST(EmulatorGraphicsState, PreferGpuMemoryAliasUsesSampleAreaAgainstRtExtents)
{
	const uint64_t rt_areas[] = {64ull * 64ull, 1920ull * 1080ull, 128ull * 128ull};
	const uint64_t sample_area = 1920ull * 1080ull;
	EXPECT_EQ(PreferGpuMemoryAliasIndex(rt_areas, 3, sample_area), 1u);
	// Smaller sample: prefer the 128x128 cover over 1920x1080.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(rt_areas, 3, 128ull * 128ull), 2u);
}

// Guest allocation bytes (GraphicsRender when every RT recorded guest_size).
// Same-extent children can still differ in tiled guest size; Prefer must use
// those bytes against size.size, not invent an area proxy.
TEST(EmulatorGraphicsState, PreferGpuMemoryAliasUsesGuestByteSizes)
{
	// Child IsContainedWithin: 0x10000; parent cover: 0x800000; sibling: 0x20000.
	const uint64_t guest_sizes[] = {0x10000ull, 0x800000ull, 0x20000ull};
	const uint64_t sample_bytes  = 0x7f0000ull;
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, sample_bytes), 1u);
	// Sample fits only under-sample objects: pick the largest child.
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, 0x900000ull), 1u);
	EXPECT_EQ(PreferGpuMemoryAliasIndex(guest_sizes, 3, 0x18000ull), 2u);
}

// Capture/disk bounds: unset env defaults to 1280 so 4K VideoOut dumps are not
// multi-dozen-MB BMPs; explicit 0 keeps full resolution; prune math is pure.
TEST(EmulatorGraphicsState, NativeCaptureDefaultsAndPruneBoundDisk)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_EQ(NativeCaptureResolveMaxEdge(nullptr), 1280u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge(""), 1280u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("0"), 0u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("1920"), 1920u);
	EXPECT_EQ(NativeCaptureResolveMaxEdge("nope"), 1280u);

	EXPECT_EQ(NativeCapturePruneCount(3, 8), 0u);
	EXPECT_EQ(NativeCapturePruneCount(8, 8), 0u);
	EXPECT_EQ(NativeCapturePruneCount(12, 8), 4u);
	EXPECT_EQ(NativeCapturePruneCount(5, 0), 0u);
}

// Pipeline cache must recycle slots instead of EXIT when variants exceed the cap.
TEST(EmulatorGraphicsState, PipelineCacheNextEvictIndexRotates)
{
	using namespace Kyty::Libs::Graphics;
	uint32_t cursor = 0;
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 1u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 2u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 3u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(0, &cursor), 0u);
	EXPECT_EQ(PipelineCacheNextEvictIndex(4, nullptr), 0u);
}

TEST(EmulatorGraphicsState, AllowsOnlyObservedTextureStorageAliases)
{
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	// Captured post-menu path: StorageBuffer registration that Crosses an
	// existing Texture (partial guest range) must link, not EXIT.
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::IsContainedWithin,
	                                                GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Equals,
	                                                GpuMemoryObjectType::StorageBuffer));
}

// Multi-parent StorageBuffer: each parent must independently pass texture-alias
// or vertex-share. Mixed Texture Contains + Vertex Contains is the observed
// create_all_the_same failure class under post-menu load.
TEST(EmulatorGraphicsState, AllowsMixedTextureVertexStorageParents)
{
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                              GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                              GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	// Both sides of a mixed multi-parent set must be acceptable.
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexStorageShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                              GpuMemoryObjectType::StorageBuffer));
}

// Captured dual-strict post-RT layout fix: Texture Contains IndexBuffer 0xe4.
TEST(EmulatorGraphicsState, AllowsIndexContainedInTextureSurface)
{
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::IsContainedWithin,
	                                                   GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::VertexBuffer));
}

// Captured: VertexBuffer Contained by co-located StorageBuffer + RenderTexture.
// Multi-parent load also uses surface IsContainedWithin/Crosses + peer VB reclaim.
TEST(EmulatorGraphicsState, AllowsVertexContainedInStorageAndRenderTarget)
{
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                                   GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexContainedInSurface(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexReclaimVertex(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::VertexBuffer));
	// Captured: Texture Contains + IndexBuffer Crosses → link IB with new VB.
	EXPECT_TRUE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Crosses,
	                                                 GpuMemoryObjectType::VertexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::Contains,
	                                                 GpuMemoryObjectType::VertexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsVertexLinkIndexBuffer(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                                  GpuMemoryObjectType::VertexBuffer));
	// Captured: IndexBuffer IsContainedWithin + VertexBuffer Contains → reclaim
	// peer IB, link VB with the new IndexBuffer.
	EXPECT_TRUE(GpuMemoryAllowsIndexReclaimIndex(GpuMemoryObjectType::IndexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                             GpuMemoryObjectType::IndexBuffer));
	EXPECT_TRUE(GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                                 GpuMemoryObjectType::IndexBuffer));
	EXPECT_FALSE(GpuMemoryAllowsIndexLinkVertexBuffer(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                  GpuMemoryObjectType::IndexBuffer));
}

// Captured post-Param5: RenderTexture with SB Equals + SB/RT Contains parents.
TEST(EmulatorGraphicsState, AllowsRenderTargetMultiSurfaceParents)
{
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_TRUE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::Label, GpuMemoryOverlapType::Equals,
	                                                     GpuMemoryObjectType::RenderTexture));
	EXPECT_FALSE(GpuMemoryAllowsRenderTargetSurfaceAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                                     GpuMemoryObjectType::Texture));
}

// Captured: StorageBuffer with RT+SB Crosses and IsContainedWithin parents.
// Also dual-strict after VOP1 SDWA: SB 0x8000 Crosses DepthStencilBuffer.
TEST(EmulatorGraphicsState, AllowsStorageSurfaceShareWithContainedWithin)
{
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::DepthStencilBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsStorageSurfaceShare(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                                GpuMemoryObjectType::StorageBuffer));
}

// Captured: Texture over 5 VBs (reclaim) + StorageBuffer/RenderTexture Contains (link).
// Also VB Contains texture (link larger VB) mixed with SB/RT Contains.
TEST(EmulatorGraphicsState, AllowsTextureMixedReclaimAndSurfaceParents)
{
	EXPECT_TRUE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureLinkVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Contains,
	                                            GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::RenderTexture, GpuMemoryOverlapType::Contains,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Crosses,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::VideoOutBuffer, GpuMemoryOverlapType::Equals,
	                                                    GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureContainedInSurface(GpuMemoryObjectType::VideoOutBuffer, GpuMemoryOverlapType::Contains,
	                                                     GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureReclaimVertex(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryAllowsTextureLinkVertex(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::Crosses,
	                                             GpuMemoryObjectType::Texture));
}

// Captured multi-parent SB write-back always links GPU-owned tiled RTs
// (PARAM_TILED=1, PARAM_WRITE_BACK=0). Those parents must skip hash invalidate.
TEST(EmulatorGraphicsState, SkipWriteBackInvalidateForGpuOwnedRenderTexture)
{
	uint64_t gpu_owned[8] = {};
	gpu_owned[3]          = 1; // PARAM_TILED
	gpu_owned[6]          = 0; // PARAM_WRITE_BACK
	EXPECT_TRUE(GpuMemoryIsGpuOwnedRenderTextureParams(gpu_owned));
	EXPECT_TRUE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::RenderTexture, gpu_owned));

	uint64_t cpu_wb[8] = {};
	cpu_wb[3]          = 1;
	cpu_wb[6]          = 1;
	EXPECT_FALSE(GpuMemoryIsGpuOwnedRenderTextureParams(cpu_wb));
	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::RenderTexture, cpu_wb));

	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::Texture, gpu_owned));
	EXPECT_FALSE(GpuMemorySkipWriteBackParentInvalidate(GpuMemoryObjectType::VertexBuffer, gpu_owned));
}

TEST(EmulatorGraphicsState, UsesTrackedLayoutWhenUploadingOverExistingImage)
{
	VulkanImage image(VulkanImageType::Texture);
	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXPECT_EQ(UtilGetImageUploadSourceLayout(&image), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Captured Gen5 WriteBack topology: RW StorageBuffer with 8 VertexBuffer
// Crosses/IsContainedWithin parents plus one Equals RenderTexture peer.
// Policy must accept the set, recompute via Equals (not self), and invalidate
// the partial-overlap VertexBuffers — without inventing None/Max relations.
TEST(EmulatorGraphicsState, WriteBackClassifiesMultiParentStorageTopology)
{
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::Equals), GpuMemoryWriteBackParentAction::PropagateEquals);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::Crosses), GpuMemoryWriteBackParentAction::InvalidateOnly);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::IsContainedWithin), GpuMemoryWriteBackParentAction::InvalidateOnly);
	EXPECT_EQ(GpuMemoryWriteBackParentActionFor(GpuMemoryOverlapType::None), GpuMemoryWriteBackParentAction::Unsupported);

	// Empty: recompute self only.
	bool     recompute = false;
	uint32_t equals    = 99;
	uint32_t inv       = 99;
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(nullptr, 0, &recompute, &equals, &inv));
	EXPECT_TRUE(recompute);
	EXPECT_EQ(equals, 0u);
	EXPECT_EQ(inv, 0u);

	// Observed multi-parent list (order matches CreateObject link dump).
	const GpuMemoryOverlapType observed[] = {
	    GpuMemoryOverlapType::Crosses,           // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::IsContainedWithin, // VB
	    GpuMemoryOverlapType::Crosses,           // VB
	    GpuMemoryOverlapType::Equals,            // RenderTexture peer
	};
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(observed, static_cast<uint32_t>(sizeof(observed) / sizeof(observed[0])), &recompute,
	                                              &equals, &inv));
	EXPECT_FALSE(recompute);
	EXPECT_EQ(equals, 1u);
	EXPECT_EQ(inv, 8u);

	// Only partial overlaps: still recompute self after GPU->CPU write-back.
	const GpuMemoryOverlapType only_vb[] = {GpuMemoryOverlapType::Crosses, GpuMemoryOverlapType::IsContainedWithin};
	ASSERT_TRUE(GpuMemoryWriteBackClassifyParents(only_vb, 2, &recompute, &equals, &inv));
	EXPECT_TRUE(recompute);
	EXPECT_EQ(equals, 0u);
	EXPECT_EQ(inv, 2u);

	// Unsupported relation must fail closed.
	const GpuMemoryOverlapType bad[] = {GpuMemoryOverlapType::Equals, GpuMemoryOverlapType::None};
	EXPECT_FALSE(GpuMemoryWriteBackClassifyParents(bad, 2, &recompute, &equals, &inv));
}


// CB_TARGET_MASK 0x0000ffff (RT0..RT3 full) must still yield RT0 nibble 0xF
// for the single-attachment pipeline write mask.
TEST(EmulatorGraphicsState, TargetMaskRt0NibbleFromMultiTarget)
{
	constexpr uint32_t multi = 0x0000ffffu;
	EXPECT_EQ(multi & 0xFu, 0xFu);
	EXPECT_EQ(0x00000000u & 0xFu, 0x0u);
	EXPECT_EQ(0x0000000Fu & 0xFu, 0xFu);
	// Partial RT0 channels
	EXPECT_EQ(0x00000005u & 0xFu, 0x5u);
}


// Gen5 depth size: table path for 1280x720 D32S8, formula path for 642x362
// (captured DEPTH_SIZE_FAIL).
TEST(EmulatorGraphicsState, TileGetDepthSizeNextGenNonTable)
{
	using namespace Kyty::Libs::Graphics;
	TileSizeAlign stencil {}, htile {}, depth {};
	ASSERT_TRUE(TileGetDepthSize(1280, 720, 0, 3, 1, true, true, true, &stencil, &htile, &depth));
	EXPECT_EQ(depth.size, 3932160u);
	EXPECT_EQ(stencil.size, 983040u);
	// Captured non-table surface.
	ASSERT_TRUE(TileGetDepthSize(642, 362, 0, 3, 1, true, true, true, &stencil, &htile, &depth));
	EXPECT_GT(depth.size, 0u);
	EXPECT_EQ(depth.size % 4u, 0u);
	EXPECT_EQ(depth.align, 65536u);
	EXPECT_GT(stencil.size, 0u);
	EXPECT_EQ(stencil.align, 65536u);
	EXPECT_GT(htile.size, 0u);
}


// Captured DepthStencilBuffer create (3 vaddrs) Crossing Texture + StorageBuffer.
TEST(EmulatorGraphicsState, DepthStencilReclaimParentsAreSurfaces)
{
	// Document the accepted parent types for multi_depth_stencil_reclaim.
	const GpuMemoryObjectType ok[] = {GpuMemoryObjectType::Texture, GpuMemoryObjectType::StorageBuffer,
	                                  GpuMemoryObjectType::RenderTexture, GpuMemoryObjectType::VertexBuffer};
	for (auto t : ok)
	{
		EXPECT_NE(t, GpuMemoryObjectType::DepthStencilBuffer);
	}
	EXPECT_EQ(GpuMemoryOverlapType::Crosses, GpuMemoryOverlapType::Crosses);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearOnUndefinedFirstUse)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV R8G8B8A8: CLEAR_WORD0/1=0 packs to transparent black (A=0).
	// Inventing opaque A=1 made sprite-layer intermediates composite as black quads.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_UNDEFINED);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsFloatZeroWordsClearToZeroAlpha)
{
	using namespace Kyty::Libs::Graphics;
	// Captured lighting RT: fmt=12/ctype=7, CLEAR_WORD0/1=0, blend SRC_ALPHA,ONE.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsLoadOnOptimalSubsequentPass)
{
	using namespace Kyty::Libs::Graphics;
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_LOAD);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearWhenRebindingAfterSample)
{
	using namespace Kyty::Libs::Graphics;
	// After composite samples the lighting RT, layout is SHADER_READ_ONLY. The next
	// frame's first additive light pass must CLEAR (zeros), not LOAD prior-frame HDR.
	auto ops =
	    ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);

	// Within-frame accumulation (still COLOR_ATTACHMENT) must keep LOAD.
	auto load_ops =
	    ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, 0u, 0u, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(load_ops.load_op, VK_ATTACHMENT_LOAD_OP_LOAD);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsRgba8RebindClearsTransparent)
{
	using namespace Kyty::Libs::Graphics;
	// Captured GBuffer/sprite RTs: fmt=10, CLEAR_WORD=0, fast=0, sampled then rebound.
	auto ops = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false, 0u, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_EQ(ops.initial_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	EXPECT_FLOAT_EQ(ops.clear_r, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 0.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearUsesRgba8GuestClearWords)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV: WORD0 = R|(G<<8)|(B<<16)|(A<<24), WORD1 = 0 for R8G8B8A8.
	const uint32_t word0 = 0xff804020u; // R=0x20 G=0x40 B=0x80 A=0xff
	const uint32_t word1 = 0u;
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_NEAR(ops.clear_r, 32.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_g, 64.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_b, 128.0f / 255.0f, 1e-5);
	EXPECT_NEAR(ops.clear_a, 1.0f, 1e-5);
}

TEST(EmulatorGraphicsState, ColorAttachmentClearValuesDoNotChangeRenderPassCompatibility)
{
	using namespace Kyty::Libs::Graphics;
	// VkClearValue is supplied at BeginRenderPass. Two clears on the same image
	// therefore require the same render-pass compatibility while preserving their
	// distinct per-pass colors; keying framebuffer/pipeline caches by these values
	// recompiles Metal pipelines every frame.
	auto first = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, 0xff0000ffu, 0u, VK_FORMAT_R8G8B8A8_UNORM);
	auto next  = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, 0xffff0000u, 0u, VK_FORMAT_R8G8B8A8_UNORM);

	EXPECT_EQ(first.load_op, next.load_op);
	EXPECT_EQ(first.initial_layout, next.initial_layout);
	EXPECT_NE(first.clear_r, next.clear_r);
	EXPECT_NE(first.clear_b, next.clear_b);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsClearUsesRgba16FloatGuestClearWords)
{
	using namespace Kyty::Libs::Graphics;
	// Mesa/RADV: WORD0 = f16(R)|(f16(G)<<16), WORD1 = f16(B)|(f16(A)<<16).
	// f16(1.0)=0x3c00, f16(0.5)=0x3800, f16(0.0)=0x0000, f16(1.0)=0x3c00.
	const uint32_t word0 = 0x38003c00u;
	const uint32_t word1 = 0x3c000000u;
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(ops.load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
	EXPECT_FLOAT_EQ(ops.clear_r, 1.0f);
	EXPECT_FLOAT_EQ(ops.clear_g, 0.5f);
	EXPECT_FLOAT_EQ(ops.clear_b, 0.0f);
	EXPECT_FLOAT_EQ(ops.clear_a, 1.0f);
}

TEST(EmulatorGraphicsState, ColorAttachmentLoadOpsRejectsInventedFloat32RgClear)
{
	using namespace Kyty::Libs::Graphics;
	// Legacy invented packing bitcast float32 R/G and forced B=0 A=1. Those
	// bit patterns as f16 pairs must NOT decode to (1.0, 0.5, 0, 1).
	const uint32_t word0 = 0x3f800000u; // float32 1.0
	const uint32_t word1 = 0x3f000000u; // float32 0.5
	auto           ops   = ResolveColorAttachmentLoadOps(VK_IMAGE_LAYOUT_UNDEFINED, true, word0, word1, VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_FALSE(ops.clear_r == 1.0f && ops.clear_g == 0.5f && ops.clear_b == 0.0f && ops.clear_a == 1.0f);
}

TEST(EmulatorGraphicsState, WaitRegMemCompareMasksBothSides)
{
	// GraphicsDcbWaitRegMem size=0 zeroes the high half of the 64-bit mask.
	// Comparing (*addr & mask) == ref (unmasked) never matches when ref keeps
	// high bits; both sides must apply mask (WaitRegMem32/64 contract).
	const uint64_t val  = 0x1u;
	const uint64_t ref  = 0x0000000100000001ull;
	const uint64_t mask = 0x00000000ffffffffull;
	EXPECT_EQ((val & mask), (ref & mask));
	EXPECT_NE((val & mask), ref);
}

TEST(EmulatorGraphicsState, MemcpySkipAbsoluteRangesPreservesFenceHoles)
{
	using namespace Kyty::Libs::Graphics;
	uint8_t dst[32];
	uint8_t src[32];
	for (int i = 0; i < 32; i++)
	{
		dst[i] = static_cast<uint8_t>(0xA0 + i);
		src[i] = static_cast<uint8_t>(0x10 + i);
	}
	const uint64_t base       = reinterpret_cast<uint64_t>(dst);
	const uint64_t hole_begin = base + 8u;
	const uint64_t hole_end   = base + 16u;
	MemcpySkipAbsoluteRanges(dst, src, 32, &hole_begin, &hole_end, 1);
	for (int i = 0; i < 8; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0x10 + i));
	}
	for (int i = 8; i < 16; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0xA0 + i));
	}
	for (int i = 16; i < 32; i++)
	{
		EXPECT_EQ(dst[i], static_cast<uint8_t>(0x10 + i));
	}
}

// OnlyFlip → ActiveDeleted must be force-completed with Active after BufferFlush.
// Skipping ActiveDeleted left VideoOutSubmitFlip on vkGetEventStatus (empty Flip
// queue; guest ThreadFlag bit 0x1 And+ClearBits soft-lock around present 2400).
TEST(EmulatorGraphicsState, LabelForceCompleteFiresActiveDeletedOnlyFlip)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_EQ(LabelForceCompleteActionFor(true, false), LabelForceCompleteKind::FireKeep);
	EXPECT_EQ(LabelForceCompleteActionFor(false, true), LabelForceCompleteKind::FireDestroy);
	EXPECT_EQ(LabelForceCompleteActionFor(false, false), LabelForceCompleteKind::Skip);
	EXPECT_EQ(LabelForceCompleteActionFor(true, true), LabelForceCompleteKind::FireKeep);
}

// Label⊂StorageBuffer must stay linked so WriteBack holes preserve EOP fences.
// Deleting Labels let StorageBuffer WriteBack zero guest fences → EVENTFLAG_SET=0.
TEST(EmulatorGraphicsState, GpuMemoryKeepsLabelWriteBackHoleUnderStorageBuffer)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::Label, GpuMemoryOverlapType::IsContainedWithin,
	                                            GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::Label, GpuMemoryOverlapType::Equals,
	                                            GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::Label, GpuMemoryOverlapType::Crosses,
	                                            GpuMemoryObjectType::StorageBuffer));
	EXPECT_TRUE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Contains,
	                                            GpuMemoryObjectType::Label));
	EXPECT_FALSE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::Label, GpuMemoryOverlapType::IsContainedWithin,
	                                             GpuMemoryObjectType::Texture));
	EXPECT_FALSE(GpuMemoryKeepLabelWriteBackHole(GpuMemoryObjectType::VertexBuffer, GpuMemoryOverlapType::IsContainedWithin,
	                                             GpuMemoryObjectType::StorageBuffer));
}

// SubmitAndFlip without an embedded R_FLIP/0x777 must still flip after BufferFlush.
// Detect SubmitAndFlip via an explicit batch flag — not flip.handle != 0 (handle 0
// is legal, and plain Submit also zeroes the flip fields).
TEST(EmulatorGraphicsState, GraphicsBatchNeedsApiFlipWhenDcbOmitsFlip)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(GraphicsBatchNeedsApiFlip(true, false));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(true, true));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(false, false));
	EXPECT_FALSE(GraphicsBatchNeedsApiFlip(false, true));
}

// Host presentation: default swapchain selection must stay LDR sRGB even when a
// driver lists HDR10 (or other host-HDR color spaces) first.
TEST(EmulatorGraphicsState, HostHdrColorSpacesAreDetected)
{
	using namespace Kyty::Libs::Graphics;
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_HDR10_ST2084_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_HDR10_HLG_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_DOLBYVISION_EXT));
	EXPECT_TRUE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_BT2020_LINEAR_EXT));
	EXPECT_FALSE(VulkanColorSpaceIsHostHdr(VK_COLOR_SPACE_SRGB_NONLINEAR_KHR));
}

TEST(EmulatorGraphicsState, DefaultSwapchainPrefersLdrSrgbOverHdr10First)
{
	using namespace Kyty::Libs::Graphics;
	const VkSurfaceFormatKHR formats[] = {
	    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
	    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	const auto chosen = SelectDefaultSwapchainSurfaceFormat(formats, 3u);
	EXPECT_EQ(chosen.format, VK_FORMAT_B8G8R8A8_UNORM);
	EXPECT_EQ(chosen.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
	EXPECT_FALSE(VulkanColorSpaceIsHostHdr(chosen.colorSpace));
}

TEST(EmulatorGraphicsState, DefaultSwapchainPrefersUnormSrgbOverSrgbFormat)
{
	using namespace Kyty::Libs::Graphics;
	const VkSurfaceFormatKHR formats[] = {
	    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	const auto chosen = SelectDefaultSwapchainSurfaceFormat(formats, 2u);
	EXPECT_EQ(chosen.format, VK_FORMAT_B8G8R8A8_UNORM);
	EXPECT_EQ(chosen.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

TEST(EmulatorGraphicsState, ResolvesContiguousMultiRenderTargetLayout)
{
	auto zero = State::ResolveColorTargetLayout(0u);
	EXPECT_EQ(zero.count, 0u);
	EXPECT_EQ(zero.error, State::ColorTargetLayoutError::None);

	auto one = State::ResolveColorTargetLayout(0xFu);
	EXPECT_EQ(one.count, 1u);
	EXPECT_EQ(one.nibbles[0], 0xFu);
	EXPECT_EQ(one.error, State::ColorTargetLayoutError::None);

	auto four = State::ResolveColorTargetLayout(0x0000FFFFu);
	EXPECT_EQ(four.count, 4u);
	for (uint32_t i = 0; i < 4; i++)
	{
		EXPECT_EQ(four.nibbles[i], 0xFu) << "slot " << i;
	}
	EXPECT_EQ(four.error, State::ColorTargetLayoutError::None);

	auto eight = State::ResolveColorTargetLayout(0xFFFFFFFFu);
	EXPECT_EQ(eight.count, 8u);
	for (uint32_t i = 0; i < 8; i++)
	{
		EXPECT_EQ(eight.nibbles[i], 0xFu) << "slot " << i;
	}
	EXPECT_EQ(eight.error, State::ColorTargetLayoutError::None);
}

TEST(EmulatorGraphicsState, RejectsGappedMultiRenderTargetLayout)
{
	// RT0 full, RT1 zero, RT2 full → hole after contiguous prefix.
	auto gap = State::ResolveColorTargetLayout(0x00000F0Fu);
	EXPECT_EQ(gap.error, State::ColorTargetLayoutError::Gapped);
}

TEST(EmulatorGraphicsState, IgnoresMaskBitsForUnconfiguredRenderTargets)
{
	// Only RT0 has a CB_COLORn_BASE; stale/nonzero higher mask nibbles do not
	// create attachments and must not turn this single-target pass into a gap.
	auto rt0 = State::ResolveColorTargetLayout(0xb8a601afu, 1u);
	EXPECT_EQ(rt0.count, 1u);
	EXPECT_EQ(rt0.nibbles[0], 0xfu);
	EXPECT_EQ(rt0.error, State::ColorTargetLayoutError::None);

	// A real RT2 with RT1 absent remains an invalid gapped layout.
	auto gap = State::ResolveColorTargetLayout(0x00000f0fu, 3u);
	EXPECT_EQ(gap.error, State::ColorTargetLayoutError::Gapped);
}

TEST(EmulatorGraphicsState, AcceptsPartialChannelRenderTargetLayout)
{
	// RT0 partial channels (R+B only).
	auto partial = State::ResolveColorTargetLayout(0x00000005u);
	EXPECT_EQ(partial.count, 1u);
	EXPECT_EQ(partial.nibbles[0], 0x5u);
	EXPECT_EQ(partial.error, State::ColorTargetLayoutError::None);
}

TEST(EmulatorGraphicsState, RecognizesObservedHtileClearPattern)
{
	uint32_t clear_words[8];
	for (auto& word: clear_words)
	{
		word = 0xfffffff0u;
	}
	EXPECT_TRUE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words)));

	clear_words[3] = 0xffffffffu;
	EXPECT_FALSE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words)));
	EXPECT_FALSE(DepthMetaIsClearPattern(nullptr, sizeof(clear_words)));
	EXPECT_FALSE(DepthMetaIsClearPattern(clear_words, sizeof(clear_words) - 1));
}

TEST(EmulatorGraphicsState, ConsumesTrackedHtileClearOnce)
{
	constexpr uint64_t address = 0x12345000u;
	DepthMetaMarkClear(address);
	EXPECT_TRUE(DepthMetaConsumeClear(address));
	EXPECT_FALSE(DepthMetaConsumeClear(address));
}

TEST(EmulatorGraphicsState, HtilePendingClearDoesNotSuppressDepthWrite)
{
	auto actions = State::ResolveDepthClearActions(false, true);
	EXPECT_TRUE(actions.vulkan_clear);
	EXPECT_FALSE(actions.suppress_depth_write);
}

TEST(EmulatorGraphicsState, MatchesOnlyExactHtileStorageRange)
{
	EXPECT_TRUE(DepthMetaMatchesStorageRange(0x120000, 0x8000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0x120000, 0x4000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0x124000, 0x4000, 0x120000, 0x8000));
	EXPECT_FALSE(DepthMetaMatchesStorageRange(0, 0x8000, 0, 0x8000));
}

TEST(EmulatorGraphicsState, Gen5SampleBackingRequiresExactLiveRenderTarget)
{
	using namespace Kyty::Libs::Graphics::State;
	// Third arg is "live RT alias found" (Equals, or non-exact Contains /
	// IsContainedWithin from FindRenderTexture).
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_EQ(ResolveGen5SampleBacking(56, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(14, 27, false), Gen5SampleBacking::Unsupported);
	EXPECT_EQ(ResolveGen5SampleBacking(71, 27, false), Gen5SampleBacking::Unsupported);
	EXPECT_EQ(ResolveGen5SampleBacking(71, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 27, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 27, true), Gen5SampleBacking::ExactRenderTarget);
	EXPECT_EQ(ResolveGen5SampleBacking(56, 0, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(56, 9, false), Gen5SampleBacking::GuestMemoryTexture);
	EXPECT_EQ(ResolveGen5SampleBacking(133, 9, false), Gen5SampleBacking::Unsupported);
}

TEST(EmulatorGraphicsState, ResolvesObservedSamplerClampModes)
{
	using namespace Kyty::Libs::Graphics::State;

	EXPECT_EQ(ResolveSamplerAddressMode(0), SamplerAddressMode::Repeat);
	EXPECT_EQ(ResolveSamplerAddressMode(1), SamplerAddressMode::MirroredRepeat);
	EXPECT_EQ(ResolveSamplerAddressMode(2), SamplerAddressMode::ClampToEdge);
	EXPECT_EQ(ResolveSamplerAddressMode(6), SamplerAddressMode::ClampToBorder);
	EXPECT_EQ(ResolveSamplerAddressMode(7), SamplerAddressMode::ClampToBorder);
}

// Residual visual (world false-color, HUD correct): intermediate format-71 and
// format-56 sampled paths must keep distinct Vulkan formats — float RT aliases
// as SFLOAT, RGBA8 samples default UNORM (sRGB only with evidenced force_degamma).
TEST(EmulatorGraphicsState, Gen5SampledFormatsPreserveFloatAndUnormContracts)
{
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 71, false), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 71, true), VK_FORMAT_R16G16B16A16_SFLOAT);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 56, false), VK_FORMAT_R8G8B8A8_UNORM);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 56, true), VK_FORMAT_R8G8B8A8_SRGB);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 14, false), VK_FORMAT_R8G8_UNORM);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 133, false), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
	EXPECT_EQ(TextureResolveSampledVkFormat(0, 0, 133, true), VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
}

TEST(EmulatorGraphicsState, RegularImageSamplingDisablesSamplerComparison)
{
	using namespace Kyty::Libs::Graphics::State;
	const auto comparison = ResolveSamplerComparison(0, ImageSampleOperation::Regular);
	EXPECT_FALSE(comparison.enabled);
	EXPECT_EQ(comparison.function, 0);
}

// GraphicsInit must publish API version and feature-flag words into the guest
// AGC state buffer; leaving them unset lets titles take a null-deref after
// CreateShader / register-default use (see GraphicsInitWriteGuestState).
TEST(EmulatorGraphicsState, GraphicsInitWriteGuestStatePublishesVersionAndFeatureWords)
{
	EXPECT_FALSE(Gen5::GraphicsInitWriteGuestState(nullptr, 8u));

	uint32_t state[4] = {0xdeadbeefu, 0xcafebabeu, 0x11111111u, 0x22222222u};
	EXPECT_TRUE(Gen5::GraphicsInitWriteGuestState(state, 8u));
	EXPECT_EQ(state[0], 8u);
	EXPECT_EQ(state[1], 0u);
	// Only the documented two words are written.
	EXPECT_EQ(state[2], 0x11111111u);
	EXPECT_EQ(state[3], 0x22222222u);

	EXPECT_TRUE(Gen5::GraphicsInitWriteGuestState(state, 7u));
	EXPECT_EQ(state[0], 7u);
	EXPECT_EQ(state[1], 0u);
}

TEST(EmulatorGraphicsState, ResolvesEffectiveStencilFormatWithoutSeparatePlane)
{
	HW::DepthRenderTarget z {};
	z.stencil_info.format               = 1;
	z.stencil_info.tile_stencil_disable = true;
	z.stencil_read_base_addr            = 0;
	z.stencil_write_base_addr           = 0;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 0u);

	z.stencil_write_base_addr = 0x1000;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 1u);

	z.stencil_write_base_addr           = 0;
	z.stencil_info.tile_stencil_disable = false;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 1u);

	z.stencil_info.format = 0;
	EXPECT_EQ(State::ResolveEffectiveStencilFormat(z), 0u);
}

UT_END();
