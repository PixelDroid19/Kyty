#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/Errors.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Log.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/VideoFrameMemory.h"
#include "Kyty/Core/VirtualMemory.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Kyty::Libs::LibC {
FILE* KYTY_SYSV_ABI c_fopen(const char*, const char*);
FILE* KYTY_SYSV_ABI c_fdopen(int, const char*);
FILE* KYTY_SYSV_ABI c_freopen(const char*, const char*, FILE*);
int KYTY_SYSV_ABI c_fclose(FILE*);
int KYTY_SYSV_ABI c_fileno(FILE*);
size_t KYTY_SYSV_ABI c_fread(void*, size_t, size_t, FILE*);
size_t KYTY_SYSV_ABI c_fwrite(const void*, size_t, size_t, FILE*);
int KYTY_SYSV_ABI c_fseek(FILE*, long, int);
int KYTY_SYSV_ABI c_feof(FILE*);
int KYTY_SYSV_ABI c_ferror(FILE*);
char* KYTY_SYSV_ABI c_fgets(char*, int, FILE*);
int KYTY_SYSV_ABI c_setvbuf(FILE*, char*, int, size_t);
int KYTY_SYSV_ABI c_fputc(int, FILE*);
int KYTY_SYSV_ABI c_fputs(const char*, FILE*);
int KYTY_SYSV_ABI c_fgetc(FILE*);
void KYTY_SYSV_ABI c_rewind(FILE*);
long KYTY_SYSV_ABI c_ftell(FILE*);
int KYTY_SYSV_ABI c_fstat(int, Kyty::Kernel::FileSystem::FileStat*);
}

namespace Kyty::Libs::LibcInternal {
int KYTY_SYSV_ABI fflush(FILE*);
}

UT_BEGIN(EmulatorFileDescriptors);

using namespace Kernel;
using namespace Kernel::FileSystem;
using namespace Libs::LibC;

namespace {

class StdioBufferWatch;
StdioBufferWatch* g_stdio_buffer_watch = nullptr;

// A deterministic artificial write-watch: every completed production operation
// must return the caller buffer to read-only. A missing lease makes native
// refill fail (or a userspace buffer write fault), rather than merely a counter
// mismatch. The callbacks are process-lifetime functions, including for workers.
class StdioBufferWatch
{
public:
	StdioBufferWatch()
	{
		size = Core::VirtualMemory::GetPageSize();
		address = Core::VirtualMemory::Alloc(0, size, Core::VirtualMemory::Mode::ReadWrite);
		if (address == 0) { return; }
		std::memset(Data(), 0, size);
		read_only = Core::VirtualMemory::Protect(address, size, Core::VirtualMemory::Mode::Read);
		g_stdio_buffer_watch = this;
		installed = Emulator::VideoFrameMemory::InstallCallbacks({&Register, &Unregister, &Notify, &Begin, &End});
	}
	~StdioBufferWatch()
	{
		if (installed) { EXPECT_TRUE(Emulator::VideoFrameMemory::InstallCallbacks({})); }
		g_stdio_buffer_watch = nullptr;
		if (address != 0) { EXPECT_TRUE(Core::VirtualMemory::Free(address)); }
	}
	bool Ready() const { return address != 0 && read_only && installed; }
	char* Data() const { return reinterpret_cast<char*>(address); }
	size_t Size() const { return size; }
	uint32_t Begins()
	{
		std::lock_guard lock(mutex);
		return begins;
	}
	void ExpectReleasedSince(uint32_t before)
	{
		std::lock_guard lock(mutex);
		EXPECT_GT(begins, before);
		EXPECT_EQ(ends, begins);
		EXPECT_EQ(active, 0u);
		EXPECT_TRUE(read_only);
	}
	bool WaitForActive()
	{
		std::unique_lock lock(mutex);
		return entered.wait_for(lock, std::chrono::seconds(2), [&] { return active != 0 && !read_only; });
	}

private:
	static void Register(uint64_t, size_t, uint32_t) {}
	static void Unregister(uint64_t) {}
	static void Notify(uint64_t, uint64_t) { ADD_FAILURE() << "paired write lease was bypassed"; }
	static uint64_t Begin(uint64_t base, uint64_t bytes)
	{
		auto* watch = g_stdio_buffer_watch;
		if (watch == nullptr || base == 0 || bytes == 0 || base > UINT64_MAX - bytes ||
		    base >= watch->address + watch->size || base + bytes <= watch->address) { return 0; }
		std::lock_guard lock(watch->mutex);
		EXPECT_EQ(base, watch->address);
		EXPECT_EQ(bytes, watch->size);
		if (watch->active++ == 0)
		{
			EXPECT_TRUE(Core::VirtualMemory::Protect(watch->address, watch->size, Core::VirtualMemory::Mode::ReadWrite));
			watch->read_only = false;
		}
		++watch->begins;
		watch->entered.notify_all();
		errno = ERANGE; // The production lease must preserve the native errno.
		return 1;
	}
	static void End(uint64_t token)
	{
		if (token == 0) { return; }
		auto* watch = g_stdio_buffer_watch;
		std::lock_guard lock(watch->mutex);
		EXPECT_EQ(token, 1u);
		EXPECT_GT(watch->active, 0u);
		if (--watch->active == 0)
		{
			watch->read_only = Core::VirtualMemory::Protect(watch->address, watch->size, Core::VirtualMemory::Mode::Read);
			EXPECT_TRUE(watch->read_only);
		}
		++watch->ends;
		errno = ERANGE;
	}
	uint64_t address = 0;
	size_t size = 0;
	bool installed = false;
	bool read_only = false;
	uint32_t begins = 0;
	uint32_t ends = 0;
	uint32_t active = 0;
	std::mutex mutex;
	std::condition_variable entered;
};

} // namespace

