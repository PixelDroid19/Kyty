#include "Kyty/UnitTest.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

UT_BEGIN(EmulatorPrimitiveState);

using namespace Libs::Graphics;

namespace {

namespace VM = Core::VirtualMemory;

constexpr uint32_t kGsEnable = 1u << 5u;
constexpr uint32_t kHsEnable = 1u << 2u;
constexpr uint32_t kVertexStages = (1u << 25u) | (1u << 13u);
constexpr uint32_t kHullStages = kHsEnable | 1u;
constexpr uint32_t kGsControl = 0x13572468u;
constexpr uint32_t kGsUser = 0x125u;
constexpr uint32_t kHsUser = 0x248u;
#if defined(_WIN32)
constexpr int kRejectedExit = 321;
#else
constexpr int kRejectedExit = 65;
#endif

// No process-lifetime emulator initialization in the parent, including repeats.
class ScopedDeathTestStyle final
{
public:
	ScopedDeathTestStyle(): m_previous(::testing::FLAGS_gtest_death_test_style)
	{
		::testing::FLAGS_gtest_death_test_style = "threadsafe";
	}
	~ScopedDeathTestStyle() { ::testing::FLAGS_gtest_death_test_style = m_previous; }

private:
	std::string m_previous;
};

void InitChildRuntime()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

void RequireSetup(bool ok)
{
	if (!ok)
	{
		std::fputs("primitive-state fixture setup failed\n", stderr);
		std::_Exit(90);
	}
}

class GuestRange final
{
public:
	GuestRange(): page(VM::GetPageSize()), address(VM::Alloc(0, page * 2u, VM::Mode::ReadWrite))
	{
		RequireSetup(page >= 512u && address != 0u);
		RequireSetup(VM::Protect(address + page, page, VM::Mode::NoAccess));
	}
	~GuestRange() { RequireSetup(VM::Free(address)); }
	GuestRange(const GuestRange&) = delete;
	GuestRange& operator=(const GuestRange&) = delete;
	void Protect(VM::Mode mode) const { RequireSetup(VM::Protect(address, page, mode)); }
	[[nodiscard]] uint64_t End() const { return address + page; }

	const uint64_t page;
	const uint64_t address;
};

template <typename T> void Store(uint64_t address, const T& object)
{
	RequireSetup(VM::CopyToGuest(address, &object, sizeof(object)));
}

template <typename T> T Load(uint64_t address)
{
	T object {};
	RequireSetup(VM::CopyFromGuest(&object, address, sizeof(object)));
	return object;
}

using CxWindow = std::array<ShaderRegister, 4>; // canary, two pairs, canary
using UcWindow = std::array<ShaderRegister, 5>; // canary, three pairs, canary

template <size_t N> std::array<ShaderRegister, N> Sentinels()
{
	std::array<ShaderRegister, N> result {};
	for (size_t i = 0; i < N; ++i)
	{
		result[i] = {0xabc00000u + static_cast<uint32_t>(i), 0xdef00000u + static_cast<uint32_t>(i)};
	}
	return result;
}

struct Fixture
{
	GuestRange gs_memory;
	GuestRange hs_memory;
	GuestRange gs_special_memory;
	GuestRange hs_special_memory;
	GuestRange cx_memory;
	GuestRange uc_memory;
	Shader gs {};
	Shader hs {};
	ShaderSpecialRegs gs_specials {};
	ShaderSpecialRegs hs_specials {};

	Fixture()
	{
		gs.type = 2;
		hs.type = 3;
		gs.special_sizes_bytes = hs.special_sizes_bytes = static_cast<uint16_t>(sizeof(ShaderSpecialRegs));
		gs.specials = reinterpret_cast<ShaderSpecialRegs*>(gs_special_memory.address);
		hs.specials = reinterpret_cast<ShaderSpecialRegs*>(hs_special_memory.address);
		gs_specials.vgt_shader_stages_en = {Pm4::VGT_SHADER_STAGES_EN, kVertexStages};
		gs_specials.vgt_gs_out_prim_type = {Pm4::VGT_GS_OUT_PRIM_TYPE, 4u};
		gs_specials.ge_cntl = {Pm4::GE_CNTL, kGsControl};
		gs_specials.ge_user_vgpr_en = {Pm4::GE_USER_VGPR_EN, kGsUser};
		hs_specials.vgt_shader_stages_en = {Pm4::VGT_SHADER_STAGES_EN, kHullStages};
		hs_specials.vgt_gs_out_prim_type = {Pm4::VGT_GS_OUT_PRIM_TYPE, 1u};
		hs_specials.ge_cntl = {Pm4::GE_CNTL, 0xdeadbeefu};
		hs_specials.ge_user_vgpr_en = {Pm4::GE_USER_VGPR_EN, kHsUser};
	}

