#include <gtest/gtest.h>

#include "Emulator/Libs/CxxLocale.h"

#include <cstdint>

namespace {

using Kyty::Libs::LibC::CxxLocaleLayout;
using Kyty::Libs::LibC::CxxLocimpFacetLookupOk;
using Kyty::Libs::LibC::CxxLocimpLayout;
using Kyty::Libs::LibC::kCxxCtypeCharId;
using Kyty::Libs::LibC::kCxxLocimpFacetCount;

// Mirrors the HLE classic-locale wiring without pulling LibC.cpp symbols.
TEST(EmulatorLibcCxxLocale, ClassicLocimpFacetLookupMatchesGuestUseFacetLayout)
{
	void* facets[kCxxLocimpFacetCount] = {};
	void* ctype_stub                   = &facets; // any non-null stand-in
	facets[kCxxCtypeCharId]            = ctype_stub;

	CxxLocimpLayout locimp {};
	locimp.facet_vec   = facets;
	locimp.facet_count = kCxxLocimpFacetCount;
	locimp.name        = "C";

	CxxLocaleLayout locale {};
	locale.ptr = &locimp;

	// Guest: mov r12, &locale; mov rdi, [r12] → Locimp*.
	EXPECT_EQ(locale.ptr, &locimp);
	EXPECT_NE(locale.ptr, nullptr);

	// Guest: cmp id, count; ja only when count > id (AT&T cmp order).
	EXPECT_GT(locimp.facet_count, kCxxCtypeCharId);
	EXPECT_TRUE(CxxLocimpFacetLookupOk(locimp, kCxxCtypeCharId));
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, kCxxLocimpFacetCount));
	EXPECT_FALSE(CxxLocimpFacetLookupOk(locimp, 0)); // slot 0 empty

	// Field offsets used by Dreaming Sarah use_facet body (0x900134a80).
	EXPECT_EQ(offsetof(CxxLocimpLayout, facet_vec), 0x10u);
	EXPECT_EQ(offsetof(CxxLocimpLayout, facet_count), 0x18u);
	EXPECT_EQ(offsetof(CxxLocimpLayout, flag_24), 0x24u);
	EXPECT_EQ(offsetof(CxxLocimpLayout, name), 0x28u);
}

} // namespace
