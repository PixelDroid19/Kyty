#include "Kyty/UnitTest.h"

#include "Kyty/Core/File.h"
#include "Kyty/Sys/SysFileIO.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Emulator/VideoFrameMemory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

UT_BEGIN(EmulatorAmprRead);

using namespace Libs;
using namespace Kernel::EventQueue;

namespace {

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
constexpr int kReadError = EACCES;
constexpr int kKernelReadError = LibKernel::KERNEL_ERROR_EACCES;
#else
constexpr int kReadError = EISDIR;
constexpr int kKernelReadError = LibKernel::KERNEL_ERROR_EISDIR;
#endif

struct LeaseObservation
{
	uint32_t begins = 0;
	uint32_t ends = 0;
	uint32_t notifications = 0;
	uint64_t base = 0;
	uint64_t size = 0;
};

LeaseObservation* g_lease_observation = nullptr;

void IgnoreFrame(uint64_t, size_t, uint32_t) {}
void IgnoreUnregister(uint64_t) {}
void ObserveNotification(uint64_t, uint64_t) { ++g_lease_observation->notifications; }

uint64_t ObserveBegin(uint64_t base, uint64_t size)
{
	++g_lease_observation->begins;
	g_lease_observation->base = base;
	g_lease_observation->size = size;
	return 1;
}

void ObserveEnd(uint64_t token)
{
	EXPECT_EQ(token, 1u);
	++g_lease_observation->ends;
}

class ScopedLeaseObservation
{
public:
	explicit ScopedLeaseObservation(LeaseObservation* state)
	{
		g_lease_observation = state;
		EXPECT_TRUE(Emulator::VideoFrameMemory::InstallCallbacks(
		    {&IgnoreFrame, &IgnoreUnregister, &ObserveNotification, &ObserveBegin, &ObserveEnd}));
	}
	~ScopedLeaseObservation()
	{
		EXPECT_TRUE(Emulator::VideoFrameMemory::InstallCallbacks({}));
		g_lease_observation = nullptr;
	}
};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
class ScopedReadPipe
{
public:
	~ScopedReadPipe()
	{
		if (stream != nullptr) { std::fclose(stream); }
		if (descriptors[0] >= 0) { close(descriptors[0]); }
		if (descriptors[1] >= 0) { close(descriptors[1]); }
	}

	bool Open()
	{
		if (pipe(descriptors.data()) != 0) { return false; }
		const int flags = fcntl(descriptors[0], F_GETFL);
		if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) != 0) { return false; }
		stream = fdopen(descriptors[0], "r");
		if (stream == nullptr) { return false; }
		descriptors[0] = -1; // the FILE now owns the read descriptor
		return true;
	}

	FILE* stream = nullptr;
	std::array<int, 2> descriptors {-1, -1};
};
#endif

} // namespace

class EmulatorAmprRead: public ::testing::Test
{
protected:
	using CommandFn = KYTY_SYSV_ABI int (*)(void*);
	using SetBufferFn = KYTY_SYSV_ABI int (*)(void*, void*, uint32_t);
	using ReadFn = KYTY_SYSV_ABI int (*)(void*, uint64_t, uint64_t, uint32_t, void*, uint64_t, uint64_t);
	using WriteFn = KYTY_SYSV_ABI int (*)(void*, uint64_t*, uint64_t);
	using EventFn = KYTY_SYSV_ABI int (*)(void*, void*, uint64_t, uint64_t, uint64_t);