class EmulatorFileDescriptors: public ::testing::Test
{
protected:
	void SetUp() override
	{
		if (!Config::IsInitialized()) { Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		if (!PthreadIsInitialized()) { PthreadSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		if (!IsMounted()) { FileSystemSubsystem::Instance()->Init(Core::SubsystemsList::Instance()); }
		static std::atomic_uint64_t sequence {0};
		const char* temporary = std::getenv("KYTY_TEST_TMPDIR");
		const auto base = temporary != nullptr ? std::filesystem::path(temporary) : std::filesystem::temp_directory_path();
		root = base / ("kyty-descriptors-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
		               "-" + std::to_string(sequence.fetch_add(1)));
		ASSERT_TRUE(std::filesystem::create_directories(root));
		Mount(String::FromUtf8(root.string().c_str()), U"/descriptor-test/");
	}

	void TearDown() override
	{
		for (FILE* stream: streams) { c_fclose(stream); }
		for (int descriptor: descriptors) { KernelClose(descriptor); }
		// Keep buffer and callbacks alive through native fclose, even after a
		// fatal test assertion. This order is part of the lifetime control.
		watch.reset();
		Umount(U"/descriptor-test/");
		std::error_code error;
		std::filesystem::remove_all(root, error);
	}

	void Seed(const char* name, const std::string& bytes)
	{
		std::ofstream output(root / name, std::ios::binary);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		ASSERT_TRUE(output.good());
	}

	std::string Contents(const char* name)
	{
		std::ifstream input(root / name, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	int Open(const char* name, int flags)
	{
		const int descriptor = KernelOpen((std::string("/descriptor-test/") + name).c_str(), flags, 0600);
		if (descriptor >= 0) { descriptors.push_back(descriptor); }
		return descriptor;
	}

	FILE* Stream(const char* name, const char* mode)
	{
		FILE* stream = c_fopen((std::string("/descriptor-test/") + name).c_str(), mode);
		if (stream != nullptr) { streams.push_back(stream); }
		return stream;
	}

	std::filesystem::path root;
	std::vector<int> descriptors;
	std::vector<FILE*> streams;
	std::unique_ptr<StdioBufferWatch> watch;
};

TEST_F(EmulatorFileDescriptors, StreamDescriptorCannotCollideWithUnrelatedGuestOpens)
{
	Seed("a", "abc");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	ASSERT_GE(descriptor, 3);
	for (int i = 0; i < 64; ++i)
	{
		const int other = Open("b", 0);
		ASSERT_GE(other, 3);
		ASSERT_NE(other, descriptor);
	}
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 3);
	char bytes[3] {};
	ASSERT_EQ(KernelRead(descriptor, bytes, sizeof(bytes)), 3);
	EXPECT_EQ(std::string(bytes, sizeof(bytes)), "abc");
}

TEST_F(EmulatorFileDescriptors, FdopenRetainsDescriptorAndSynchronizedSharedOffset)
{
	Seed("a", "abcdef");
	const int descriptor = Open("a", 2);
	ASSERT_GE(descriptor, 3);
	FILE* stream = c_fdopen(descriptor, "r+b");
	ASSERT_NE(stream, nullptr);
	streams.push_back(stream);
	EXPECT_EQ(c_fileno(stream), descriptor);
	char bytes[3] {};
	ASSERT_EQ(c_fread(bytes, 1, 2, stream), 2u);
	EXPECT_EQ(std::string(bytes, 2), "ab");
	// POSIX active-handle handoff: reconcile any stream read-ahead first.
	ASSERT_EQ(std::fflush(stream), 0);
	EXPECT_EQ(KernelLseek(descriptor, 0, SEEK_CUR), 2);
	ASSERT_EQ(KernelRead(descriptor, bytes, 2), 2);
	EXPECT_EQ(std::string(bytes, 2), "cd");
	ASSERT_EQ(c_fseek(stream, 4, SEEK_SET), 0);
	ASSERT_EQ(c_fread(bytes, 1, 2, stream), 2u);
	EXPECT_EQ(std::string(bytes, 2), "ef");
}

TEST_F(EmulatorFileDescriptors, FcloseClosesOnlyItsDescriptorAndDupSurvives)
{
	Seed("a", "abcdef");
	FILE* stream = Stream("a", "r+b");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	ASSERT_EQ(c_fwrite("XY", 1, 2, stream), 2u);
	ASSERT_EQ(c_fclose(stream), 0);
	streams.clear();
	FileStat status {};
	EXPECT_EQ(KernelFstat(descriptor, &status), KERNEL_ERROR_EBADF);
	EXPECT_EQ(KernelLseek(duplicate, 0, SEEK_CUR), 2);
	char bytes[3] {};
	ASSERT_EQ(KernelRead(duplicate, bytes, 2), 2);
	EXPECT_EQ(std::string(bytes, 2), "cd");
	EXPECT_EQ(Contents("a"), "XYcdef");
}

TEST_F(EmulatorFileDescriptors, ClosingDetachedStreamCannotCloseReusedGuestSlot)
{
	Seed("a", "abc");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	ASSERT_EQ(KernelClose(descriptor), OK);
	const int replacement = Open("b", 0);
	ASSERT_EQ(replacement, descriptor);
	EXPECT_EQ(c_fileno(stream), -1);
	EXPECT_EQ(c_fclose(stream), 0);
	streams.clear();
	FileStat status {};
	ASSERT_EQ(KernelFstat(replacement, &status), OK);
	EXPECT_EQ(status.st_size, 7);
}

TEST_F(EmulatorFileDescriptors, ReopenRebindsStreamDescriptorWithoutRetargetingDuplicate)
{
	Seed("a", "abc");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	FILE* reopened = c_freopen("/descriptor-test/b", "rb", stream);
	streams.clear();
	ASSERT_NE(reopened, nullptr);
	EXPECT_EQ(reopened, stream);
	streams.push_back(reopened);
	EXPECT_EQ(c_fileno(reopened), descriptor);
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 7);
	ASSERT_EQ(KernelFstat(duplicate, &status), OK);
	EXPECT_EQ(status.st_size, 3);
}

TEST_F(EmulatorFileDescriptors, ReopenWriteTruncatesNewFileAndRetainsOldDescription)
{
	Seed("a", "abc");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	FILE* reopened = c_freopen("/descriptor-test/b", "w+b", stream);
	streams.clear();
	ASSERT_NE(reopened, nullptr);
	EXPECT_EQ(reopened, stream);
	streams.push_back(reopened);
	EXPECT_EQ(c_fileno(reopened), descriptor);
	EXPECT_EQ(Contents("b"), "");
	ASSERT_EQ(c_fwrite("XY", 1, 2, reopened), 2u);
	ASSERT_EQ(std::fflush(reopened), 0);
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 2);
	ASSERT_EQ(KernelFstat(duplicate, &status), OK);
	EXPECT_EQ(status.st_size, 3);
	EXPECT_EQ(Contents("a"), "abc");
	EXPECT_EQ(Contents("b"), "XY");
}

TEST_F(EmulatorFileDescriptors, FailedReopenClosesOriginalStreamDescriptor)
{
	Seed("a", "abc");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	streams.clear(); // freopen closes the original even on failure.
	EXPECT_EQ(c_freopen("/descriptor-test/missing", "rb", stream), nullptr);
	FileStat status {};
	EXPECT_EQ(KernelFstat(descriptor, &status), KERNEL_ERROR_EBADF);
	ASSERT_EQ(KernelFstat(duplicate, &status), OK);
	EXPECT_EQ(status.st_size, 3);
}

TEST_F(EmulatorFileDescriptors, Dup2RetargetsRegisteredStreamAndKeepsOldDuplicate)
{
	Seed("a", "abcdef");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	char byte = 0;
	ASSERT_EQ(c_fread(&byte, 1, 1, stream), 1u);
	ASSERT_EQ(std::fflush(stream), 0);
	const int source = Open("b", 0);
	ASSERT_GE(source, 3);
	ASSERT_EQ(KernelLseek(source, 2, SEEK_SET), 2);
	ASSERT_EQ(KernelDup2(source, descriptor), descriptor);
	EXPECT_EQ(c_fileno(stream), descriptor);
	ASSERT_EQ(c_fseek(stream, 2, SEEK_SET), 0);
	ASSERT_EQ(c_fread(&byte, 1, 1, stream), 1u);
	EXPECT_EQ(byte, '3');
	ASSERT_EQ(KernelRead(duplicate, &byte, 1), 1);
	EXPECT_EQ(byte, 'b');
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 7);
}

