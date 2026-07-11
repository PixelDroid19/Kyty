#include "Emulator/Config.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/UnitTest.h"

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

UT_END();