	void SetUp() override
	{
		if (!Config::IsInitialized())
		{
			Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		if (!Kernel::PthreadIsInitialized())
		{
			Kernel::PthreadSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}
		if (!Kernel::FileSystem::IsMounted())
		{
			Kernel::FileSystem::FileSystemSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		}

		static std::atomic_uint64_t sequence {0};
		const auto id = sequence.fetch_add(1);
		const char* temporary = std::getenv("TMPDIR");
		std::error_code error;
		const auto base = temporary != nullptr && temporary[0] != '\0'
		                      ? std::filesystem::absolute(std::filesystem::path(temporary), error)
		                      : std::filesystem::current_path(error);
		ASSERT_FALSE(error) << error.message();
		root = base / ("kyty-ampr-read-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
		               "-" + std::to_string(id));
		root_created = std::filesystem::create_directory(root, error);
		ASSERT_TRUE(root_created) << error.message();
		guest_mount = "/ampr-read-test-" + std::to_string(id) + "/";
		Kernel::FileSystem::Mount(String::FromUtf8(root.u8string().c_str()), String::FromUtf8(guest_mount.c_str()));
		mounted = true;

		Loader::SymbolDatabase symbols;
		ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));
		auto resolve = [&symbols](const char* nid) -> uintptr_t {
			Loader::SymbolResolve query {};
			query.name = String::FromUtf8(nid);
			query.library = U"Ampr";
			query.library_version = 1;
			query.module = U"Ampr";
			query.module_version_major = 1;
			query.module_version_minor = 1;
			query.type = Loader::SymbolType::Func;
			const auto* record = symbols.Find(query);
			return record != nullptr ? static_cast<uintptr_t>(record->vaddr) : 0;
		};
		auto construct = reinterpret_cast<CommandFn>(resolve("8aI7R7WaOlc"));
		destroy = reinterpret_cast<CommandFn>(resolve("GuchCTefuZw"));
		auto set_buffer = reinterpret_cast<SetBufferFn>(resolve("N-FSPA4S3nI"));
		read_file = reinterpret_cast<ReadFn>(resolve("mQ16-QdKv7k"));
		write_address = reinterpret_cast<WriteFn>(resolve("sJXyWHjP-F8"));
		write_event = reinterpret_cast<EventFn>(resolve("o67gODLFpls"));
		ASSERT_NE(construct, nullptr);
		ASSERT_NE(destroy, nullptr);
		ASSERT_NE(set_buffer, nullptr);
		ASSERT_NE(read_file, nullptr);
		ASSERT_NE(write_address, nullptr);
		ASSERT_NE(write_event, nullptr);
		ASSERT_EQ(construct(command.data()), OK);
		command_constructed = true;
		ASSERT_EQ(set_buffer(command.data(), stream.data(), static_cast<uint32_t>(stream.size())), OK);
	}

	void TearDown() override
	{
		ReleaseNativeLock();
		if (command_constructed) { EXPECT_EQ(destroy(command.data()), OK); }
		if (queue != nullptr) { EXPECT_EQ(KernelDeleteEqueue(queue), OK); }
		if (mounted) { Kernel::FileSystem::Umount(String::FromUtf8(guest_mount.c_str())); }
		if (root_created)
		{
			std::error_code error;
			std::filesystem::remove_all(root, error);
			EXPECT_FALSE(error) << error.message();
		}
	}

	String HostPath(const char* name) const { return String::FromUtf8((root / name).u8string().c_str()); }

	bool Seed(const char* name, const std::string& data)
	{
		std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
		file.write(data.data(), static_cast<std::streamsize>(data.size()));
		file.close();
		return !file.fail();
	}

	int Resolve(const char* name, uint32_t* id)
	{
		const std::string path = guest_mount + name;
		const char* paths[] = {path.c_str()};
		return Kernel::FileSystem::KernelAprResolveFilepathsToIds(paths, 1, id);
	}

	bool MakeNativeReadFail(const char* name)
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Another read handle is compatible with AMPR's FILE_SHARE_READ open,
		// but an exclusive byte-range lock rejects ReadFile on that new handle.
		locked_file = CreateFileW((root / name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                          FILE_ATTRIBUTE_NORMAL, nullptr);
		if (locked_file == INVALID_HANDLE_VALUE) { return false; }
		return LockFileEx(locked_file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 64, 0, &lock_range) != 0;
#else
		// Resolve a regular file first, then replace only this fixture path.
		// fopen/fseek still succeed on the directory; fread produces EISDIR.
		std::error_code error;
		if (!std::filesystem::remove(root / name, error) || error) { return false; }
		return std::filesystem::create_directory(root / name, error) && !error;
#endif
	}

	void ReleaseNativeLock()
	{
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		if (locked_file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(locked_file); // closing also releases the byte-range lock
			locked_file = INVALID_HANDLE_VALUE;
		}
#endif
	}

	bool RestoreReadableFile(const char* name, const std::string& data)
	{
		ReleaseNativeLock();
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		std::error_code error;
		if (!std::filesystem::remove(root / name, error) || error) { return false; }
#endif
		return Seed(name, data);
	}

	int32_t CommandCount() const
	{
		int32_t count = 0;
		std::memcpy(&count, command.data() + 8, sizeof(count));
		return count;
	}

	void ExpectNoEvent()
	{
		KernelEvent event {};
		Kernel::KernelUseconds zero = 0;
		int count = 0;
		EXPECT_EQ(KernelWaitEqueue(queue, &event, 1, &count, &zero), LibKernel::KERNEL_ERROR_ETIMEDOUT);
		EXPECT_EQ(count, 0);
	}

	std::filesystem::path root;
	std::string guest_mount;
	bool root_created = false;
	bool mounted = false;
	bool command_constructed = false;
	CommandFn destroy = nullptr;
	ReadFn read_file = nullptr;
	WriteFn write_address = nullptr;
	EventFn write_event = nullptr;
	KernelEqueue queue = nullptr;
	alignas(8) std::array<uint8_t, 0x28> command {};
	alignas(8) std::array<uint8_t, 0x80> stream {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	HANDLE locked_file = INVALID_HANDLE_VALUE;
	OVERLAPPED lock_range {};
#endif
};

