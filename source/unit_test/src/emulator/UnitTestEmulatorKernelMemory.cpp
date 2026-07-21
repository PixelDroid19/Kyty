#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Kernel/EventFlag.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/UnitTest.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

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

TEST(EmulatorKernelMemory, GpuUnmapGateKeepsAdmissionsClosedThroughHostUnmap)
{
	Graphics::GpuSubmissionAdmissionGate gate;

	std::mutex              state_mutex;
	std::condition_variable state_condition;
	bool                    admission_started = false;
	std::atomic_bool        admission_entered  = false;
	bool                    drained            = false;
	bool                    detached           = false;
	bool                    host_mapping_live  = true;

	std::thread submitter;
	gate.RunQuiesced(
	    [&]
	    {
		    drained = true;
		    submitter = std::thread(
		        [&]
		        {
			        {
				        std::lock_guard<std::mutex> lock(state_mutex);
				        admission_started = true;
			        }
			        state_condition.notify_one();
			        gate.RunAdmitted([&] { admission_entered = true; });
		        });

		    std::unique_lock<std::mutex> lock(state_mutex);
		    state_condition.wait(lock, [&] { return admission_started; });
	    },
	    [&]
	    {
		    EXPECT_TRUE(drained);
		    EXPECT_FALSE(admission_entered.load());
		    EXPECT_TRUE(host_mapping_live);

		    detached = true;
		    EXPECT_FALSE(admission_entered.load());
		    EXPECT_TRUE(host_mapping_live);

		    host_mapping_live = false;
		    EXPECT_TRUE(detached);
		    EXPECT_FALSE(admission_entered.load());
	    });

	submitter.join();
	EXPECT_TRUE(admission_entered.load());
	EXPECT_FALSE(host_mapping_live);
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

TEST(EmulatorKernelMemory, DirectMemorySizeTracksGuestGeneration)
{
	EnsureMemorySubsystemInitialized();

	Config::SetNextGen(false);
	EXPECT_EQ(KernelGetDirectMemorySize(), static_cast<size_t>(5376) * 1024 * 1024);

	Config::SetNextGen(true);
	EXPECT_EQ(KernelGetDirectMemorySize(), static_cast<size_t>(16) * 1024 * 1024 * 1024);

	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, VirtualQueryReportsReservedRangeAsUncommitted)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t kSize = 0x10000;
	void*            address = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&address, kSize, 0, kSize), OK);
	ASSERT_NE(address, nullptr);

	VirtualQueryInfo info {};
	ASSERT_EQ(KernelVirtualQuery(static_cast<uint8_t*>(address) + 0x4000, 0, &info, sizeof(info)), OK);
	EXPECT_EQ(info.start, reinterpret_cast<uintptr_t>(address));
	EXPECT_EQ(info.end, reinterpret_cast<uintptr_t>(address) + kSize);
	EXPECT_EQ(info.protection, 0);
	EXPECT_EQ(info.is_direct, 0u);
	EXPECT_EQ(info.is_flexible, 0u);
	EXPECT_EQ(info.is_committed, 0u);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(address), kSize), OK);
}

