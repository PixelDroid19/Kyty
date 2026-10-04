#pragma once

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Opt-in local evidence dumps (shader programs, SPIR-V of a failed pipeline) are
// written through one policy so a diagnostic switch can never overwrite earlier
// evidence, follow a planted link, fill the disk or write outside the directory
// the operator named:
//  - the directory comes from the environment and must already exist; it is
//    never created;
//  - the file name is a fixed component of [A-Za-z0-9._-], at most 96 characters
//    and not starting with '.', so it cannot leave the directory;
//  - the joined path must fit without truncation;
//  - the file is created exclusively (an existing file, symlink or FIFO fails);
//  - each file and the process as a whole have a byte budget. A dump cut by a
//    budget is written up to it and reported as Truncated, never silently.
inline constexpr size_t kDiagnosticDumpFileBytesMax  = 16u * 1024u * 1024u;
inline constexpr size_t kDiagnosticDumpTotalBytesMax = 64u * 1024u * 1024u;

enum class DiagnosticDumpStatus: uint8_t
{
	Written,
	Disabled,
	InvalidName,
	InvalidData,
	PathTooLong,
	DirectoryInvalid,
	OpenFailed,
	WriteFailed,
	Truncated,
	BudgetExhausted,
};

[[nodiscard]] const char* DiagnosticDumpStatusName(DiagnosticDumpStatus status);

class DiagnosticDumpWriter
{
public:
	DiagnosticDumpWriter(size_t file_bytes_max, size_t total_bytes_max);

	// dir may be null or empty (disabled). data may be null only when size is zero.
	[[nodiscard]] DiagnosticDumpStatus Write(const char* dir, const char* name, const void* data, size_t size);

	[[nodiscard]] size_t FileBytesMax() const { return m_file_bytes_max; }
	[[nodiscard]] size_t TotalBytesWritten() const;

private:
	size_t             m_file_bytes_max;
	size_t             m_total_bytes_max;
	mutable std::mutex m_mutex;
	size_t             m_total_written = 0;
};

// The writer shared by every dump hook of the process.
[[nodiscard]] DiagnosticDumpWriter& DiagnosticDumpProcessWriter();

} // namespace Kyty::Libs::Graphics

#endif
