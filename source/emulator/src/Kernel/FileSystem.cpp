#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/FileSystemPath.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/AmprPort.h"
#include "HostFile.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DateTime.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Trace.h"
#include "Emulator/VideoFrameMemory.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::FileSystem {

KERNEL_LIB_NAME();

static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name);

constexpr int DESCRIPTOR_MIN = 3;

constexpr uint8_t kStandardDescriptorMask = (1u << DESCRIPTOR_MIN) - 1u;

// Guest standard descriptors are logical handles. Their lifecycle must not
// affect the emulator process streams that provide diagnostics and input.
static std::atomic_uint8_t g_standard_descriptors {kStandardDescriptorMask};

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
	return String::FromUtf8(Path::ResolveContained(GetSandboxRoot().C_Str(), guest_path.RemoveFirst(1).C_Str()).c_str());
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
	[[nodiscard]] bool IsHostFilenameAllowed(const String& filename);

private:
	Vector<MountPair> m_mount_pairs;
	Core::Mutex       m_mutex;
};

struct File
{
	Host::File                   f;
	String                       name;
	String                       real_name;
	std::atomic_bool             opened;
	std::atomic_bool             directory;
	std::atomic_uint32_t         status_flags {0};
	Core::Mutex                  mutex;
	Vector<Core::File::DirEntry> dents;
	uint32_t                     dents_index;
	// Protected by mutex. Native FILE locks also cover legacy stdio callers
	// which use the opaque host stream directly.
	std::vector<FILE*>           streams;
};

class StreamLocks
{
public:
	explicit StreamLocks(const File& file): m_streams(file.streams)
	{
		for (FILE* stream: m_streams) { Host::File::LockStream(stream); }
	}
	~StreamLocks()
	{
		const int saved = errno;
		for (auto stream = m_streams.rbegin(); stream != m_streams.rend(); ++stream) { Host::File::UnlockStream(*stream); }
		errno = saved;
	}
	KYTY_CLASS_NO_COPY(StreamLocks);
private:
	const std::vector<FILE*>& m_streams;
};

struct StreamState
{
	explicit StreamState(FILE* value): stream(value) {}
	Core::Mutex mutex;
	FILE* stream;
	char* buffer = nullptr;
	size_t size = 0;
	int buffer_mode = _IOFBF;
	// Used when freopen needs to detach caller storage independently of the
	// host libc's buffer-retention policy. Kept alive until the FILE closes.
	std::unique_ptr<char[]> host_buffer;
	bool closed = false;
};

// Lock order: registry -> description -> buffer state -> native FILE. Ordinary
// stdio releases the registry before acquiring state and never takes a
// description lock. The state gate pins the buffer until the lease is released.
// Close/reopen use libc's own FILE lock: unlocking a freed FILE would be invalid.
class StreamUse
{
public:
	explicit StreamUse(std::shared_ptr<StreamState> state, bool lock_native = true): m_state(std::move(state))
	{
		m_state->mutex.Lock();
		if (m_state->closed) { errno = EBADF; return; }
		if (m_state->buffer != nullptr && m_state->size != 0)
		{
			m_lease.emplace(reinterpret_cast<uint64_t>(m_state->buffer), m_state->size);
		}
		m_valid = true;
		m_native_locked = lock_native;
		if (lock_native) { Host::File::LockStream(m_state->stream); }
	}
	~StreamUse()
	{
		const int saved = errno;
		if (m_native_locked) { Host::File::UnlockStream(m_state->stream); }
		m_lease.reset();
		m_state->mutex.Unlock();
		errno = saved;
	}
	bool IsValid() const { return m_valid; }
	KYTY_CLASS_NO_COPY(StreamUse);
private:
	std::shared_ptr<StreamState> m_state;
	std::optional<Emulator::VideoFrameMemory::HostWriteLease> m_lease;
	bool m_valid = false;
	bool m_native_locked = false;
};

class FileDescriptors
{
public:
	using FileHandle = std::shared_ptr<File>;

	FileDescriptors() { if (!Core::Thread::IsMainThread()) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); } }
	virtual ~FileDescriptors() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FileDescriptors);

	int   CreateDescriptor();
	int   DeleteDescriptor(int d);
	int   DupDescriptor(int old_d);
	int   Dup2Descriptor(int old_d, int new_d);
	FileHandle GetFile(int d);
	FileHandle GetFile(const String& real_name);
	void  CloseAll();
	FILE* OpenStream(int d, const char* mode, uint32_t flags);
	FILE* ReopenStream(FILE* stream, const String& path, const String& host_path, const char* mode, uint32_t flags);
	int   CloseStream(FILE* stream);
	int   StreamDescriptor(FILE* stream);
	FILE* StandardStream(int descriptor);
	std::shared_ptr<StreamState> GetStreamState(FILE* stream);
	int FlushStreams();

private:
	int EnsureDescriptorCapacity(uint32_t index);
	struct StreamEntry
	{
		int descriptor;
		uint64_t generation;
		FileHandle file;
		std::shared_ptr<StreamState> state;
	};
	void Detach(const StreamEntry& stream);
	bool Matches(const StreamEntry& stream) const;

	std::vector<FileHandle> m_files;
	std::vector<uint64_t>   m_generations;
	uint64_t                m_next_generation = 0;
	std::unordered_map<FILE*, StreamEntry> m_streams;
	std::array<FILE*, DESCRIPTOR_MIN> m_standard_streams {};
	Core::Mutex             m_mutex;
};

static MountPoints*     g_mount_points = nullptr;
static FileDescriptors* g_files        = nullptr;
static Core::Mutex      g_files_init_mutex;