	void Publish() const
	{
		Store(gs_memory.address, gs);
		Store(hs_memory.address, hs);
		Store(gs_special_memory.address, gs_specials);
		Store(hs_special_memory.address, hs_specials);
		Store(cx_memory.address, Sentinels<4>());
		Store(uc_memory.address, Sentinels<5>());
	}
	[[nodiscard]] const Shader* Gs() const { return reinterpret_cast<const Shader*>(gs_memory.address); }
	[[nodiscard]] const Shader* Hs() const { return reinterpret_cast<const Shader*>(hs_memory.address); }
	[[nodiscard]] ShaderRegister* Cx() const
	{
		return reinterpret_cast<ShaderRegister*>(cx_memory.address + sizeof(ShaderRegister));
	}
	[[nodiscard]] ShaderRegister* Uc() const
	{
		return reinterpret_cast<ShaderRegister*>(uc_memory.address + sizeof(ShaderRegister));
	}
};

void ExpectPair(const ShaderRegister& pair, uint32_t offset, uint32_t value)
{
	EXPECT_EQ(pair.offset, offset);
	EXPECT_EQ(pair.value, value);
}

void ExpectOutputs(const Fixture& f, uint32_t input, uint32_t stages, uint32_t output, uint32_t user,
                   bool has_cx = true, bool has_uc = true)
{
	auto cx_expected = Sentinels<4>();
	auto uc_expected = Sentinels<5>();
	if (has_cx)
	{
		cx_expected[1] = {Pm4::VGT_SHADER_STAGES_EN, stages};
		cx_expected[2] = {Pm4::VGT_GS_OUT_PRIM_TYPE, output};
	}
	if (has_uc)
	{
		uc_expected[1] = {Pm4::GE_CNTL, kGsControl};
		uc_expected[2] = {Pm4::GE_USER_VGPR_EN, user};
		uc_expected[3] = {Pm4::VGT_PRIMITIVE_TYPE, input};
	}
	const auto cx_actual = Load<CxWindow>(f.cx_memory.address);
	const auto uc_actual = Load<UcWindow>(f.uc_memory.address);
	for (size_t i = 0; i < cx_actual.size(); ++i)
	{
		ExpectPair(cx_actual[i], cx_expected[i].offset, cx_expected[i].value);
	}
	for (size_t i = 0; i < uc_actual.size(); ++i)
	{
		ExpectPair(uc_actual[i], uc_expected[i].offset, uc_expected[i].value);
	}
}

void CheckInputSelection()
{
	Fixture f;
	for (const uint32_t stages: {kVertexStages, 0u, 1u << 13u})
	{
		f.gs_specials.vgt_shader_stages_en.value = stages;
		for (const uint32_t metadata: {0u, 4u, 0xa5a50002u})
		{
			f.gs_specials.vgt_gs_out_prim_type.value = metadata;
			f.Publish();
			ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), nullptr, f.Gs(), 7u), 0);
			ExpectOutputs(f, 7u, stages, 3u, kGsUser);
		}
	}
}

