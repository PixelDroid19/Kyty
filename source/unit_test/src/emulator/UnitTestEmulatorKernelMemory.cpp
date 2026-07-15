#include "Emulator/Config.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/UnitTest.h"

#include <cstring>

UT_BEGIN(EmulatorKernelMemory);

using namespace Libs;
using namespace Libs::LibKernel::Memory;

static void EnsureMemorySubsystemInitialized()
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	static bool memory_inited = false;
	if (!memory_inited)
	{
		MemorySubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		memory_inited = true;
	}
}

TEST(EmulatorKernelMemory, CheckedReleaseReportsGuestErrors)
{
	EnsureMemorySubsystemInitialized();

	int64_t address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(0x10000, 0x40000, 0x10000, 0x10000, 12, &address), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0), LibKernel::KERNEL_ERROR_EINVAL);
}

TEST(EmulatorKernelMemory, ReleaseDirectMemoryKeepsVirtualMappingUntilMunmap)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize = 0x10000;
	int64_t          physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(0x40000, 0x80000, kSize, kSize, 12, &physical_address), OK);

	void* mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	ASSERT_NE(mapping, nullptr);

	void* mapping_start = nullptr;
	void* mapping_end   = nullptr;
	int   protection    = 0;
	ASSERT_EQ(KernelQueryMemoryProtection(mapping, &mapping_start, &mapping_end, &protection), OK);
	EXPECT_EQ(mapping_start, mapping);
	EXPECT_EQ(mapping_end, static_cast<uint8_t*>(mapping) + kSize - 1);
	EXPECT_EQ(protection, 0x02);

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);

	struct DirectMemoryInfo
	{
		int64_t start;
		int64_t end;
		int     memory_type;
	};
	DirectMemoryInfo info {};
	EXPECT_EQ(KernelDirectMemoryQuery(physical_address, 0, &info, sizeof(info)), LibKernel::KERNEL_ERROR_EACCES);

	mapping_start = nullptr;
	mapping_end   = nullptr;
	protection    = 0;
	const int query_after_release = KernelQueryMemoryProtection(mapping, &mapping_start, &mapping_end, &protection);
	EXPECT_EQ(query_after_release, OK);
	if (query_after_release == OK)
	{
		EXPECT_EQ(mapping_start, mapping);
		EXPECT_EQ(mapping_end, static_cast<uint8_t*>(mapping) + kSize - 1);
		EXPECT_EQ(protection, 0x02);
		EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(mapping), kSize), OK);
		EXPECT_EQ(KernelQueryMemoryProtection(mapping, nullptr, nullptr, nullptr), LibKernel::KERNEL_ERROR_EACCES);
	}
}

TEST(EmulatorKernelMemory, ReusedDirectMemoryKeepsVirtualAliasesCoherent)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x100000;
	constexpr int64_t kSearchEnd   = kSearchStart + kSize;
	int64_t           first_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchEnd, kSize, kSize, 12, &first_physical_address), OK);
	ASSERT_EQ(first_physical_address, kSearchStart);

	void* first_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&first_mapping, kSize, 0x02, 0, first_physical_address, kSize), OK);
	ASSERT_NE(first_mapping, nullptr);
	auto* first_bytes = static_cast<uint8_t*>(first_mapping);
	first_bytes[0]    = 0x5a;
	first_bytes[1]    = 0xc3;

	ASSERT_EQ(KernelCheckedReleaseDirectMemory(first_physical_address, kSize), OK);

	int64_t second_physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchEnd, kSize, kSize, 12, &second_physical_address), OK);
	ASSERT_EQ(second_physical_address, first_physical_address);

	void* second_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&second_mapping, kSize, 0x02, 0, second_physical_address, kSize), OK);
	ASSERT_NE(second_mapping, nullptr);
	ASSERT_NE(second_mapping, first_mapping);
	auto* second_bytes = static_cast<uint8_t*>(second_mapping);

	EXPECT_EQ(second_bytes[0], 0x5a);
	EXPECT_EQ(second_bytes[1], 0xc3);
	second_bytes[2] = 0x7e;
	EXPECT_EQ(first_bytes[2], 0x7e);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(second_mapping), kSize), OK);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(first_mapping), kSize), OK);

#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
	// macOS arm64 rejects W+X MAP_SHARED views with EPERM and MAP_JIT cannot
	// be combined with a shared backing. The RW alias contract above remains
	// testable; the executable remap is an explicit host policy limitation.
	ASSERT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
	GTEST_SKIP() << "macOS arm64 does not permit shared ExecuteReadWrite mappings";
#endif

	void* remapped_at_first_address = first_mapping;
	ASSERT_EQ(KernelMapDirectMemory(&remapped_at_first_address, kSize, 0x07, 0x10, second_physical_address, kSize), OK);
	ASSERT_EQ(remapped_at_first_address, first_mapping);
	EXPECT_EQ(static_cast<uint8_t*>(remapped_at_first_address)[2], 0x7e);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(remapped_at_first_address), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
}