static FileDescriptors* InitializeDescriptors()
{
	Core::LockGuard lock(g_files_init_mutex);
	if (g_files == nullptr) { g_files = new FileDescriptors; }
	return g_files;
}

// Defined with APR helpers; used by KernelOpen/KernelStat for package font fallback.
static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name);
static int HostErrorFromKernel(int error);

static void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

static int KernelErrorFromHost(int error)
{
	// POSIX/BSD error numbers at the guest boundary, never host errno values.
	constexpr int base = -2147352576;
	switch (error)
	{
		case EPERM: return KERNEL_ERROR_EPERM;
		case ENOENT: return KERNEL_ERROR_ENOENT;
		case EINTR: return base + 4;
		case EBADF: return KERNEL_ERROR_EBADF;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		case EACCES: return KERNEL_ERROR_EACCES;
		case EFAULT: return KERNEL_ERROR_EFAULT;
		case EEXIST: return KERNEL_ERROR_EEXIST;
		case ENOTDIR: return KERNEL_ERROR_ENOTDIR;
		case EISDIR: return KERNEL_ERROR_EISDIR;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case ENFILE: return base + 23;
		case EMFILE: return base + 24;
		case EFBIG: return base + 27;
		case ENOSPC: return base + 28;
		case ESPIPE: return base + 29;
		case EROFS: return base + 30;
		case EPIPE: return base + 32;
		case EAGAIN: return KERNEL_ERROR_EAGAIN;
		case ENOSYS: return KERNEL_ERROR_EOPNOTSUPP;
		case ENAMETOOLONG: return KERNEL_ERROR_ENAMETOOLONG;
		default: return KERNEL_ERROR_EIO;
	}
}

int FileDescriptors::CreateDescriptor()
{
	Core::LockGuard lock(m_mutex);
	const int capacity = Host::File::DescriptorCapacity();
	if (capacity < 0) { return KernelErrorFromHost(errno); }
	if (capacity <= DESCRIPTOR_MIN) { return KernelErrorFromHost(EMFILE); }

	auto file       = std::make_shared<File>();
	file->opened    = false;
	file->directory = false;

	const int files_num = std::min(static_cast<int>(m_files.size()), capacity - DESCRIPTOR_MIN);
	for (int index = 0; index < files_num; index++)
	{
		if (m_files[static_cast<size_t>(index)] == nullptr)
		{
			m_files[static_cast<size_t>(index)] = file;
			m_generations[static_cast<size_t>(index)] = ++m_next_generation;
			return index + DESCRIPTOR_MIN;
		}
	}

	if (files_num >= capacity - DESCRIPTOR_MIN) { return KernelErrorFromHost(EMFILE); }
	const int error = EnsureDescriptorCapacity(static_cast<uint32_t>(files_num));
	if (error != OK) { return error; }
	m_files[files_num] = std::move(file);
	m_generations[files_num] = ++m_next_generation;
	return files_num + DESCRIPTOR_MIN;
}

int FileDescriptors::EnsureDescriptorCapacity(uint32_t index)
{
	const int capacity = Host::File::DescriptorCapacity();
	if (capacity < 0) { return KernelErrorFromHost(errno); }
	if (capacity <= DESCRIPTOR_MIN || index >= static_cast<uint32_t>(capacity - DESCRIPTOR_MIN))
	{
		return KERNEL_ERROR_EBADF;
	}
	if (index < m_files.size()) { return OK; }
	const size_t count = static_cast<size_t>(index) + 1;
	const size_t reserve = std::min(static_cast<size_t>(capacity - DESCRIPTOR_MIN),
	                                std::max(count, m_files.size() * 2));
	m_files.reserve(reserve);
	m_generations.reserve(reserve);
	m_files.resize(count);
	m_generations.resize(count, 0);
	return OK;
}

int FileDescriptors::DeleteDescriptor(int d)
{
	Core::LockGuard lock(m_mutex);

	if (d < DESCRIPTOR_MIN) { return KERNEL_ERROR_EBADF; }
	auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);
	if (index >= m_files.size() || m_files[index] == nullptr) { return KERNEL_ERROR_EBADF; }
	m_files[index].reset();
	return OK;
}

