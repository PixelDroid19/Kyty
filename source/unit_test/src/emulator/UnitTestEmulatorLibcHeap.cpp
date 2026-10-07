#include "Kyty/UnitTest.h"
#include "Kyty/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/GuestRuntimePort.h"
#include "Emulator/Libs/ApplicationHeap.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/ProcessEnvironment.h"
#include "Emulator/Loader/GuestCall.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

UT_BEGIN(EmulatorLibcHeap);

namespace {
namespace Heap = Kyty::Libs::LibKernel::ApplicationHeap;
namespace Vm = Kyty::Core::VirtualMemory;
namespace Port = Kyty::Emulator::GuestRuntimePort;

uint64_t g_parameters = 0;
uint64_t g_code = 0;
int g_initialize_count = 0;
int g_free_count = 0;
bool g_heap_published_during_initialize = false;
int g_initialize_result = 0;
bool g_reenter = false;
bool g_reentrant_result = false;
std::mutex g_probe_mutex;
std::condition_variable g_probe_changed;
bool g_hold_initializer = false;
bool g_initializer_entered = false;
char g_allocation[64] {};

int KYTY_SYSV_ABI InitializeHeap()
{
	++g_initialize_count;
	g_heap_published_during_initialize = Heap::HasAllocator();
	if (g_reenter) { g_reentrant_result = Heap::InitializeProcessHeap(g_parameters); }
	{
		std::unique_lock lock(g_probe_mutex);
		g_initializer_entered = true;
		g_probe_changed.notify_all();
		g_probe_changed.wait(lock, [] { return !g_hold_initializer; });
	}
	return g_initialize_result;
}

void* KYTY_SYSV_ABI Allocate(size_t size)
{
	return g_initialize_count == 1 && size <= sizeof(g_allocation) ? g_allocation : nullptr;
}

void KYTY_SYSV_ABI Free(void* ptr)
{
	if (ptr == g_allocation) { ++g_free_count; }
}

uint64_t GetParameters() { return g_parameters; }
bool IsExecutable(uint64_t address) { return address >= g_code && address < g_code + 48; }

class HeapFixture
{
public:
	HeapFixture()
	{
		Heap::Reset();
		g_initialize_count = 0;
		g_free_count = 0;
		g_initialize_result = 0;
		g_reenter = false;
		g_reentrant_result = false;
		g_hold_initializer = false;
		g_initializer_entered = false;
		g_heap_published_during_initialize = false;
		g_parameters = Vm::Alloc(0, 4096, Vm::Mode::ReadWrite);
		g_code = Vm::Alloc(0, 4096, Vm::Mode::ExecuteReadWrite);
		if (g_parameters == 0 || g_code == 0) { return; }
		WriteThunk(0, reinterpret_cast<uint64_t>(InitializeHeap));
		WriteThunk(16, reinterpret_cast<uint64_t>(Allocate));
		WriteThunk(32, reinterpret_cast<uint64_t>(Free));
		Vm::FlushInstructionCache(g_code, 48);
		Vm::Protect(g_code, 4096, Vm::Mode::ExecuteRead);
		auto* words = reinterpret_cast<uint64_t*>(g_parameters);
		std::memset(words, 0, 4096);
		words[0] = 0x60;
		words[1] = (uint64_t{5} << 32) | 0x4942524f;
		words[7] = g_parameters + 0x100;
		words[0x100 / 8] = 0xa8;
		words[0x108 / 8] = (uint64_t{1} << 32) | 14;
		words[0x130 / 8] = g_parameters + 0x200;
		words[0x200 / 8] = 0x78;
		words[0x208 / 8] = 2;
		words[0x210 / 8] = g_code;
		words[0x220 / 8] = g_code + 16;
		words[0x228 / 8] = g_code + 32;
		Port::Install({nullptr, Kyty::Loader::GuestCall::Invoke, nullptr, nullptr, nullptr, GetParameters, IsExecutable});
	}
	~HeapFixture()
	{
		Heap::Reset();
		Port::Install({});
		if (g_parameters != 0) { Vm::Free(g_parameters); }
		if (g_code != 0) { Vm::Free(g_code); }
		g_parameters = 0;
		g_code = 0;
	}
	bool Valid() const { return g_parameters != 0 && g_code != 0; }

private:
	static void WriteThunk(size_t offset, uint64_t target)
	{
		uint8_t code[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0}; // movabs target,%rax; jmp *%rax
		std::memcpy(code + 2, &target, sizeof(target));
		std::memcpy(reinterpret_cast<void*>(g_code + offset), code, sizeof(code));
	}
};

void CallInitEnv()
{
	if (!Kyty::Config::IsInitialized())
	{
		Kyty::Config::ConfigSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
	}
	Kyty::Log::LogSubsystem::Instance()->Init(Kyty::Core::SubsystemsList::Instance());
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));
	Kyty::Loader::SymbolResolve resolve {};
	resolve.name = U"bzQExy189ZI";
	resolve.library = U"libc";
	resolve.library_version = 1;
	resolve.module = U"libc";
	resolve.module_version_major = 1;
	resolve.module_version_minor = 1;
	resolve.type = Kyty::Loader::SymbolType::Func;
	const auto* record = symbols.Find(resolve);
	ASSERT_NE(record, nullptr);
	using InitEnv = void KYTY_SYSV_ABI (*)(const Kyty::Libs::ProcessEnvironment::InitParameters*);
	Kyty::Libs::ProcessEnvironment::InitParameters args {};
	args.argc = 1;
	args.argv[0] = "heap-contract";
	reinterpret_cast<InitEnv>(record->vaddr)(&args);
}
} // namespace