// Covers the explicit Gen5 protection family observed in one allocation path.
// The pure decoder is the shipped decision path used by KernelMprotect.
TEST(EmulatorKernelMemory, DecodesGen5MprotectProtectionFamily)
{
	Core::VirtualMemory::Mode     mode {};
	Graphics::GpuMemoryMode       gpu {};

	ASSERT_TRUE(KernelDecodeMprotectProt(0x11, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::Read);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x12, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0xC2, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::ReadWrite);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x42, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::Read);

	ASSERT_TRUE(KernelDecodeMprotectProt(0x82, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::ReadWrite);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::Write);

	EXPECT_FALSE(KernelDecodeMprotectProt(0x99, &mode, &gpu));
}

// Share_v1 NIDs from second-title first strict fail must resolve after InitShare_1.
TEST(EmulatorKernelMemory, ResolvesShareV1ExportsForGen5Boot)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libShare_1", &symbols));

	const char* nids[] = {"nBDD66kiFW8", "5wjxESwX68I", "T64o-315wbg", "YBiIdcDPrxs", "7QZtURYnXG4"};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"Share";
		query.library_version      = 1;
		query.module               = U"Share";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		ASSERT_NE(symbols.Find(query), nullptr) << nid;
	}
}

// Ampr measure APIs return fixed command-record sizes (0x30 / 0x30 / 0x20).
TEST(EmulatorKernelMemory, AmprMeasureCommandSizesMatchRecordLayout)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	struct Case
	{
		const char* nid;
		uint64_t    size;
	};
	const Case cases[] = {
	    {"vWU-odnS+fU", 0x30u},
	    {"sSAUCCU1dv4", 0x30u},
	    {"C+IEj+BsAFM", 0x20u},
	};

	for (const auto& c: cases)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(c.nid);
		query.library              = U"Ampr";
		query.library_version      = 1;
		query.module               = U"Ampr";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		const auto* rec            = symbols.Find(query);
		ASSERT_NE(rec, nullptr) << c.nid;
		using measure_fn_t = uint64_t (*)();
		auto* fn           = reinterpret_cast<measure_fn_t>(static_cast<uintptr_t>(rec->vaddr));
		ASSERT_NE(fn, nullptr);
		EXPECT_EQ(fn(), c.size) << c.nid;
	}
}

// Constructor writes a 0x28-byte header: self, data, size, aux0, aux1.
TEST(EmulatorKernelMemory, AmprCommandBufferConstructorWritesHeader)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"8aI7R7WaOlc";
	query.library              = U"Ampr";
	query.library_version      = 1;
	query.module               = U"Ampr";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec            = symbols.Find(query);
	ASSERT_NE(rec, nullptr);

	using ctor_fn_t = uint64_t (*)(void*, void*, uint64_t);
	auto* ctor      = reinterpret_cast<ctor_fn_t>(static_cast<uintptr_t>(rec->vaddr));

	alignas(8) uint8_t cmd_mem[0x28] {};
	alignas(8) uint8_t data_mem[64] {};
	const uint64_t ret = ctor(cmd_mem, data_mem, 64);
	EXPECT_EQ(ret, reinterpret_cast<uint64_t>(cmd_mem));
	uint64_t self = 0;
	uint64_t data = 0;
	uint64_t size = 0;
	std::memcpy(&self, cmd_mem + 0x00, 8);
	std::memcpy(&data, cmd_mem + 0x08, 8);
	std::memcpy(&size, cmd_mem + 0x10, 8);
	EXPECT_EQ(self, reinterpret_cast<uint64_t>(cmd_mem));
	EXPECT_EQ(data, reinterpret_cast<uint64_t>(data_mem));
	EXPECT_EQ(size, 64u);
}

// libc vsnprintf NID Q2V+iqvjgC0 resolves on libc_1 / libc_internal_1.
TEST(EmulatorKernelMemory, ResolvesLibcVsnprintfExport)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"Q2V+iqvjgC0";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	ASSERT_NE(symbols.Find(query), nullptr);
}

// libkernel_1: sceKernelNanosleep must resolve to the existing validated
// KernelNanosleep implementation rather than the generic missing-symbol path.
TEST(EmulatorKernelMemory, ResolvesKernelNanosleepExport)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"QvsZxomvUHs";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec = symbols.Find(query);
	ASSERT_NE(rec, nullptr);

	using nanosleep_fn_t = int (*)(const LibKernel::KernelTimespec*, LibKernel::KernelTimespec*);
	auto* nanosleep      = reinterpret_cast<nanosleep_fn_t>(static_cast<uintptr_t>(rec->vaddr));
	ASSERT_NE(nanosleep, nullptr);
	EXPECT_EQ(nanosleep(nullptr, nullptr), LibKernel::KERNEL_ERROR_EFAULT);
	LibKernel::KernelTimespec invalid {-1, 0};
	EXPECT_EQ(nanosleep(&invalid, nullptr), LibKernel::KERNEL_ERROR_EINVAL);
}

