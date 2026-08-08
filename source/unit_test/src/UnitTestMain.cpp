#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/Core.h"
#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/SDLSubsystem.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Math/MathAll.h"
#include "Kyty/Scripts/Scripts.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Emulator.h"
#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include <cstdio>

int main(int argc, char* argv[])
{
	Kyty::Core::BringUp::ConfigError bringup_error {};
	if (!Kyty::Core::BringUp::InitializeFromEnvironment(&bringup_error))
	{
		std::fprintf(stderr, "KYTY_BRINGUP: invalid configuration: %s\n", bringup_error.message);
		return 125;
	}

	Kyty::Core::mem_set_max_size(static_cast<size_t>(2048) * 1024 * 1024 - 1);

	Kyty::Core::SubsystemsList       lifecycle;
	Kyty::Core::ScopedSubsystemsList active_lifecycle(lifecycle);
	auto&                            subsystems = lifecycle;
	subsystems.SetArgs(argc, argv);

	auto* core      = Kyty::Core::CoreSubsystem::Instance();
	auto* scripts   = Kyty::Scripts::ScriptsSubsystem::Instance();
	auto* math      = Kyty::Math::MathSubsystem::Instance();
	auto* sdl       = Kyty::Core::SDLSubsystem::Instance();
	auto* threads   = Kyty::Core::ThreadsSubsystem::Instance();
	auto* config    = Kyty::Config::ConfigSubsystem::Instance();
	auto* log       = Kyty::Log::LogSubsystem::Instance();
	auto* emulator  = Kyty::Emulator::EmulatorSubsystem::Instance();
	auto* unit_test = Kyty::UnitTest::UnitTestSubsystem::Instance();

	subsystems.Add(core, {});
	subsystems.Add(scripts, {core});
	subsystems.Add(math, {core});
	subsystems.Add(sdl, {core});
	subsystems.Add(threads, {core, sdl});
	subsystems.Add(config, {core});
	subsystems.Add(log, {core, config, threads});
	subsystems.Add(emulator, {core, scripts});
	subsystems.Add(unit_test, {core});

	if (!subsystems.InitAll(false))
	{
		std::fprintf(stderr, "failed to initialize '%s': %s\n", subsystems.GetFailName(), subsystems.GetFailMsg());
		return 125;
	}

	const bool passed = Kyty::UnitTest::unit_test_all();
	subsystems.DestroyAll(false);
	return passed ? 0 : 1;
}
