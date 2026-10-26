#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "ScopedTestDirectory.h"

#include <algorithm>
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

TEST(EmulatorLibcFilesystem, StatReportsGuestTypesPermissionsAndErrors)
{
	ScopedTestDirectory directory("kyty-fs-stat");
	ASSERT_FALSE(directory.Path().empty());

	const auto file_path = directory.Path() / "file.txt";
	{
		std::ofstream file(file_path);
		file << "hello";
	}
	const std::string file = file_path.string();

	uint32_t permissions = 0xffff;
	int      error       = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(file.c_str(), &permissions, &error), 1);
	EXPECT_EQ(error, 0);
	EXPECT_EQ(permissions & 0xf000u, 0u);
	EXPECT_NE(permissions & 0x1ffu, 0u);

	int size_error = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_file_size(file.c_str(), &size_error), 5u);
	EXPECT_EQ(size_error, 0);

	const std::string root = directory.Path().string();
	permissions            = 0;
	error                  = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(root.c_str(), &permissions, &error), 2);
	EXPECT_EQ(error, 0);

	const auto missing_path = directory.Path() / "missing";
	const std::string missing = missing_path.string();
	permissions               = 0xffff;
	error                     = 0;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(missing.c_str(), &permissions, &error), -1);
	EXPECT_EQ(error, 2);
	EXPECT_EQ(permissions, 0xffffu);
	size_error = 0;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_file_size(missing.c_str(), &size_error), static_cast<uint64_t>(-1));
	EXPECT_EQ(size_error, static_cast<int>(0x80020002u));

	const auto link_path = directory.Path() / "link";
	std::error_code link_error;
	std::filesystem::create_symlink(file_path, link_path, link_error);
	ASSERT_FALSE(link_error);
	const std::string link = link_path.string();
	permissions            = 0;
	error                  = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_stat(link.c_str(), &permissions, &error), 1);
	EXPECT_EQ(error, 0);
	permissions = 0;
	error       = -1;
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_lstat(link.c_str(), &permissions, &error), 3);
	EXPECT_EQ(error, 0);
}

TEST(EmulatorLibcFilesystem, DirectoryIterationSkipsDotsAndReportsNames)
{
	ScopedTestDirectory directory("kyty-fs-dir");
	ASSERT_FALSE(directory.Path().empty());

	{
		std::ofstream alpha(directory.Path() / "alpha.txt");
		std::ofstream beta(directory.Path() / "beta.txt");
	}
	const std::string root = directory.Path().string();

	char first_entry[260] = {};
	int  error            = -1;
	int  type             = -1;
	void* stream          = Kyty::Libs::LibC::c_fs_open_dir(first_entry, root.c_str(), &error, &type);
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

	const auto empty_path = directory.Path() / "empty";
	std::filesystem::create_directory(empty_path);
	error  = -1;
	stream = Kyty::Libs::LibC::c_fs_open_dir(first_entry, empty_path.string().c_str(), &error, &type);
	EXPECT_EQ(stream, nullptr);
	EXPECT_EQ(error, 0);

	const auto missing_path = directory.Path() / "missing";
	error                   = 0;
	stream = Kyty::Libs::LibC::c_fs_open_dir(first_entry, missing_path.string().c_str(), &error, &type);
	EXPECT_EQ(stream, nullptr);
	EXPECT_EQ(error, 2);
}

TEST(EmulatorLibcFilesystem, MutationsMatchGuestErrorConventions)
{
	ScopedTestDirectory directory("kyty-fs-ops");
	ASSERT_FALSE(directory.Path().empty());
	const std::string root = directory.Path().string();

	const auto sub_path = directory.Path() / "sub";
	const auto sub      = sub_path.string();
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(sub.c_str(), nullptr), 1);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(sub.c_str(), nullptr), 0);

	const auto inherited_path = directory.Path() / "inherited";
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_make_dir(inherited_path.string().c_str(), root.c_str()), 1);

	const auto file_path = directory.Path() / "file";
	{
		std::ofstream file(file_path);
		file << "x";
	}
	const auto renamed_path = directory.Path() / "renamed";
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_rename(file_path.string().c_str(), renamed_path.string().c_str()), 0);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_unlink(renamed_path.string().c_str()), 0);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_unlink(renamed_path.string().c_str()), 2);
	EXPECT_EQ(Kyty::Libs::LibC::c_fs_rename(file_path.string().c_str(), renamed_path.string().c_str()), 2);

	EXPECT_TRUE(Kyty::Libs::LibC::c_fs_remove_dir(sub.c_str()));
	EXPECT_FALSE(Kyty::Libs::LibC::c_fs_remove_dir(sub.c_str()));

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