TEST_F(EmulatorFileDescriptors, ReusedDescriptorAfterFcloseIdentifiesNewStream)
{
	Seed("a", "abc");
	Seed("b", "1234567");
	FILE* stream = Stream("a", "rb");
	ASSERT_NE(stream, nullptr);
	const int descriptor = c_fileno(stream);
	ASSERT_EQ(c_fclose(stream), 0);
	streams.clear();
	stream = Stream("b", "rb");
	ASSERT_NE(stream, nullptr);
	EXPECT_EQ(c_fileno(stream), descriptor);
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 7);
}

TEST_F(EmulatorFileDescriptors, FdopenValidatesAccessWithoutTruncatingOrClosingOnFailure)
{
	Seed("a", "abc");
	const int descriptor = Open("a", 0);
	ASSERT_GE(descriptor, 3);
	EXPECT_EQ(c_fdopen(descriptor, "wb"), nullptr);
	EXPECT_EQ(c_fdopen(descriptor, "invalid"), nullptr);
	FileStat status {};
	ASSERT_EQ(KernelFstat(descriptor, &status), OK);
	EXPECT_EQ(status.st_size, 3);
	const int writable = Open("a", 2);
	ASSERT_GE(writable, 3);
	FILE* stream = c_fdopen(writable, "wb");
	ASSERT_NE(stream, nullptr);
	streams.push_back(stream);
	ASSERT_EQ(KernelFstat(writable, &status), OK);
	EXPECT_EQ(status.st_size, 3);
}