int FileDescriptors::DupDescriptor(int old_d)
{
	Core::LockGuard lock(m_mutex);

	if (old_d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	const auto old_index = static_cast<uint32_t>(old_d - DESCRIPTOR_MIN);
	if (old_index >= m_files.size())
	{
		return KERNEL_ERROR_EBADF;
	}

	auto file = m_files[old_index];
	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	const int capacity = Host::File::DescriptorCapacity();
	if (capacity < 0) { return KernelErrorFromHost(errno); }
	if (capacity <= DESCRIPTOR_MIN) { return KernelErrorFromHost(EMFILE); }
	int new_fd = -1;
	const int files_num = std::min(static_cast<int>(m_files.size()), capacity - DESCRIPTOR_MIN);
	for (int index = 0; index < files_num; index++)
	{
		if (m_files[static_cast<size_t>(index)] == nullptr)
		{
			new_fd = index + DESCRIPTOR_MIN;
			m_files[static_cast<uint32_t>(index)] = file;
			m_generations[static_cast<size_t>(index)] = ++m_next_generation;
			break;
		}
	}

	if (new_fd < 0)
	{
		if (files_num >= capacity - DESCRIPTOR_MIN) { return KernelErrorFromHost(EMFILE); }
		const int error = EnsureDescriptorCapacity(static_cast<uint32_t>(files_num));
		if (error != OK) { return error; }
		m_files[files_num] = file;
		m_generations[files_num] = ++m_next_generation;
		new_fd = files_num + DESCRIPTOR_MIN;
	}
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
	if (old_index >= m_files.size())
	{
		return KERNEL_ERROR_EBADF;
	}

	auto source = m_files[old_index];
	if (source == nullptr || !source->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (old_d == new_d)
	{
		return new_d;
	}

	const auto new_index = static_cast<uint32_t>(new_d - DESCRIPTOR_MIN);
	const int capacity_error = EnsureDescriptorCapacity(new_index);
	if (capacity_error != OK) { return capacity_error; }

	// A stream remains associated with its guest descriptor across an explicit
	// dup2 replacement. Its private native descriptor must follow that change.
	std::vector<FILE*> targets;
	for (const auto& stream: m_streams)
	{
		if (stream.second.descriptor != new_d || !Matches(stream.second)) { continue; }
		targets.push_back(stream.first);
	}
	if (!targets.empty())
	{
		if (source->directory) { return KERNEL_ERROR_EISDIR; }
		auto previous = m_files[new_index];
		Core::LockGuard old_lock(previous->mutex);
		Core::LockGuard source_lock(source->mutex);
		// Leases cover both the initial flush and all replacement/rollback calls.
		std::vector<std::unique_ptr<StreamUse>> uses;
		for (FILE* stream: targets) { uses.push_back(std::make_unique<StreamUse>(m_streams.at(stream).state)); }
		int error = 0;
		for (FILE* stream: targets)
		{
			if (std::fflush(stream) != 0) { error = errno; break; }
		}
		size_t replaced = 0;
		while (error == 0 && replaced < targets.size())
		{
			if (!source->f.ReplaceStreamDescriptor(targets[replaced])) { error = errno; break; }
			++replaced;
		}
		if (error != 0)
		{
			for (size_t i = 0; i < replaced; ++i) { previous->f.ReplaceStreamDescriptor(targets[i]); }
		} else
		{
			for (FILE* stream: targets)
			{
				auto& streams = previous->streams;
				streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
				source->streams.push_back(stream);
				auto& entry = m_streams.at(stream);
				entry.file = source;
				entry.generation = m_next_generation + 1;
			}
		}
		if (error != 0) { return KernelErrorFromHost(error); }
	}
	m_files[new_index] = source;
	m_generations[new_index] = ++m_next_generation;
	return new_d;
}

FileDescriptors::FileHandle FileDescriptors::GetFile(int d)
{
	Core::LockGuard lock(m_mutex);

	if (d < DESCRIPTOR_MIN)
	{
		return nullptr;
	}

	auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);

	if (index >= m_files.size())
	{
		return nullptr;
	}

	return m_files[index];
}

FileDescriptors::FileHandle FileDescriptors::GetFile(const String& real_name)
{
	Core::LockGuard lock(m_mutex);

	for (const auto& f: m_files)
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

	for (const auto& stream: m_streams)
	{
		Core::LockGuard file_lock(stream.second.file->mutex);
		const StreamUse use(stream.second.state, false);
		stream.second.state->closed = true;
		std::fclose(stream.first);
		stream.second.file->streams.clear();
	}
	m_streams.clear();
	m_standard_streams.fill(nullptr);
	for (auto& f: m_files)
	{
		f.reset();
	}
	m_files.clear();
	m_generations.clear();
}

bool FileDescriptors::Matches(const StreamEntry& stream) const
{
	if (stream.descriptor < DESCRIPTOR_MIN)
	{
		return stream.descriptor >= 0 && m_standard_streams[stream.descriptor] == stream.state->stream;
	}
	const auto index = static_cast<size_t>(stream.descriptor - DESCRIPTOR_MIN);
	return index < m_files.size() && m_files[index] == stream.file && m_generations[index] == stream.generation;
}

void FileDescriptors::Detach(const StreamEntry& stream)
{
	if (!Matches(stream)) { return; }
	if (stream.descriptor < DESCRIPTOR_MIN)
	{
		m_standard_streams[stream.descriptor] = nullptr;
		g_standard_descriptors.fetch_and(static_cast<uint8_t>(~(1u << stream.descriptor)), std::memory_order_release);
	} else
	{
		m_files[static_cast<size_t>(stream.descriptor - DESCRIPTOR_MIN)].reset();
	}
}

FILE* FileDescriptors::StandardStream(int descriptor)
{
	Core::LockGuard lock(m_mutex);
	if (descriptor < 0 || descriptor >= DESCRIPTOR_MIN || !KernelIsStandardDescriptorOpen(descriptor))
	{
		errno = EBADF;
		return nullptr;
	}
	if (m_standard_streams[descriptor] != nullptr) { return m_standard_streams[descriptor]; }
	FILE* const native[] = {stdin, stdout, stderr};
	auto file = std::make_shared<File>();
	if (!file->f.DuplicateFromStream(native[descriptor], false)) { return nullptr; }
	FILE* stream = file->f.DuplicateStream(descriptor == 0 ? "rb" : "wb");
	if (stream == nullptr) { return nullptr; }
	// Guest setvbuf must never install storage into a host diagnostics FILE.
	// Match stderr's unbuffered default and stdout's native terminal policy.
	if (descriptor == 2) { std::setvbuf(stream, nullptr, _IONBF, 0); }
	file->opened = true;
	file->directory = false;
	file->status_flags.store(descriptor == 0 ? 0u : 1u, std::memory_order_release);
	file->streams.push_back(stream);
	auto state = std::make_shared<StreamState>(stream);
	state->buffer_mode = descriptor == 2 ? _IONBF : _IOFBF;
	m_streams.emplace(stream, StreamEntry {descriptor, 0, file, state});
	m_standard_streams[descriptor] = stream;
	return stream;
}

std::shared_ptr<StreamState> FileDescriptors::GetStreamState(FILE* stream)
{
	Core::LockGuard lock(m_mutex);
	const auto found = m_streams.find(stream);
	return found == m_streams.end() ? nullptr : found->second.state;
}

int FileDescriptors::FlushStreams()
{
	int result = 0;
	int saved = 0;
	{
		Core::LockGuard lock(m_mutex);
		// Keep membership fixed through the native global operation. Lease all
		// caller buffers, but let libc select output streams; individually flushing
		// every FILE would also discard read-ahead on input/update streams.
		std::unordered_set<File*> locked;
		std::vector<std::unique_ptr<Core::LockGuard>> descriptions;
		for (const auto& stream: m_streams)
		{
			auto* file = stream.second.file.get();
			if (!locked.insert(file).second) { continue; }
			descriptions.push_back(std::make_unique<Core::LockGuard>(file->mutex));
		}
		std::vector<std::unique_ptr<StreamUse>> uses;
		for (const auto& stream: m_streams) { uses.push_back(std::make_unique<StreamUse>(stream.second.state, false)); }
		// No external native locks: libc owns its global-list/FILE lock ordering.
		result = std::fflush(nullptr);
		saved = errno;
	}
	errno = saved;
	return result;
}

FILE* FileDescriptors::OpenStream(int d, const char* mode, uint32_t flags)
{
	Core::LockGuard lock(m_mutex);
	if (d < DESCRIPTOR_MIN) { errno = EBADF; return nullptr; }
	const auto index = static_cast<size_t>(d - DESCRIPTOR_MIN);
	if (index >= m_files.size() || m_files[index] == nullptr || !m_files[index]->opened)
	{
		errno = EBADF;
		return nullptr;
	}
	auto file = m_files[index];
	Core::LockGuard file_lock(file->mutex);
	if (file->directory) { errno = EISDIR; return nullptr; }
	const uint32_t current = file->status_flags.load(std::memory_order_acquire);
	const auto access = current & 3u;
	const auto requested = flags & 3u;
	if ((access != 2u && access != requested) || file->f.IsInvalid())
	{
		errno = EBADF;
		return nullptr;
	}
	const bool add_append = (flags & 8u) != 0 && (current & 8u) == 0;
	if (add_append && !file->f.SetAppend(true)) { return nullptr; }
	FILE* stream = file->f.DuplicateStream(mode);
	if (stream == nullptr)
	{
		const int saved = errno;
		if (add_append) { file->f.SetAppend(false); }
		errno = saved;
		return nullptr;
	}
	if (add_append) { file->status_flags.store(current | 8u, std::memory_order_release); }
	file->streams.push_back(stream);
	m_streams.emplace(stream, StreamEntry {d, m_generations[index], file, std::make_shared<StreamState>(stream)});
	return stream;
}

int FileDescriptors::StreamDescriptor(FILE* stream)
{
	Core::LockGuard lock(m_mutex);
	const auto found = m_streams.find(stream);
	if (found == m_streams.end() || !Matches(found->second)) { errno = EBADF; return -1; }
	return found->second.descriptor;
}

int FileDescriptors::CloseStream(FILE* stream)
{
	Core::LockGuard lock(m_mutex);
	const auto found = m_streams.find(stream);
	if (found == m_streams.end()) { errno = EBADF; return EOF; }
	const auto entry = found->second;
	auto file = entry.file;
	Core::LockGuard file_lock(file->mutex);
	const StreamUse use(entry.state, false);
	Detach(entry);
	m_streams.erase(found);
	auto& streams = file->streams;
	streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
	entry.state->closed = true;
	return std::fclose(stream);
}

FILE* FileDescriptors::ReopenStream(FILE* stream, const String& path, const String& host_path, const char* mode, uint32_t flags)
{
	Core::LockGuard lock(m_mutex);
	const auto found = m_streams.find(stream);
	if (found == m_streams.end()) { errno = EBADF; return nullptr; }
	const auto previous = found->second;
	const bool attached = Matches(previous);
	Core::LockGuard file_lock(previous.file->mutex);
	const StreamUse use(previous.state, false);
	previous.state->closed = true;
	auto& streams = previous.file->streams;
	streams.erase(std::remove(streams.begin(), streams.end(), stream), streams.end());
	m_streams.erase(found);
	Detach(previous);
	// freopen retains a genuine host FILE object, including libc-private layout.
	// Existing guest dup handles retain the old native open-file description.
	FILE* reopened = std::freopen(host_path.C_Str(), mode, stream);
	if (reopened == nullptr) { return nullptr; }
	auto state = std::make_shared<StreamState>(reopened);
	if (previous.state->buffer != nullptr || previous.state->host_buffer != nullptr)
	{
		// Some libcs retain the old buffer on reopen. Explicit host-owned storage
		// makes ending the old guest association portable; nullptr alone would
		// allow libc to keep using the caller's buffer.
		state->buffer_mode = previous.state->buffer_mode;
		state->host_buffer = std::make_unique<char[]>(BUFSIZ);
		const int previous_error = errno;
		errno = 0;
		if (std::setvbuf(reopened, state->host_buffer.get(), state->buffer_mode, BUFSIZ) != 0)
		{
			const int saved = errno != 0 ? errno : EIO;
			std::fclose(reopened);
			errno = saved;
			return nullptr;
		}
		errno = previous_error;
	}
	auto file = std::make_shared<File>();
	if (!file->f.DuplicateFromStream(reopened, (flags & 8u) != 0))
	{
		const int saved = errno;
		std::fclose(reopened);
		errno = saved;
		return nullptr;
	}
	file->opened = true;
	file->directory = false;
	file->name = path;
	file->real_name = host_path;
	file->status_flags.store(flags, std::memory_order_release);
	file->streams.push_back(reopened);
	const int descriptor = attached ? previous.descriptor : CreateDescriptor();
	if (descriptor < 0)
	{
		std::fclose(reopened);
		errno = HostErrorFromKernel(descriptor);
		return nullptr;
	}
	const auto generation = ++m_next_generation;
	if (descriptor < DESCRIPTOR_MIN)
	{
		m_standard_streams[descriptor] = reopened;
		g_standard_descriptors.fetch_or(static_cast<uint8_t>(1u << descriptor), std::memory_order_release);
	} else
	{
		const auto index = static_cast<size_t>(descriptor - DESCRIPTOR_MIN);
		m_files[index] = file;
		m_generations[index] = generation;
	}
	m_streams.emplace(reopened, StreamEntry {descriptor, generation, file, state});
	return reopened;
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

	const auto normalized = Path::NormalizeGuest(mounted_file_name.C_Str());
	if (normalized.empty()) { return {}; }
	const auto guest = String::FromUtf8(normalized.c_str());
	for (const auto& pair: m_mount_pairs)
	{
		const bool at_root = guest.FixDirectorySlash() == pair.point;
		if (!at_root && !guest.StartsWith(pair.point)) { continue; }
		const auto suffix = at_root ? String() : guest.RemoveFirst(pair.point.Size());
		const auto relative = Path::MatchCaseInsensitive(pair.dir.C_Str(), suffix.C_Str());
		return String::FromUtf8(Path::ResolveContained(pair.dir.C_Str(), relative).c_str());
	}

	return MapToSandbox(guest);
}

String MountPoints::GetRealDirectory(const String& mounted_directory)
{
	const auto resolved = GetRealFilename(mounted_directory);
	return resolved.IsEmpty() ? String() : resolved.FixDirectorySlash();
}

bool MountPoints::IsHostFilenameAllowed(const String& filename)
{
	if (filename.IsEmpty()) { return false; }
	Core::LockGuard lock(m_mutex);
	for (const auto& pair: m_mount_pairs)
	{
		if (Path::Allows(pair.dir.C_Str(), filename.C_Str())) { return true; }
	}
	return Path::Allows(GetSandboxRoot().C_Str(), filename.C_Str());
}

void FileSystemSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	g_mount_points = new MountPoints;
	InitializeDescriptors();
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

String GetExistingFilename(const String& mounted_file_name)
{
	EXIT_IF(g_mount_points == nullptr);
	const auto normalized = Path::NormalizeGuest(mounted_file_name.C_Str());
	if (normalized.empty()) { return {}; }
	const auto guest = String::FromUtf8(normalized.c_str());
	return ResolveExistingHostFile(guest, g_mount_points->GetRealFilename(guest));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static int KYTY_SYSV_ABI KernelOpenResolved(const char* path, int flags, uint16_t mode, bool kernel_path_policy = true)
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
	bool dsync     = (flags_u & 0x1000u) != 0;
	bool direct    = (flags_u & 0x00010000u) != 0;
	bool directory = (flags_u & 0x00020000u) != 0;

	// Durability/direct-I/O policy is unchanged by the descriptor bridge.
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
		default: return KERNEL_ERROR_EINVAL;
	}

	if (directory && rw_mode != Core::File::Mode::Read) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (directory && (trunc || creat)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	int   descriptor = g_files->CreateDescriptor();
	if (descriptor < 0) { return descriptor; }
	auto file        = g_files->GetFile(descriptor);

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
		const auto mapped = g_mount_points->GetRealFilename(file->name);
		file->real_name = ResolveGuestDeviceFilename(
			file->name, kernel_path_policy ? ResolveExistingHostFile(file->name, mapped) : mapped);
	}

	if (trunc && rw_mode == Core::File::Mode::Read)
	{
		g_files->DeleteDescriptor(descriptor);
		return KERNEL_ERROR_EACCES;
	}
	if (file->real_name.IsEmpty())
	{
		g_files->DeleteDescriptor(descriptor);
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
		if (creat && kernel_path_policy)
		{
			// Ensure parent directories exist for sandbox-mapped paths.
			const String parent_dir = file->real_name.DirectoryWithoutFilename();
			if (!parent_dir.IsEmpty() && !Core::File::IsDirectoryExisting(parent_dir))
			{
				CreateDirectoryRecursive(parent_dir);
			}
		}
		// O_CREAT/O_EXCL/O_TRUNC/O_APPEND are one native open operation. Append
		// placement belongs to every host write, including independent opens.
		if (!file->f.Open(file->real_name.C_Str(), status_flags, mode))
		{
			const int error = KernelErrorFromHost(errno);
			g_files->DeleteDescriptor(descriptor);
			return error;
		}
	}

	file->status_flags.store(status_flags, std::memory_order_release);
	file->opened = true;
	return descriptor;
}

