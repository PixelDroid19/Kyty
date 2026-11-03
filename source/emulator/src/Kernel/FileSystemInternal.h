#ifndef EMULATOR_SRC_KERNEL_FILESYSTEMINTERNAL_H_
#define EMULATOR_SRC_KERNEL_FILESYSTEMINTERNAL_H_

// Narrow interface between FileSystem.cpp, which owns the descriptor table and the
// File internals, and the operations split out into FileSystemSync.cpp. Nothing
// outside src/Kernel should include this header.

namespace Kyty::Kernel::FileSystem {

// Maps a host errno value to the guest kernel error code.
int KernelErrorFromHost(int error);

// Flushes the stdio streams of an open guest descriptor, then syncs its host file
// (or the host directory). Expects d >= 3 and returns OK or a kernel error code.
int SyncOpenDescriptor(int d);

} // namespace Kyty::Kernel::FileSystem

#endif // EMULATOR_SRC_KERNEL_FILESYSTEMINTERNAL_H_