TEST_F(EmulatorFileDescriptors, AppendSurvivesSeekDupAndIndependentOpen)
{
	Seed("a", "abc");
	const int first = Open("a", 1 | 8);
	const int second = Open("a", 1 | 8);
	ASSERT_GE(first, 3);
	ASSERT_GE(second, 3);
	const int duplicate = KernelDup(first);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	ASSERT_EQ(KernelLseek(first, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(duplicate, "X", 1), 1);
	ASSERT_EQ(KernelLseek(second, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(second, "Y", 1), 1);
	ASSERT_EQ(KernelLseek(first, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(first, "Z", 1), 1);
	EXPECT_EQ(KernelLseek(duplicate, 0, SEEK_CUR), 6);
	EXPECT_EQ(Contents("a"), "abcXYZ");
}

TEST_F(EmulatorFileDescriptors, FdopenAppendUsesSharedNativeAppendFlag)
{
	Seed("a", "abc");
	const int descriptor = Open("a", 2);
	ASSERT_GE(descriptor, 3);
	FILE* stream = c_fdopen(descriptor, "a+b");
	ASSERT_NE(stream, nullptr);
	streams.push_back(stream);
	EXPECT_EQ(c_fileno(stream), descriptor);
	EXPECT_NE(KernelFcntl(descriptor, 3, 0) & 8, 0);
	ASSERT_EQ(c_fseek(stream, 0, SEEK_SET), 0);
	ASSERT_EQ(c_fwrite("X", 1, 1, stream), 1u);
	ASSERT_EQ(std::fflush(stream), 0);
	ASSERT_EQ(KernelLseek(descriptor, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(descriptor, "Y", 1), 1);
	EXPECT_EQ(Contents("a"), "abcXY");
}

TEST_F(EmulatorFileDescriptors, AppendStatusChangesAreSharedAndPositionedWriteKeepsOffset)
{
	Seed("a", "abc");
	const int descriptor = Open("a", 2);
	ASSERT_GE(descriptor, 3);
	const int duplicate = KernelDup(descriptor);
	ASSERT_GE(duplicate, 3);
	descriptors.push_back(duplicate);
	ASSERT_EQ(KernelFcntl(duplicate, 4, 8), OK);
	EXPECT_NE(KernelFcntl(descriptor, 3, 0) & 8, 0);
	ASSERT_EQ(KernelLseek(descriptor, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(descriptor, "X", 1), 1);
	ASSERT_EQ(KernelPwrite(duplicate, "Q", 1, 1), 1);
	EXPECT_EQ(KernelLseek(descriptor, 0, SEEK_CUR), 4);
	EXPECT_EQ(Contents("a"), "aQcX");
	ASSERT_EQ(KernelLseek(descriptor, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(duplicate, "Y", 1), 1);
	EXPECT_EQ(Contents("a"), "aQcXY");
	ASSERT_EQ(KernelFcntl(descriptor, 4, 0), OK);
	ASSERT_EQ(KernelLseek(duplicate, 0, SEEK_SET), 0);
	ASSERT_EQ(KernelWrite(duplicate, "R", 1), 1);
	EXPECT_EQ(Contents("a"), "RQcXY");
}

TEST_F(EmulatorFileDescriptors, ConcurrentIndependentAppendWritesRetainWholeRecords)
{
	Seed("a", "");
	const int first = Open("a", 1 | 8);
	const int second = Open("a", 1 | 8);
	ASSERT_GE(first, 3);
	ASSERT_GE(second, 3);
	std::atomic_bool valid {true};
	auto write_records = [&](int descriptor, char value) {
		const std::string record(64, value);
		for (int i = 0; i < 128; ++i)
		{
			if (KernelWrite(descriptor, record.data(), record.size()) != 64) { valid.store(false); }
		}
	};
	std::thread a(write_records, first, 'A');
	std::thread b(write_records, second, 'B');
	a.join();
	b.join();
	ASSERT_TRUE(valid.load());
	const auto bytes = Contents("a");
	ASSERT_EQ(bytes.size(), 256u * 64u);
	size_t a_count = 0;
	for (size_t offset = 0; offset < bytes.size(); offset += 64)
	{
		const auto record = bytes.substr(offset, 64);
		ASSERT_TRUE(record == std::string(64, 'A') || record == std::string(64, 'B'));
		if (record[0] == 'A') { ++a_count; }
	}
	EXPECT_EQ(a_count, 128u);
}

TEST_F(EmulatorFileDescriptors, EofAndWrongAccessErrorsAreDistinct)
{
	Seed("a", "abc");
	const int read_only = Open("a", 0);
	const int write_only = Open("a", 1);
	ASSERT_GE(read_only, 3);
	ASSERT_GE(write_only, 3);
	char bytes[8] {};
	EXPECT_EQ(KernelRead(read_only, bytes, sizeof(bytes)), 3);
	EXPECT_EQ(KernelRead(read_only, bytes, sizeof(bytes)), 0);
	EXPECT_EQ(KernelRead(write_only, bytes, sizeof(bytes)), KERNEL_ERROR_EBADF);
	EXPECT_EQ(KernelWrite(read_only, "X", 1), KERNEL_ERROR_EBADF);
	FILE* stream = Stream("a", "wb");
	ASSERT_NE(stream, nullptr);
	EXPECT_EQ(c_fread(bytes, 1, sizeof(bytes), stream), 0u);
	EXPECT_NE(c_ferror(stream), 0);
	EXPECT_EQ(c_feof(stream), 0);
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
TEST_F(EmulatorFileDescriptors, UnregisteredLiveHostDescriptorIsNotGuestDescriptor)
{
	Seed("a", "abc");
	const int original = ::open((root / "a").c_str(), O_RDONLY);
	ASSERT_GE(original, 0);
	const int host_only = ::fcntl(original, F_DUPFD, 128);
	::close(original);
	ASSERT_GE(host_only, 128);
	FileStat status {};
	EXPECT_EQ(KernelFstat(host_only, &status), KERNEL_ERROR_EBADF);
	::close(host_only);
}

TEST_F(EmulatorFileDescriptors, HostReadFaultIsNotReportedAsEof)
{
	Seed("a", "abc");
	const int descriptor = Open("a", 0);
	ASSERT_GE(descriptor, 3);
	// An unmapped destination reaches read(2), which reports EFAULT without a
	// userspace dereference. The callback-free fixture has no artificial watch.
	EXPECT_EQ(KernelRead(descriptor, reinterpret_cast<void*>(1), 1), KERNEL_ERROR_EFAULT);
	EXPECT_EQ(KernelLseek(descriptor, 0, SEEK_CUR), 0);
}
#endif

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseCoversNativeRefillAndInputOperations)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	Seed("buffer-input", "abc\n" + std::string(watch->Size() * 2, 'r'));
	FILE* stream = Stream("buffer-input", "rb");
	ASSERT_NE(stream, nullptr);
	auto before = watch->Begins();
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	char byte = 0;
	ASSERT_EQ(c_fread(&byte, 1, 1, stream), 1u);
	EXPECT_EQ(byte, 'a');
	EXPECT_EQ(std::memcmp(watch->Data(), "abc\n", 4), 0);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	ASSERT_EQ(c_fseek(stream, 0, SEEK_SET), 0);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	char line[8] {};
	ASSERT_EQ(c_fgets(line, sizeof(line), stream), line);
	EXPECT_STREQ(line, "abc\n");
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	EXPECT_EQ(c_fgetc(stream), 'r');
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	c_rewind(stream);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(c_ftell(stream), 0);
	EXPECT_EQ(c_feof(stream), 0);
	EXPECT_EQ(c_ferror(stream), 0);
	// A rejected buffer-mode update must retain the previous association.
	EXPECT_EQ(c_setvbuf(stream, nullptr, -1, 0), -1);
	before = watch->Begins();
	EXPECT_EQ(c_fgetc(stream), 'a');
	watch->ExpectReleasedSince(before);
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseCoversOutputFlushAllSeekAndClose)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	FILE* stream = Stream("buffer-output", "w+b");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	auto before = watch->Begins();
	ASSERT_EQ(c_fputc('A', stream), 'A');
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(watch->Data()[0], 'A');
	EXPECT_EQ(Contents("buffer-output"), "");
	before = watch->Begins();
	ASSERT_EQ(c_fwrite("BC", 1, 2, stream), 2u);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	ASSERT_GE(c_fputs("D", stream), 0);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	ASSERT_EQ(Libs::LibcInternal::fflush(nullptr), 0);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("buffer-output"), "ABCD");
	ASSERT_EQ(c_fputc('E', stream), 'E');
	before = watch->Begins();
	ASSERT_EQ(c_fseek(stream, 0, SEEK_SET), 0);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("buffer-output"), "ABCDE");
	ASSERT_EQ(c_fputc('Z', stream), 'Z');
	before = watch->Begins();
	EXPECT_EQ(c_fclose(stream), 0);
	streams.clear();
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("buffer-output"), "ZBCDE");
}

TEST_F(EmulatorFileDescriptors, NullBufferModeChangeRetainsCallerStorageLease)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	FILE* stream = Stream("buffer-mode", "wb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	// Native setvbuf(NULL) may keep the already supplied storage. This control
	// targets that host behavior; it does not require the host to reallocate.
	auto before = watch->Begins();
	ASSERT_EQ(c_setvbuf(stream, nullptr, 0, watch->Size()), 0);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	EXPECT_EQ(c_fputc('A', stream), 'A');
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Libs::LibcInternal::fflush(stream), 0);
	EXPECT_EQ(Contents("buffer-mode"), "A");
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseSurvivesDup2FlushAndRetarget)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	Seed("retarget", "1234");
	FILE* stream = Stream("buffer-before-dup", "w+b");
	ASSERT_NE(stream, nullptr);
	const int target = c_fileno(stream);
	const int source = Open("retarget", 2);
	ASSERT_GE(source, 3);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	ASSERT_EQ(c_fputc('A', stream), 'A');
	auto before = watch->Begins();
	ASSERT_EQ(KernelDup2(source, target), target);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("buffer-before-dup"), "A");
	before = watch->Begins();
	ASSERT_EQ(c_fputc('Z', stream), 'Z');
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	ASSERT_EQ(Libs::LibcInternal::fflush(stream), 0);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("retarget"), "Z234");
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseEndsOnReopenAndDropsOldAssociation)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	FILE* stream = Stream("before-reopen", "wb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	ASSERT_EQ(c_fputc('A', stream), 'A');
	const auto before = watch->Begins();
	FILE* reopened = c_freopen("/descriptor-test/after-reopen", "wb", stream);
	streams.clear();
	ASSERT_NE(reopened, nullptr);
	streams.push_back(reopened);
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Contents("before-reopen"), "A");
	const auto after = watch->Begins();
	EXPECT_EQ(c_fputc('B', reopened), 'B');
	EXPECT_EQ(Libs::LibcInternal::fflush(reopened), 0);
	EXPECT_EQ(watch->Begins(), after);
	EXPECT_EQ(Contents("after-reopen"), "B");
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseEndsOnFailedReopenAndNativeError)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	Seed("read-only-buffer", "abc");
	FILE* stream = Stream("read-only-buffer", "rb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	auto before = watch->Begins();
	errno = 0;
	EXPECT_EQ(c_fwrite("x", 1, 1, stream), 0u);
	EXPECT_EQ(errno, EBADF);
	watch->ExpectReleasedSince(before);
	EXPECT_NE(c_ferror(stream), 0);
	before = watch->Begins();
	streams.clear();
	EXPECT_EQ(c_freopen("/descriptor-test/absent", "rb", stream), nullptr);
	EXPECT_EQ(errno, ENOENT);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	EXPECT_EQ(Libs::LibcInternal::fflush(nullptr), 0);
	EXPECT_EQ(watch->Begins(), before);
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseCoversDetachedCloseWithoutTouchingReusedSlot)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	Seed("replacement", "safe");
	FILE* stream = Stream("detached-buffer", "wb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	ASSERT_EQ(c_fputc('A', stream), 'A');
	const int descriptor = c_fileno(stream);
	ASSERT_EQ(KernelClose(descriptor), 0);
	const int replacement = Open("replacement", 0);
	ASSERT_EQ(replacement, descriptor);
	const auto before = watch->Begins();
	EXPECT_EQ(c_fclose(stream), 0);
	streams.clear();
	watch->ExpectReleasedSince(before);
	FileStat status {};
	EXPECT_EQ(KernelFstat(replacement, &status), 0);
	EXPECT_EQ(status.st_size, 4);
	EXPECT_EQ(Contents("detached-buffer"), "A");
}

