#include "Emulator/Kernel/FileSystem.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"

#include "Kyty/Core/VirtualMemory.h"

#include <cstdint>
#include <limits>
#include <vector>

#ifdef KYTY_EMU_ENABLED

// Vectored guest writes (sceKernelWritev). The iovec table and every buffer it
// names live in guest memory, so both are validated before the first byte is
// written: a bad entry fails the whole call with nothing transferred, which is
// the writev(2) contract for EFAULT. The bytes themselves go through KernelWrite,
// so descriptor, append and file-position rules stay in one place, and descriptors
// 0-2 get the same EPERM that sceKernelWrite gives.
// Limit: the gather is not atomic against other writers. Each entry takes the file
// lock on its own, so another writer can interleave between entries.

namespace Kyty::Kernel::FileSystem {

KERNEL_LIB_NAME();

namespace {

// IOV_MAX on Linux. The PS5 limit is unverified; the value matches the sendmsg cap.
constexpr int kIovMax = 1024;

// A zero-length entry transfers nothing, so its buffer pointer is never read.
bool IsEntryReadable(const KernelIovec& entry)
{
	if (entry.size == 0) { return true; }
	return entry.base != nullptr && Core::VirtualMemory::IsRangeReadableOrLent(reinterpret_cast<uint64_t>(entry.base), entry.size);
}

} // namespace

int64_t KYTY_SYSV_ABI KernelWritev(int d, const KernelIovec* iov, int count)
{
	PRINT_NAME();

	if (d < 0)
	{
		return KERNEL_ERROR_EBADF;
	}
	if (count <= 0 || count > kIovMax)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (iov == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	std::vector<KernelIovec> entries(static_cast<size_t>(count));
	const uint64_t           table_size = entries.size() * sizeof(KernelIovec);
	if (!Core::VirtualMemory::CopyFromGuestOrLent(entries.data(), reinterpret_cast<uint64_t>(iov), table_size))
	{
		return KERNEL_ERROR_EFAULT;
	}

	// The total must fit the signed result before any byte is transferred.
	uint64_t total = 0;
	for (const auto& entry: entries)
	{
		if (entry.size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - total) { return KERNEL_ERROR_EINVAL; }
		total += entry.size;
	}
	for (const auto& entry: entries)
	{
		if (!IsEntryReadable(entry)) { return KERNEL_ERROR_EFAULT; }
	}

	// Each entry goes through KernelWrite, so zero-length entries still check the descriptor.
	int64_t written = 0;
	for (const auto& entry: entries)
	{
		const int64_t result = KernelWrite(d, entry.base, static_cast<size_t>(entry.size));
		if (result < 0) { return written != 0 ? written : result; }
		written += result;
		if (static_cast<uint64_t>(result) < entry.size) { break; }
	}
	return written;
}

} // namespace Kyty::Kernel::FileSystem

#endif // KYTY_EMU_ENABLED
