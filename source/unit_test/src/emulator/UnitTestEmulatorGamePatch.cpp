#include "Kyty/Core/String.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Loader/GamePatch.h"

UT_BEGIN(EmulatorGamePatch);

using namespace Loader;

namespace {

const char32_t* const VALID_FILE = U"{\"id\": \"PPSA00000\", \"version\": \"01.000.000\", \"process\": \"eboot.bin\", \"mods\": ["
                                   U"{\"name\": \"on\", \"enabled\": true, \"memory\": ["
                                   U"{\"offset\": \"0x10\", \"off\": \"9090\", \"on\": \"C3cc\"},"
                                   U"{\"offset\": \"12000\", \"off\": \"00\", \"on\": \"ff\"}]},"
                                   U"{\"name\": \"off\", \"enabled\": false, \"memory\": [{\"offset\": \"0x20\", \"off\": \"xx\", \"on\": \"\"}]}]}";

Core::String WithFirstOn(const char32_t* value)
{
	return Core::String(VALID_FILE).ReplaceStr(U"C3cc", value);
}

} // namespace

TEST(EmulatorGamePatch, ReadsEnabledMods)
{
	GamePatchPlan plan;
	EXPECT_TRUE(GamePatchParse(VALID_FILE, &plan).IsEmpty());

	EXPECT_TRUE(plan.title_id == U"PPSA00000");
	EXPECT_TRUE(plan.version == U"01.000.000");
	EXPECT_TRUE(plan.process == U"eboot.bin");
	ASSERT_EQ(plan.mods.size(), 1u);
	EXPECT_TRUE(plan.mods[0] == U"on");
	ASSERT_EQ(plan.writes.size(), 2u);
	EXPECT_EQ(plan.writes[0].offset, 0x10u);
	EXPECT_EQ(plan.writes[0].off, (std::vector<uint8_t> {0x90, 0x90}));
	EXPECT_EQ(plan.writes[0].on, (std::vector<uint8_t> {0xc3, 0xcc}));
	EXPECT_EQ(plan.writes[1].offset, 0x12000u);
}

TEST(EmulatorGamePatch, RejectsMalformedWrites)
{
	for (const auto* on: {U"C3", U"C3c", U"C3zz", U""})
	{
		GamePatchPlan plan;
		EXPECT_FALSE(GamePatchParse(WithFirstOn(on), &plan).IsEmpty());
	}
	GamePatchPlan plan;
	EXPECT_FALSE(GamePatchParse(Core::String(VALID_FILE).ReplaceStr(U"0x10", U"0xnope"), &plan).IsEmpty());
}

TEST(EmulatorGamePatch, RejectsOtherDocuments)
{
	GamePatchPlan plan;
	EXPECT_FALSE(GamePatchParse(U"not json", &plan).IsEmpty());
	EXPECT_FALSE(GamePatchParse(U"{\"id\": \"PPSA00000\"}", &plan).IsEmpty());
}

UT_END();
