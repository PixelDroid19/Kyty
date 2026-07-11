#include "Emulator/Graphics/GraphicsState.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/VideoOut.h"
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

TEST(EmulatorGraphicsState, AllowsOnlyObservedTextureStorageAliases)
{
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::StorageBuffer, GpuMemoryOverlapType::Equals,
	                                               GpuMemoryObjectType::Texture));
	EXPECT_TRUE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                               GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::IsContainedWithin,
	                                                GpuMemoryObjectType::StorageBuffer));
	EXPECT_FALSE(GpuMemoryAllowsTextureStorageAlias(GpuMemoryObjectType::Texture, GpuMemoryOverlapType::Contains,
	                                                GpuMemoryObjectType::RenderTexture));
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

UT_END();
