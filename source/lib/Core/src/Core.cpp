#include "Kyty/Core/Core.h"

#include "Kyty/Core/ArrayWrapper.h" // IWYU pragma: associated
#include "Kyty/Core/BringUp.h"
#include "Kyty/Core/ByteBuffer.h"   // IWYU pragma: associated
#include "Kyty/Core/Common.h"       // IWYU pragma: associated
#include "Kyty/Core/Database.h"
#include "Kyty/Core/Debug.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/Hash.h" // IWYU pragma: associated
#include "Kyty/Core/Language.h"
#include "Kyty/Core/LinkList.h"  // IWYU pragma: associated
#include "Kyty/Core/MagicEnum.h" // IWYU pragma: associated
#include "Kyty/Core/MemoryAlloc.h"
#include "Kyty/Core/RefCounter.h"  // IWYU pragma: associated
#include "Kyty/Core/SafeDelete.h"  // IWYU pragma: associated
#include "Kyty/Core/SimpleArray.h" // IWYU pragma: associated
#include "Kyty/Core/Singleton.h"   // IWYU pragma: associated
#include "Kyty/Core/Vector.h"      // IWYU pragma: associated
#include "Kyty/Core/VirtualMemory.h"

#include <cstdio>
#include <cstdlib>

namespace Kyty::Core {
namespace {

constexpr int kConfigurationErrorExitCode = 125;

} // namespace

void CoreSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	// Fail-closed bring-up policy before guest/HLE work. Invalid KYTY_BRINGUP_*
	// aborts here — never convert a configuration error into silent strict.
	BringUp::ConfigError bringup_error {};
	if (!BringUp::InitializeFromEnvironment(&bringup_error))
	{
		std::fprintf(stderr, "KYTY_BRINGUP: invalid configuration: %s\n", bringup_error.message);
		std::fflush(stderr);
		std::_Exit(kConfigurationErrorExitCode);
	}

	// Core setup order is memory -> file -> debug -> language -> database -> virtual memory.
	core_memory_init();
	core_file_init();
	core_debug_init(parent->GetArgv()[0]);
	Language::Init();
	Database::Init();
	VirtualMemory::Init();
}

void CoreSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	// SubsystemsList invokes Core after dependents, while the Core allocator is still available.
	Language::Shutdown();
}

void CoreSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	// Keep normal teardown aligned with the fatal-shutdown ownership boundary.
	Language::Shutdown();
}

} // namespace Kyty::Core
