#include "Emulator/Config.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

#include <cstring>

UT_BEGIN(EmulatorKernelMemory);

using namespace Libs;
using namespace Libs::LibKernel::Memory;

TEST(EmulatorKernelMemory, CheckedReleaseReportsGuestErrors)
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

	int64_t address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(0x10000, 0x40000, 0x10000, 0x10000, 12, &address), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelReleaseDirectMemory(address, 0x10000), LibKernel::KERNEL_ERROR_ENOENT);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(address, 0), LibKernel::KERNEL_ERROR_EINVAL);
}

// Red→green for Gen5 boot mprotect prot=0xC2 (was EXIT "unknown prot: 194").
// Pure decoder is the shipped decision path used by KernelMprotect.
TEST(EmulatorKernelMemory, DecodesGen5MprotectProtC2AsReadWriteGpu)
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

UT_END();