TEST(EmulatorKernelMemory, FixedDirectMapCommitsOwnedReservation)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kSize = 0x10000;
	void*            address = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&address, kSize, 0, kSize), OK);

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kSize, kSize, 12, &physical_address), OK);
	ASSERT_EQ(KernelMapDirectMemory(&address, kSize, 0x02, 0x10, physical_address, kSize), OK);

	VirtualQueryInfo info {};
	ASSERT_EQ(KernelVirtualQuery(address, 0, &info, sizeof(info)), OK);
	EXPECT_EQ(info.start, reinterpret_cast<uintptr_t>(address));
	EXPECT_EQ(info.end, reinterpret_cast<uintptr_t>(address) + kSize);
	EXPECT_EQ(info.is_direct, 1u);
	EXPECT_EQ(info.is_committed, 1u);

	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(address), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, FixedDirectMapConsumesPrefixOfLargerReservation)
{
	EnsureMemorySubsystemInitialized();
	Config::SetNextGen(true);

	constexpr size_t kReservationSize = 0x40000;
	constexpr size_t kMappingSize     = 0x4000;
	void*            reservation      = nullptr;
	ASSERT_EQ(KernelReserveVirtualRange(&reservation, kReservationSize, 0, kReservationSize), OK);
	const auto reservation_base = reinterpret_cast<uint64_t>(reservation);

	int64_t physical_address = 0;
	ASSERT_EQ(KernelAllocateMainDirectMemory(kMappingSize, 0, 12, &physical_address), OK);

	void*     mapping    = reservation;
	const int map_result = KernelMapDirectMemory(&mapping, kMappingSize, 0x02, 0x10, physical_address, 0x1000);
	EXPECT_EQ(map_result, OK);
	if (map_result == OK)
	{
		ASSERT_EQ(mapping, reservation);
		static_cast<uint8_t*>(mapping)[0] = 0x5a;

		VirtualQueryInfo mapped_info {};
		ASSERT_EQ(KernelVirtualQuery(mapping, 0, &mapped_info, sizeof(mapped_info)), OK);
		EXPECT_EQ(mapped_info.is_direct, 1u);
		EXPECT_EQ(mapped_info.is_committed, 1u);

		VirtualQueryInfo suffix_info {};
		ASSERT_EQ(KernelVirtualQuery(reinterpret_cast<void*>(reservation_base + kMappingSize), 0, &suffix_info,
		                             sizeof(suffix_info)),
		          OK);
		EXPECT_EQ(suffix_info.start, reservation_base + kMappingSize);
		EXPECT_EQ(suffix_info.end, reservation_base + kReservationSize);
		EXPECT_EQ(suffix_info.is_committed, 0u);

		EXPECT_EQ(KernelMunmap(reservation_base, kMappingSize), OK);
		EXPECT_EQ(KernelMunmap(reservation_base + kMappingSize, kReservationSize - kMappingSize), OK);
	} else
	{
		EXPECT_EQ(KernelMunmap(reservation_base, kReservationSize), OK);
	}
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kMappingSize), OK);
	Config::SetNextGen(false);
}

TEST(EmulatorKernelMemory, DirectMemoryAllocationFindsAFreeEarlierRange)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t  kSize      = 0x10000;
	constexpr int64_t kLowerBase = 0x08000000;
	constexpr int64_t kUpperBase = 0x10000000;
	int64_t           upper_addr = 0;
	int64_t           lower_addr = 0;

	ASSERT_EQ(KernelAllocateDirectMemory(kUpperBase, kUpperBase + kSize, kSize, kSize, 12, &upper_addr), OK);
	ASSERT_EQ(upper_addr, kUpperBase);
	ASSERT_EQ(KernelAllocateDirectMemory(kLowerBase, kLowerBase + kSize, kSize, kSize, 12, &lower_addr), OK);
	EXPECT_EQ(lower_addr, kLowerBase);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(lower_addr, kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(upper_addr, kSize), OK);
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

	void* remapped_at_first_address = first_mapping;
	const int remap_result = KernelMapDirectMemory(&remapped_at_first_address, kSize, 0x07, 0x10, second_physical_address, kSize);
	#if defined(__APPLE__)
	// macOS can reject an executable writable MAP_SHARED view even when the
	// requested address is free. Keep alias coherence covered above and make
	// the host policy explicit instead of replacing a mapping unsafely.
	if (remap_result == LibKernel::KERNEL_ERROR_EBUSY)
	{
		ASSERT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
		GTEST_SKIP() << "macOS rejected the shared ExecuteReadWrite remap";
	}
	#endif
	ASSERT_EQ(remap_result, OK);
	ASSERT_EQ(remapped_at_first_address, first_mapping);
	EXPECT_EQ(static_cast<uint8_t*>(remapped_at_first_address)[2], 0x7e);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(remapped_at_first_address), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(second_physical_address, kSize), OK);
}

