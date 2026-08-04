#include "Kyty/Core/Language.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(CoreLanguage);

namespace Language = Core::Language;

namespace {

class LanguageRegistryRestore final
{
public:
	~LanguageRegistryRestore()
	{
		Language::Init();
	}
};

void ExpectRegistryContents()
{
	EXPECT_EQ(Language::GetId(U"de"), Core::LanguageId::German);
	EXPECT_EQ(Language::GetId(U"en"), Core::LanguageId::English);
	EXPECT_EQ(Language::GetId(U"zz"), Core::LanguageId::Unknown);
	EXPECT_EQ(Language::GetLanguages().Size(), 7u);
	EXPECT_EQ(Language::GetNameOfMonth(1, Core::LanguageId::English), U"January");
	EXPECT_EQ(Language::GetNameOfMonth(1, Core::LanguageId::Russian), U"Январь");
}

} // namespace

TEST(CoreLanguage, ShutdownClearsRegistryAcrossRestarts)
{
	LanguageRegistryRestore restore;

	// Repeated calls intentionally verify that both lifecycle operations are idempotent.
	Language::Shutdown();
	Language::Shutdown();
	Language::Init();
	Language::Init();
	ExpectRegistryContents();

	Language::Shutdown();
	Language::Init();
	Language::Init();
	ExpectRegistryContents();
	Language::Shutdown();
	Language::Shutdown();
}

UT_END();