TEST_F(EmulatorFileDescriptors, GuestStandardStreamsAreIsolatedFromHostBufferStorage)
{
	FILE* guest_output = StandardStream(1);
	FILE* guest_error = StandardStream(2);
	ASSERT_NE(guest_output, nullptr);
	ASSERT_NE(guest_error, nullptr);
	EXPECT_NE(guest_output, stdout);
	EXPECT_NE(guest_error, stderr);
	EXPECT_EQ(c_fileno(guest_output), 1);
	EXPECT_EQ(c_fileno(guest_error), 2);
	char buffer[256] {};
	EXPECT_EQ(c_setvbuf(stdout, buffer, 0, sizeof(buffer)), -1);
	EXPECT_EQ(errno, EBADF);
}

TEST_F(EmulatorFileDescriptors, LibcFstatUsesMinusOneAndGuestErrno)
{
	Seed("fstat-adapter", "abc");
	const int descriptor = Open("fstat-adapter", 0);
	ASSERT_GE(descriptor, 3);
	FileStat status {};
	int* error = Libs::Posix::GetErrorAddr();
	ASSERT_NE(error, nullptr);
	const int saved_error = *error;
	*error = Libs::Posix::POSIX_EIO;
	EXPECT_EQ(c_fstat(descriptor, &status), 0);
	EXPECT_EQ(*error, Libs::Posix::POSIX_EIO);
	EXPECT_EQ(status.st_size, 3);
	EXPECT_EQ(c_fstat(INT_MAX, &status), -1);
	EXPECT_EQ(*error, Libs::Posix::POSIX_EBADF);
	EXPECT_EQ(c_fstat(descriptor, nullptr), -1);
	EXPECT_EQ(*error, Libs::Posix::POSIX_EFAULT);
	*error = saved_error;
}

