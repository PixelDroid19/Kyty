#include "Kyty/UnitTest.h"

#include "Emulator/Libs/CxxLocale.h"

UT_BEGIN(EmulatorLibcCxxLocale);

using Kyty::Libs::LibC::CxxLocaleLayout;
using Kyty::Libs::LibC::CxxLocimpFacetLookupOk;
using Kyty::Libs::LibC::CxxLocimpLayout;
using Kyty::Libs::LibC::kCxxCtypeCharId;
using Kyty::Libs::LibC::kCxxLocimpFacetCount;

TEST(EmulatorLibcCxxLocale, ClassicLocimpFacetLookupMatchesGuestUseFacetLayout)
{
	void* facets[kCxxLocimpFacetCount] = {nullptr, reinterpret_cast<void*>(0x1)};
	CxxLocimpLayout locimp {};
	locimp.facet_vec   = facets;
	locimp.facet_count = kCxxLocimpFacetCount;

	EXPECT_TRUE(CxxLocimpFacetLookupOk(locimp, kCxxCtypeCharId));
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, 0));
	facets[1] = nullptr;
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, kCxxCtypeCharId));

	CxxLocaleLayout locale {};
	locale.ptr = &locimp;
	EXPECT_EQ(locale.ptr, &locimp);
}

UT_END();
