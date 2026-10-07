#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/FileSystemPath.h"

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

UT_END();
