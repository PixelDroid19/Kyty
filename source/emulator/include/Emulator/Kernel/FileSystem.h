#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::FileSystem {

struct FileStat
{
	uint32_t       st_dev;
	uint32_t       st_ino;
	uint16_t       st_mode;
	uint16_t       st_nlink;
	uint32_t       st_uid;
	uint32_t       st_gid;
	uint32_t       st_rdev;
	KernelTimespec st_atim;
	KernelTimespec st_mtim;
	KernelTimespec st_ctim;
	int64_t        st_size;
	int64_t        st_blocks;
	uint32_t       st_blksize;
	uint32_t       st_flags;
	uint32_t       st_gen;
	int32_t        st_lspare;
	KernelTimespec st_birthtim;
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
	unsigned int: (8 / 2) * (16 - static_cast<int>(sizeof(KernelTimespec)));
};

struct KernelPollFd
{
	int32_t  fd;
	int16_t  events;
	int16_t  revents;
};

KYTY_SUBSYSTEM_DEFINE(FileSystem);

void   Mount(const String& folder, const String& point);
void   Umount(const String& folder_or_point);
String GetRealFilename(const String& mounted_file_name);

// Score a same-directory package font candidate against a missing request.
// Higher is better; negative means the candidate is not a font file.
// Used when incomplete dumps omit SIE system fonts or weight variants under app0.
[[nodiscard]] int ScorePackageFontFallback(const String& requested_filename, const String& candidate_filename);

// When requested_host_path is missing, pick the best existing OTF/TTF/TTC in the
// same host directory by weight class (Heavy/Bold/Medium/Light). Returns the
// original path when no substitute exists (or when the exact file is present).
[[nodiscard]] String PreferPackageFontHostPath(const String& requested_host_path);

// When the exact host file is missing, try a known dump extension alias
// (e.g. .odx → .odxb for Astro FIXED packages). Returns an existing path or
// the original request when no alias hits.
[[nodiscard]] String PreferHostExtensionAlias(const String& requested_host_path);

// Some Astro path builders open /app0/<tree>/... when the package stores the
// tree under /app0/data/<tree>/... (e.g. /app0/prein/... → /app0/data/prein/...).
// When guest_path is under /app0/ and the mapped host file is missing, insert
// a data/ segment after app0 and return that host path if it exists (also
// applying PreferHostExtensionAlias). Otherwise returns the original host path.
[[nodiscard]] String PreferHostApp0DataSegment(const String& guest_path, const String& requested_host_path);

// After a successful OD open (.../odx/NAME.odxb), Astro may open bare companion
// extensions under app0 root (`/app0/.jxm`, `/app0/.skel`, `/app0/.anim`) when
// path composition loses the basename. Map those to package companions of the
// last OD: gfx/NAME.jxm, anim/NAME.skel, anim/NAME_anim_play.anim (then NAME.anim).
// last_od_host_path overrides the remembered OD path when non-empty (tests).
[[nodiscard]] String PreferHostOdCompanionAsset(const String& guest_path, const String& requested_host_path,
                                                const String& last_od_host_path = {});

int KYTY_SYSV_ABI     KernelOpen(const char* path, int flags, uint16_t mode);
int KYTY_SYSV_ABI     KernelClose(int d);
int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes);
int64_t KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset);
int64_t KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence);
int KYTY_SYSV_ABI     KernelStat(const char* path, FileStat* sb);
int KYTY_SYSV_ABI     KernelFstat(int d, FileStat* sb);
int KYTY_SYSV_ABI     KernelFtruncate(int d, int64_t length);
int KYTY_SYSV_ABI     KernelRename(const char* from, const char* to);
int KYTY_SYSV_ABI     KernelUnlink(const char* path);
int KYTY_SYSV_ABI     KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep);
int KYTY_SYSV_ABI     KernelGetdents(int fd, char* buf, int nbytes);
int KYTY_SYSV_ABI     KernelMkdir(const char* path, uint16_t mode);
int KYTY_SYSV_ABI     KernelRmdir(const char* path);
int KYTY_SYSV_ABI     KernelDup(int old_d);
int KYTY_SYSV_ABI     KernelDup2(int old_d, int new_d);
int KYTY_SYSV_ABI     KernelPoll(struct KernelPollFd* fds, uint32_t count, int /*timeout*/);
// sceKernelAprResolveFilepathsToIdsAndFileSizes: paths[count] → ids[count] (u32, optional)
// and sizes[count] (u64, optional). File ids are stable host-side hashes of the guest path.
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizes(const char* const* paths, uint64_t count, uint32_t* ids,
                                                             uint64_t* sizes);