TEST(EmulatorKernelMemory, FixedDirectMemoryRemapsFreedReadWriteView)
{
	EnsureMemorySubsystemInitialized();

	constexpr size_t  kSize        = 0x10000;
	constexpr int64_t kSearchStart = 0x01800000;
	int64_t           physical_address = 0;
	ASSERT_EQ(KernelAllocateDirectMemory(kSearchStart, kSearchStart + kSize, kSize, kSize, 12, &physical_address), OK);

	void* first_mapping = nullptr;
	ASSERT_EQ(KernelMapDirectMemory(&first_mapping, kSize, 0x02, 0, physical_address, kSize), OK);
	ASSERT_NE(first_mapping, nullptr);
	ASSERT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(first_mapping), kSize), OK);

	void* remapped = first_mapping;
	ASSERT_EQ(KernelMapDirectMemory(&remapped, kSize, 0x02, 0x10, physical_address, kSize), OK);
	EXPECT_EQ(remapped, first_mapping);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(remapped), kSize), OK);
	EXPECT_EQ(KernelCheckedReleaseDirectMemory(physical_address, kSize), OK);
}

TEST(EmulatorKernelMemory, InternalNamedFlexibleMemoryNidUsesOutPointerAbi)
{
	EnsureMemorySubsystemInitialized();

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libkernel_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"4h6F1LLbTiw";
	query.library              = U"libkernel";
	query.library_version      = 1;
	query.module               = U"libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* rec            = symbols.Find(query);
	ASSERT_NE(rec, nullptr);

	static std::array<uint8_t, 0x4000> out_storage {};
	auto**                             out_addr = reinterpret_cast<void**>(out_storage.data());
	*out_addr                                  = nullptr;

	using map_named_flexible_internal_fn_t = int (*)(void**, size_t, int, int, const char*);
	auto* map_named_flexible_internal = reinterpret_cast<map_named_flexible_internal_fn_t>(static_cast<uintptr_t>(rec->vaddr));
	ASSERT_NE(map_named_flexible_internal, nullptr);

	constexpr size_t kSize = 0x4000;
	ASSERT_EQ(map_named_flexible_internal(out_addr, kSize, 0x03, 0x8000, "internal-test"), OK);
	ASSERT_NE(*out_addr, nullptr);

	void* start      = nullptr;
	void* end        = nullptr;
	int   protection = 0;
	EXPECT_EQ(KernelQueryMemoryProtection(*out_addr, &start, &end, &protection), OK);
	EXPECT_EQ(start, *out_addr);
	EXPECT_EQ(end, static_cast<uint8_t*>(*out_addr) + kSize - 1);
	EXPECT_EQ(protection, 0x03);
	EXPECT_EQ(KernelMunmap(reinterpret_cast<uint64_t>(*out_addr), kSize), OK);
}

