#include "Kyty/UnitTest.h"
#include "ScopedTestDirectory.h"

#include "Emulator/Graphics/DiagnosticDump.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

UT_BEGIN(EmulatorDiagnosticDump);

using namespace Libs::Graphics;

namespace {

namespace fs = std::filesystem;

// Fresh directory per test under the current (scratch) directory.
class ScopedDir final
{
public:
	explicit ScopedDir(const char* name): m_directory((std::string("diag-dump-") + name).c_str()), m_path(m_directory.Path())
	{
		if (m_path.empty())
		{
			std::_Exit(90);
		}
	}
	[[nodiscard]] std::string Path() const { return m_path.string(); }
	[[nodiscard]] fs::path    File(const char* name) const { return m_path / name; }

private:
	ScopedTestDirectory m_directory;
	fs::path m_path;
};

std::string ReadAll(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, const std::string& text)
{
	std::ofstream file(path, std::ios::binary);
	file << text;
}

} // namespace

TEST(EmulatorDiagnosticDump, IsDisabledWithoutADirectory)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("disabled");
	EXPECT_EQ(writer.Write(nullptr, "a.txt", "x", 1), DiagnosticDumpStatus::Disabled);
	EXPECT_EQ(writer.Write("", "a.txt", "x", 1), DiagnosticDumpStatus::Disabled);
	EXPECT_EQ(writer.TotalBytesWritten(), 0u);
	EXPECT_TRUE(fs::is_empty(dir.Path()));
}

TEST(EmulatorDiagnosticDump, WritesExactBytesAndNeverOverwrites)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("exclusive");
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "ps.bin", "abc\0def", 7), DiagnosticDumpStatus::Written);
	EXPECT_EQ(ReadAll(dir.File("ps.bin")), std::string("abc\0def", 7));

	// A second dump of the same name keeps the first evidence.
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "ps.bin", "ZZZ", 3), DiagnosticDumpStatus::OpenFailed);
	EXPECT_EQ(ReadAll(dir.File("ps.bin")), std::string("abc\0def", 7));
	EXPECT_EQ(writer.TotalBytesWritten(), 7u);
}

TEST(EmulatorDiagnosticDump, RejectsNamesThatCouldEscapeOrHide)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("names");
	const std::string    too_long(97, 'n');
	for (const char* name: {"", ".", "..", "../escape", "a/b", "a\\b", ".hidden", "sp ace", "semi;colon", "nul\n"})
	{
		EXPECT_EQ(writer.Write(dir.Path().c_str(), name, "x", 1), DiagnosticDumpStatus::InvalidName) << name;
	}
	EXPECT_EQ(writer.Write(dir.Path().c_str(), nullptr, "x", 1), DiagnosticDumpStatus::InvalidName);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), too_long.c_str(), "x", 1), DiagnosticDumpStatus::InvalidName);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "fail_vs.spv", "x", 1), DiagnosticDumpStatus::Written);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "ps_0123-abc.TXT", "x", 1), DiagnosticDumpStatus::Written);
}

TEST(EmulatorDiagnosticDump, ReportsAPathThatWouldBeTruncated)
{
	DiagnosticDumpWriter writer(64, 256);
	const std::string    long_dir(1000, 'd');
	EXPECT_EQ(writer.Write(long_dir.c_str(), "0123456789012345678901234567890123456789.bin", "x", 1), DiagnosticDumpStatus::PathTooLong);
	EXPECT_EQ(writer.TotalBytesWritten(), 0u);
}

TEST(EmulatorDiagnosticDump, NeverCreatesTheDirectory)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("nodir");
	const fs::path       missing = dir.File("missing");
	EXPECT_EQ(writer.Write(missing.string().c_str(), "a.bin", "x", 1), DiagnosticDumpStatus::DirectoryInvalid);
	EXPECT_FALSE(fs::exists(missing));

	// A regular file is not a directory either.
	WriteAll(dir.File("plain"), "x");
	EXPECT_EQ(writer.Write(dir.File("plain").string().c_str(), "a.bin", "x", 1), DiagnosticDumpStatus::DirectoryInvalid);
}

