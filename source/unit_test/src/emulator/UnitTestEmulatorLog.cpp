#include "Kyty/UnitTest.h"

#include "Emulator/Log.h"

UT_BEGIN(EmulatorLog);

TEST(EmulatorLog, SeverityThresholdKeepsWarningsVisible)
{
	const auto previous = Kyty::Log::GetMinLevel();

	Kyty::Log::SetMinLevel(Kyty::Log::Level::Info);
	EXPECT_TRUE(Kyty::Log::ShouldLog(Kyty::Log::Level::Error));
	EXPECT_TRUE(Kyty::Log::ShouldLog(Kyty::Log::Level::Warn));
	EXPECT_TRUE(Kyty::Log::ShouldLog(Kyty::Log::Level::Info));
	EXPECT_FALSE(Kyty::Log::ShouldLog(Kyty::Log::Level::Debug));

	Kyty::Log::SetMinLevel(Kyty::Log::Level::Debug);
	EXPECT_TRUE(Kyty::Log::ShouldLog(Kyty::Log::Level::Debug));

	Kyty::Log::SetMinLevel(previous);
}

UT_END();