int KYTY_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode)
{
	PRINT_NAME();

	const int result = KernelOpenResolved(path, flags, mode);
	FsTrace("open", path, flags, result);
	return result;
}

struct StreamMode
{
	uint32_t flags = 0;
	char native[5] {};
};

static bool ParseStreamMode(const char* mode, StreamMode* result)
{
	if (mode == nullptr || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) { errno = EINVAL; return false; }
	bool update = false;
	bool binary = false;
	bool exclusive = false;
	for (size_t i = 1; mode[i] != '\0'; ++i)
	{
		if (mode[i] == '+' && !update) { update = true; }
		else if (mode[i] == 'b' && !binary) { binary = true; }
		else if (mode[i] == 'x' && !exclusive && mode[0] == 'w') { exclusive = true; }
		else { errno = EINVAL; return false; }
	}
	result->flags = mode[0] == 'r' ? 0u : mode[0] == 'w' ? 0x0601u : 0x0209u;
	if (update) { result->flags = (result->flags & ~3u) | 2u; }
	if (exclusive) { result->flags |= 0x0800u; }
	result->native[0] = mode[0];
	// Guest text bytes must not acquire host newline translation on Windows.
	result->native[1] = 'b';
	size_t length = 2;
	if (update) { result->native[length++] = '+'; }
	if (exclusive) { result->native[length++] = 'x'; }
	result->native[length] = '\0';
	return true;
}