void CheckMappingTable()
{
	// Independent expectations for every supported no-owner input, not an oracle
	// that calls the production mapping helper. High metadata bits must disappear.
	constexpr std::array<std::array<uint32_t, 2>, 16> cases {{
	    {1u, 0u}, {2u, 1u}, {3u, 1u}, {4u, 2u}, {5u, 2u}, {6u, 2u}, {7u, 3u}, {10u, 1u},
	    {11u, 1u}, {12u, 2u}, {13u, 2u}, {17u, 4u}, {18u, 1u}, {19u, 2u}, {20u, 2u}, {21u, 2u}}};
	Fixture f;
	f.gs_specials.vgt_gs_out_prim_type.value = 0xfffffff4u;
	for (const auto& item: cases)
	{
		SCOPED_TRACE(item[0]);
		f.Publish();
		ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), nullptr, f.Gs(), item[0]), 0);
		ExpectOutputs(f, item[0], kVertexStages, item[1], kGsUser);
	}
}

void CheckOwners()
{
	Fixture f;
	for (const bool hull: {false, true})
	{
		for (const uint32_t output: {0u, 0xa5b60102u})
		{
			f.gs_specials.vgt_shader_stages_en.value = kVertexStages | kGsEnable;
			f.gs_specials.vgt_gs_out_prim_type.value = output;
			f.hs_specials.vgt_gs_out_prim_type.value = output ^ 0xffffffffu;
			for (const uint32_t input: {7u, 17u})
			{
				f.Publish();
				ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), hull ? f.Hs() : nullptr, f.Gs(), input), 0);
				ExpectOutputs(f, input, kVertexStages | kGsEnable | (hull ? kHullStages : 0u), output,
				              hull ? kHsUser : kGsUser);
			}
		}
	}
	for (const bool geometry: {false, true})
	{
		f.gs_specials.vgt_shader_stages_en.value = kVertexStages | (geometry ? kGsEnable : 0u);
		f.gs_specials.vgt_gs_out_prim_type.value = 0x80000102u;
		for (const uint32_t output: {0u, 0x40000001u})
		{
			f.hs_specials.vgt_gs_out_prim_type.value = output;
			for (const uint32_t input: {7u, 9u})
			{
				f.Publish();
				ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), f.Hs(), f.Gs(), input), 0);
				ExpectOutputs(f, input, kVertexStages | kHullStages | (geometry ? kGsEnable : 0u),
				              geometry ? 0x80000102u : output, kHsUser);
			}
		}
	}
}

void CheckOptionalOutputs()
{
	Fixture f;
	for (const bool hull: {false, true})
	{
		f.Publish();
		ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), nullptr, hull ? f.Hs() : nullptr, f.Gs(), 17u), 0);
		ExpectOutputs(f, 17u, kVertexStages | (hull ? kHullStages : 0u), hull ? 1u : 4u, kGsUser, true, false);
		f.Publish();
		ASSERT_EQ(Gen5::GraphicsCreatePrimState(nullptr, f.Uc(), hull ? f.Hs() : nullptr, f.Gs(), 17u), 0);
		ExpectOutputs(f, 17u, 0u, 0u, hull ? kHsUser : kGsUser, false, true);
	}
	// Both null is a no-op: no metadata dereference or primitive admission needed.
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(nullptr, nullptr, nullptr, nullptr, 0xffffffffu), 0);
}

void CheckUnselectedPairs()
{
	Fixture f;
	// The derived pair must not inherit an unowned metadata pair's offset/bits.
	f.gs_specials.vgt_gs_out_prim_type = {0xdeadbeefu, 0xfffffff0u};
	f.Publish();
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), nullptr, f.Gs(), 7u), 0);
	ExpectOutputs(f, 7u, kVertexStages, 3u, kGsUser);
	// UC-only does not publish or validate an unrequested owner output pair.
	f.gs_specials.vgt_shader_stages_en.value |= kGsEnable;
	f.Publish();
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(nullptr, f.Uc(), nullptr, f.Gs(), 7u), 0);
	ExpectOutputs(f, 7u, 0u, 0u, kGsUser, false, true);
	// CX-only does not publish UC. GS/HS stage metadata is still mandatory.
	f.gs_specials.vgt_gs_out_prim_type = {Pm4::VGT_GS_OUT_PRIM_TYPE, 0u};
	f.gs_specials.ge_cntl.offset = 0u;
	f.gs_specials.ge_user_vgpr_en.offset = 0u;
	f.Publish();
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), nullptr, nullptr, f.Gs(), 7u), 0);
	ExpectOutputs(f, 7u, kVertexStages | kGsEnable, 0u, kGsUser, true, false);
}

