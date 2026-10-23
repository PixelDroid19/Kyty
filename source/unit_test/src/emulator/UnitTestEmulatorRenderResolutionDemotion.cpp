#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/RenderResolutionDemotion.h"

#include <cstdint>
#include <memory>

UT_BEGIN(EmulatorRenderResolutionDemotion);

using namespace Libs::Graphics;

TEST(EmulatorRenderResolutionDemotion, ReportsOverlappingRangesOnly)
{
	RenderResolutionDemotionSet set;
	set.Demote(0x10000, 0x10000);

	EXPECT_TRUE(set.Demoted(0x10000, 0x10000));
	EXPECT_TRUE(set.Demoted(0x18000, 0x10000));
	EXPECT_TRUE(set.Demoted(0x0, 0x10001));
	EXPECT_FALSE(set.Demoted(0x0, 0x10000));
	EXPECT_FALSE(set.Demoted(0x20000, 0x10000));
	EXPECT_FALSE(set.Demoted(0x10000, 0x0));
}

TEST(EmulatorRenderResolutionDemotion, IgnoresEmptyAndCoveredRanges)
{
	auto set = std::make_unique<RenderResolutionDemotionSet>();
	set->Demote(0x10000, 0x0);
	EXPECT_FALSE(set->Demoted(0x10000, 0x10000));

	set->Demote(0x10000, 0x20000);
	for (uint32_t index = 0; index < RenderResolutionDemotionSet::RANGES_MAX * 2; index++)
	{
		set->Demote(0x18000, 0x1000);
	}
	EXPECT_FALSE(set->Demoted(0x40000, 0x10000));
}

TEST(EmulatorRenderResolutionDemotion, SaturatesWhenFull)
{
	auto set = std::make_unique<RenderResolutionDemotionSet>();
	for (uint64_t index = 0; index < RenderResolutionDemotionSet::RANGES_MAX; index++)
	{
		set->Demote(index * 0x20000, 0x10000);
	}
	EXPECT_FALSE(set->Demoted(0x10000, 0x10000));

	set->Demote(RenderResolutionDemotionSet::RANGES_MAX * 0x20000ull, 0x10000);
	EXPECT_TRUE(set->Demoted(0x10000, 0x10000));
}

UT_END();