// sceKernelAprResolveFilepathsToIds — same as above without sizes (NID WT-5NKy42fw).
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIds(const char* const* paths, uint64_t count, uint32_t* ids);
// Prefix + path resolve (Gen5). empty/null prefix is plain resolve.
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIds(const char* prefix, const char* const* paths, uint64_t count,
                                                           uint32_t* ids);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(const char* prefix, const char* const* paths,
                                                                       uint64_t count, uint32_t* ids, uint64_t* sizes);
// ForEach variants: fill optional per-path results[]; return OK or first error when results is null.
// When results is non-null, return success count (non-negative).
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsForEach(const char* const* paths, uint64_t count, uint32_t* ids,
                                                        int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizesForEach(const char* const* paths, uint64_t count, uint32_t* ids,
                                                                    uint64_t* sizes, int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsForEach(const char* prefix, const char* const* paths, uint64_t count,
                                                                  uint32_t* ids, int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach(const char* prefix, const char* const* paths,
                                                                              uint64_t count, uint32_t* ids, uint64_t* sizes,
                                                                              int32_t* results);
// Host path lookup for APR file ids minted by resolve APIs.
bool AprTryGetHostPath(uint32_t file_id, String* out_host_path);
// sceKernelAprGetFileSize / GetFileStat by previously resolved file id.
int KYTY_SYSV_ABI KernelAprGetFileSize(uint32_t file_id, uint64_t* size);
int KYTY_SYSV_ABI KernelAprGetFileStat(uint32_t file_id, FileStat* st);

// sceKernelAprSubmitCommandBuffer — NID eE4Szl8sil8.
// Observed SysV ABI: (cmd, 1, ptr, completion_ident, ptr).
int KYTY_SYSV_ABI KernelAprSubmitCommandBuffer(void* cmd, uint64_t arg1, void* arg2, uint64_t arg3, void* arg4);
// Submit variants with submission id / result blob (Gen5 APR waiters).
int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetId(void* cmd, uint64_t arg1, uint32_t* out_submission_id);
int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetResult(void* cmd, uint64_t arg1, void* result, uint32_t* out_submission_id);
int KYTY_SYSV_ABI KernelAprWaitCommandBuffer(uint32_t submission_id);

int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizes(const char* const* paths, uint64_t count, uint32_t* ids, uint64_t* sizes);
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIds(const char* const* paths, uint64_t count, uint32_t* ids);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIds(const char* prefix, const char* const* paths, uint64_t count,
                                                           uint32_t* ids);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(const char* prefix, const char* const* paths,
                                                                       uint64_t count, uint32_t* ids, uint64_t* sizes);
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsForEach(const char* const* paths, uint64_t count, uint32_t* ids,
                                                        int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizesForEach(const char* const* paths, uint64_t count, uint32_t* ids,
                                                                    uint64_t* sizes, int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsForEach(const char* prefix, const char* const* paths, uint64_t count,
                                                                  uint32_t* ids, int32_t* results);
int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach(const char* prefix, const char* const* paths,
                                                                              uint64_t count, uint32_t* ids, uint64_t* sizes,
                                                                              int32_t* results);
bool AprTryGetHostPath(uint32_t file_id, String* out_host_path);
int KYTY_SYSV_ABI KernelAprGetFileSize(uint32_t file_id, uint64_t* size);
int KYTY_SYSV_ABI KernelAprGetFileStat(uint32_t file_id, FileStat* st);
int KYTY_SYSV_ABI KernelAprSubmitCommandBuffer(void* cmd, uint64_t arg1, void* arg2, uint64_t arg3, void* arg4);
int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetId(void* cmd, uint64_t arg1, uint32_t* out_submission_id);
int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetResult(void* cmd, uint64_t arg1, void* result, uint32_t* out_submission_id);
int KYTY_SYSV_ABI KernelAprWaitCommandBuffer(uint32_t submission_id);

} // namespace Kyty::Libs::LibKernel::FileSystem

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_FILESYSTEM_H_ */
