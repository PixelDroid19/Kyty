#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Np.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/Core/VirtualMemory.h"

#include <algorithm>
#include <array>
#include <cstdint>

UT_BEGIN(EmulatorNpTrophy2);

namespace {

Loader::SymbolResolve ResolveFor(const char16_t* nid, Loader::SymbolType type)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"NpTrophy2";
	query.library_version      = 1;
	query.module               = U"NpTrophy2";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = type;
	return query;
}

using CreateHandleFn  = int (*)(int32_t* handle);
using CreateContextFn = int (*)(int32_t* context, int32_t user_id, uint32_t service_label, uint64_t options);

} // namespace

// sceNpTrophy2CreateHandle (NID Gz1rmUZpROM): int*(int32_t* handle) — a valid
// pointer receives a positive opaque handle and OK; null fails with the
// trophy-specific invalid-argument code instead of faulting the guest.
TEST(EmulatorNpTrophy2, CreateHandleWritesOpaqueHandle)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpTrophy2_1", &symbols));

	const auto* record = symbols.Find(ResolveFor(u"Gz1rmUZpROM", Loader::SymbolType::Func));
	ASSERT_NE(record, nullptr);
	auto* create_handle = reinterpret_cast<CreateHandleFn>(record->vaddr);
	ASSERT_NE(create_handle, nullptr);

	EXPECT_EQ(create_handle(nullptr), static_cast<int>(0x80551604u));

	int32_t handle_a = 0;
	int32_t handle_b = 0;
	EXPECT_EQ(create_handle(&handle_a), 0);
	EXPECT_GT(handle_a, 0);
	EXPECT_EQ(create_handle(&handle_b), 0);
	EXPECT_NE(handle_a, handle_b);
}

// sceNpTrophy2CreateContext (NID Bagshr7OQ6Q): int*(int32_t*, int, uint32,
// uint64) — the canonical public NID. The alternate Fbshr7OQ6Q identity also
// resolves to the same handler; both must agree on the out-context contract.
TEST(EmulatorNpTrophy2, CreateContextAgreesOnCanonicalAndAliasNid)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpTrophy2_1", &symbols));

	const auto* canonical = symbols.Find(ResolveFor(u"Bagshr7OQ6Q", Loader::SymbolType::Func));
	ASSERT_NE(canonical, nullptr);
	const auto* alias = symbols.Find(ResolveFor(u"Fbshr7OQ6Q", Loader::SymbolType::Func));
	ASSERT_NE(alias, nullptr);
	EXPECT_EQ(canonical->vaddr, alias->vaddr);

	auto* create_context = reinterpret_cast<CreateContextFn>(canonical->vaddr);
	ASSERT_NE(create_context, nullptr);

	EXPECT_EQ(create_context(nullptr, 1, 0, 0), static_cast<int>(0x80551604u));
	// Non-zero options are not accepted.
	int32_t context = 0;
	EXPECT_EQ(create_context(&context, 1, 0, 1), static_cast<int>(0x80551604u));

	EXPECT_EQ(create_context(&context, 1, 0, 0), 0);
	EXPECT_GT(context, 0);
}

TEST(EmulatorNpTrophy2, GameInfoUsesFourArgumentAbiAndInitializesEveryOutputByte)
{
	const uint64_t page = Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(page, 0u);
	auto* details = reinterpret_cast<uint8_t*>(page);
	auto* data    = details + 152;
	std::fill_n(details, 152, uint8_t {0xcd});
	std::fill_n(data, 24, uint8_t {0xcd});

	EXPECT_EQ(Libs::NpTrophy2::GetGameInfo(1, 1, details, data), 0);
	EXPECT_TRUE(std::all_of(details, details + 152, [](uint8_t value) { return value == 0; }));
	EXPECT_TRUE(std::all_of(data, data + 24, [](uint8_t value) { return value == 0; }));
	EXPECT_TRUE(Core::VirtualMemory::Free(page));
}

TEST(EmulatorNpTrophy2, TrophyInfoUsesFiveArgumentAbiAndPublishesTheRequestedId)
{
	const uint64_t page = Core::VirtualMemory::Alloc(0, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(page, 0u);
	auto* details = reinterpret_cast<uint8_t*>(page);
	auto* data    = details + 1312;
	std::fill_n(details, 1312, uint8_t {0xcd});
	std::fill_n(data, 32, uint8_t {0xcd});

	constexpr int32_t trophy_id = 7;
	EXPECT_EQ(Libs::NpTrophy2::GetTrophyInfo(1, 1, trophy_id, details, data), 0);
	EXPECT_EQ(*reinterpret_cast<const int32_t*>(details), trophy_id);
	EXPECT_EQ(*reinterpret_cast<const int32_t*>(data), trophy_id);
	EXPECT_TRUE(std::all_of(details + sizeof(trophy_id), details + 1312, [](uint8_t value) { return value == 0; }));
	EXPECT_TRUE(std::all_of(data + sizeof(trophy_id), data + 32, [](uint8_t value) { return value == 0; }));
	EXPECT_TRUE(Core::VirtualMemory::Free(page));
}

UT_END();