// Gen5 AudioOut2_v1 / AudioOut_v1.1: core context lifecycle exports resolve and
// ContextQueryMemory writes a non-zero host workspace size.
TEST(EmulatorKernelMemory, ResolvesAudioOut2ContextLifecycle)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAudio_1", &symbols));

	const char* nids[] = {
	    "g2tViFIohHE", // Initialize
	    "t5YrizufpQc", // ContextResetParam
	    "pDmme7Bgm6E", // ContextQueryMemory
	    "0x6o1VVAYSY", // ContextCreate
	    "on6ZH7Abo10", // ContextDestroy
	    "JK2wamZPzwM", // PortCreate
	    "xywYcRB7nbQ", // UserCreate
	};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"AudioOut2";
		query.library_version      = 1;
		query.module               = U"AudioOut";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		EXPECT_NE(symbols.Find(query), nullptr) << nid;
	}

	Loader::SymbolResolve init_q {};
	init_q.name                 = U"g2tViFIohHE";
	init_q.library              = U"AudioOut2";
	init_q.library_version      = 1;
	init_q.module               = U"AudioOut";
	init_q.module_version_major = 1;
	init_q.module_version_minor = 1;
	init_q.type                 = Loader::SymbolType::Func;
	const auto* init_rec        = symbols.Find(init_q);
	ASSERT_NE(init_rec, nullptr);
	using init_fn_t = int (*)();
	EXPECT_EQ(reinterpret_cast<init_fn_t>(static_cast<uintptr_t>(init_rec->vaddr))(), 0);

	Loader::SymbolResolve qmem_q = init_q;
	qmem_q.name                  = U"pDmme7Bgm6E";
	const auto* qmem_rec         = symbols.Find(qmem_q);
	ASSERT_NE(qmem_rec, nullptr);
	using qmem_fn_t = int (*)(const void*, uint64_t*);
	uint64_t size   = 0;
	EXPECT_EQ(reinterpret_cast<qmem_fn_t>(static_cast<uintptr_t>(qmem_rec->vaddr))(nullptr, &size), 0);
	EXPECT_GT(size, 0u);

	Loader::SymbolResolve create_q = init_q;
	create_q.name                  = U"0x6o1VVAYSY";
	const auto* create_rec         = symbols.Find(create_q);
	ASSERT_NE(create_rec, nullptr);
	using create_fn_t = int (*)(const void*, void*, uint64_t, int32_t*);
	alignas(8) uint8_t buf[64] {};
	int32_t            handle = 0;
	EXPECT_EQ(reinterpret_cast<create_fn_t>(static_cast<uintptr_t>(create_rec->vaddr))(nullptr, buf, sizeof(buf), &handle), 0);
	EXPECT_GT(handle, 0);
}

// Residual Ampr NIDs from second-title boot resolve under libAmpr_1.
TEST(EmulatorKernelMemory, ResolvesAmprResidualBootNids)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libAmpr_1", &symbols));

	const char* nids[] = {"Zi3dBUjgyXI", "4muPEJ-x5N8", "qesF88X4DRg", "8aI7R7WaOlc", "GuchCTefuZw"};
	for (const char* nid: nids)
	{
		Loader::SymbolResolve query {};
		query.name                 = String::FromUtf8(nid);
		query.library              = U"Ampr";
		query.library_version      = 1;
		query.module               = U"Ampr";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type                 = Loader::SymbolType::Func;
		EXPECT_NE(symbols.Find(query), nullptr) << nid;
	}
}

// Live EventFlag registry: Wait/Set/Delete on garbage handles must return
// ESRCH without dereferencing (Linux VibrationTrackThread poison pointer).
// Create/Wait/Set/Delete on a real flag exercises the shipped registry path.
TEST(EmulatorKernelMemory, EventFlagRejectsUnregisteredHandles)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	static bool threads_inited = false;
	if (!threads_inited)
	{
		Core::ThreadsSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
		threads_inited = true;
	}

	using namespace LibKernel::EventFlag;

	// Poison pointer observed on Linux vibration wait before CreateEventFlag.
	auto* poison = reinterpret_cast<KernelEventFlag>(static_cast<uintptr_t>(0xcccccccc00007fffULL));
	EXPECT_EQ(KernelWaitEventFlag(poison, 1, 0x21, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelSetEventFlag(poison, 1), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelDeleteEventFlag(poison), LibKernel::KERNEL_ERROR_ESRCH);
	EXPECT_EQ(KernelWaitEventFlag(nullptr, 1, 0x21, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);

	KernelEventFlag ef = nullptr;
	ASSERT_EQ(KernelCreateEventFlag(&ef, "UnitTestThreadFlag", 0x10, 0, nullptr), OK);
	ASSERT_NE(ef, nullptr);

	// Timeout=0 poll-style wait on empty bits returns TimedOut path.
	LibKernel::KernelUseconds zero = 0;
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x01, nullptr, &zero), LibKernel::KERNEL_ERROR_ETIMEDOUT);
	EXPECT_EQ(KernelSetEventFlag(ef, 1), OK);
	zero = 0;
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x21, nullptr, &zero), OK);
	EXPECT_EQ(KernelDeleteEventFlag(ef), OK);
	// After delete, same pointer is no longer live.
	EXPECT_EQ(KernelWaitEventFlag(ef, 1, 0x01, nullptr, nullptr), LibKernel::KERNEL_ERROR_ESRCH);
}

UT_END();
