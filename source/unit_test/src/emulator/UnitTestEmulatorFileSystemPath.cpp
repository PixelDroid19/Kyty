#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/FileSystemPath.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

UT_BEGIN(EmulatorFileSystemPath);

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
	void AddSymlink(const std::string& relative, const std::filesystem::path& target) const
	{
		std::filesystem::create_directory_symlink(target, m_root / relative);
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
	std::filesystem::last_write_time(root / "Pack", std::filesystem::file_time_type::clock::now() - age);
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/ASSET.BIN"), "Pack/Asset.bin");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/Other.bin"), "Pack/Other.bin");

	tree.AddFile("Pack/other.BIN");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/Other.bin"), "Pack/other.BIN");

	tree.AddFile("Pack/ASSET.bin");
	std::filesystem::last_write_time(root / "Pack", std::filesystem::file_time_type::clock::now() - 2 * age);
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/asset.BIN"), "Pack/asset.BIN");
	EXPECT_EQ(MatchCaseInsensitive(tree.Root(), "pack/other.bin"), "Pack/other.BIN");
}

TEST(EmulatorFileSystemPath, ResolvesBelowTheRootWithoutEscapingThroughSymlinks)
{
	using namespace Kernel::FileSystem::Path;
	const TemporaryTree tree("kyty-ut-path-contained");
	const TemporaryTree outside("kyty-ut-path-outside");
	tree.AddFile("data/asset.bin");
	tree.AddSymlink("inner", "data");
	tree.AddSymlink("escape", outside.Root());
	tree.AddSymlink("dangling", tree.Root() + "/missing-target");
	const auto root = Canonical(tree.Root());
	EXPECT_EQ(ResolveContained(root, "inner/asset.bin"), (root / "data/asset.bin").u8string());
	EXPECT_EQ(ResolveContained(root, "data/new/file.bin"), (root / "data/new/file.bin").u8string());
	EXPECT_EQ(ResolveContained(root, "data/"), (root / "data").u8string());
	EXPECT_TRUE(ResolveContained(root, "escape/file.bin").empty());
	EXPECT_TRUE(ResolveContained(root, "dangling").empty());
	EXPECT_TRUE(ResolveContained(root, "data/../../outside").empty());
	EXPECT_EQ(CanonicalFrom({root}, root / "inner/asset.bin"), root / "data/asset.bin");
	EXPECT_EQ(CanonicalFrom({root}, root / "escape"), Canonical(outside.Root()));
}

UT_END();