void CheckFixedSnapshots()
{
	Fixture f;
	// A larger declared specials tail cannot enlarge the required fixed copy.
	f.gs.special_sizes_bytes = 0xffffu;
	f.gs.specials = reinterpret_cast<ShaderSpecialRegs*>(f.gs_special_memory.End() - sizeof(ShaderSpecialRegs));
	f.Publish();
	Store(reinterpret_cast<uint64_t>(f.gs.specials), f.gs_specials);
	f.gs_memory.Protect(VM::Mode::Read);
	f.gs_special_memory.Protect(VM::Mode::Read);
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), nullptr, f.Gs(), 7u), 0);
	ExpectOutputs(f, 7u, kVertexStages, 3u, kGsUser);
}

void CheckAliasedMetadata()
{
	Fixture f;
	f.Publish();
	// CX overwrites the original GE_CNTL/stages pairs. UC must use the snapshot.
	auto* cx = reinterpret_cast<ShaderRegister*>(f.gs_special_memory.address);
	ASSERT_EQ(Gen5::GraphicsCreatePrimState(cx, f.Uc(), nullptr, f.Gs(), 7u), 0);
	const auto produced = Load<std::array<ShaderRegister, 2>>(f.gs_special_memory.address);
	ExpectPair(produced[0], Pm4::VGT_SHADER_STAGES_EN, kVertexStages);
	ExpectPair(produced[1], Pm4::VGT_GS_OUT_PRIM_TYPE, 3u);
	ExpectOutputs(f, 7u, 0u, 0u, kGsUser, false, true);
}

void CheckWrittenZero()
{
	Fixture f;
	HW::Context context;
	EXPECT_FALSE(context.GetShaderRegisters().gs_out_primitive_raw.written);
	for (const bool explicit_geometry: {false, true})
	{
		f.gs_specials.vgt_shader_stages_en.value = kVertexStages | (explicit_geometry ? kGsEnable : 0u);
		f.gs_specials.vgt_gs_out_prim_type.value = explicit_geometry ? 0u : 4u;
		f.Publish();
		ASSERT_EQ(Gen5::GraphicsCreatePrimState(f.Cx(), f.Uc(), nullptr, f.Gs(), explicit_geometry ? 7u : 1u), 0);
		const auto cx = Load<CxWindow>(f.cx_memory.address);
		ExpectPair(cx[2], Pm4::VGT_GS_OUT_PRIM_TYPE, 0u);
		context.SetGsOutPrimType(0xffffffffu);
		context.SetGsOutPrimType(cx[2].value);
		const auto& raw = context.GetShaderRegisters().gs_out_primitive_raw;
		EXPECT_EQ(raw.value, 0u);
		EXPECT_TRUE(raw.known);
		EXPECT_TRUE(raw.written);
	}
}

void CheckInChild(void (*check)())
{
	const ScopedDeathTestStyle style;
	ASSERT_EXIT(
	    {
		    InitChildRuntime();
		    check();
		    std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
	    },
	    ::testing::ExitedWithCode(0), "");
}

// Observe rejection before Core tears down mappings. A signal, unexpected
// return, setup failure, or a partial output write cannot satisfy this check.
struct Watch
{
	uint64_t address = 0;
	size_t size = 0;
	std::array<unsigned char, 64> before {};
};
std::array<Watch, 3> g_watches {};
size_t g_watch_count = 0;

void WatchOutput(uint64_t address, size_t size)
{
	RequireSetup(g_watch_count < g_watches.size() && size <= g_watches[0].before.size());
	auto& watch = g_watches[g_watch_count++];
	watch.address = address;
	watch.size = size;
	RequireSetup(VM::CopyFromGuest(watch.before.data(), address, size));
}

void ObserveRejection(const char* code, const char*) noexcept
{
	if (std::strcmp(code, "exit") != 0)
	{
		return;
	}
	for (size_t i = 0; i < g_watch_count; ++i)
	{
		const auto& watch = g_watches[i];
		std::array<unsigned char, 64> after {};
		if (!VM::CopyFromGuest(after.data(), watch.address, watch.size) ||
		    std::memcmp(after.data(), watch.before.data(), watch.size) != 0)
		{
			std::_Exit(91);
		}
	}
	std::fputs("primitive-state outputs intact\n", stderr);
	std::fflush(stderr);
}