TEST(EmulatorLibcHeap, InitEnvInitializesDeclaredHeapBeforeFirstAllocation)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	CallInitEnv();
	EXPECT_EQ(g_initialize_count, 1);
	EXPECT_FALSE(g_heap_published_during_initialize);
	EXPECT_TRUE(Heap::HasAllocator());
	EXPECT_EQ(Heap::Malloc(40), g_allocation);
	EXPECT_TRUE(Heap::Free(g_allocation));
	EXPECT_EQ(g_free_count, 1);
	CallInitEnv();
	EXPECT_EQ(g_initialize_count, 1);
}

TEST(EmulatorLibcHeap, MissingReplacementLeavesTheDefaultAllocatorUnchanged)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	EXPECT_TRUE(Heap::InitializeProcessHeap(0));
	*reinterpret_cast<uint64_t*>(g_parameters + 0x130) = 0;
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_EQ(g_initialize_count, 0);
	EXPECT_FALSE(Heap::HasAllocator());
}

TEST(EmulatorLibcHeap, AcceptsTheVersionOnePrefixWithoutAnAlignedAllocSlot)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	*reinterpret_cast<uint64_t*>(g_parameters + 0x200) = 0x70;
	*reinterpret_cast<uint64_t*>(g_parameters + 0x208) = 1;
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_EQ(g_initialize_count, 1);
	EXPECT_EQ(Heap::Malloc(40), g_allocation);
}

TEST(EmulatorLibcHeap, EmptyDeclaredTableKeepsTheDefaultAllocator)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	std::memset(reinterpret_cast<void*>(g_parameters + 0x210), 0, 0x68);
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_EQ(g_initialize_count, 0);
	EXPECT_FALSE(Heap::HasAllocator());
}

TEST(EmulatorLibcHeap, InitializerResultIsNotAFailureSignal)
{
	// A shipped title declares initialize as a bare `ret`: rax still holds the
	// callback address. libc publishes the allocator regardless.
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	g_initialize_result = 0x2e1b30;
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters)) << Heap::ProcessHeapFailureReason();
	EXPECT_EQ(g_initialize_count, 1);
	EXPECT_TRUE(Heap::HasAllocator());
	EXPECT_EQ(Heap::Malloc(40), g_allocation);
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_EQ(g_initialize_count, 1);
}

TEST(EmulatorLibcHeap, RejectsMalformedRecordsBeforeCallingGuestCode)
{
	struct InvalidField { size_t offset; uint64_t value; };
	const InvalidField cases[] = {
	    {0, 0x38}, {8, 0}, {0x38, 1}, {0x100, 0x30}, {0x130, UINT64_MAX - 7},
	    {0x200, 0x68}, {0x200, UINT64_MAX}, {0x208, 99}, {0x210, 1},
	    {0x220, 0}, {0x228, 0}, {0x240, 1}, {0x270, 1},
	};
	for (const auto& field: cases)
	{
		SCOPED_TRACE(field.offset);
		HeapFixture fixture;
		ASSERT_TRUE(fixture.Valid());
		*reinterpret_cast<uint64_t*>(g_parameters + field.offset) = field.value;
		EXPECT_FALSE(Heap::InitializeProcessHeap(g_parameters));
		// Every refusal names the record or callback that failed.
		EXPECT_NE(Heap::ProcessHeapFailureReason()[0], '\0');
		EXPECT_EQ(g_initialize_count, 0);
		EXPECT_FALSE(Heap::HasAllocator());
	}
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	*reinterpret_cast<uint64_t*>(g_parameters + 0x208) = 99;
	EXPECT_FALSE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_NE(std::strstr(Heap::ProcessHeapFailureReason(), "unsupported layout (size 0x78 version 99)"), nullptr)
	    << Heap::ProcessHeapFailureReason();
	EXPECT_TRUE(Heap::InitializeProcessHeap(0));
	EXPECT_EQ(Heap::ProcessHeapFailureReason()[0], '\0');
}

TEST(EmulatorLibcHeap, ReadableDataIsNotAnExecutableInitializer)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	*reinterpret_cast<uint64_t*>(g_parameters + 0x210) = g_parameters + 0x300;
	EXPECT_FALSE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_EQ(g_initialize_count, 0);
}

TEST(EmulatorLibcHeap, RecursiveStartupDoesNotInitializeOrPublishTwice)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	g_reenter = true;
	EXPECT_TRUE(Heap::InitializeProcessHeap(g_parameters));
	EXPECT_TRUE(g_reentrant_result);
	EXPECT_FALSE(g_heap_published_during_initialize);
	EXPECT_EQ(g_initialize_count, 1);
}

TEST(EmulatorLibcHeap, ConcurrentStartupWaitsForTheInitializingThread)
{
	HeapFixture fixture;
	ASSERT_TRUE(fixture.Valid());
	g_hold_initializer = true;
	bool first_result = false;
	bool second_result = false;
	std::atomic_bool second_done {false};
	std::thread first([&] { first_result = Heap::InitializeProcessHeap(g_parameters); });
	bool entered = false;
	{
		std::unique_lock lock(g_probe_mutex);
		entered = g_probe_changed.wait_for(lock, std::chrono::seconds(2), [] { return g_initializer_entered; });
	}
	std::thread second([&] {
		second_result = Heap::InitializeProcessHeap(g_parameters);
		second_done.store(true);
		g_probe_changed.notify_all();
	});
	{
		std::unique_lock lock(g_probe_mutex);
		const bool returned_early = g_probe_changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return second_done.load(); });
		EXPECT_FALSE(returned_early);
		g_hold_initializer = false;
	}
	g_probe_changed.notify_all();
	first.join();
	second.join();
	EXPECT_TRUE(entered);
	EXPECT_TRUE(first_result);
	EXPECT_TRUE(second_result);
	EXPECT_EQ(g_initialize_count, 1);
}

UT_END();
