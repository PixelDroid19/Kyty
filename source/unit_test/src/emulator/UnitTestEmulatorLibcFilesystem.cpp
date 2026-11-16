#include "Kyty/UnitTest.h"

#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "ScopedTestDirectory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// HLE bodies live in LibC.cpp; declare the std::filesystem surface under test.
namespace Kyty::Libs::LibC {
int      c_fs_stat(const char* path, uint32_t* permissions, int* error);
int      c_fs_lstat(const char* path, uint32_t* permissions, int* error);
uint64_t c_fs_file_size(const char* path, int* error);
int      c_fs_make_dir(const char* path, const char* permissions_source);
bool     c_fs_remove_dir(const char* path);
int      c_fs_rename(const char* from, const char* to);
int      c_fs_unlink(const char* path);
bool     c_fs_current_get(char* buffer);
void*    c_fs_open_dir(char* buffer, const char* path, int* error, int* type);
char*    c_fs_read_dir(char* buffer, void* stream, int* type);
void     c_fs_close_dir(void* stream);
} // namespace Kyty::Libs::LibC

UT_BEGIN(EmulatorLibcFilesystem);

// The guest passes guest paths: each test mounts its scratch directory and
// addresses it through the mount, as a title addresses /app0 or a save mount.
class GuestMountedDirectory final
{
public:
	explicit GuestMountedDirectory(const char* prefix): m_directory(prefix)
	{
		using namespace Kyty;
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		if (!Kernel::FileSystem::IsMounted())
		{
			Kernel::FileSystem::FileSystemSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		static std::atomic_uint64_t serial {0};
		m_mount = "/fs-test-" + std::to_string(serial.fetch_add(1)) + "/";
		if (!m_directory.Path().empty())
		{
			Kernel::FileSystem::Mount(String::FromUtf8(m_directory.Path().u8string().c_str()), String::FromUtf8(m_mount.c_str()));
		}
	}
	~GuestMountedDirectory()
	{
		if (!m_directory.Path().empty())
		{
			Kyty::Kernel::FileSystem::Umount(String::FromUtf8(m_mount.c_str()));
		}
	}
	GuestMountedDirectory(const GuestMountedDirectory&)            = delete;
	GuestMountedDirectory& operator=(const GuestMountedDirectory&) = delete;

	[[nodiscard]] const std::filesystem::path& Host() const { return m_directory.Path(); }
	[[nodiscard]] std::string                  Guest(const char* name = "") const { return m_mount + name; }

private:
	ScopedTestDirectory m_directory;
	std::string         m_mount;
};

TEST(EmulatorLibcFilesystem, StatReportsGuestTypesPermissionsAndErrors)
{
	GuestMountedDirectory directory("kyty-fs-stat");
	ASSERT_FALSE(directory.Host().empty());
	{
		std::ofstream file(directory.Host() / "file.txt");
		file << "hello";
	}
	const std::string file = directory.Guest("file.txt");

	uint32_t permissions = 0xffff;
	int      error       = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(file.c_str(), &permissions, &error), 1);
	EXPECT_EQ(error, 0);
	EXPECT_EQ(permissions & 0xf000u, 0u);
	EXPECT_NE(permissions & 0x1ffu, 0u);

	int size_error = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_file_size(file.c_str(), &size_error), 5u);
	EXPECT_EQ(size_error, 0);

	const std::string root = directory.Guest();
	permissions            = 0;
	error                  = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(root.c_str(), &permissions, &error), 2);
	EXPECT_EQ(error, 0);

	const std::string missing = directory.Guest("missing");
	permissions               = 0xffff;
	error                     = 0;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(missing.c_str(), &permissions, &error), -1);
	EXPECT_EQ(error, 2);
	EXPECT_EQ(permissions, 0xffffu);
	size_error = 0;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_file_size(missing.c_str(), &size_error), static_cast<uint64_t>(-1));
	EXPECT_EQ(size_error, static_cast<int>(0x80020002u));

	// A host path is not a guest path: it resolves outside every mount.
	const std::string host = (directory.Host() / "file.txt").string();
	error                  = 0;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(host.c_str(), &permissions, &error), -1);
	EXPECT_EQ(error, 2);

	// Guest mounts expose no symbolic links, so lstat reports the target like stat.
	std::error_code link_error;
	std::filesystem::create_symlink(directory.Host() / "file.txt", directory.Host() / "link", link_error);
	ASSERT_FALSE(link_error);
	const std::string link = directory.Guest("link");
	error                  = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_lstat(link.c_str(), &permissions, &error), 1);
	EXPECT_EQ(error, 0);
}