static int HostErrorFromKernel(int error)
{
	constexpr int base = -2147352576;
	switch (error)
	{
		case KERNEL_ERROR_EPERM: return EPERM;
		case base + 4: return EINTR;
		case KERNEL_ERROR_EBADF: return EBADF;
		case KERNEL_ERROR_ENOENT: return ENOENT;
		case KERNEL_ERROR_EINVAL: return EINVAL;
		case KERNEL_ERROR_EACCES: return EACCES;
		case KERNEL_ERROR_EEXIST: return EEXIST;
		case KERNEL_ERROR_EISDIR: return EISDIR;
		case KERNEL_ERROR_ENOTDIR: return ENOTDIR;
		case KERNEL_ERROR_ENOMEM: return ENOMEM;
		case KERNEL_ERROR_EFAULT: return EFAULT;
		case base + 23: return ENFILE;
		case base + 24: return EMFILE;
		case base + 27: return EFBIG;
		case base + 28: return ENOSPC;
		case base + 29: return ESPIPE;
		case base + 30: return EROFS;
		case base + 32: return EPIPE;
		case KERNEL_ERROR_EAGAIN: return EAGAIN;
		case KERNEL_ERROR_EOPNOTSUPP: return ENOSYS;
		case KERNEL_ERROR_ENAMETOOLONG: return ENAMETOOLONG;
		default: return EIO;
	}
}

