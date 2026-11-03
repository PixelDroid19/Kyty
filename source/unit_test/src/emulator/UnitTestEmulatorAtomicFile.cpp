#include "Kyty/UnitTest.h"

#include "Emulator/AtomicFile.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

UT_BEGIN(EmulatorAtomicFile);

using Libs::AtomicFileRemoveOrphans;

namespace {

uint64_t CurrentProcessId()
{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

void Touch(const std::filesystem::path& path)
{
	std::ofstream file(path, std::ios::binary);
	file << "x";
}

} // namespace

// Only a temporary sibling whose writer has exited is removed; the cache file,
// this process's own temporaries and other names stay.
TEST(EmulatorAtomicFile, RemovesOnlyTemporariesOfExitedWriters)
{
	const auto root = std::filesystem::temp_directory_path() /
	                  ("kyty-atomic-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	ASSERT_TRUE(std::filesystem::create_directories(root));

	// Above the largest pid either host can assign, so no process holds it.
	const auto exited  = root / "cache.bin.4194305.3.tmp";
	const auto own     = root / ("cache.bin." + std::to_string(CurrentProcessId()) + ".0.tmp");
	const auto cache   = root / "cache.bin";
	const auto unnamed = root / "notes.tmp";
	const auto foreign = root / "cache.bin.x1.3.tmp";
	for (const auto& path: {exited, own, cache, unnamed, foreign})
	{
		Touch(path);
	}

	AtomicFileRemoveOrphans(root);

	EXPECT_FALSE(std::filesystem::exists(exited));
	EXPECT_TRUE(std::filesystem::exists(own));
	EXPECT_TRUE(std::filesystem::exists(cache));
	EXPECT_TRUE(std::filesystem::exists(unnamed));
	EXPECT_TRUE(std::filesystem::exists(foreign));

	std::error_code error;
	std::filesystem::remove_all(root, error);
}

UT_END();