enum class Invalid
{
	Input, GsOwnedInput, HsOwnedInput, NullGs, GsType, HsType, SmallGs, SmallHs,
	GsStageOffset, HsStageOffset, GsOutputOffset, HsOutputOffset, GeOffset, GsUserOffset, HsUserOffset,
	MissingHs, HsIntroducesGs, HsWithGsBit, HsNotEnabled,
	HostHeader, HostSpecials, NullSpecials, UnreadableGsHeader, UnreadableHsHeader,
	TruncatedGsHeader, TruncatedHsHeader, UnreadableGsSpecials, UnreadableHsSpecials,
	TruncatedGsSpecials, TruncatedHsSpecials, WrappingHeader,
	ReadOnlyCx, ReadOnlyUc, TruncatedCx, TruncatedUc, HostCx, HostUc, OverlapCxFirst, OverlapUcFirst
};

void Reject(Invalid invalid, uint32_t argument)
{
	Fixture f;
	uint32_t input = 7u;
	const Shader* gs = f.Gs();
	const Shader* hs = nullptr;
	auto* cx = f.Cx();
	auto* uc = f.Uc();
	Shader host_header = f.gs;
	ShaderSpecialRegs host_specials = f.gs_specials;
	auto host_output = Sentinels<3>();
	switch (invalid)
	{
		case Invalid::Input: input = argument; break;
		case Invalid::GsOwnedInput: input = argument; f.gs_specials.vgt_shader_stages_en.value |= kGsEnable; break;
		case Invalid::HsOwnedInput: input = argument; hs = f.Hs(); break;
		case Invalid::NullGs: gs = nullptr; break;
		case Invalid::GsType: f.gs.type = 6; break;
		case Invalid::HsType: hs = f.Hs(); f.hs.type = 2; break;
		case Invalid::SmallGs: f.gs.special_sizes_bytes = static_cast<uint16_t>(sizeof(ShaderSpecialRegs) - 1u); break;
		case Invalid::SmallHs: hs = f.Hs(); f.hs.special_sizes_bytes = static_cast<uint16_t>(sizeof(ShaderSpecialRegs) - 1u); break;
		case Invalid::GsStageOffset: f.gs_specials.vgt_shader_stages_en.offset = 0u; break;
		case Invalid::HsStageOffset: hs = f.Hs(); f.hs_specials.vgt_shader_stages_en.offset = 0u; break;
		case Invalid::GsOutputOffset:
			f.gs_specials.vgt_shader_stages_en.value |= kGsEnable;
			f.gs_specials.vgt_gs_out_prim_type.offset = 0u;
			break;
		case Invalid::HsOutputOffset: hs = f.Hs(); f.hs_specials.vgt_gs_out_prim_type.offset = 0u; break;
		case Invalid::GeOffset: f.gs_specials.ge_cntl.offset = 0u; break;
		case Invalid::GsUserOffset: f.gs_specials.ge_user_vgpr_en.offset = 0u; break;
		case Invalid::HsUserOffset: hs = f.Hs(); f.hs_specials.ge_user_vgpr_en.offset = 0u; break;
		case Invalid::MissingHs: f.gs_specials.vgt_shader_stages_en.value |= argument; break;
		case Invalid::HsIntroducesGs: hs = f.Hs(); f.hs_specials.vgt_shader_stages_en.value |= kGsEnable; break;
		case Invalid::HsWithGsBit:
			hs = f.Hs();
			f.gs_specials.vgt_shader_stages_en.value |= kGsEnable;
			f.hs_specials.vgt_shader_stages_en.value |= kGsEnable;
			break;
		case Invalid::HsNotEnabled: hs = f.Hs(); f.hs_specials.vgt_shader_stages_en.value &= ~kHsEnable; break;
		case Invalid::HostHeader: gs = &host_header; break;
		case Invalid::HostSpecials: f.gs.specials = &host_specials; break;
		case Invalid::NullSpecials: f.gs.specials = nullptr; break;
		case Invalid::UnreadableHsHeader: hs = f.Hs(); break;
		case Invalid::TruncatedGsHeader: gs = reinterpret_cast<const Shader*>(f.gs_memory.End() - sizeof(Shader) / 2u); break;
		case Invalid::TruncatedHsHeader: hs = reinterpret_cast<const Shader*>(f.hs_memory.End() - sizeof(Shader) / 2u); break;
		case Invalid::UnreadableHsSpecials: hs = f.Hs(); break;
		case Invalid::TruncatedGsSpecials:
			f.gs.specials = reinterpret_cast<ShaderSpecialRegs*>(f.gs_special_memory.End() - sizeof(ShaderSpecialRegs) / 2u);
			break;
		case Invalid::TruncatedHsSpecials:
			hs = f.Hs();
			f.hs.specials = reinterpret_cast<ShaderSpecialRegs*>(f.hs_special_memory.End() - sizeof(ShaderSpecialRegs) / 2u);
			break;
		case Invalid::WrappingHeader: gs = reinterpret_cast<const Shader*>(UINT64_MAX - sizeof(Shader) / 2u); break;
		case Invalid::TruncatedCx: cx = reinterpret_cast<ShaderRegister*>(f.cx_memory.End() - sizeof(ShaderRegister)); break;
		case Invalid::TruncatedUc: uc = reinterpret_cast<ShaderRegister*>(f.uc_memory.End() - 2u * sizeof(ShaderRegister)); break;
		case Invalid::HostCx: cx = host_output.data(); break;
		case Invalid::HostUc: uc = host_output.data(); break;
		case Invalid::OverlapCxFirst: uc = f.Cx() + 1; break;
		case Invalid::OverlapUcFirst: uc = f.Cx() - 1; break;
		case Invalid::UnreadableGsHeader: case Invalid::UnreadableGsSpecials:
		case Invalid::ReadOnlyCx: case Invalid::ReadOnlyUc: break;
	}
	f.Publish();
	switch (invalid)
	{
		case Invalid::UnreadableGsHeader: f.gs_memory.Protect(VM::Mode::NoAccess); break;
		case Invalid::UnreadableHsHeader: f.hs_memory.Protect(VM::Mode::NoAccess); break;
		case Invalid::UnreadableGsSpecials: f.gs_special_memory.Protect(VM::Mode::NoAccess); break;
		case Invalid::UnreadableHsSpecials: f.hs_special_memory.Protect(VM::Mode::NoAccess); break;
		case Invalid::ReadOnlyCx: f.cx_memory.Protect(VM::Mode::Read); break;
		case Invalid::ReadOnlyUc: f.uc_memory.Protect(VM::Mode::Read); break;
		default: break;
	}
	g_watch_count = 0;
	WatchOutput(f.cx_memory.address, sizeof(CxWindow));
	WatchOutput(f.uc_memory.address, sizeof(UcWindow));
	if (invalid == Invalid::TruncatedCx || invalid == Invalid::TruncatedUc)
	{
		const bool is_cx = invalid == Invalid::TruncatedCx;
		const auto address = reinterpret_cast<uint64_t>(is_cx ? cx : uc);
		WatchOutput(address, (is_cx ? 1u : 2u) * sizeof(ShaderRegister));
	}
	Core::SetHostFaultHook(ObserveRejection);
	Gen5::GraphicsCreatePrimState(cx, uc, hs, gs, input);
	std::_Exit(92); // Returning instead of taking strict EXIT must fail the test.
}