FILE* OpenStream(const char* path, const char* mode)
{
	StreamMode parsed;
	if (!ParseStreamMode(mode, &parsed)) { return nullptr; }
	if (path == nullptr) { errno = EINVAL; return nullptr; }
	// libc uses the same table, while retaining its exact mounted-path policy.
	const int descriptor = KernelOpenResolved(path, static_cast<int>(parsed.flags), 0666, false);
	FsTrace("fopen", path, parsed.flags, descriptor);
	if (descriptor < 0) { errno = HostErrorFromKernel(descriptor); return nullptr; }
	// 'x' is an open-time constraint, not an fdopen mode.
	if ((parsed.flags & 0x0800u) != 0) { parsed.native[std::strlen(parsed.native) - 1] = '\0'; }
	FILE* stream = g_files->OpenStream(descriptor, parsed.native, parsed.flags);
	if (stream == nullptr)
	{
		const int saved = errno;
		g_files->DeleteDescriptor(descriptor);
		errno = saved;
	}
	return stream;
}

FILE* OpenDescriptorStream(int descriptor, const char* mode)
{
	StreamMode parsed;
	if (!ParseStreamMode(mode, &parsed)) { return nullptr; }
	if ((parsed.flags & 0x0800u) != 0) { parsed.native[std::strlen(parsed.native) - 1] = '\0'; }
	if (g_files == nullptr) { errno = EBADF; return nullptr; }
	return g_files->OpenStream(descriptor, parsed.native, parsed.flags);
}

FILE* ReopenStream(const char* path, const char* mode, FILE* stream)
{
	StreamMode parsed;
	if (stream == nullptr || g_files == nullptr) { errno = EBADF; return nullptr; }
	if (!ParseStreamMode(mode, &parsed) || path == nullptr)
	{
		g_files->CloseStream(stream);
		errno = EINVAL;
		return nullptr;
	}
	const auto host = GetRealFilename(String::FromUtf8(path));
	if (host.IsEmpty())
	{
		g_files->CloseStream(stream);
		errno = EACCES;
		return nullptr;
	}
	return g_files->ReopenStream(stream, String::FromUtf8(path), host, parsed.native, parsed.flags);
}

int CloseStream(FILE* stream)
{
	if (g_files == nullptr) { errno = EBADF; return EOF; }
	return g_files->CloseStream(stream);
}

int StreamDescriptor(FILE* stream)
{
	// Standard streams are explicitly admitted; no host-integer fallback.
	if (stream == stdin) { return 0; }
	if (stream == stdout) { return 1; }
	if (stream == stderr) { return 2; }
	if (g_files == nullptr) { errno = EBADF; return -1; }
	return g_files->StreamDescriptor(stream);
}

FILE* StandardStream(int descriptor)
{
	// Library-only registration (including symbol-catalog tests) need not mount
	// a filesystem or create a sandbox just to obtain its standard FILE objects.
	return InitializeDescriptors()->StandardStream(descriptor);
}

struct StreamOperation::State
{
	FILE* stream = nullptr;
	std::unique_ptr<StreamUse> use;
};

StreamOperation::StreamOperation(FILE* stream): m_state(std::make_unique<State>())
{
	m_state->stream = stream;
	const auto state = g_files != nullptr ? g_files->GetStreamState(stream) : nullptr;
	if (state != nullptr) { m_state->use = std::make_unique<StreamUse>(state); }
	if (stream == nullptr) { errno = EBADF; }
}

StreamOperation::~StreamOperation() = default;

