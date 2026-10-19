#include "Emulator/Profiling.h"
#include "Kyty/UnitTest.h"
#include "ScopedTestDirectory.h"

UT_BEGIN(EmulatorProfiling);

namespace {

void ProfiledWork()
{
	KYTY_PROFILER_FUNCTION();
	{
		KYTY_PROFILER_BLOCK("UnitTestProfiling::nested");
	}
	KYTY_PROFILER_BLOCK("UnitTestProfiling::explicit_end");
	KYTY_PROFILER_END_BLOCK;
}

uint32_t RecordedBlocks(bool blocks_enabled, const std::filesystem::path& dump)
{
	Profiler::g_blocks_enabled = blocks_enabled;
	EASY_PROFILER_ENABLE;
	ProfiledWork();
	const uint32_t blocks      = profiler::dumpBlocksToFile(dump.string().c_str());
	Profiler::g_blocks_enabled = false;
	return blocks;
}

} // namespace

TEST(EmulatorProfiling, BlocksAreRecordedOnlyWhenConfigured)
{
	const ScopedTestDirectory directory("kyty-profiling");
	ASSERT_FALSE(directory.Path().empty());

	EXPECT_EQ(RecordedBlocks(false, directory.Path() / "off.prof"), 0u);
	EXPECT_EQ(RecordedBlocks(true, directory.Path() / "on.prof"), 3u);
}

UT_END();