void ExpectRejected(Invalid invalid, uint32_t argument = 0u)
{
	const ScopedDeathTestStyle style;
	SCOPED_TRACE(static_cast<unsigned>(invalid));
	SCOPED_TRACE(argument);
	ASSERT_EXIT(
	    {
		    InitChildRuntime();
		    Reject(invalid, argument);
	    },
	    ::testing::ExitedWithCode(kRejectedExit), "primitive-state outputs intact");
}

} // namespace

TEST(EmulatorPrimitiveState, InputSelectionDoesNotDependOnMetadataZeroOrPassthroughWord)
{
	CheckInChild(CheckInputSelection);
}

TEST(EmulatorPrimitiveState, StandardInputMappingKeepsBoundingAndCornerRectanglesDistinct)
{
	CheckInChild(CheckMappingTable);
}

TEST(EmulatorPrimitiveState, OwnersPreserveFullWordsAndHullPatchWhileGsTakesPrecedence)
{
	CheckInChild(CheckOwners);
}

TEST(EmulatorPrimitiveState, OptionalOutputsAreIndependentAndBothNullIsNoOp)
{
	CheckInChild(CheckOptionalOutputs);
}

TEST(EmulatorPrimitiveState, UnselectedRegisterPairsDoNotLeakIntoOutputs)
{
	CheckInChild(CheckUnselectedPairs);
}