bool StreamOperation::IsValid() const
{
	return m_state->stream != nullptr && (m_state->use == nullptr || m_state->use->IsValid());
}

int SetStreamBuffer(FILE* stream, char* buffer, int host_mode, size_t size)
{
	if (buffer != nullptr && size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(buffer)) { errno = EINVAL; return -1; }
	std::optional<Emulator::VideoFrameMemory::HostWriteLease> new_buffer;
	if (buffer != nullptr && size != 0) { new_buffer.emplace(reinterpret_cast<uint64_t>(buffer), size); }
	const auto state = g_files != nullptr ? g_files->GetStreamState(stream) : nullptr;
	// Host-created FILEs are not registered implicitly. In particular, installing
	// guest storage into the emulator's stdout/stderr would expose unleased writes
	// from logging. Exported standard streams are private registered duplicates.
	if (state == nullptr) { errno = EBADF; return -1; }
	const StreamUse use(state);
	if (!use.IsValid()) { return -1; }
	const int result = std::setvbuf(stream, buffer, host_mode, size);
	if (result == 0)
	{
		state->buffer_mode = host_mode;
		if (host_mode == _IONBF)
		{
			state->buffer = nullptr;
			state->size = 0;
		} else if (buffer != nullptr)
		{
			state->buffer = buffer;
			state->size = size;
		}
		// Buffered setvbuf(nullptr, ...) may retain an existing caller buffer.
		// Keep its association (and any host-owned storage) until explicitly
		// replaced, made unbuffered, reopened, or closed.
	}
	return result;
}

