#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include <cstddef>
#include <cstdint>

UT_BEGIN(EmulatorHttp2);

namespace {

Loader::SymbolResolve ResolveFor(const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"Http2";
	query.library_version      = 1;
	query.module               = U"Http2";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

template <typename T>
T Resolve(Loader::SymbolDatabase* symbols, const char16_t* nid)
{
	const auto* record = symbols->Find(ResolveFor(nid));
	return record == nullptr ? nullptr : reinterpret_cast<T>(record->vaddr);
}

using InitFn            = int (*)(int, int, size_t, int);
using CreateTemplateFn  = int (*)(int, const char*, int, int);
using SetAuthEnabledFn  = int (*)(int, int);

} // namespace

TEST(EmulatorHttp2, AuthOptionValidatesAndUpdatesOwnedIds)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	auto* init            = Resolve<InitFn>(&symbols, u"3JCe3lCbQ8A");
	auto* create_template = Resolve<CreateTemplateFn>(&symbols, u"+wCt7fCijgk");
	auto* set_auth        = Resolve<SetAuthEnabledFn>(&symbols, u"jjFahkBPCYs");
	ASSERT_NE(init, nullptr);
	ASSERT_NE(create_template, nullptr);
	ASSERT_NE(set_auth, nullptr);

	EXPECT_EQ(set_auth(0x7fffffff, 1), static_cast<int>(0x817b1100u));
	const int context_id = init(1, 1, 4096, 4);
	ASSERT_GT(context_id, 0);
	const int template_id = create_template(context_id, "Kyty", 1, 0);
	ASSERT_GT(template_id, 0);
	EXPECT_EQ(set_auth(template_id, 1), 0);
	EXPECT_EQ(set_auth(template_id, 0), 0);
}

UT_END();