TEST_F(EmulatorAmprRead, CoreReadDistinguishesEmptyEofFromNativeFailure)
{
	ASSERT_TRUE(Seed("empty", ""));
	ASSERT_TRUE(Seed("failure", "data"));
	ASSERT_TRUE(MakeNativeReadFail("failure"));
	std::array<uint8_t, 32> data;
	data.fill(0xcc);
	const auto sentinel = data;
	{
		Core::File file;
		ASSERT_TRUE(file.Open(HostPath("empty"), Core::File::Mode::Read));
		uint32_t count = 99;
		errno = EBUSY;
		EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), 0);
		EXPECT_EQ(count, 0u);
		EXPECT_EQ(errno, EBUSY);
		EXPECT_EQ(data, sentinel);
	}
	{
		Core::File file;
		ASSERT_TRUE(file.Open(HostPath("failure"), Core::File::Mode::Read));
		ASSERT_TRUE(file.Seek(0)); // the test requires a read failure, not an open/seek failure
		uint32_t count = 99;
		errno = EBUSY;
		EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), kReadError);
		EXPECT_EQ(count, 0u);
		EXPECT_EQ(errno, EBUSY);
		EXPECT_EQ(data, sentinel);
		EXPECT_NE(file.Read(data.data(), static_cast<uint32_t>(data.size())), 0);
	}
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
TEST_F(EmulatorAmprRead, NativePartialTransferRetainsErrorAndActualByteCount)
{
	ScopedReadPipe pipe;
	ASSERT_TRUE(pipe.Open());
	ASSERT_EQ(write(pipe.descriptors[1], "abc", 3), 3);
	sys_file_t file {};
	file.type = SYS_FILE_FILE;
	file.f = pipe.stream;
	std::array<uint8_t, 32> data;
	data.fill(0xcc);
	uint32_t count = 99;
	errno = EBUSY;
	// The writer stays open. After the three queued bytes, the same fread
	// reaches EAGAIN on the nonblocking read end, rather than EOF or a wait.
	EXPECT_EQ(sys_file_read(data.data(), static_cast<uint32_t>(data.size()), file, &count), EAGAIN);
	EXPECT_EQ(errno, EBUSY);
	ASSERT_EQ(count, 3u);
	EXPECT_NE(std::ferror(pipe.stream), 0);
	EXPECT_EQ(std::feof(pipe.stream), 0);
	EXPECT_EQ(std::memcmp(data.data(), "abc", 3), 0);
	for (size_t i = count; i < data.size(); ++i) { EXPECT_EQ(data[i], 0xcc); }

	// Once the writer closes, a later EOF must not hide the sticky error.
	// With no new native errno available, the checked API reports EIO.
	ASSERT_EQ(close(pipe.descriptors[1]), 0);
	pipe.descriptors[1] = -1;
	const auto after_read = data;
	count = 99;
	EXPECT_EQ(sys_file_read(data.data(), static_cast<uint32_t>(data.size()), file, &count), EIO);
	EXPECT_EQ(count, 0u);
	EXPECT_EQ(data, after_read);
	std::clearerr(pipe.stream);
	EXPECT_EQ(sys_file_read(data.data(), static_cast<uint32_t>(data.size()), file, &count), 0);
	EXPECT_EQ(count, 0u);
	EXPECT_NE(std::feof(pipe.stream), 0);
}
#endif

TEST_F(EmulatorAmprRead, CoreShortReadReportsOnlyTransferredBytesAndThenEof)
{
	ASSERT_TRUE(Seed("short", "abc"));
	Core::File file;
	ASSERT_TRUE(file.Open(HostPath("short"), Core::File::Mode::Read));
	std::array<uint8_t, 32> data;
	data.fill(0xcc);
	uint32_t count = 99;
	EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), 0);
	ASSERT_EQ(count, 3u);
	EXPECT_EQ(std::memcmp(data.data(), "abc", 3), 0);
	for (size_t i = count; i < data.size(); ++i) { EXPECT_EQ(data[i], 0xcc); }
	const auto after_read = data;
	EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), 0);
	EXPECT_EQ(count, 0u);
	EXPECT_EQ(data, after_read);
}

