#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstdint>

UT_BEGIN(EmulatorIme);

namespace {

using namespace Kyty;

void EnsureLog()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
}

Loader::SymbolResolve ImeFunction(const char32_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"Ime";
	query.library_version      = 1;
	query.module               = U"Ime";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

template <typename Function>
Function ResolveImeFunction(Loader::SymbolDatabase* symbols, const char32_t* nid)
{
	const auto query = ImeFunction(nid);
	const auto* rec  = symbols->Find(query);
	EXPECT_NE(rec, nullptr);
	if (rec == nullptr)
	{
		return nullptr;
	}
	EXPECT_NE(rec->vaddr, 0u);
	return reinterpret_cast<Function>(rec->vaddr);
}

} // namespace

TEST(EmulatorIme, QueriesSucceedWithoutMutatingGuestBuffers)
{
	EnsureLog();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libIme_1", &symbols));

	using GetResourceId = KYTY_SYSV_ABI int (*)(int user_id, void* resource_id_array);
	using GetInfo       = KYTY_SYSV_ABI int (*)(uint32_t resource_id, void* info);

	auto get_resource_id = ResolveImeFunction<GetResourceId>(&symbols, U"dKadqZFgKKQ");
	auto get_info        = ResolveImeFunction<GetInfo>(&symbols, U"VkqLPArfFdc");
	ASSERT_NE(get_resource_id, nullptr);
	ASSERT_NE(get_info, nullptr);

	EXPECT_EQ(get_resource_id(1, nullptr), 0);
	EXPECT_EQ(get_info(0, nullptr), 0);

	std::array<uint8_t, 24> resources {};
	resources.fill(0xa5);
	ASSERT_EQ(get_resource_id(7, resources.data()), 0);
	for (const auto value: resources)
	{
		EXPECT_EQ(value, 0xa5u);
	}

	std::array<uint8_t, 36> info {};
	info.fill(0xa5);
	ASSERT_EQ(get_info(0, info.data()), 0);
	for (const auto value: info)
	{
		EXPECT_EQ(value, 0xa5u);
	}
}

UT_END();
