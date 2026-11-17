#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/FileSystemPath.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

UT_BEGIN(EmulatorFileSystemPath);

namespace {

// Moves a directory's write time `age` into the past. MinGW's last_write_time cannot open a
// directory on Windows, so the handle is opened with backup semantics there.
void AgeDirectory(const std::filesystem::path& directory, std::chrono::hours age)
{
#if defined(_WIN32)
	HANDLE handle = CreateFileW(directory.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	ASSERT_NE(handle, INVALID_HANDLE_VALUE);
	FILETIME now {};
	GetSystemTimeAsFileTime(&now);
	ULARGE_INTEGER ticks {};
	ticks.LowPart  = now.dwLowDateTime;
	ticks.HighPart = now.dwHighDateTime;
	ticks.QuadPart -= static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(age).count()) * 10000000u;
	const FILETIME aged {ticks.LowPart, ticks.HighPart};
	const bool     set = SetFileTime(handle, nullptr, nullptr, &aged) != 0;
	CloseHandle(handle);
	ASSERT_TRUE(set);
#else
	std::filesystem::last_write_time(directory, std::filesystem::file_time_type::clock::now() - age);
#endif
}

} // namespace

TEST(EmulatorFileSystemPath, NormalizesBeforeMountSelection)
{
	using namespace Kernel::FileSystem::Path;
	EXPECT_EQ(NormalizeGuest("/app0/sub/../asset"), "/app0/asset");
	EXPECT_EQ(NormalizeGuest("/app0/../asset"), "/asset");
	EXPECT_EQ(NormalizeGuest("/app0\\sub\\..\\asset"), "/app0/asset");
	EXPECT_EQ(NormalizeGuest("//temp0///./asset"), "/temp0/asset");
	EXPECT_EQ(NormalizeGuest("relative/asset"), "/relative/asset");
	EXPECT_EQ(NormalizeGuest("/"), "/");
	EXPECT_TRUE(NormalizeGuest("/../asset").empty());
	EXPECT_TRUE(NormalizeGuest("/app0/../../asset").empty());
	EXPECT_TRUE(NormalizeGuest("").empty());
	EXPECT_TRUE(NormalizeGuest(std::string("/asset\0tail", 11)).empty());
}

TEST(EmulatorFileSystemPath, ContainmentUsesComponentsNotStringPrefixes)
{
	using namespace Kernel::FileSystem::Path;
	EXPECT_TRUE(Contains("/sandbox", "/sandbox"));
	EXPECT_TRUE(Contains("/sandbox", "/sandbox/asset"));
	EXPECT_FALSE(Contains("/sandbox", "/sandbox-sibling/asset"));
	EXPECT_FALSE(Contains("/sandbox/sub", "/sandbox"));
	EXPECT_FALSE(Contains({}, "/sandbox"));
	EXPECT_FALSE(Contains("/sandbox", {}));
	EXPECT_TRUE(ResolveContained("/", "/absolute").empty());
}

namespace {

class TemporaryTree
{
public:
	explicit TemporaryTree(const char* name): m_root(std::filesystem::temp_directory_path() / name)
	{
		std::filesystem::remove_all(m_root);
		std::filesystem::create_directories(m_root);
	}
	~TemporaryTree() { std::filesystem::remove_all(m_root); }
	TemporaryTree(const TemporaryTree&)            = delete;
	TemporaryTree& operator=(const TemporaryTree&) = delete;

	void AddFile(const std::string& relative) const
	{
		const auto path = m_root / relative;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path) << "x";
	}
	// False when the host cannot create the link (MinGW on Windows does not implement it).
	[[nodiscard]] bool AddSymlink(const std::string& relative, const std::filesystem::path& target) const
	{
		std::error_code error;
		std::filesystem::create_directory_symlink(target, m_root / relative, error);
		return !error;
	}
	[[nodiscard]] size_t EntryCount(const std::string& relative) const
	{
		return static_cast<size_t>(std::distance(std::filesystem::directory_iterator(m_root / relative), std::filesystem::directory_iterator()));
	}
	[[nodiscard]] std::string Root() const { return m_root.u8string(); }

private:
	std::filesystem::path m_root;
};

} // namespace

