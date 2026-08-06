#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/AmprPort.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Trace.h"
#include "Emulator/VideoFrameMemory.h"
#include "Emulator/Log.h"

#include <atomic>
#include <cstdlib>
#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unordered_map>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::FileSystem {

KERNEL_LIB_NAME();

static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name);

constexpr int DESCRIPTOR_MIN = 3;

constexpr uint8_t kStandardDescriptorMask = (1u << DESCRIPTOR_MIN) - 1u;

// Guest standard descriptors are logical handles. Their lifecycle must not
// affect the emulator process streams that provide diagnostics and input.
static std::atomic_uint8_t g_standard_descriptors {kStandardDescriptorMask};

// PS5 SSD files report a non-zero st_flags indicating hardware async-read
// capability. Unity (and other engines) check this field to decide whether
// APR (Async Parallel Read) is usable. A zero value causes the engine to
// reject the file for APR and fall back to synchronous I/O — or log
// "not considered suitable for apr reads flags:0x0".
constexpr uint32_t kPs5StFlagsAprCapable = 0x00000001u;

// ---------------------------------------------------------------------------
// Guest-writable sandbox
// ---------------------------------------------------------------------------
// Guest paths that do not fall under any explicit mount point (e.g. /devlog,
// /download0, /temp0, or Unity's root-level case-sensitivity probe) are mapped
// into a host-side sandbox directory so that mkdir/open(O_CREAT)/write succeed
// without touching the real filesystem root.

static Core::Mutex g_sandbox_mutex;
static String      g_sandbox_root; // trailing slash included
static bool        g_sandbox_initialized = false;

static String GetSandboxRoot()
{
	Core::LockGuard lock(g_sandbox_mutex);
	if (!g_sandbox_initialized)
	{
		g_sandbox_initialized = true;
		const char* env = std::getenv("KYTY_SANDBOX_DIR");
		if (env != nullptr && env[0] != '\0')
		{
			g_sandbox_root = String::FromUtf8(env).FixDirectorySlash();
		} else
		{
			g_sandbox_root = U"/tmp/kyty_sandbox/";
		}
		if (!Core::File::IsDirectoryExisting(g_sandbox_root))
		{
			Core::File::CreateDirectory(g_sandbox_root);
		}
	}
	return g_sandbox_root;
}

// ---------------------------------------------------------------------------
// Opt-in guest file operation trace
// ---------------------------------------------------------------------------
// KYTY_FS_TRACE=<substring> emits one stderr line per guest file operation whose
// guest path contains the substring ("*" matches every path). A guest that
// retries a failing load spins without bound, so the line count is capped by
// KYTY_FS_TRACE_LIMIT (default 100000) to keep host disk use bounded.

static const char* FsTraceFilter()
{
	static const char* filter = std::getenv("KYTY_FS_TRACE");
	return (filter != nullptr && filter[0] != '\0') ? filter : nullptr;
}

static void FsTrace(const char* op, const char* guest_path, int64_t argument, int64_t result)
{
	const char* filter = FsTraceFilter();
	if (filter == nullptr || guest_path == nullptr)
	{
		return;
	}
	if (std::strcmp(filter, "*") != 0 && std::strstr(guest_path, filter) == nullptr)
	{
		return;
	}

	static std::atomic_uint64_t emitted {0};
	static const uint64_t       limit = []
	{
		const char* spec = std::getenv("KYTY_FS_TRACE_LIMIT");
		const auto  parsed = (spec != nullptr) ? std::strtoull(spec, nullptr, 10) : 0;
		return parsed != 0 ? parsed : uint64_t {100000};
	}();

	const auto index = emitted.fetch_add(1, std::memory_order_relaxed);
	if (index >= limit)
	{
		if (index == limit)
		{
			KYTY_LOG_DEBUG( "KYTY_FS_TRACE: line limit reached; further tracing suppressed\n");
		}
		return;
	}

	KYTY_LOG_DEBUG( "KYTY_FS_TRACE: %-6s arg=%" PRId64 " result=%" PRId64 " path=%s\n", op, argument, result, guest_path);
}

// Create all intermediate directories for a host path (like mkdir -p).
static bool CreateDirectoryRecursive(const String& dir_path)
{
	if (dir_path.IsEmpty())
	{
		return false;
	}
	return Core::File::CreateDirectories(dir_path);
}

// Map an unmapped guest path into the sandbox. guest_path must start with '/'.
static String MapToSandbox(const String& guest_path)
{
	const String root = GetSandboxRoot();
	// Strip leading slash so we get sandbox/devlog/... not sandbox//devlog/...
	if (guest_path.StartsWith(U"/"))
	{
		return root + guest_path.RemoveFirst(1);
	}
	return root + guest_path;
}

// The guest runtime uses the POSIX entropy devices during Unity and libc
// initialization.  They are kernel devices on the console, rather than files
// belonging to the title, so treating them as ordinary unmapped paths sends
// them to the writable sandbox and makes every open fail.  Keep the host
// escape deliberately narrow and read-only: both PS5 devices map to the host
// non-blocking entropy source, never to a guest-controlled sandbox file.
static bool IsGuestEntropyDevice(const String& guest_path)
{
	return guest_path == U"/dev/urandom" || guest_path == U"/dev/random";
}

static String ResolveGuestDeviceFilename(const String& guest_path, const String& mapped_filename)
{
	return IsGuestEntropyDevice(guest_path) ? U"/dev/urandom" : mapped_filename;
}


class MountPoints
{
public:
	struct MountPair
	{
		String dir;
		String point;
	};

	MountPoints() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
	virtual ~MountPoints() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(MountPoints);

	void Mount(const String& folder, const String& point);
	void Umount(const String& folder_or_point);

	[[nodiscard]] String GetRealFilename(const String& mounted_file_name);
	[[nodiscard]] String GetRealDirectory(const String& mounted_directory);

private:
	Vector<MountPair> m_mount_pairs;
	Core::Mutex       m_mutex;
};

struct File
{
	Core::File                   f;
	String                       name;
	String                       real_name;
	std::atomic_bool             opened;
	std::atomic_bool             directory;
	std::atomic_uint32_t         status_flags {0};
	std::atomic_uint32_t         ref_count {1};
	Core::Mutex                  mutex;
	Vector<Core::File::DirEntry> dents;
	uint32_t                     dents_index;
};

class FileDescriptors
{
public:
	FileDescriptors() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
	virtual ~FileDescriptors() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FileDescriptors);

	int   CreateDescriptor();
	void  DeleteDescriptor(int d);
	int   DupDescriptor(int old_d);
	int   Dup2Descriptor(int old_d, int new_d);
	File* GetFile(int d);
	File* GetFile(const String& real_name);
	void  CloseAll();

private:
	void  ReleaseFile(File* file);
	void  EnsureDescriptorCapacity(uint32_t index);

	Vector<File*> m_files;
	Core::Mutex   m_mutex;
};

static MountPoints*     g_mount_points = nullptr;
static FileDescriptors* g_files        = nullptr;

// Defined with APR helpers; used by KernelOpen/KernelStat for package font fallback.
static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name);

static void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

int FileDescriptors::CreateDescriptor()
{
	Core::LockGuard lock(m_mutex);

	auto* file      = new File {};
	file->opened    = false;
	file->directory = false;
	file->ref_count = 1;

	int files_num = static_cast<int>(m_files.Size());
	for (int index = 0; index < files_num; index++)
	{
		if (m_files.At(index) == nullptr)
		{
			m_files[index] = file;
			return index + DESCRIPTOR_MIN;
		}
	}

	m_files.Add(file);
	return static_cast<int>(m_files.Size()) + DESCRIPTOR_MIN - 1;
}

void FileDescriptors::ReleaseFile(File* file)
{
	if (file == nullptr)
	{
		return;
	}

	const uint32_t remaining = file->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (remaining > 0)
	{
		return;
	}

	if (file->opened || !file->f.IsInvalid())
	{
		if (!file->directory)
		{
			file->f.Close();
		}
		file->opened = false;
	}

	delete file;
}

void FileDescriptors::EnsureDescriptorCapacity(uint32_t index)
{
	while (m_files.Size() <= index)
	{
		m_files.Add(nullptr);
	}
}