#if !defined(_WIN32)
TEST(EmulatorDiagnosticDump, DoesNotFollowAPlantedSymlink)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("symlink");
	WriteAll(dir.File("victim"), "keep");
	fs::create_symlink(dir.File("victim"), dir.File("planted.bin"));
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "planted.bin", "overwritten", 11), DiagnosticDumpStatus::OpenFailed);
	EXPECT_EQ(ReadAll(dir.File("victim")), "keep");

	// Also a dangling link, which "w" would have created through.
	fs::create_symlink(dir.File("not-there"), dir.File("dangling.bin"));
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "dangling.bin", "x", 1), DiagnosticDumpStatus::OpenFailed);
	EXPECT_FALSE(fs::exists(dir.File("not-there")));
}
#endif

TEST(EmulatorDiagnosticDump, TruncatesAtThePerFileLimitAndSaysSo)
{
	DiagnosticDumpWriter writer(8, 256);
	const ScopedDir      dir("filecap");
	const std::string    data = "0123456789abcdefghij";
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "big.bin", data.data(), data.size()), DiagnosticDumpStatus::Truncated);
	EXPECT_EQ(ReadAll(dir.File("big.bin")), "01234567");
	EXPECT_EQ(writer.TotalBytesWritten(), 8u);

	// Exactly the limit is complete, not truncated.
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "exact.bin", data.data(), 8), DiagnosticDumpStatus::Written);
}

TEST(EmulatorDiagnosticDump, BoundsEveryFileWithOneProcessBudget)
{
	DiagnosticDumpWriter writer(10, 15);
	const ScopedDir      dir("budget");
	const std::string    data(10, 'x');
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "a.bin", data.data(), 10), DiagnosticDumpStatus::Written);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "b.bin", data.data(), 10), DiagnosticDumpStatus::Truncated);
	EXPECT_EQ(ReadAll(dir.File("b.bin")).size(), 5u);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "c.bin", data.data(), 10), DiagnosticDumpStatus::BudgetExhausted);
	EXPECT_FALSE(fs::exists(dir.File("c.bin")));
	EXPECT_EQ(writer.TotalBytesWritten(), 15u);
}

TEST(EmulatorDiagnosticDump, HandlesEmptyAndMissingData)
{
	DiagnosticDumpWriter writer(64, 256);
	const ScopedDir      dir("empty");
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "empty.bin", nullptr, 0), DiagnosticDumpStatus::Written);
	EXPECT_TRUE(fs::exists(dir.File("empty.bin")));
	EXPECT_EQ(fs::file_size(dir.File("empty.bin")), 0u);
	EXPECT_EQ(writer.Write(dir.Path().c_str(), "null.bin", nullptr, 4), DiagnosticDumpStatus::InvalidData);
	EXPECT_FALSE(fs::exists(dir.File("null.bin")));
}

TEST(EmulatorDiagnosticDump, ProcessLimitsAreBoundedAndStatusNamesAreDistinct)
{
	EXPECT_LE(kDiagnosticDumpFileBytesMax, 32u * 1024u * 1024u);
	EXPECT_LE(kDiagnosticDumpTotalBytesMax, 128u * 1024u * 1024u);
	EXPECT_LE(kDiagnosticDumpFileBytesMax, kDiagnosticDumpTotalBytesMax);
	EXPECT_EQ(DiagnosticDumpProcessWriter().FileBytesMax(), kDiagnosticDumpFileBytesMax);

	std::set<std::string> names;
	for (const auto status: {DiagnosticDumpStatus::Written, DiagnosticDumpStatus::Disabled, DiagnosticDumpStatus::InvalidName,
	                         DiagnosticDumpStatus::InvalidData, DiagnosticDumpStatus::PathTooLong, DiagnosticDumpStatus::DirectoryInvalid,
	                         DiagnosticDumpStatus::OpenFailed, DiagnosticDumpStatus::WriteFailed, DiagnosticDumpStatus::Truncated,
	                         DiagnosticDumpStatus::BudgetExhausted})
	{
		const std::string name = DiagnosticDumpStatusName(status);
		EXPECT_FALSE(name.empty());
		EXPECT_TRUE(names.insert(name).second) << name;
	}
}

UT_END();