TEST_F(EmulatorAmprRead, MemoryBackedReadSharesCheckedCountContract)
{
	std::array<uint8_t, 3> source {1, 2, 3};
	std::array<uint8_t, 8> data {};
	Core::File file;
	ASSERT_TRUE(file.OpenInMem(source.data(), static_cast<uint32_t>(source.size())));
	uint32_t count = 99;
	EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), 0);
	EXPECT_EQ(count, source.size());
	EXPECT_EQ(std::memcmp(data.data(), source.data(), source.size()), 0);
	EXPECT_EQ(file.Read(data.data(), static_cast<uint32_t>(data.size()), &count), 0);
	EXPECT_EQ(count, 0u);
}

TEST_F(EmulatorAmprRead, NativeReadFailureIsTheExecutionResultAndCompletionActionsStillRun)
{
	const std::string payload = "bounded native read payload";
	ASSERT_TRUE(Seed("payload", payload));
	uint32_t file_id = 0;
	ASSERT_EQ(Resolve("payload", &file_id), OK);
	ASSERT_TRUE(MakeNativeReadFail("payload"));
	std::array<uint8_t, 32> destination;
	destination.fill(0xcc);
	const auto sentinel = destination;
	uint64_t completion = UINT64_C(0x1111222233334444);
	const auto record = [&](uint64_t value)
	{
		ASSERT_EQ(read_file(command.data(), 0, 0, file_id, destination.data(), payload.size(), 0), OK);
		ASSERT_EQ(write_address(command.data(), &completion, value), OK);
		ASSERT_EQ(write_event(command.data(), queue, 7, 0x66, 0), OK);
		ASSERT_EQ(CommandCount(), 3);
	};
	const auto expect_event = [&]()
	{
		KernelEvent event {};
		Kernel::KernelUseconds zero = 0;
		int count = 0;
		ASSERT_EQ(KernelWaitEqueue(queue, &event, 1, &count, &zero), OK);
		EXPECT_EQ(count, 1);
		EXPECT_EQ(event.ident, 7u);
		EXPECT_EQ(event.filter, KERNEL_EVFILT_AMPR);
		EXPECT_EQ(event.data, 0x66);
		ExpectNoEvent();
	};
	ASSERT_EQ(KernelCreateEqueue(&queue, "ampr-read-error"), OK);
	ASSERT_EQ(KernelAddAmprEvent(queue, 7, nullptr), OK);
	record(UINT64_C(0xabcdef));
	ExpectNoEvent();

	// The submission is accepted: the read fails, and the write and event after it
	// still tell waiters the buffer finished.
	LeaseObservation observation;
	ScopedLeaseObservation lease_scope(&observation);
	EXPECT_EQ(Kernel::FileSystem::KernelAprSubmitCommandBuffer(command.data(), 1, nullptr, 7, nullptr), OK);
	EXPECT_EQ(destination, sentinel);
	EXPECT_EQ(completion, UINT64_C(0xabcdef));
	EXPECT_EQ(CommandCount(), 0);
	EXPECT_EQ(observation.begins, 1u);
	EXPECT_EQ(observation.ends, 1u);
	EXPECT_EQ(observation.base, reinterpret_cast<uint64_t>(destination.data()));
	EXPECT_EQ(observation.size, payload.size());
	expect_event();

	// The result form reports the read's error at its record offset (the first record).
	record(UINT64_C(0xabcdee));
	uint32_t submission_id = 0;
	std::array<uint32_t, 2> result {0x11223344, 0x55667788};
	EXPECT_EQ(Kernel::FileSystem::KernelAprSubmitCommandBufferAndGetResult(command.data(), 1, result.data(), &submission_id), OK);
	EXPECT_NE(submission_id, 0u);
	EXPECT_EQ(result[0], static_cast<uint32_t>(kKernelReadError));
	EXPECT_EQ(result[1], 0u);
	EXPECT_EQ(destination, sentinel);
	EXPECT_EQ(completion, UINT64_C(0xabcdee));
	expect_event();

	// After the file reads again the same records complete with a zero result.
	ASSERT_TRUE(RestoreReadableFile("payload", payload));
	record(UINT64_C(0xabcdef));
	EXPECT_EQ(Kernel::FileSystem::KernelAprSubmitCommandBufferAndGetResult(command.data(), 1, result.data(), &submission_id), OK);
	EXPECT_EQ(result[0], 0u);
	EXPECT_EQ(std::memcmp(destination.data(), payload.data(), payload.size()), 0);
	for (size_t i = payload.size(); i < destination.size(); ++i) { EXPECT_EQ(destination[i], 0xcc); }
	EXPECT_EQ(completion, UINT64_C(0xabcdef));
	EXPECT_EQ(CommandCount(), 0);
	expect_event();
}

UT_END();