void FileDescriptors::DeleteDescriptor(int d)
{
	Core::LockGuard lock(m_mutex);

	auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);

	EXIT_IF(!m_files.IndexValid(index));
	EXIT_IF(m_files.At(index) == nullptr);

	auto* file = m_files.At(index);
	m_files[index] = nullptr;
	ReleaseFile(file);
}

int FileDescriptors::DupDescriptor(int old_d)
{
	Core::LockGuard lock(m_mutex);

	if (old_d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	const auto old_index = static_cast<uint32_t>(old_d - DESCRIPTOR_MIN);
	if (!m_files.IndexValid(old_index))
	{
		return KERNEL_ERROR_EBADF;
	}

	auto* file = m_files.At(old_index);
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	int new_fd = -1;
	const int  files_num = static_cast<int>(m_files.Size());
	for (int index = 0; index < files_num; index++)
	{
		if (m_files.At(static_cast<uint32_t>(index)) == nullptr)
		{
			new_fd = index + DESCRIPTOR_MIN;
			m_files[static_cast<uint32_t>(index)] = file;
			break;
		}
	}

	if (new_fd < 0)
	{
		m_files.Add(file);
		new_fd = static_cast<int>(m_files.Size()) + DESCRIPTOR_MIN - 1;
	}

	file->ref_count.fetch_add(1, std::memory_order_relaxed);
	return new_fd;
}

int FileDescriptors::Dup2Descriptor(int old_d, int new_d)
{
	Core::LockGuard lock(m_mutex);

	if (old_d < DESCRIPTOR_MIN || new_d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	const auto old_index = static_cast<uint32_t>(old_d - DESCRIPTOR_MIN);
	if (!m_files.IndexValid(old_index))
	{
		return KERNEL_ERROR_EBADF;
	}

	auto* source = m_files.At(old_index);
	if (source == nullptr || !source->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (old_d == new_d)
	{
		return new_d;
	}

	const auto new_index = static_cast<uint32_t>(new_d - DESCRIPTOR_MIN);
	EnsureDescriptorCapacity(new_index);

	auto* existing = m_files.At(new_index);
	if (existing != nullptr && existing != source)
	{
		m_files[new_index] = nullptr;
		ReleaseFile(existing);
	}

	source->ref_count.fetch_add(1, std::memory_order_relaxed);
	m_files[new_index] = source;
	return new_d;
}

File* FileDescriptors::GetFile(int d)
{
	Core::LockGuard lock(m_mutex);

	if (d < DESCRIPTOR_MIN)
	{
		return nullptr;
	}

	auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);

	if (!m_files.IndexValid(index))
	{
		return nullptr;
	}

	return m_files.At(index);
}

File* FileDescriptors::GetFile(const String& real_name)
{
	Core::LockGuard lock(m_mutex);

	for (auto* f: m_files)
	{
		if (f != nullptr && f->real_name == real_name)
		{
			return f;
		}
	}

	return nullptr;
}

void FileDescriptors::CloseAll()
{
	Core::LockGuard lock(m_mutex);

	for (auto& f: m_files)
	{
		if (f != nullptr && f->opened)
		{
			f->f.Close();
			delete f;
			f = nullptr;
		}
	}
}

void MountPoints::Mount(const String& folder, const String& point)
{
	Core::LockGuard lock(m_mutex);

	auto folder_str = folder.FixDirectorySlash();
	auto point_str  = point.FixDirectorySlash();

	Umount(folder_str);
	Umount(point_str);

	MountPair p;
	p.dir   = folder_str;
	p.point = point_str;

	m_mount_pairs.Add(p);
}

void MountPoints::Umount(const String& folder_or_point)
{
	Core::LockGuard lock(m_mutex);

	auto folder_or_point_str = folder_or_point.FixDirectorySlash();

	if (auto index =
	        m_mount_pairs.Find(folder_or_point_str, [](const MountPair& p, const String& s) { return p.dir == s || p.point == s; });
	    m_mount_pairs.IndexValid(index))
	{
		m_mount_pairs.RemoveAt(index);
	}
}

String MountPoints::GetRealFilename(const String& mounted_file_name)
{
	Core::LockGuard lock(m_mutex);

	auto mounted_path = mounted_file_name.FixFilenameSlash().DirectoryWithoutFilename();

	if (auto index = m_mount_pairs.Find(mounted_path, [](const MountPair& p, const String& s) { return s.StartsWith(p.point); });
	    m_mount_pairs.IndexValid(index))
	{
		const auto& p = m_mount_pairs.At(index);
		return p.dir + mounted_file_name.RemoveFirst(p.point.Size());
	}

		// No mount matched — redirect to the writable sandbox so that file
		// creation and writes succeed without touching the host root.
		return MapToSandbox(mounted_file_name);
}

String MountPoints::GetRealDirectory(const String& mounted_directory)
{
	Core::LockGuard lock(m_mutex);

	auto mounted_path = mounted_directory.FixDirectorySlash();

	if (auto index = m_mount_pairs.Find(mounted_path, [](const MountPair& p, const String& s) { return s.StartsWith(p.point); });
	    m_mount_pairs.IndexValid(index))
	{
		const auto& p = m_mount_pairs.At(index);
		return p.dir + mounted_directory.RemoveFirst(p.point.Size());
	}

		// No mount matched — redirect to the writable sandbox.
		return MapToSandbox(mounted_directory);
}

void FileSystemSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	g_mount_points = new MountPoints;
	g_files        = new FileDescriptors;
	g_standard_descriptors.store(kStandardDescriptorMask, std::memory_order_release);

	// Eagerly initialize the sandbox root so the directory exists before any
	// guest filesystem call.
	GetSandboxRoot();
}

void FileSystemSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent)
{
	if (g_files != nullptr)
	{
		g_files->CloseAll();
	}
}

void FileSystemSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent)
{
	if (g_files != nullptr)
	{
		g_files->CloseAll();
	}
}

void Mount(const String& folder, const String& point)
{
	EXIT_IF(g_mount_points == nullptr);

	g_mount_points->Mount(folder, point);
}

void Umount(const String& folder_or_point)
{
	EXIT_IF(g_mount_points == nullptr);

	g_mount_points->Umount(folder_or_point);
}

bool IsMounted()
{
	return g_mount_points != nullptr;
}