TEST(EmulatorPrimitiveState, FixedSnapshotsAcceptReadOnlyInputsWithoutFollowingDeclaredTails)
{
	CheckInChild(CheckFixedSnapshots);
}

TEST(EmulatorPrimitiveState, MetadataIsSnapshottedBeforeAnAliasedOutputIsPublished)
{
	CheckInChild(CheckAliasedMetadata);
}

TEST(EmulatorPrimitiveState, ExplicitAndDerivedZeroRemainKnownWrittenZero)
{
	CheckInChild(CheckWrittenZero);
}

TEST(EmulatorPrimitiveState, NoneReservedAndUnknownInputsRefuseBeforePublication)
{
	for (const uint32_t input: {0u, 8u, 14u, 15u, 16u, 22u, 0xffffffffu})
	{
		ExpectRejected(Invalid::Input, input);
		ExpectRejected(Invalid::GsOwnedInput, input);
		ExpectRejected(Invalid::HsOwnedInput, input);
	}
	ExpectRejected(Invalid::Input, 9u);
	ExpectRejected(Invalid::GsOwnedInput, 9u);
}

TEST(EmulatorPrimitiveState, MalformedFixedMetadataCannotPartiallyPublishEitherOutput)
{
	for (const auto invalid: {Invalid::NullGs, Invalid::GsType, Invalid::HsType, Invalid::SmallGs, Invalid::SmallHs,
	                         Invalid::GsStageOffset, Invalid::HsStageOffset, Invalid::GsOutputOffset, Invalid::HsOutputOffset,
	                         Invalid::GeOffset, Invalid::GsUserOffset, Invalid::HsUserOffset})
	{
		ExpectRejected(invalid);
	}
}

TEST(EmulatorPrimitiveState, InconsistentHullOwnershipRefusesBeforePublication)
{
	for (const uint32_t stages: {1u, 2u, kHsEnable, 7u})
	{
		ExpectRejected(Invalid::MissingHs, stages);
	}
	for (const auto invalid: {Invalid::HsIntroducesGs, Invalid::HsWithGsBit, Invalid::HsNotEnabled})
	{
		ExpectRejected(invalid);
	}
}

TEST(EmulatorPrimitiveState, UnownedUnreadableAndTruncatedMetadataRefuseBeforePublication)
{
	for (const auto invalid: {Invalid::HostHeader, Invalid::HostSpecials, Invalid::NullSpecials,
	                         Invalid::UnreadableGsHeader, Invalid::UnreadableHsHeader,
	                         Invalid::TruncatedGsHeader, Invalid::TruncatedHsHeader,
	                         Invalid::UnreadableGsSpecials, Invalid::UnreadableHsSpecials,
	                         Invalid::TruncatedGsSpecials, Invalid::TruncatedHsSpecials, Invalid::WrappingHeader})
	{
		ExpectRejected(invalid);
	}
}

TEST(EmulatorPrimitiveState, BothRequestedOutputRangesAreValidatedBeforeEitherWrite)
{
	for (const auto invalid: {Invalid::ReadOnlyCx, Invalid::ReadOnlyUc, Invalid::TruncatedCx, Invalid::TruncatedUc,
	                         Invalid::HostCx, Invalid::HostUc, Invalid::OverlapCxFirst, Invalid::OverlapUcFirst})
	{
		ExpectRejected(invalid);
	}
}

UT_END();
