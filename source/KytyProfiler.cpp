#include "Emulator/Profiler.h"

#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Config.h"

#include <easy/profiler.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Profiler {

void Close()
{
	auto dir = Config::GetProfilerDirection();
	if (dir == Config::ProfilerDirection::File || dir == Config::ProfilerDirection::FileAndNetwork)
	{
		profiler::dumpBlocksToFile(Config::GetProfilerOutputFile().C_Str());
	}
}

void ProfilerSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	switch (Config::GetProfilerDirection())
	{
		case Config::ProfilerDirection::File: EASY_PROFILER_ENABLE; break;
		case Config::ProfilerDirection::Network: profiler::startListen(); break;
		case Config::ProfilerDirection::FileAndNetwork:
			EASY_PROFILER_ENABLE;
			profiler::startListen();
			break;
		case Config::ProfilerDirection::None:
		default: break;
	}
}

void ProfilerSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	Close();
}

void ProfilerSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	Close();
}

} // namespace Kyty::Profiler

#endif // KYTY_EMU_ENABLED