TEST_F(EmulatorFileDescriptors, VerifiedStreamExportsUseLibcVersionOneAndPrivateStandardObjects)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));
	Loader::SymbolResolve query {};
	query.library = U"libc";
	query.library_version = 1;
	query.module = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Loader::SymbolType::Func;
	query.name = U"qdlHjTa9hQ4";
	const auto* fdopen_export = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(fdopen_export, nullptr);
	EXPECT_EQ(fdopen_export->vaddr, reinterpret_cast<uint64_t>(&c_fdopen));
	query.name = U"gkWgn0p1AfU";
	const auto* freopen_export = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(freopen_export, nullptr);
	EXPECT_EQ(freopen_export->vaddr, reinterpret_cast<uint64_t>(&c_freopen));
	query.type = Loader::SymbolType::Object;
	query.name = U"2sWzhYqFH4E";
	const auto* output_export = symbols.FindByCanonicalName(Loader::SymbolDatabase::GenerateName(query));
	ASSERT_NE(output_export, nullptr);
	EXPECT_EQ(output_export->vaddr, reinterpret_cast<uint64_t>(StandardStream(1)));
	EXPECT_NE(output_export->vaddr, reinterpret_cast<uint64_t>(stdout));
}

TEST_F(EmulatorFileDescriptors, Dup2RejectsUnrepresentableTargetWithoutChangingSource)
{
	Seed("capacity", "abc");
	const int descriptor = Open("capacity", 0);
	ASSERT_GE(descriptor, 3);
	EXPECT_EQ(KernelDup2(descriptor, INT_MAX), KERNEL_ERROR_EBADF);
	char byte = 0;
	EXPECT_EQ(KernelRead(descriptor, &byte, 1), 1);
	EXPECT_EQ(byte, 'a');
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
TEST_F(EmulatorFileDescriptors, CallerBufferLeaseReleasesAfterNativeFlushAndCloseErrors)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	FILE* stream = Stream("buffer-error", "wb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	ASSERT_EQ(c_fputc('A', stream), 'A');
	// Fault injection affects only this FILE's private native descriptor. No
	// intervening open can recycle it before fclose attempts its own cleanup.
	ASSERT_EQ(::close(::fileno(stream)), 0);
	auto before = watch->Begins();
	EXPECT_EQ(Libs::LibcInternal::fflush(stream), EOF);
	EXPECT_EQ(errno, EBADF);
	watch->ExpectReleasedSince(before);
	before = watch->Begins();
	EXPECT_EQ(c_fclose(stream), EOF);
	EXPECT_EQ(errno, EBADF);
	streams.clear();
	watch->ExpectReleasedSince(before);
}

TEST_F(EmulatorFileDescriptors, CallerBufferLeaseSpansNativeFileLockWait)
{
	watch = std::make_unique<StdioBufferWatch>();
	ASSERT_TRUE(watch->Ready());
	FILE* stream = Stream("buffer-blocked", "wb");
	ASSERT_NE(stream, nullptr);
	ASSERT_EQ(c_setvbuf(stream, watch->Data(), 0, watch->Size()), 0);
	const auto before = watch->Begins();
	::flockfile(stream);
	std::atomic_bool completed {false};
	int result = EOF;
	std::thread writer([&] { result = c_fputc('A', stream); completed.store(true); });
	const bool active_while_blocked = watch->WaitForActive();
	EXPECT_TRUE(active_while_blocked);
	EXPECT_FALSE(completed.load());
	::funlockfile(stream);
	writer.join();
	EXPECT_EQ(result, 'A');
	watch->ExpectReleasedSince(before);
	EXPECT_EQ(Libs::LibcInternal::fflush(stream), 0);
	EXPECT_EQ(Contents("buffer-blocked"), "A");
}

TEST_F(EmulatorFileDescriptors, Dup2RespectsReportedHostDescriptorCapacity)
{
	struct rlimit limit {};
	ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
	if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > INT_MAX) { GTEST_SKIP() << "host has no finite int descriptor bound"; }
	Seed("host-capacity", "abc");
	const int descriptor = Open("host-capacity", 0);
	ASSERT_GE(descriptor, 3);
	EXPECT_EQ(KernelDup2(descriptor, static_cast<int>(limit.rlim_cur)), KERNEL_ERROR_EBADF);
	EXPECT_EQ(KernelLseek(descriptor, 0, SEEK_CUR), 0);
}
#endif

UT_END();