TEST(EmulatorFileSystemPath, MatchesGuestNamesIgnoringCase)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-case");
	tree.AddFile("Media/Modules/Il2cppUserAssemblies.prx");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "Media/Modules/Il2CppUserAssemblies.prx"), "Media/Modules/Il2cppUserAssemblies.prx");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "media/MODULES/il2cppuserassemblies.prx"), "Media/Modules/Il2cppUserAssemblies.prx");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "Media/Modules/Il2cppUserAssemblies.prx"), "Media/Modules/Il2cppUserAssemblies.prx");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), ""), "");
}

TEST(EmulatorFileSystemPath, KeepsNamesWithoutAnEntryVerbatim)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-case-missing");
	tree.AddFile("Save/Slot0.dat");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "save/NewDir/Slot1.dat"), "Save/NewDir/Slot1.dat");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "Missing/save/Slot0.dat"), "Missing/save/Slot0.dat");
}

TEST(EmulatorFileSystemPath, PrefersTheExactEntryAndRefusesAmbiguousOnes)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-case-ambiguous");
	tree.AddFile("data.bin");
	tree.AddFile("DATA.bin");
	tree.AddFile("Twin/a");
	tree.AddFile("tWIN/b");
	if (tree.EntryCount("") != 4)
	{
		GTEST_SKIP() << "the host filesystem folds case, so names that differ only in case are one entry";
	}
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "DATA.bin"), "DATA.bin");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "Data.bin"), "Data.bin");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "twin/a"), "twin/a");
}

TEST(EmulatorFileSystemPath, ReusedDirectoryIndexFollowsDirectoryChanges)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-case-index");
	const auto          root = std::filesystem::u8path(tree.Root());
	const auto          age  = std::chrono::hours(1);
	tree.AddFile("Pack/Asset.bin");
	AgeDirectory(root / "Pack", age);
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/ASSET.BIN"), "Pack/Asset.bin");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/Other.bin"), "Pack/Other.bin");

	tree.AddFile("Pack/other.BIN");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/Other.bin"), "Pack/other.BIN");

	tree.AddFile("Pack/ASSET.bin");
	if (tree.EntryCount("Pack") != 3)
	{
		GTEST_SKIP() << "the host filesystem folds case, so a second spelling cannot make the name ambiguous";
	}
	AgeDirectory(root / "Pack", 2 * age);
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/asset.BIN"), "Pack/asset.BIN");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/other.bin"), "Pack/other.BIN");
}

TEST(EmulatorFileSystemPath, ResolvesBelowTheRootWithoutEscapingThroughSymlinks)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-contained");
	const TemporaryTree outside("kyty-ut-path-outside");
	tree.AddFile("data/asset.bin");
	const auto root = Canonical(tree.Root());
	EXPECT_EQ(ResolveContained(root, "data/new/file.bin"), (root / "data/new/file.bin").generic_u8string());
	EXPECT_EQ(ResolveContained(root, "data/"), (root / "data").generic_u8string());
	EXPECT_TRUE(ResolveContained(root, "data/../../outside").empty());
	if (!tree.AddSymlink("inner", "data"))
	{
		GTEST_SKIP() << "the host cannot create directory symlinks";
	}
	ASSERT_TRUE(tree.AddSymlink("escape", outside.Root()));
	ASSERT_TRUE(tree.AddSymlink("dangling", tree.Root() + "/missing-target"));
	EXPECT_EQ(ResolveContained(root, "inner/asset.bin"), (root / "data/asset.bin").generic_u8string());
	EXPECT_TRUE(ResolveContained(root, "escape/file.bin").empty());
	EXPECT_TRUE(ResolveContained(root, "dangling").empty());
	EXPECT_EQ(CanonicalFrom({root}, root / "inner/asset.bin"), root / "data/asset.bin");
	EXPECT_EQ(CanonicalFrom({root}, root / "escape"), Canonical(outside.Root()));
}

UT_END();
