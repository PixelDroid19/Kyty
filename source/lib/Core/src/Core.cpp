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

namespace Kyty::Core {

KYTY_SUBSYSTEM_INIT(Core)
{
	// Fail-closed bring-up policy before any guest/HLE work. Invalid KYTY_BRINGUP_*
	// aborts the process here (no silent fallback to strict after a parse error).
	BringUp::InitFromEnvironment();

	core_memory_init();
	core_file_init();
	core_debug_init(parent->GetArgv()[0]);
	Language::Init();
	Database::Init();
	VirtualMemory::Init();
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Core) {}

KYTY_SUBSYSTEM_DESTROY(Core) {}

} // namespace Kyty::Core
