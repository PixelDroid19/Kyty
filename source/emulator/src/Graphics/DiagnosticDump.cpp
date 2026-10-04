#include "Emulator/Graphics/DiagnosticDump.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr size_t kNameLengthMax = 96;
constexpr size_t kPathBytesMax  = 1024; // including the terminator

bool NameCharacterAllowed(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool NameAllowed(const char* name)
{
	if (name == nullptr || name[0] == '\0' || name[0] == '.')
	{
		return false;
	}
	const size_t length = std::strlen(name);
	if (length > kNameLengthMax)
	{
		return false;
	}
	return std::all_of(name, name + length, NameCharacterAllowed);
}

bool DirectoryExists(const char* dir)
{
	std::error_code error;
	return std::filesystem::is_directory(std::filesystem::path(dir), error) && !error;
}

} // namespace

const char* DiagnosticDumpStatusName(DiagnosticDumpStatus status)
{
	switch (status)
	{
		case DiagnosticDumpStatus::Written: return "written";
		case DiagnosticDumpStatus::Disabled: return "disabled";
		case DiagnosticDumpStatus::InvalidName: return "invalid_name";
		case DiagnosticDumpStatus::InvalidData: return "invalid_data";
		case DiagnosticDumpStatus::PathTooLong: return "path_too_long";
		case DiagnosticDumpStatus::DirectoryInvalid: return "directory_invalid";
		case DiagnosticDumpStatus::OpenFailed: return "open_failed";
		case DiagnosticDumpStatus::WriteFailed: return "write_failed";
		case DiagnosticDumpStatus::Truncated: return "truncated";
		case DiagnosticDumpStatus::BudgetExhausted: return "budget_exhausted";
	}
	return "unknown";
}

DiagnosticDumpWriter::DiagnosticDumpWriter(size_t file_bytes_max, size_t total_bytes_max)
    : m_file_bytes_max(file_bytes_max), m_total_bytes_max(total_bytes_max)
{
}

size_t DiagnosticDumpWriter::TotalBytesWritten() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_total_written;
}

DiagnosticDumpStatus DiagnosticDumpWriter::Write(const char* dir, const char* name, const void* data, size_t size)
{
	if (dir == nullptr || dir[0] == '\0')
	{
		return DiagnosticDumpStatus::Disabled;
	}
	if (!NameAllowed(name))
	{
		return DiagnosticDumpStatus::InvalidName;
	}
	if (data == nullptr && size != 0)
	{
		return DiagnosticDumpStatus::InvalidData;
	}
	// Compose with explicit room for the separator and terminator: a silently
	// truncated path would dump somewhere the operator did not name.
	if (std::strlen(dir) + 1 + std::strlen(name) >= kPathBytesMax)
	{
		return DiagnosticDumpStatus::PathTooLong;
	}
	if (!DirectoryExists(dir))
	{
		return DiagnosticDumpStatus::DirectoryInvalid;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	const size_t                remaining = m_total_bytes_max - std::min(m_total_bytes_max, m_total_written);
	if (remaining == 0 && size != 0)
	{
		return DiagnosticDumpStatus::BudgetExhausted;
	}
	const size_t allowed = std::min({size, m_file_bytes_max, remaining});

	const std::string path = std::string(dir) + '/' + name;
	// "x" creates exclusively: an existing file, symlink (dangling or not) or FIFO
	// makes the open fail instead of being written through.
	std::FILE* file = std::fopen(path.c_str(), "wbx");
	if (file == nullptr)
	{
		return DiagnosticDumpStatus::OpenFailed;
	}
	const bool written = allowed == 0 || std::fwrite(data, 1, allowed, file) == allowed;
	const bool closed  = std::fclose(file) == 0;
	if (!written || !closed)
	{
		return DiagnosticDumpStatus::WriteFailed;
	}
	m_total_written += allowed;
	return allowed < size ? DiagnosticDumpStatus::Truncated : DiagnosticDumpStatus::Written;
}

DiagnosticDumpWriter& DiagnosticDumpProcessWriter()
{
	static DiagnosticDumpWriter writer(kDiagnosticDumpFileBytesMax, kDiagnosticDumpTotalBytesMax);
	return writer;
}

} // namespace Kyty::Libs::Graphics

#endif