// Covers the explicit Gen5 protection family observed in one allocation path.
// The pure decoder is the shipped decision path used by KernelMprotect.
TEST(EmulatorKernelMemory, DecodesGen5MprotectProtectionFamily)
{
	Core::VirtualMemory::Mode     mode {};
	Graphics::GpuMemoryMode       gpu {};

	ASSERT_TRUE(KernelDecodeMprotectProt(0x0, &mode, &gpu));
	EXPECT_EQ(mode, Core::VirtualMemory::Mode::NoAccess);
	EXPECT_EQ(gpu, Graphics::GpuMemoryMode::NoAccess);

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

TEST(EmulatorKernelMemory, GpuVisibleMprotectMarksContainingMappingUntilUnmap)
{
	constexpr uint64_t       mapping_base = 0x100000u;
	constexpr uint64_t       mapping_size = 0x10000u;
	Graphics::GpuMemoryMode cleanup_mode = Graphics::GpuMemoryMode::NoAccess;

	for (const auto requested:
	     {Graphics::GpuMemoryMode::Read, Graphics::GpuMemoryMode::Write, Graphics::GpuMemoryMode::ReadWrite})
	{
		auto fresh_mode = Graphics::GpuMemoryMode::NoAccess;
		EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x1000u, 0x1000u, requested,
		                                      &fresh_mode),
		          KernelGpuMappingPromotionStatus::Promoted);
		EXPECT_EQ(fresh_mode, requested);
	}

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x2000u, 0x3000u,
	                                      Graphics::GpuMemoryMode::Read, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::Promoted);
	EXPECT_EQ(cleanup_mode, Graphics::GpuMemoryMode::Read);

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0x2000u, 0x3000u,
	                                      Graphics::GpuMemoryMode::NoAccess, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::Retained);
	EXPECT_EQ(cleanup_mode, Graphics::GpuMemoryMode::Read);

	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base + 0xf000u, 0x2000u,
	                                      Graphics::GpuMemoryMode::Write, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::NotContained);
	EXPECT_EQ(cleanup_mode, Graphics::GpuMemoryMode::Read);
	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base - 1u, 1u,
	                                      Graphics::GpuMemoryMode::Write, &cleanup_mode),
	          KernelGpuMappingPromotionStatus::NotContained);
	EXPECT_EQ(KernelPromoteGpuMappingRange(mapping_base, mapping_size, mapping_base, 0u, Graphics::GpuMemoryMode::Write,
	                                      &cleanup_mode),
	          KernelGpuMappingPromotionStatus::InvalidArgument);
	EXPECT_EQ(KernelPromoteGpuMappingRange(UINT64_MAX - 3u, 8u, mapping_base, 4u, Graphics::GpuMemoryMode::Write,
	                                      &cleanup_mode),
	          KernelGpuMappingPromotionStatus::InvalidArgument);

	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::Promoted),
	          KernelGpuMappingRegistrationAction::RegisterOwnerMapping);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::Retained),
	          KernelGpuMappingRegistrationAction::Retain);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::NotContained),
	          KernelGpuMappingRegistrationAction::RegisterProtectedRange);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::InvalidArgument),
	          KernelGpuMappingRegistrationAction::Reject);
	EXPECT_EQ(KernelGpuMappingRegistrationActionFor(KernelGpuMappingPromotionStatus::UnmapPending),
	          KernelGpuMappingRegistrationAction::Reject);
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

	const char* nids[] = {"Zi3dBUjgyXI", "4muPEJ-x5N8", "qesF88X4DRg", "8aI7R7WaOlc", "GuchCTefuZw",
	                      "0BMj1hgG+kE", "NNIZ-FMyz3M", "VGkEj4d6-Kg", "Eul7AGEpjLo", "X169CE6G3Y4",
	                      "RPCAhx-aabE", "tNn5WBkta60", "mZSbNJVJpV8"};
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

TEST(EmulatorKernelMemory, ResolvesLibcSincosfWithFloatResults)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"pztV4AF18iI";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* record         = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using sincosf_fn_t = void (*)(float, float*, float*);
	float sine          = 0.0f;
	float cosine        = 0.0f;
	reinterpret_cast<sincosf_fn_t>(static_cast<uintptr_t>(record->vaddr))(0.0f, &sine, &cosine);
	EXPECT_FLOAT_EQ(sine, 0.0f);
	EXPECT_FLOAT_EQ(cosine, 1.0f);
}

TEST(EmulatorKernelMemory, ResolvesLibcExpfWithFloatResult)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libc_1", &symbols));

	Loader::SymbolResolve query {};
	query.name                 = U"8zsu04XNsZ4";
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	const auto* record         = symbols.Find(query);
	ASSERT_NE(record, nullptr);

	using expf_fn_t = float (*)(float);
	EXPECT_FLOAT_EQ(reinterpret_cast<expf_fn_t>(static_cast<uintptr_t>(record->vaddr))(0.0f), 1.0f);
}

TEST(EmulatorKernelMemory, CondWaitDiagnosticsStayInactiveWithoutOptIn)
{
	LibKernel::PthreadCondWaitDiagnostics diagnostics {};
	EXPECT_FALSE(LibKernel::PthreadGetCondWaitDiagnostics(&diagnostics));
	EXPECT_FALSE(diagnostics.enabled);
	EXPECT_EQ(diagnostics.blocked_count, 0u);
	EXPECT_EQ(diagnostics.blocked[0].signal_count, 0u);
}

TEST(EmulatorKernelMemory, ThreadDiagnosticsAreUnavailableWithoutPthreadContext)
{
	LibKernel::PthreadThreadDiagnostics diagnostics {};

	EXPECT_FALSE(LibKernel::PthreadGetThreadDiagnostics(&diagnostics));
	EXPECT_FALSE(diagnostics.available);
	EXPECT_EQ(diagnostics.allocated_count, 0u);
	EXPECT_EQ(diagnostics.thread_count, 0u);
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