int FlushStreams(FILE* stream)
{
	if (stream != nullptr)
	{
		const StreamOperation operation(stream);
		return operation.IsValid() ? std::fflush(stream) : EOF;
	}
	if (g_files != nullptr) { return g_files->FlushStreams(); }
	return std::fflush(nullptr);
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

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (!file->opened) { return KERNEL_ERROR_EBADF; }

	KYTY_LOG_DEBUG("\tClose: " FG_WHITE BOLD "%s" DEFAULT "\n", file->real_name.C_Str());

	FsTrace("close", file->name.C_Str(), d, OK);

	return g_files->DeleteDescriptor(d);
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

static int64_t ReadFileToCompletion(Host::File& file, void* buffer, size_t size, int64_t offset = -1)
{
	auto*    out   = static_cast<uint8_t*>(buffer);
	uint64_t total = 0;
	while (total < size)
	{
		const uint64_t remaining = static_cast<uint64_t>(size) - total;
		const uint32_t request   = remaining > INT_MAX ? INT_MAX : static_cast<uint32_t>(remaining);
		const int64_t current   = offset < 0 ? file.Read(out + total, request) :
		                                      file.ReadAt(out + total, request, offset + static_cast<int64_t>(total));
		if (current < 0) { return total != 0 ? static_cast<int64_t>(total) : KernelErrorFromHost(errno); }
		total += current;
		if (current < request)
		{
			break;
		}
	}
	return total;
}

static int64_t WriteFileToCompletion(Host::File& file, const void* buffer, size_t size, int64_t offset = -1)
{
	const auto* input = static_cast<const uint8_t*>(buffer);
	uint64_t    total = 0;
	while (total < size)
	{
		const uint64_t remaining = static_cast<uint64_t>(size) - total;
		const uint32_t request   = remaining > INT_MAX ? INT_MAX : static_cast<uint32_t>(remaining);
		const int64_t current   = offset < 0 ? file.Write(input + total, request) :
		                                      file.WriteAt(input + total, request, offset + static_cast<int64_t>(total));
		if (current < 0) { return total != 0 ? static_cast<int64_t>(total) : KernelErrorFromHost(errno); }
		total += current;
		if (current < request)
		{
			break;
		}
	}
	return total;
}

int64_t KYTY_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr && nbytes != 0)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (nbytes > static_cast<size_t>(INT64_MAX))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}
	if (!file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}
	if ((file->status_flags.load(std::memory_order_acquire) & 3u) == 1u) { return KERNEL_ERROR_EBADF; }

	const Kyty::Emulator::VideoFrameMemory::HostWriteLease write_lease(reinterpret_cast<uint64_t>(buf), nbytes);
	int64_t bytes_read = 0;
	{
		Core::LockGuard lock(file->mutex);
		if (file->f.IsInvalid())
		{
			return KERNEL_ERROR_EIO;
		}
		bytes_read = ReadFileToCompletion(file->f, buf, nbytes);
	}
	KYTY_LOG_DEBUG("\tRead result %" PRId64 " from: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_read, file->real_name.C_Str());

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

	if (buf == nullptr && nbytes != 0)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (nbytes > static_cast<size_t>(INT64_MAX))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}
	if (!file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}
	if ((file->status_flags.load(std::memory_order_acquire) & 3u) == 0u) { return KERNEL_ERROR_EBADF; }

	int64_t bytes_written = 0;
	{
		Core::LockGuard lock(file->mutex);
		if (file->f.IsInvalid())
		{
			return KERNEL_ERROR_EIO;
		}
		bytes_written = WriteFileToCompletion(file->f, buf, nbytes);
	}

	KYTY_LOG_DEBUG("\tWrite result %" PRId64 " to: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_written, file->real_name.C_Str());

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

	if (buf == nullptr && nbytes != 0)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (nbytes > static_cast<size_t>(INT64_MAX - offset))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}
	if (!file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}
	if ((file->status_flags.load(std::memory_order_acquire) & 3u) == 1u) { return KERNEL_ERROR_EBADF; }

	const Kyty::Emulator::VideoFrameMemory::HostWriteLease write_lease(reinterpret_cast<uint64_t>(buf), nbytes);
	int64_t bytes_read = 0;
	{
		Core::LockGuard lock(file->mutex);
		if (file->f.IsInvalid())
		{
			return KERNEL_ERROR_EIO;
		}
		const StreamLocks streams(*file);
		bytes_read = ReadFileToCompletion(file->f, buf, nbytes, offset);
	}
	KYTY_LOG_DEBUG("\tRead result %" PRId64 " (pos = %" PRId64 ") from: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_read, offset,
	               file->real_name.C_Str());

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

	if (buf == nullptr && nbytes != 0)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}
	if (nbytes > static_cast<size_t>(INT64_MAX - offset))
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}
	if (!file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}
	if ((file->status_flags.load(std::memory_order_acquire) & 3u) == 0u) { return KERNEL_ERROR_EBADF; }

	int64_t bytes_written = 0;
	{
		Core::LockGuard lock(file->mutex);
		if (file->f.IsInvalid())
		{
			return KERNEL_ERROR_EIO;
		}
		const StreamLocks streams(*file);
		bytes_written = WriteFileToCompletion(file->f, buf, nbytes, offset);
	}

	KYTY_LOG_DEBUG("\tWrite result %" PRId64 " (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", bytes_written, offset,
	               file->real_name.C_Str());

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

	auto file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}
	if (!file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}
	if (whence < 0 || whence > 2)
	{
		return KERNEL_ERROR_EINVAL;
	}

	Core::LockGuard lock(file->mutex);
	if (file->f.IsInvalid())
	{
		return KERNEL_ERROR_EIO;
	}

	int64_t signed_base = 0;
	if (whence == 1)
	{
		signed_base = file->f.Tell();
	} else if (whence == 2)
	{
		signed_base = file->f.Size();
	}
	if (signed_base < 0) { return KernelErrorFromHost(errno); }
	const auto base = static_cast<uint64_t>(signed_base);

	uint64_t target = 0;
	if (whence == 0)
	{
		if (offset < 0)
		{
			return KERNEL_ERROR_EINVAL;
		}
		target = static_cast<uint64_t>(offset);
	} else if (offset < 0)
	{
		const uint64_t magnitude = static_cast<uint64_t>(-(offset + 1)) + 1u;
		if (magnitude > base)
		{
			return KERNEL_ERROR_EINVAL;
		}
		target = base - magnitude;
	} else
	{
		const uint64_t delta = static_cast<uint64_t>(offset);
		if (delta > std::numeric_limits<uint64_t>::max() - base)
		{
			return KERNEL_ERROR_EINVAL;
		}
		target = base + delta;
	}

	if (target > static_cast<uint64_t>(INT64_MAX))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const int64_t position = file->f.Seek(static_cast<int64_t>(target), SEEK_SET);
	if (position < 0)
	{
		return KernelErrorFromHost(errno);
	}

	KYTY_LOG_DEBUG("\tLseek (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", position, file->real_name.C_Str());
	return static_cast<int64_t>(position);
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
	sb->st_flags = 0;

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
		return KERNEL_ERROR_EBADF;
	}

	if (sb == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto file = g_files->GetFile(d);

	if (file == nullptr || !file->opened)
	{
		return KERNEL_ERROR_EBADF;
	}

	Core::LockGuard lock(file->mutex);
	memset(sb, 0, sizeof(FileStat));
	if (file->directory)
	{
		sb->st_mode = 0000777u | 0040000u;
		sb->st_blksize = 512;
		return OK;
	}
	Host::File::Stat host_stat {};
	if (!file->f.GetStat(&host_stat)) { return KernelErrorFromHost(errno); }
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
	sb->st_flags       = 0;
	sb->st_blksize     = 512;
	sb->st_blocks      = (sb->st_size + 511) / 512;
	sb->st_atim.tv_sec = static_cast<int64_t>(host_stat.st_atime);
	sb->st_mtim.tv_sec = static_cast<int64_t>(host_stat.st_mtime);
	sb->st_ctim.tv_sec = static_cast<int64_t>(host_stat.st_ctime);
#else
	sb->st_flags        = 0;
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

	auto file = g_files->GetFile(d);
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

	return file->f.Truncate(length) ? OK : KernelErrorFromHost(errno);
}

int KYTY_SYSV_ABI KernelFcntl(int d, int command, int64_t argument)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);
	auto file = g_files->GetFile(d);
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
			Core::LockGuard lock(file->mutex);
			const auto requested = static_cast<uint32_t>(argument);
			auto       current   = file->status_flags.load(std::memory_order_acquire);
			current              = (current & ~mutable_status_flags) | (requested & mutable_status_flags);
			if (!file->directory && !file->f.SetAppend((current & 8u) != 0)) { return KernelErrorFromHost(errno); }
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

	auto file = g_files->GetFile(d);
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

	const auto position = file->f.Tell();
	if (position < 0) { return KernelErrorFromHost(errno); }
	const auto size = file->f.Size();
	if (size < 0) { return KernelErrorFromHost(errno); }
	*available = size > position ? static_cast<uint64_t>(size - position) : 0;
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

	auto file = g_files->GetFile(d);
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

	auto file = g_files->GetFile(fd);

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
static String ResolveExistingHostFileUnchecked(const String& guest_path, const String& real_file_name)
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

static String ResolveExistingHostFile(const String& guest_path, const String& real_file_name)
{
	if (real_file_name.IsEmpty()) { return {}; }
	const auto candidate = ResolveExistingHostFileUnchecked(guest_path, real_file_name);
	return g_mount_points->IsHostFilenameAllowed(candidate) ? candidate : String();
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
	st->st_flags   = 0;
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