TEST(EmulatorLibcFilesystem, DirectoryIterationSkipsDotsAndReportsNames)
{
	GuestMountedDirectory directory("kyty-fs-dir");
	ASSERT_FALSE(directory.Host().empty());
	std::filesystem::create_directory(directory.Host() / "list");
	{
		std::ofstream alpha(directory.Host() / "list" / "alpha.txt");
		std::ofstream beta(directory.Host() / "list" / "beta.txt");
	}
	const std::string list = directory.Guest("list");

	char first_entry[260] = {};
	int  error            = -1;
	int  type             = -1;
	void* stream          = Kyty::Libs::LibC::c_fs_open_dir(first_entry, list.c_str(), &error, &type);
	ASSERT_NE(stream, nullptr);
	EXPECT_EQ(error, 0);
	EXPECT_EQ(type, 8);

	std::vector<std::string> names;
	names.emplace_back(first_entry);
	while (true)
	{
		char entry[260] = {};
		int  entry_type = -1;
		Kyty::Libs::LibC::c_fs_read_dir(entry, stream, &entry_type);
		EXPECT_EQ(entry_type, 8);
		if (entry[0] == 0)
		{
			break;
		}
		names.emplace_back(entry);
	}
	Kyty::Libs::LibC::c_fs_close_dir(stream);
	std::sort(names.begin(), names.end());
	ASSERT_EQ(names.size(), 2u);
	EXPECT_EQ(names[0], "alpha.txt");
	EXPECT_EQ(names[1], "beta.txt");

	std::filesystem::create_directory(directory.Host() / "empty");
	error  = -1;
	stream = Kyty::Libs::LibC::c_fs_open_dir(first_entry, directory.Guest("empty").c_str(), &error, &type);
	EXPECT_EQ(stream, nullptr);
	EXPECT_EQ(error, 0);

	error  = 0;
	stream = Kyty::Libs::LibC::c_fs_open_dir(first_entry, directory.Guest("missing").c_str(), &error, &type);
	EXPECT_EQ(stream, nullptr);
	EXPECT_EQ(error, 2);
}

TEST(EmulatorLibcFilesystem, MutationsMatchGuestErrorConventions)
{
	GuestMountedDirectory directory("kyty-fs-ops");
	ASSERT_FALSE(directory.Host().empty());
	const std::string root = directory.Guest();

	const std::string sub = directory.Guest("sub");
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(sub.c_str(), nullptr), 1);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(sub.c_str(), nullptr), 0);
	EXPECT_TRUE(std::filesystem::is_directory(directory.Host() / "sub"));

	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(directory.Guest("inherited").c_str(), root.c_str()), 1);

	{
		std::ofstream file(directory.Host() / "file");
		file << "x";
	}
	const std::string file    = directory.Guest("file");
	const std::string renamed = directory.Guest("renamed");
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_rename(file.c_str(), renamed.c_str()), 0);
	EXPECT_TRUE(std::filesystem::exists(directory.Host() / "renamed"));
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_unlink(renamed.c_str()), 0);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_unlink(renamed.c_str()), 2);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_rename(file.c_str(), renamed.c_str()), 2);

	EXPECT_TRUE(Kyty::Libs::LibC::c_fs_remove_dir(sub.c_str()));
	EXPECT_FALSE(Kyty::Libs::LibC::c_fs_remove_dir(sub.c_str()));

	// The application package is read-only to the guest.
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_unlink("/app0/eboot.bin"), 1 /*EPERM*/);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir("/app0/new", nullptr), -1 /*EPERM*/);

	char cwd[260] = {};
	EXPECT_TRUE(Kyty::Libs::LibC::c_fs_current_get(cwd));
	EXPECT_STREQ(cwd, "/");
}

TEST(EmulatorLibcFilesystem, ResolvesFilesystemInternals)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));

	const char16_t* nids[] = {u"8iuHpl+kg8A", u"w5CGykBBU5M", u"xGbaQPsHCFI", u"8KeeeFcEdls", u"tizCFt051h8",
	                          u"3U2eeEqUwR0", u"x7pQExTeqBY", u"YDQxE4cIwa4", u"srwl1hhFoUI", u"Iok1WdvAROg",
	                          u"8VKAqiw7lC0", u"B0CeIhQty7Y", u"C6-7Mo5WbwU", u"Wut42WAe7Rw"};
	for (const char16_t* nid: nids)
	{
		Kyty::Loader::SymbolResolve query {};
		query.name                 = nid;
		query.library              = U"libc";
		query.library_version      = 1;
		query.module               = U"libc";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Kyty::Loader::SymbolType::Func;
		EXPECT_NE(symbols.Find(query), nullptr) << nid;
	}
}

UT_END();
