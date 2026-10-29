#include "Emulator/Kernel/FileSystem.h"

#include "FileSystemInternal.h"
#include "HostFile.h"

#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/Trace.h"

#include "Kyty/Core/File.h"

#include <cerrno>

#ifdef KYTY_EMU_ENABLED

// Durability and size operations that take a path or a standard-descriptor rule
// (sceKernelFsync, sceKernelTruncate). The descriptor table and File internals stay
// in FileSystem.cpp; this file reaches them only through FileSystemInternal.h.

namespace Kyty::Kernel::FileSystem {

KERNEL_LIB_NAME();

namespace {

// Descriptors below 3 are the standard streams, which are not regular files.
constexpr int kFirstRegularDescriptor = 3;

// Guest O_WRONLY. Host::File::Open takes guest open flags.
constexpr int kGuestOpenWriteOnly = 1;

} // namespace

int KYTY_SYSV_ABI KernelFsync(int d)
{
	PRINT_NAME();

	if (d < kFirstRegularDescriptor)
	{
		// POSIX fsync reports EINVAL for an open descriptor that is not a file.
		return KernelIsStandardDescriptorOpen(d) ? KERNEL_ERROR_EINVAL : KERNEL_ERROR_EBADF;
	}
	return SyncOpenDescriptor(d);
}

int KYTY_SYSV_ABI KernelTruncate(const char* path, int64_t length)
{
	PRINT_NAME();

	// A null path is EINVAL, as in KernelUnlink, KernelRename and the other path calls.
	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (length < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const auto real_file_name = GetRealFilename(String::FromUtf8(path));
	if (!Core::File::IsFileExisting(real_file_name))
	{
		return Core::File::IsDirectoryExisting(real_file_name) ? KERNEL_ERROR_EISDIR : KERNEL_ERROR_ENOENT;
	}

	// Like POSIX truncate, this leaves the stdio buffers of open streams alone.
	Host::File file;
	if (!file.Open(real_file_name.C_Str(), kGuestOpenWriteOnly, 0))
	{
		return KernelErrorFromHost(errno);
	}
	return file.Truncate(length) ? OK : KernelErrorFromHost(errno);
}

} // namespace Kyty::Kernel::FileSystem

#endif // KYTY_EMU_ENABLED