String GetRealFilename(const String& mounted_file_name)
{
	EXIT_IF(g_mount_points == nullptr);

	return g_mount_points->GetRealFilename(mounted_file_name);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static int KYTY_SYSV_ABI KernelOpenResolved(const char* path, int flags, uint16_t mode)
{
	EXIT_IF(g_mount_points == nullptr || g_files == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto       flags_u       = static_cast<uint32_t>(flags);
	const auto status_flags  = flags_u;

	KYTY_LOG_DEBUG("\t path = %s\n", path);
	KYTY_LOG_DEBUG("\t flags = %08" PRIx32 "\n", flags_u);
	KYTY_LOG_DEBUG("\t mode = %04" PRIx16 "\n", mode);

	bool nonblock  = (flags_u & 0x0004u) != 0;
	bool append    = (flags_u & 0x0008u) != 0;
	bool fsync     = (flags_u & 0x0080u) != 0;
	bool sync      = (flags_u & 0x0080u) != 0;
	bool creat     = (flags_u & 0x0200u) != 0;
	bool trunc     = (flags_u & 0x0400u) != 0;
	bool excl      = (flags_u & 0x0800u) != 0;
	bool dsync     = (flags_u & 0x1000u) != 0;
	bool direct    = (flags_u & 0x00010000u) != 0;
	bool directory = (flags_u & 0x00020000u) != 0;

	// Core::File has buffered host I/O. Its close/flush boundary is the strongest
	// durability contract available here; the PS5 advisory sync/direct flags do
	// not change guest-visible read/write semantics.
	(void)fsync;
	(void)sync;
	(void)dsync;
	(void)direct;

	// nonblock on regular files is advisory-only; safely ignore it.
	(void)nonblock;

	flags_u &= 0x3u;

	Core::File::Mode rw_mode = Core::File::Mode::Read;

	switch (flags_u)
	{
		case 0: rw_mode = Core::File::Mode::Read; break;
		case 1: rw_mode = Core::File::Mode::Write; break;
		case 2: rw_mode = Core::File::Mode::ReadWrite; break;
		default: EXIT("invalid flag_u: %u\n", flags_u);
	}

	if (directory && rw_mode != Core::File::Mode::Read) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (directory && (trunc || creat)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	int   descriptor = g_files->CreateDescriptor();
	auto* file       = g_files->GetFile(descriptor);

	EXIT_IF(file == nullptr || file->opened || file->directory);

	file->name = path;
	const bool entropy_device = IsGuestEntropyDevice(file->name);
	if (entropy_device && (rw_mode != Core::File::Mode::Read || creat || trunc || append || directory))
	{
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_EACCES;
	}
	if (directory)
	{
		file->real_name = g_mount_points->GetRealDirectory(file->name);
	}
	else
	{
		// Package font fallback for incomplete dumps (SIE system fonts under app0).
		file->real_name = ResolveGuestDeviceFilename(
			file->name, ResolveExistingHostFile(file->name, g_mount_points->GetRealFilename(file->name)));
	}

	if (trunc && rw_mode == Core::File::Mode::Read)
	{
		return KERNEL_ERROR_EACCES;
	}

	bool dir_exist = Core::File::IsDirectoryExisting(file->real_name);

	if (directory || dir_exist)
	{
		if (!dir_exist)
		{
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_ENOTDIR;
		}

		if (!directory && rw_mode != Core::File::Mode::Read) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
		if (!directory && (trunc || creat)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

		const auto host_entries = Core::File::GetDirEntries(file->real_name);
		file->dents.Clear();
		for (const auto& entry: host_entries)
		{
			if (entry.name != U"." && entry.name != U"..")
			{
				file->dents.Add(entry);
			}
		}
		file->dents_index = 0;
		file->directory   = true;

		KYTY_LOG_DEBUG("\tOpen dir: " FG_WHITE BOLD "%s" DEFAULT ", entries = %" PRIu32 ", " FG_GREEN "[ok]" FG_DEFAULT "\n",
		       file->real_name.C_Str(), file->dents.Size());

		for (const auto& f: file->dents)
		{
			KYTY_LOG_DEBUG("\t\t%s %s\n", f.is_file ? "[file]" : "[dir ]", f.name.C_Str());
		}
	} else
	{
		bool       result      = false;
		const bool file_exists = Core::File::IsFileExisting(file->real_name);

		if (creat && excl && file_exists)
		{
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EEXIST;
		}

		if (creat && (trunc || !file_exists))
		{
			// Ensure parent directories exist for sandbox-mapped paths.
			const String parent_dir = file->real_name.DirectoryWithoutFilename();
			if (!parent_dir.IsEmpty() && !Core::File::IsDirectoryExisting(parent_dir))
			{
				CreateDirectoryRecursive(parent_dir);
			}
			result = file->f.Create(file->real_name);

			KYTY_LOG_DEBUG("\tCreate: " FG_WHITE BOLD "%s" DEFAULT ", %s\n", file->real_name.C_Str(),
			       (result ? FG_GREEN "[ok]" FG_DEFAULT : FG_RED "[fail]" FG_DEFAULT));

			if (result && !trunc)
			{
				file->f.Close();
				result = file->f.Open(file->real_name, rw_mode);
			}
		} else
		{
			result = file->f.Open(file->real_name, rw_mode);

			KYTY_LOG_DEBUG("\tOpen: " FG_WHITE BOLD "%s" DEFAULT ", %s\n", file->real_name.C_Str(),
			       (result ? FG_GREEN "[ok]" FG_DEFAULT : FG_RED "[fail]" FG_DEFAULT));
		}

		if (result && trunc)
		{
			result = file->f.Truncate(0);
		}
		if (result && append)
		{
			result = file->f.Seek(file->f.Size());
		}

		if (!result || file->f.IsInvalid())
		{
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EACCES;
		}
	}

	file->opened = true;
	file->status_flags.store(status_flags, std::memory_order_release);
	return descriptor;
}

int KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode)
{
	PRINT_NAME();

	const int result = KernelOpenResolved(path, flags, mode);
	FsTrace("open", path, flags, result);
	return result;
}

int KYTY_SYSV_ABI KernelClose(int d)
{
	PRINT_NAME();

	if (d < DESCRIPTOR_MIN)
	{
		if (d < 0)
		{
			return KERNEL_ERROR_EBADF;
		}

		// Guest file descriptors start at DESCRIPTOR_MIN.  Values below that
		// range are reserved process descriptors and are also used by runtimes
		// as invalid-handle sentinels.  Never let a guest close recycle one of
		// the host's standard descriptors into a real file descriptor.
		return OK;
	}

	EXIT_IF(g_files == nullptr);

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	EXIT_IF(!file->opened);

	if (!file->directory)
	{
		file->f.Close();
	}

	file->opened = false;

	KYTY_LOG_DEBUG("\tClose: " FG_WHITE BOLD "%s" DEFAULT "\n", file->real_name.C_Str());

	FsTrace("close", file->name.C_Str(), d, OK);

	g_files->DeleteDescriptor(d);

	return OK;
}

bool KernelIsStandardDescriptorOpen(int d)
{
	if (d < 0 || d >= DESCRIPTOR_MIN)
	{
		return false;
	}

	const auto descriptor_bit = static_cast<uint8_t>(1u << d);
	return (g_standard_descriptors.load(std::memory_order_acquire) & descriptor_bit) != 0;
}

int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(!file->opened);

	if (nbytes > UINT_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	file->mutex.Lock();

	bool     is_invalid = file->f.IsInvalid();
	uint32_t bytes_read = 0;
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);

	file->mutex.Unlock();
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(buf), bytes_read);

	if (is_invalid)
	{
		KYTY_LOG_DEBUG("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tRead %u bytes from: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_read, file->real_name.C_Str());

	FsTrace("read", file->name.C_Str(), static_cast<int64_t>(nbytes), bytes_read);

	return bytes_read;
}

int64_t KYTY_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(!file->opened);

	if (nbytes > UINT_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	file->mutex.Lock();

	bool     is_invalid    = file->f.IsInvalid();
	uint32_t bytes_written = 0;
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);

	file->mutex.Unlock();

	if (is_invalid)
	{
		KYTY_LOG_DEBUG("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tWrite %u bytes to: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_written, file->real_name.C_Str());

	return bytes_written;
}

int64_t KYTY_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(!file->opened);

	if (nbytes > UINT_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	file->mutex.Lock();

	bool     is_invalid = file->f.IsInvalid();
	auto     pos        = file->f.Tell();
	uint32_t bytes_read = 0;
	file->f.Seek(offset);
	file->f.Read(buf, static_cast<uint32_t>(nbytes), &bytes_read);
	file->f.Seek(pos);

	file->mutex.Unlock();
	Kyty::Emulator::VideoFrameMemory::NotifyHostWrite(reinterpret_cast<uint64_t>(buf), bytes_read);

	if (is_invalid)
	{
		KYTY_LOG_DEBUG("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tRead %u bytes (pos = %" PRId64 ") from: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_read, offset, file->real_name.C_Str());

	FsTrace("pread", file->name.C_Str(), offset, bytes_read);

	return bytes_read;
}

int64_t KYTY_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(!file->opened);

	if (nbytes > UINT_MAX) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	file->mutex.Lock();

	bool     is_invalid    = file->f.IsInvalid();
	auto     pos           = file->f.Tell();
	uint32_t bytes_written = 0;
	file->f.Seek(offset);
	file->f.Write(buf, static_cast<uint32_t>(nbytes), &bytes_written);
	file->f.Seek(pos);

	file->mutex.Unlock();

	if (is_invalid)
	{
		KYTY_LOG_DEBUG("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tWrite %u bytes (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_written, offset, file->real_name.C_Str());

	return bytes_written;
}

int64_t KYTY_SYSV_ABI KernelLseek(int d, int64_t offset, int whence)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(!file->opened);

	file->mutex.Lock();

	bool is_invalid = file->f.IsInvalid();

	if (whence == 1)
	{
		offset = static_cast<int64_t>(file->f.Tell()) + offset;
		whence = 0;
	}

	if (whence == 2)
	{
		offset = static_cast<int64_t>(file->f.Size()) + offset;
		whence = 0;
	}

	if (whence != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	file->f.Seek(offset);
	auto pos = static_cast<int64_t>(file->f.Tell());

	EXIT_IF(pos != offset);

	file->mutex.Unlock();

	if (is_invalid)
	{
		KYTY_LOG_DEBUG("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tLseek (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", offset, file->real_name.C_Str());

	return pos;
}

int KYTY_SYSV_ABI KernelStat(const char* path, FileStat* sb)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr);

	if (path == nullptr || sb == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KYTY_LOG_DEBUG("\t KernelStat: %s\n", path);

	String path_s         = String::FromUtf8(path);
	auto   real_file_name = ResolveGuestDeviceFilename(path_s, ResolveExistingHostFile(path_s, g_mount_points->GetRealFilename(path_s)));
	auto   real_directory = g_mount_points->GetRealDirectory(path_s);

	bool is_dir  = Core::File::IsDirectoryExisting(real_file_name) || Core::File::IsDirectoryExisting(real_directory);
	bool is_file = Core::File::IsFileExisting(real_file_name);

	if (!is_dir && !is_file)
	{
		KYTY_LOG_DEBUG("\t file not found\n");
		return KERNEL_ERROR_ENOENT;
	}

	if (is_dir && is_file) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	memset(sb, 0, sizeof(FileStat));

	sb->st_mode = 0000777u | (is_dir ? 0040000u : 0100000u);
	sb->st_flags = is_dir ? 0u : kPs5StFlagsAprCapable;

	Core::DateTime at;
	Core::DateTime wt;

	if (is_dir)
	{
		sb->st_size    = 0;
		sb->st_blksize = 512;
		sb->st_blocks  = 0;
	} else
	{
		sb->st_size    = static_cast<int64_t>(Core::File::Size(real_file_name));
		sb->st_blksize = 512;
		sb->st_blocks  = (sb->st_size + 511) / 512;

		Core::File::GetLastAccessAndWriteTimeUTC(real_file_name, &at, &wt);
	}

	sec_to_timespec(&sb->st_atim, at.ToUnix());
	sec_to_timespec(&sb->st_mtim, wt.ToUnix());
	sb->st_ctim     = sb->st_atim;
	sb->st_birthtim = sb->st_mtim;

	return OK;
}

int KYTY_SYSV_ABI KernelFstat(int d, FileStat* sb)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (sb == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		// libc FILE* functions are backed by the host C runtime. A guest that
		// calls fopen -> fileno -> fstat therefore presents a valid host
		// descriptor which is intentionally absent from Kyty's guest table.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		struct _stat64 host_stat {};
		if (::_fstat64(d, &host_stat) != 0)
#else
		struct stat host_stat {};
		if (::fstat(d, &host_stat) != 0)
#endif
		{
			return KERNEL_ERROR_EBADF;
		}

		memset(sb, 0, sizeof(FileStat));
		sb->st_dev   = static_cast<uint32_t>(host_stat.st_dev);
		sb->st_ino   = static_cast<uint32_t>(host_stat.st_ino);
		sb->st_mode  = static_cast<uint16_t>(host_stat.st_mode);
		sb->st_nlink = static_cast<uint16_t>(host_stat.st_nlink);
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		sb->st_uid  = static_cast<uint32_t>(host_stat.st_uid);
		sb->st_gid  = static_cast<uint32_t>(host_stat.st_gid);
		sb->st_rdev = static_cast<uint32_t>(host_stat.st_rdev);
#endif
		sb->st_size = static_cast<int64_t>(host_stat.st_size);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		sb->st_flags       = ((host_stat.st_mode & _S_IFMT) == _S_IFREG) ? kPs5StFlagsAprCapable : 0u;
		sb->st_blksize     = 512;
		sb->st_blocks      = (sb->st_size + 511) / 512;
		sb->st_atim.tv_sec = static_cast<int64_t>(host_stat.st_atime);
		sb->st_mtim.tv_sec = static_cast<int64_t>(host_stat.st_mtime);
		sb->st_ctim.tv_sec = static_cast<int64_t>(host_stat.st_ctime);
#else
		sb->st_flags        = S_ISREG(host_stat.st_mode) ? kPs5StFlagsAprCapable : 0u;
		sb->st_blocks       = static_cast<int64_t>(host_stat.st_blocks);
		sb->st_blksize      = static_cast<uint32_t>(host_stat.st_blksize);
		sb->st_atim.tv_sec  = static_cast<int64_t>(host_stat.st_atim.tv_sec);
		sb->st_atim.tv_nsec = static_cast<int64_t>(host_stat.st_atim.tv_nsec);
		sb->st_mtim.tv_sec  = static_cast<int64_t>(host_stat.st_mtim.tv_sec);
		sb->st_mtim.tv_nsec = static_cast<int64_t>(host_stat.st_mtim.tv_nsec);
		sb->st_ctim.tv_sec  = static_cast<int64_t>(host_stat.st_ctim.tv_sec);
		sb->st_ctim.tv_nsec = static_cast<int64_t>(host_stat.st_ctim.tv_nsec);
#endif
		sb->st_birthtim = sb->st_mtim;
		return OK;
	}

	EXIT_IF(!file->opened);

	KYTY_LOG_DEBUG("\tKernelFstat: %s\n", file->real_name.C_Str());

	memset(sb, 0, sizeof(FileStat));

	sb->st_mode = 0000777u | (file->directory ? 0040000u : 0100000u);
	sb->st_flags = file->directory ? 0u : kPs5StFlagsAprCapable;

	Core::DateTime at;
	Core::DateTime wt;

	if (!file->directory)
	{
		file->mutex.Lock();

		bool is_invalid = file->f.IsInvalid();
		auto size       = file->f.Size();
		file->f.GetLastAccessAndWriteTimeUTC(&at, &wt);

		file->mutex.Unlock();

		if (is_invalid)
		{
			KYTY_LOG_DEBUG("\tfile is invalid\n");
			return KERNEL_ERROR_EIO;
		}

		sb->st_size    = static_cast<int64_t>(size);
		sb->st_blksize = 512;
		sb->st_blocks  = (sb->st_size + 511) / 512;
	} else
	{
		sb->st_size    = 0;
		sb->st_blksize = 512;
		sb->st_blocks  = 0;
	}

	sec_to_timespec(&sb->st_atim, at.ToUnix());
	sec_to_timespec(&sb->st_mtim, wt.ToUnix());
	sb->st_ctim     = sb->st_atim;
	sb->st_birthtim = sb->st_mtim;

	return OK;
}

int KYTY_SYSV_ABI KernelFtruncate(int d, int64_t length)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (length < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}

	Core::LockGuard lock(file->mutex);
	if (file->f.IsInvalid())
	{
		return KERNEL_ERROR_EIO;
	}

	return file->f.Truncate(static_cast<uint64_t>(length)) ? OK : KERNEL_ERROR_EIO;
}

int KYTY_SYSV_ABI KernelFcntl(int d, int command, int64_t argument)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	constexpr int      f_getfl              = 3;
	constexpr int      f_setfl              = 4;
	constexpr uint32_t mutable_status_flags = 0x0004u | 0x0008u;
	switch (command)
	{
		case f_getfl: return static_cast<int>(file->status_flags.load(std::memory_order_acquire));
		case f_setfl:
		{
			const auto requested = static_cast<uint32_t>(argument);
			auto       current   = file->status_flags.load(std::memory_order_acquire);
			current              = (current & ~mutable_status_flags) | (requested & mutable_status_flags);
			file->status_flags.store(current, std::memory_order_release);
			return OK;
		}
		default: return KERNEL_ERROR_EINVAL;
	}
}

int KYTY_SYSV_ABI KernelGetReadAvailability(int d, uint64_t* available)
{
	if (available == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}
	*available = 0;

	EXIT_IF(g_files == nullptr);
	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	Core::LockGuard lock(file->mutex);
	if (file->directory)
	{
		const uint32_t entry_count = file->dents.Size() + 2;
		*available                 = file->dents_index < entry_count ? entry_count - file->dents_index : 0;
		return OK;
	}
	if (file->f.IsInvalid())
	{
		return KERNEL_ERROR_EIO;
	}

	*available = file->f.Remaining();
	return OK;
}

int KYTY_SYSV_ABI KernelGetWriteAvailability(int d, bool* available)
{
	if (available == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}
	*available = false;

	EXIT_IF(g_files == nullptr);
	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	Core::LockGuard lock(file->mutex);
	if (file->directory || file->f.IsInvalid())
	{
		return file->directory ? KERNEL_ERROR_EISDIR : KERNEL_ERROR_EIO;
	}

	const uint32_t access_mode = file->status_flags.load(std::memory_order_acquire) & 0x3u;
	*available                 = access_mode == 1u || access_mode == 2u;
	return OK;
}

int KYTY_SYSV_ABI KernelUnlink(const char* path)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr);
	EXIT_IF(g_files == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto path_s         = String::FromUtf8(path);
	auto real_file_name = g_mount_points->GetRealFilename(path_s);
	auto real_directory = g_mount_points->GetRealDirectory(path_s);

	// Allow unlink even if the file descriptor is open.

	bool is_dir  = Core::File::IsDirectoryExisting(real_file_name) || Core::File::IsDirectoryExisting(real_directory);
	bool is_file = Core::File::IsFileExisting(real_file_name);

	if (is_dir)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (!is_file)
	{
		return KERNEL_ERROR_ENOENT;
	}

	bool ok = Core::File::DeleteFile(real_file_name);

	if (!ok)
	{
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tKernelUnlink: %s\n", path);

	return OK;
}

int KYTY_SYSV_ABI KernelRename(const char* from, const char* to)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr);

	if (from == nullptr || to == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	const auto from_s = String::FromUtf8(from);
	const auto to_s   = String::FromUtf8(to);
	const auto from_real =
	    ResolveExistingHostFile(from_s, g_mount_points->GetRealFilename(from_s));
	const auto to_real = g_mount_points->GetRealFilename(to_s);

	if (!Core::File::IsFileExisting(from_real) && !Core::File::IsDirectoryExisting(from_real))
	{
		return KERNEL_ERROR_ENOENT;
	}

	if (!Core::File::MoveFile(from_real, to_real))
	{
		return KERNEL_ERROR_EIO;
	}

	KYTY_LOG_DEBUG("\tKernelRename: %s -> %s\n", from, to);
	return OK;
}

int KYTY_SYSV_ABI KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (fd < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(fd);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (!file->directory || nbytes < 32)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(!file->opened);
	Core::LockGuard lock(file->mutex);

	KYTY_LOG_DEBUG("\t dir    = %s\n", file->real_name.C_Str());
	KYTY_LOG_DEBUG("\t nbytes = %d\n", nbytes);
	KYTY_LOG_DEBUG("\t index = %d\n", file->dents_index);

	const uint32_t entry_count = file->dents.Size() + 2;
	if (file->dents_index > entry_count)
	{
		return KERNEL_ERROR_EINVAL;
	}

	if (basep != nullptr)
	{
		*basep = file->dents_index;
	}

	uint32_t written = 0;
	while (file->dents_index < entry_count)
	{
		const bool   dot_entry = file->dents_index < 2;
		const String name      = dot_entry ? (file->dents_index == 0 ? U"." : U"..") : file->dents.At(file->dents_index - 2).name;
		const bool   is_file   = !dot_entry && file->dents.At(file->dents_index - 2).is_file;
		const auto   name_utf8 = name.utf8_str();
		const auto   name_size = name_utf8.Size() - 1;
		if (name_size > UINT8_MAX)
		{
			return KERNEL_ERROR_ENAMETOOLONG;
		}

		const uint32_t record_size = (8u + name_size + 1u + 3u) & ~3u;
		if (record_size > static_cast<uint32_t>(nbytes) - written)
		{
			break;
		}

		char* record = buf + written;
		std::memset(record, 0, record_size);
		uint32_t file_number = name.Hash();
		if (file_number == 0)
		{
			file_number = file->dents_index + 1;
		}
		*reinterpret_cast<uint32_t*>(record + 0) = file_number;
		*reinterpret_cast<uint16_t*>(record + 4) = static_cast<uint16_t>(record_size);
		*reinterpret_cast<uint8_t*>(record + 6)  = is_file ? 8 : 4;
		*reinterpret_cast<uint8_t*>(record + 7)  = static_cast<uint8_t>(name_size);
		std::memcpy(record + 8, name_utf8.GetDataConst(), name_size + 1);

		KYTY_LOG_DEBUG("\t name  = %s\n", name_utf8.GetDataConst());
		written += record_size;
		file->dents_index++;
	}

	return static_cast<int>(written);
}

int KYTY_SYSV_ABI KernelGetdents(int fd, char* buf, int nbytes)
{
	PRINT_NAME();

	return KernelGetdirentries(fd, buf, nbytes, nullptr);
}

int KYTY_SYSV_ABI KernelMkdir(const char* path, uint16_t mode)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_files == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KYTY_LOG_DEBUG("\t path = %s\n", path);
	KYTY_LOG_DEBUG("\t mode = %04" PRIx16 "\n", mode);

	String real_name = g_mount_points->GetRealDirectory(String::FromUtf8(path));

	if (Core::File::IsDirectoryExisting(real_name))
	{
		return KERNEL_ERROR_EEXIST;
	}

	// Ensure parent directories exist (sandbox paths may be nested).
	if (!CreateDirectoryRecursive(real_name))
	{
		return KERNEL_ERROR_EIO;
	}

	if (!Core::File::IsDirectoryExisting(real_name))
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelRmdir(const char* path)
{
	PRINT_NAME();
	EXIT_IF(g_mount_points == nullptr);
	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	KYTY_LOG_DEBUG("\t path = %s\n", path);
	const String real_name = g_mount_points->GetRealDirectory(String::FromUtf8(path));
	if (!Core::File::IsDirectoryExisting(real_name))
	{
		return KERNEL_ERROR_ENOENT;
	}
	if (!Core::File::DeleteDirectory(real_name))
	{
		return KERNEL_ERROR_EIO;
	}
	return OK;
}

static uint32_t AprStableFileId(const char* guest_path)
{
	// FNV-1a 32-bit over the guest path bytes. Stable across runs; not a firmware
	// hash — only needs to be unique enough for subsequent APR look-ups by id.
	uint32_t hash = 2166136261u;
	if (guest_path != nullptr)
	{
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(guest_path); *p != 0; ++p)
		{
			hash ^= *p;
			hash *= 16777619u;
		}
	}
	if (hash == 0)
	{
		hash = 1;
	}
	return hash;
}

static Core::Mutex                          g_apr_mutex;
static std::unordered_map<uint32_t, String>  g_apr_id_to_host;
static uint32_t                             g_apr_next_submission_id = 1;
static std::unordered_map<uint32_t, uint64_t> g_apr_submissions; // id → cmd (diagnostic)

// Weight classes for package-font substitution (incomplete dumps / SIE fonts under app0).
enum class PackageFontWeight : int
{
	Light    = 0,
	Regular  = 1,
	Medium   = 2,
	Bold     = 3,
	Heavy    = 4,
};

static bool IsPackageFontExtension(const String& name_lower)
{
	return name_lower.EndsWith(U".otf") || name_lower.EndsWith(U".ttf") || name_lower.EndsWith(U".ttc");
}

static PackageFontWeight ClassifyPackageFontWeight(const String& name_lower)
{
	// More specific tokens first (xbold/xbd before bold).
	if (name_lower.ContainsStr(U"heavy") || name_lower.ContainsStr(U"black") || name_lower.ContainsStr(U"xbold") ||
	    name_lower.ContainsStr(U"xbd") || name_lower.ContainsStr(U"blk"))
	{
		return PackageFontWeight::Heavy;
	}
	if (name_lower.ContainsStr(U"bold"))
	{
		return PackageFontWeight::Bold;
	}
	if (name_lower.ContainsStr(U"medium") || name_lower.ContainsStr(U"book"))
	{
		return PackageFontWeight::Medium;
	}
	if (name_lower.ContainsStr(U"light") || name_lower.ContainsStr(U"thin"))
	{
		return PackageFontWeight::Light;
	}
	return PackageFontWeight::Regular;
}

int ScorePackageFontFallback(const String& requested_filename, const String& candidate_filename)
{
	const String req = requested_filename.FilenameWithoutDirectory().ToLower();
	const String can = candidate_filename.FilenameWithoutDirectory().ToLower();
	if (req.IsEmpty() || can.IsEmpty() || !IsPackageFontExtension(can))
	{
		return -1;
	}
	if (req == can)
	{
		return 100000;
	}
	if (!IsPackageFontExtension(req))
	{
		return -1;
	}

	const int req_w = static_cast<int>(ClassifyPackageFontWeight(req));
	const int can_w = static_cast<int>(ClassifyPackageFontWeight(can));
	int       score = 1000 - (req_w > can_w ? req_w - can_w : can_w - req_w) * 200;

	// Prefer candidates that share a leading family token (before first '-' or '_').
	const auto family_token = [](const String& n) -> String {
		uint32_t cut = n.FindIndex(U'-');
		const uint32_t us = n.FindIndex(U'_');
		if (us != Core::STRING_INVALID_INDEX && (cut == Core::STRING_INVALID_INDEX || us < cut))
		{
			cut = us;
		}
		return cut == Core::STRING_INVALID_INDEX ? n : n.Left(cut);
	};
	const String req_fam = family_token(req);
	const String can_fam = family_token(can);
	if (!req_fam.IsEmpty() && req_fam == can_fam)
	{
		score += 300;
	}
	// Mild preference for larger/heavier siblings when request is Heavy/Black (SIE system fonts).
	if (req_w >= static_cast<int>(PackageFontWeight::Heavy) && can_w >= static_cast<int>(PackageFontWeight::Bold))
	{
		score += 50;
	}
	return score;
}

String PreferPackageFontHostPath(const String& requested_host_path)
{
	if (requested_host_path.IsEmpty())
	{
		return requested_host_path;
	}
	if (Core::File::IsFileExisting(requested_host_path))
	{
		return requested_host_path;
	}

	const String requested_name = requested_host_path.FilenameWithoutDirectory();
	const String dir            = requested_host_path.DirectoryWithoutFilename();
	if (requested_name.IsEmpty() || dir.IsEmpty() || !IsPackageFontExtension(requested_name.ToLower()))
	{
		return requested_host_path;
	}
	if (!Core::File::IsDirectoryExisting(dir))
	{
		return requested_host_path;
	}

	const auto entries = Core::File::GetDirEntries(dir);
	int        best_score = -1;
	String     best_path;
	for (const auto& entry: entries)
	{
		if (!entry.is_file)
		{
			continue;
		}
		const int score = ScorePackageFontFallback(requested_name, entry.name);
		if (score > best_score)
		{
			best_score = score;
			best_path  = dir + entry.name;
		}
	}
	if (best_score >= 0 && !best_path.IsEmpty() && Core::File::IsFileExisting(best_path))
	{
		KYTY_LOG_DEBUG("\t package font fallback: %s -> %s (score=%d)\n", requested_host_path.C_Str(), best_path.C_Str(), best_score);
		return best_path;
	}
	return requested_host_path;
}

String PreferHostExtensionAlias(const String& requested_host_path)
{
	if (requested_host_path.IsEmpty() || Core::File::IsFileExisting(requested_host_path))
	{
		return requested_host_path;
	}
	const String lower = requested_host_path.ToLower();
	// Astro FIXED dumps ship object defs as .odxb while guest requests .odx
	// (ObjectDefinition.cpp: "odx not found [prein/effects/odx/....odx]").
	if (lower.EndsWith(U".odx"))
	{
		const String alias = requested_host_path + U"b";
		if (Core::File::IsFileExisting(alias))
		{
			KYTY_LOG_DEBUG("\t host extension alias: %s -> %s\n", requested_host_path.C_Str(), alias.C_Str());
			return alias;
		}
	}
	return requested_host_path;
}

String PreferHostApp0DataSegment(const String& guest_path, const String& requested_host_path)
{
	if (requested_host_path.IsEmpty() || Core::File::IsFileExisting(requested_host_path))
	{
		return requested_host_path;
	}
	// Guest open of /app0/prein/... when package layout is /app0/data/prein/...
	// (observed: ODX resolve miss host=ROOT/prein/... while file is ROOT/data/prein/...).
	const String guest = guest_path.FixFilenameSlash();
	if (!guest.StartsWith(U"/app0/") || guest.StartsWith(U"/app0/data/"))
	{
		return requested_host_path;
	}
	// Do not rewrite known app0 roots that live next to data/.
	const String rest  = guest.RemoveFirst(6); // strip "/app0/"
	const auto   parts = rest.Split(U"/");
	if (!parts.IndexValid(0))
	{
		return requested_host_path;
	}
	const String first = parts.At(0).ToLower();
	// Skip known package roots and single-file app0 entries (eboot.bin, args.txt, ...).
	if (first.IsEmpty() || first == U"data" || first == U"sce_sys" || first == U"sce_module" || first == U"fakelib" ||
	    first.ContainsChar(U'.'))
	{
		return requested_host_path;
	}

	// Prefer string rewrite on the already-mapped host path so unit tests need no mount:
	// host ROOT/prein/... → ROOT/data/prein/...
	const String host   = requested_host_path.FixFilenameSlash();
	const String needle = U"/" + parts.At(0) + U"/";
	const String insert = U"/data/" + parts.At(0) + U"/";
	String       alt_host;
	if (host.ContainsStr(needle))
	{
		alt_host = host.ReplaceStr(needle, insert);
	}
	else if (g_mount_points != nullptr)
	{
		alt_host = g_mount_points->GetRealFilename(U"/app0/data/" + rest);
	}
	else
	{
		return requested_host_path;
	}
	if (alt_host == host)
	{
		return requested_host_path;
	}

	if (Core::File::IsFileExisting(alt_host))
	{
		KYTY_LOG_DEBUG("\t host app0 data segment: %s -> %s\n", guest.C_Str(), alt_host.C_Str());
		return alt_host;
	}
	const String alt_aliased = PreferHostExtensionAlias(alt_host);
	if (Core::File::IsFileExisting(alt_aliased))
	{
		KYTY_LOG_DEBUG("\t host app0 data segment+ext: %s -> %s\n", guest.C_Str(), alt_aliased.C_Str());
		return alt_aliased;
	}
	return requested_host_path;
}

// Last successfully resolved ObjectDefinition host path (.../odx/NAME.odx[b]).
// Used to recover bare `/app0/.jxm|.skel|.anim` companion opens.
static String g_last_od_host_path;

static void RememberOdHostPath(const String& guest_path, const String& host_path)
{
	const String g = guest_path.ToLower();
	if (!g.ContainsStr(U"/odx/") || (!g.EndsWith(U".odx") && !g.EndsWith(U".odxb")))
	{
		return;
	}
	if (!Core::File::IsFileExisting(host_path))
	{
		return;
	}
	g_last_od_host_path = host_path;
}

String PreferHostOdCompanionAsset(const String& guest_path, const String& requested_host_path, const String& last_od_host_path)
{
	if (requested_host_path.IsEmpty() || Core::File::IsFileExisting(requested_host_path))
	{
		return requested_host_path;
	}
	const String last_od = !last_od_host_path.IsEmpty() ? last_od_host_path : g_last_od_host_path;
	if (last_od.IsEmpty())
	{
		return requested_host_path;
	}
	// Bare companion: /app0/.jxm, /app0/.skel, /app0/.anim (basename empty, only extension).
	const String guest = guest_path.FixFilenameSlash();
	const String name  = guest.FilenameWithoutDirectory().ToLower();
	if (name != U".jxm" && name != U".skel" && name != U".anim" && name != U".jpx")
	{
		return requested_host_path;
	}

	// last OD host: .../odx/NAME.odxb → stem NAME, parent before /odx/
	String od = last_od.FixFilenameSlash();
	const String od_file = od.FilenameWithoutDirectory();
	String       stem    = od_file;
	const String od_low  = od_file.ToLower();
	if (od_low.EndsWith(U".odxb"))
	{
		stem = od_file.RemoveLast(5);
	}
	else if (od_low.EndsWith(U".odx"))
	{
		stem = od_file.RemoveLast(4);
	}
	const String odx_dir = od.DirectoryWithoutFilename(); // .../odx/
	// parent of odx/ → effects/ or ui/
	String parent = odx_dir;
	if (parent.EndsWith(U"/"))
	{
		parent = parent.RemoveLast(1);
	}
	// strip trailing "odx"
	const String parent_name = parent.FilenameWithoutDirectory().ToLower();
	if (parent_name != U"odx")
	{
		return requested_host_path;
	}
	const String tree_root = parent.DirectoryWithoutFilename(); // .../effects/ or .../ui/

	String candidate;
	if (name == U".jxm" || name == U".jpx")
	{
		candidate = tree_root + U"gfx/" + stem + name;
	}
	else if (name == U".skel")
	{
		candidate = tree_root + U"anim/" + stem + U".skel";
	}
	else // .anim
	{
		candidate = tree_root + U"anim/" + stem + U"_anim_play.anim";
		if (!Core::File::IsFileExisting(candidate))
		{
			candidate = tree_root + U"anim/" + stem + U".anim";
		}
	}

	if (Core::File::IsFileExisting(candidate))
	{
		KYTY_LOG_DEBUG("\t host OD companion: %s -> %s (from %s)\n", guest.C_Str(), candidate.C_Str(), last_od.C_Str());
		return candidate;
	}
	return requested_host_path;
}

static String PreferHostPatchFile(const String& guest_path, const String& requested_host_path)
{
	const String guest = guest_path.FixFilenameSlash();
	if (!guest.StartsWith(U"/app0/") || g_mount_points == nullptr)
	{
		return requested_host_path;
	}
	const String rest       = guest.RemoveFirst(6); // strip "/app0/"
	const String patch_guest = U"/app0_patch/" + rest;
	const String patch_host  = g_mount_points->GetRealFilename(patch_guest);
	if (!patch_host.IsEmpty() && patch_host != patch_guest && Core::File::IsFileExisting(patch_host))
	{
		KYTY_LOG_DEBUG("\t host app0_patch override: %s -> %s\n", guest.C_Str(), patch_host.C_Str());
		return patch_host;
	}
	return requested_host_path;
}

// Map guest path → existing host file (extension aliases, app0 data/, OD companions, fonts).
static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name)
{
	const String patched = PreferHostPatchFile(guest_path, real_file_name);
	if (Core::File::IsFileExisting(patched) && patched != real_file_name)
	{
		RememberOdHostPath(guest_path, patched);
		return patched;
	}
	if (Core::File::IsFileExisting(real_file_name))
	{
		RememberOdHostPath(guest_path, real_file_name);
		return real_file_name;
	}
	const String aliased = PreferHostExtensionAlias(real_file_name);
	if (Core::File::IsFileExisting(aliased))
	{
		RememberOdHostPath(guest_path, aliased);
		return aliased;
	}
	const String data_seg = PreferHostApp0DataSegment(guest_path, real_file_name);
	if (Core::File::IsFileExisting(data_seg))
	{
		RememberOdHostPath(guest_path, data_seg);
		return data_seg;
	}
	const String companion = PreferHostOdCompanionAsset(guest_path, real_file_name);
	if (Core::File::IsFileExisting(companion))
	{
		return companion;
	}
	const String guest_name = guest_path.FilenameWithoutDirectory().ToLower();
	// Only substitute font assets (package external styles / SIE system fonts under app0).
	if (!IsPackageFontExtension(guest_name))
	{
		return real_file_name;
	}
	return PreferPackageFontHostPath(real_file_name);
}

bool AprTryGetHostPath(uint32_t file_id, String* out_host_path)
{
	EXIT_IF(out_host_path == nullptr);
	Core::LockGuard lock(g_apr_mutex);
	auto            it = g_apr_id_to_host.find(file_id);
	if (it == g_apr_id_to_host.end())
	{
		return false;
	}
	*out_host_path = it->second;
	return true;
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizes(const char* const* paths, uint64_t count, uint32_t* ids, uint64_t* sizes)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr);

	KYTY_LOG_DEBUG("\t paths = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(paths));
	KYTY_LOG_DEBUG("\t count = %" PRIu64 "\n", count);
	KYTY_LOG_DEBUG("\t ids   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(ids));
	KYTY_LOG_DEBUG("\t sizes = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(sizes));

	// sizes is optional (ResolveFilepathsToIds variants pass null).
	if (paths == nullptr || count == 0 || count > 1024)
	{
		return KERNEL_ERROR_EINVAL;
	}

	for (uint64_t i = 0; i < count; ++i)
	{
		const char* guest_path = paths[i];
		if (guest_path == nullptr)
		{
			return KERNEL_ERROR_EFAULT;
		}

		KYTY_LOG_DEBUG("\t [%llu] path = %s\n", static_cast<unsigned long long>(i), guest_path);

		const String path_s         = String::FromUtf8(guest_path);
		const auto   real_file_name = ResolveExistingHostFile(path_s, g_mount_points->GetRealFilename(path_s));
		if (!Core::File::IsFileExisting(real_file_name))
		{
			KYTY_LOG_DEBUG("\t file not found: %s\n", real_file_name.C_Str());
			return KERNEL_ERROR_ENOENT;
		}

		const uint64_t file_size = Core::File::Size(real_file_name);
		const uint32_t file_id   = AprStableFileId(guest_path);
		if (sizes != nullptr)
		{
			sizes[i] = file_size;
		}
		if (ids != nullptr)
		{
			ids[i] = file_id;
		}
		{
			Core::LockGuard lock(g_apr_mutex);
			g_apr_id_to_host[file_id] = real_file_name;
		}
		KYTY_LOG_DEBUG("\t [%llu] id = 0x%08" PRIx32 " size = %" PRIu64 "\n", static_cast<unsigned long long>(i), file_id, file_size);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsToIds(const char* const* paths, uint64_t count, uint32_t* ids)
{
	return KernelAprResolveFilepathsToIdsAndFileSizes(paths, count, ids, nullptr);
}

static int AprResolveOnePath(const char* guest_path, uint32_t* out_id, uint64_t* out_size)
{
	if (guest_path == nullptr || guest_path[0] == '\0')
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (g_mount_points == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	const String path_s         = String::FromUtf8(guest_path);
	const auto   real_file_name = ResolveExistingHostFile(path_s, g_mount_points->GetRealFilename(path_s));
	if (!Core::File::IsFileExisting(real_file_name))
	{
		return KERNEL_ERROR_ENOENT;
	}
	const uint32_t file_id   = AprStableFileId(guest_path);
	const uint64_t file_size = Core::File::Size(real_file_name);
	if (out_id != nullptr)
	{
		*out_id = file_id;
	}
	if (out_size != nullptr)
	{
		*out_size = file_size;
	}
	Core::LockGuard lock(g_apr_mutex);
	g_apr_id_to_host[file_id] = real_file_name;
	return OK;
}

static void AprJoinPrefix(const char* prefix, const char* path, char* out, size_t out_cap)
{
	if (out == nullptr || out_cap == 0)
	{
		return;
	}
	out[0] = '\0';
	if (path == nullptr)
	{
		return;
	}
	if (prefix == nullptr || prefix[0] == '\0')
	{
		std::snprintf(out, out_cap, "%s", path);
		return;
	}
	const size_t plen = std::strlen(prefix);
	const bool   need_slash = plen > 0 && prefix[plen - 1] != '/' && path[0] != '/';
	if (need_slash)
	{
		std::snprintf(out, out_cap, "%s/%s", prefix, path);
	}
	else
	{
		std::snprintf(out, out_cap, "%s%s", prefix, path);
	}
}

static int AprResolveBatch(const char* prefix, const char* const* paths, uint64_t count, uint32_t* ids, uint64_t* sizes,
                           int32_t* results)
{
	if (paths == nullptr || count == 0 || count > 1024)
	{
		return KERNEL_ERROR_EINVAL;
	}
	int      first_error    = OK;
	uint32_t success_count  = 0;
	for (uint64_t i = 0; i < count; ++i)
	{
		char full[2048] {};
		AprJoinPrefix(prefix, paths[i], full, sizeof(full));
		const int rc = AprResolveOnePath(full, ids != nullptr ? &ids[i] : nullptr, sizes != nullptr ? &sizes[i] : nullptr);
		if (results != nullptr)
		{
			results[i] = rc;
		}
		if (rc == OK)
		{
			++success_count;
		}
		else
		{
			if (ids != nullptr)
			{
				ids[i] = 0xffffffffu;
			}
			if (sizes != nullptr)
			{
				sizes[i] = 0;
			}
			if (first_error == OK)
			{
				first_error = rc;
			}
			if (results == nullptr)
			{
				return rc;
			}
		}
	}
	return results != nullptr ? static_cast<int>(success_count) : first_error;
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIds(const char* prefix, const char* const* paths, uint64_t count, uint32_t* ids)
{
	PRINT_NAME();
	return AprResolveBatch(prefix, paths, count, ids, nullptr, nullptr);
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(const char* prefix, const char* const* paths, uint64_t count,
                                                                       uint32_t* ids, uint64_t* sizes)
{
	PRINT_NAME();
	return AprResolveBatch(prefix, paths, count, ids, sizes, nullptr);
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsForEach(const char* const* paths, uint64_t count, uint32_t* ids, int32_t* results)
{
	PRINT_NAME();
	return AprResolveBatch(nullptr, paths, count, ids, nullptr, results);
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsToIdsAndFileSizesForEach(const char* const* paths, uint64_t count, uint32_t* ids,
                                                                    uint64_t* sizes, int32_t* results)
{
	PRINT_NAME();
	return AprResolveBatch(nullptr, paths, count, ids, sizes, results);
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsForEach(const char* prefix, const char* const* paths, uint64_t count,
                                                                  uint32_t* ids, int32_t* results)
{
	PRINT_NAME();
	return AprResolveBatch(prefix, paths, count, ids, nullptr, results);
}

int KYTY_SYSV_ABI KernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach(const char* prefix, const char* const* paths,
                                                                              uint64_t count, uint32_t* ids, uint64_t* sizes,
                                                                              int32_t* results)
{
	PRINT_NAME();
	return AprResolveBatch(prefix, paths, count, ids, sizes, results);
}

int KYTY_SYSV_ABI KernelAprGetFileSize(uint32_t file_id, uint64_t* size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t file_id = 0x%08" PRIx32 "\n", file_id);
	KYTY_LOG_DEBUG("\t size    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(size));
	if (size == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	String host_path;
	if (!AprTryGetHostPath(file_id, &host_path))
	{
		return KERNEL_ERROR_ENOENT;
	}
	if (!Core::File::IsFileExisting(host_path))
	{
		return KERNEL_ERROR_ENOENT;
	}
	*size = Core::File::Size(host_path);
	return OK;
}

int KYTY_SYSV_ABI KernelAprGetFileStat(uint32_t file_id, FileStat* st)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t file_id = 0x%08" PRIx32 "\n", file_id);
	KYTY_LOG_DEBUG("\t st      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(st));
	if (st == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	String host_path;
	if (!AprTryGetHostPath(file_id, &host_path))
	{
		return KERNEL_ERROR_ENOENT;
	}
	if (!Core::File::IsFileExisting(host_path))
	{
		return KERNEL_ERROR_ENOENT;
	}
	memset(st, 0, sizeof(FileStat));
	st->st_mode    = 0000777u | 0100000u;
	st->st_flags   = kPs5StFlagsAprCapable;
	st->st_size    = static_cast<int64_t>(Core::File::Size(host_path));
	st->st_blksize = 512;
	st->st_blocks  = (st->st_size + 511) / 512;
	return OK;
}

int KYTY_SYSV_ABI KernelAprSubmitCommandBuffer(void* cmd, uint64_t arg1, void* arg2, uint64_t arg3, void* arg4)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t cmd  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd));
	KYTY_LOG_DEBUG("\t arg1 = 0x%016" PRIx64 "\n", arg1);
	KYTY_LOG_DEBUG("\t arg2 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg2));
	KYTY_LOG_DEBUG("\t arg3 = 0x%016" PRIx64 "\n", arg3);
	KYTY_LOG_DEBUG("\t arg4 = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(arg4));

	if (cmd == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	return ::Kyty::Kernel::AmprPort::SubmitCommandBuffer(cmd, static_cast<uintptr_t>(arg3));
}

static uint32_t AprAllocateSubmissionId(uint64_t cmd)
{
	Core::LockGuard lock(g_apr_mutex);
	uint32_t        id = g_apr_next_submission_id++;
	if (id == 0)
	{
		id = g_apr_next_submission_id++;
	}
	g_apr_submissions[id] = cmd;
	return id;
}

int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetId(void* cmd, uint64_t arg1, uint32_t* out_submission_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " out_id = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd),
	       reinterpret_cast<uint64_t>(out_submission_id));
	if (cmd == nullptr || out_submission_id == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int submit_rc = KernelAprSubmitCommandBuffer(cmd, arg1, nullptr, 0, nullptr);
	if (submit_rc != OK)
	{
		return submit_rc;
	}
	*out_submission_id = AprAllocateSubmissionId(reinterpret_cast<uint64_t>(cmd));
	return OK;
}

int KYTY_SYSV_ABI KernelAprSubmitCommandBufferAndGetResult(void* cmd, uint64_t arg1, void* result, uint32_t* out_submission_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t cmd = 0x%016" PRIx64 " result = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cmd), reinterpret_cast<uint64_t>(result));
	if (cmd == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int submit_rc = KernelAprSubmitCommandBuffer(cmd, arg1, result, 0, nullptr);
	if (submit_rc != OK)
	{
		return submit_rc;
	}
	if (out_submission_id != nullptr)
	{
		*out_submission_id = AprAllocateSubmissionId(reinterpret_cast<uint64_t>(cmd));
	}
	// Optional result blob: two dwords (result, error_offset) zeroed on success.
	if (result != nullptr)
	{
		uint32_t words[2] = {0, 0};
		std::memcpy(result, words, sizeof(words));
	}
	return OK;
}

int KYTY_SYSV_ABI KernelAprWaitCommandBuffer(uint32_t submission_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t submission_id = 0x%08" PRIx32 "\n", submission_id);
	Core::LockGuard lock(g_apr_mutex);
	auto            it = g_apr_submissions.find(submission_id);
	if (it == g_apr_submissions.end())
	{
		// Eager submit means waiters may race; unknown id is not a hard error if
		// builders already completed. Report ESRCH only for id 0.
		return submission_id == 0 ? KERNEL_ERROR_EINVAL : OK;
	}
	g_apr_submissions.erase(it);
	return OK;
}

namespace {

constexpr int16_t kPollIn  = 0x0001;
constexpr int16_t kPollOut = 0x0004;

} // namespace

int KYTY_SYSV_ABI KernelDup(int old_d)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	KYTY_LOG_DEBUG("\t old_d = %d\n", old_d);

	const int new_d = g_files->DupDescriptor(old_d);
	if (new_d < 0)
	{
		return new_d;
	}

	KYTY_LOG_DEBUG("\t new_d = %d\n", new_d);
	return new_d;
}

int KYTY_SYSV_ABI KernelDup2(int old_d, int new_d)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	KYTY_LOG_DEBUG("\t old_d = %d\n", old_d);
	KYTY_LOG_DEBUG("\t new_d = %d\n", new_d);

	return g_files->Dup2Descriptor(old_d, new_d);
}

int KYTY_SYSV_ABI KernelPoll(KernelPollFd* fds, uint32_t count, int /*timeout*/)
{
	PRINT_NAME();

	if (fds == nullptr || count == 0)
	{
		return 0;
	}

	KYTY_LOG_DEBUG("\t count = %" PRIu32 "\n", count);

	int ready = 0;
	for (uint32_t i = 0; i < count && i < 4096; i++)
	{
		auto& entry = fds[i];
		entry.revents = static_cast<int16_t>(entry.events & (kPollIn | kPollOut));
		if (entry.revents != 0)
		{
			ready++;
		}
	}

	KYTY_LOG_DEBUG("\t ready = %d\n", ready);
	return ready;
}

} // namespace Kyty::Kernel::FileSystem

#endif // KYTY_EMU_ENABLED
