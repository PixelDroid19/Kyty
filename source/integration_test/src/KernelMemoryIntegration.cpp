#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/GpuMappingLifecycle.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace Kyty;

namespace {

constexpr uint32_t kRegistrationAliasMarker = 0x51a5c0deu;

[[noreturn]] void Die(const char* message)
{
	std::fprintf(stderr, "kernel-memory integration failure: %s\n", message);
	std::fflush(stderr);
	std::_Exit(1);
}

void Expect(bool condition, const char* message)
{
	if (!condition)
	{
		Die(message);
	}
}

int InitializeMemory(int argc, char** argv)
{
	auto* subsystems = Core::SubsystemsListSingleton::Instance();
	subsystems->SetArgs(argc, argv);

	auto* core    = Core::CoreSubsystem::Instance();
	auto* config  = Config::ConfigSubsystem::Instance();
	auto* threads = Core::ThreadsSubsystem::Instance();
	auto* log     = Log::LogSubsystem::Instance();
	auto* memory  = Kernel::Memory::MemorySubsystem::Instance();

	subsystems->Add(core, {});
	subsystems->Add(config, {core});
	subsystems->Add(threads, {core});
	subsystems->Add(log, {core, config, threads});
	subsystems->Add(memory, {core, log});

	if (!subsystems->InitAll(false))
	{
		std::fprintf(stderr, "host subsystem init failed: %s\n", subsystems->GetFailMsg());
		return 125;
	}
	return 0;
}

int ReserveVirtualRangeContract()
{
	constexpr uint64_t kGuestPage = 0x4000;

	Loader::SymbolDatabase symbols;
	Expect(Libs::Init(U"libkernel_1", &symbols), "libkernel HLE registration must succeed");
	Loader::SymbolResolve reserve_symbol {};
	reserve_symbol.name                 = U"7oxv3PPCumo";
	reserve_symbol.library              = U"libkernel";
	reserve_symbol.library_version      = 1;
	reserve_symbol.module               = U"libkernel";
	reserve_symbol.module_version_major = 1;
	reserve_symbol.module_version_minor = 1;
	reserve_symbol.type                 = Loader::SymbolType::Func;
	const auto* record                  = symbols.Find(reserve_symbol);
	Expect(record != nullptr, "7oxv3PPCumo must resolve through the libkernel HLE registry");
	Expect(record->vaddr == reinterpret_cast<uint64_t>(&Kernel::Memory::KernelReserveVirtualRange),
	       "7oxv3PPCumo must resolve to KernelReserveVirtualRange");

	Expect(Kernel::Memory::KernelReserveVirtualRange(nullptr, kGuestPage, 0, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "null address pointer must be rejected");

	void* addr = nullptr;
	Expect(Kernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage - 1, 0, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "length must be guest-page aligned");
	Expect(Kernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage, 0, 0x6000) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "alignment must be a guest-page-sized power of two");

	Expect(Kernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage * 2, 0, kGuestPage * 2) == 0,
	       "valid reservation must succeed");
	Expect(addr != nullptr, "successful reservation must publish an address");
	Expect((reinterpret_cast<uintptr_t>(addr) & (kGuestPage * 2 - 1)) == 0, "reservation must honor alignment");
	Expect(Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(addr), kGuestPage * 2) == 0,
	       "reserved range must be releasable through KernelMunmap");

	void* fixed_addr = addr;
	Expect(Kernel::Memory::KernelReserveVirtualRange(&fixed_addr, kGuestPage * 2, 0x10 | 0x80, kGuestPage * 2) == 0,
	       "fixed no-overwrite reservation must succeed at a free aligned address");
	Expect(fixed_addr == addr, "fixed reservation must preserve the requested address");
	void* duplicate_addr = fixed_addr;
	Expect(Kernel::Memory::KernelReserveVirtualRange(&duplicate_addr, kGuestPage * 2, 0x10 | 0x80, kGuestPage * 2) ==
	           Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "fixed no-overwrite reservation must reject an occupied range");
	Expect(Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(fixed_addr), kGuestPage * 2) == 0,
	       "fixed reservation must be releasable");

	void* large_addr = nullptr;
	Expect(Kernel::Memory::KernelReserveVirtualRange(&large_addr, UINT64_C(0x8000000000), 0, UINT64_C(0x200000)) == 0,
	       "observed 512 GiB reservation contract must fit the guest virtual window");
	Expect(Kernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(large_addr), UINT64_C(0x8000000000)) == 0,
	       "large reservation must be releasable");
	return 0;
}

struct GpuMappingPortProbe
{
	struct RegisteredRange
	{
		uint64_t vaddr  = 0;
		uint64_t size   = 0;
		bool     active = false;
	};

	uint64_t register_vaddr   = 0;
	uint64_t register_size    = 0;
	uint64_t invalidate_vaddr = 0;
	uint64_t invalidate_size  = 0;
	uint64_t release_vaddr    = 0;
	uint64_t release_size     = 0;
	uint64_t protect_vaddr    = 0;
	uint64_t protect_size     = 0;
	uint32_t register_count   = 0;
	uint32_t invalidate_count = 0;
	size_t   invalidated_range_count = 0;
	uint32_t release_count    = 0;
	uint32_t protect_count    = 0;
	size_t   reject_invalidation_number = 0;
	std::mutex registered_ranges_mutex;
	std::vector<RegisteredRange> registered_ranges;
	uint64_t alias_during_registration = 0;
	bool     registration_alias_backing_matches = false;
	uint64_t alias_during_invalidation = 0;
	std::mutex release_mutex;
	std::condition_variable release_condition;
	bool hold_next_release = false;
	bool release_entered = false;
	bool allow_release = false;
	std::mutex invalidation_mutex;
	std::condition_variable invalidation_condition;
	bool hold_next_invalidation = false;
	bool invalidation_entered = false;
	bool allow_invalidation = false;
	std::mutex registration_mutex;
	std::condition_variable registration_condition;
	bool hold_next_registration = false;
	bool registration_entered = false;
	bool allow_registration = false;
	std::atomic<bool> registration_active {false};
	std::atomic<bool> release_during_registration {false};
	std::atomic<bool> invalidate_during_registration {false};
	std::atomic<uint64_t> lifecycle_sequence {0};
	std::atomic<uint64_t> register_completed_sequence {0};
	std::atomic<uint64_t> invalidate_started_sequence {0};
	std::atomic<uint64_t> release_started_sequence {0};

	void RegisterGpuRange(uint64_t vaddr, uint64_t size)
	{
		std::lock_guard<std::mutex> lock(registered_ranges_mutex);
		for (auto& range: registered_ranges)
		{
			if (range.vaddr == vaddr && range.size == size)
			{
				range.active = true;
				return;
			}
		}
		registered_ranges.push_back({vaddr, size, true});
	}

	void InvalidateGpuRange(uint64_t vaddr, uint64_t size)
	{
		const uint64_t end = vaddr + size;
		std::lock_guard<std::mutex> lock(registered_ranges_mutex);
		for (auto& range: registered_ranges)
		{
			if (range.active && range.vaddr < end && vaddr < range.vaddr + range.size) { range.active = false; }
		}
	}

	void ReleaseGpuRange(uint64_t vaddr, uint64_t size) { InvalidateGpuRange(vaddr, size); }

	bool HasGpuRange(uint64_t vaddr, uint64_t size)
	{
		std::lock_guard<std::mutex> lock(registered_ranges_mutex);
		for (const auto& range: registered_ranges)
		{
			if (range.active && range.vaddr == vaddr && range.size == size) { return true; }
		}
		return false;
	}

	void HoldNextRelease()
	{
		std::lock_guard<std::mutex> lock(release_mutex);
		hold_next_release = true;
		release_entered = false;
		allow_release = false;
	}

	bool WaitForHeldRelease(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(release_mutex);
		return release_condition.wait_for(lock, timeout, [this] { return release_entered; });
	}

	void AllowRelease()
	{
		{
			std::lock_guard<std::mutex> lock(release_mutex);
			allow_release = true;
		}
		release_condition.notify_all();
	}

	void HoldNextInvalidation()
	{
		std::lock_guard<std::mutex> lock(invalidation_mutex);
		hold_next_invalidation = true;
		invalidation_entered = false;
		allow_invalidation = false;
	}

	bool WaitForHeldInvalidation(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(invalidation_mutex);
		return invalidation_condition.wait_for(lock, timeout, [this] { return invalidation_entered; });
	}

	void AllowInvalidation()
	{
		{
			std::lock_guard<std::mutex> lock(invalidation_mutex);
			allow_invalidation = true;
		}
		invalidation_condition.notify_all();
	}

	void HoldNextRegistration()
	{
		std::lock_guard<std::mutex> lock(registration_mutex);
		hold_next_registration = true;
		registration_entered = false;
		allow_registration = false;
	}

	bool WaitForHeldRegistration(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(registration_mutex);
		return registration_condition.wait_for(lock, timeout, [this] { return registration_entered; });
	}

	void AllowRegistration()
	{
		{
			std::lock_guard<std::mutex> lock(registration_mutex);
			allow_registration = true;
		}
		registration_condition.notify_all();
	}

	static void RegisterRange(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingBacking backing)
	{
		auto* probe           = static_cast<GpuMappingPortProbe*>(context);
		if (backing == Kernel::Memory::KernelGpuMappingBacking::Physical)
		{
			probe->alias_during_registration = Kernel::Memory::KernelMapPhysicalAlias(vaddr, size);
			if (probe->alias_during_registration != 0)
			{
				const auto* guest_word = reinterpret_cast<const uint32_t*>(vaddr);
				const auto* alias_word = reinterpret_cast<const uint32_t*>(probe->alias_during_registration);
				probe->registration_alias_backing_matches =
				    *guest_word == kRegistrationAliasMarker && *alias_word == kRegistrationAliasMarker;
				(void)Kernel::Memory::KernelUnmapPhysicalAlias(probe->alias_during_registration);
			}
		}
		{
			std::unique_lock<std::mutex> lock(probe->registration_mutex);
			probe->registration_active.store(true);
			if (probe->hold_next_registration)
			{
				probe->hold_next_registration = false;
				probe->registration_entered = true;
				probe->registration_condition.notify_all();
				(void)probe->registration_condition.wait_for(lock, std::chrono::seconds(5), [probe] { return probe->allow_registration; });
			}
		}
		probe->register_vaddr = vaddr;
		probe->register_size  = size;
		probe->register_count += 1;
		probe->RegisterGpuRange(vaddr, size);
		probe->register_completed_sequence.store(++probe->lifecycle_sequence);
		probe->registration_active.store(false);
	}

	static bool InvalidateRanges(void* context, const Kernel::Memory::KernelGpuMappingRange* ranges, size_t range_count)
	{
		auto* probe = static_cast<GpuMappingPortProbe*>(context);
		if (ranges == nullptr || range_count == 0) { return false; }
		for (size_t i = 0; i < range_count; ++i)
		{
			if (ranges[i].vaddr == 0 || ranges[i].size == 0 ||
			    ranges[i].vaddr > std::numeric_limits<uint64_t>::max() - ranges[i].size)
			{
				return false;
			}
		}
		if (probe->registration_active.load())
		{
			probe->invalidate_during_registration.store(true);
		}
		probe->invalidate_vaddr = ranges[0].vaddr;
		probe->invalidate_size  = ranges[0].size;
		probe->invalidate_count += 1;
		probe->invalidated_range_count += range_count;
		probe->invalidate_started_sequence.store(++probe->lifecycle_sequence);
		probe->alias_during_invalidation = Kernel::Memory::KernelMapPhysicalAlias(ranges[0].vaddr, ranges[0].size);
		if (probe->alias_during_invalidation != 0)
		{
			(void)Kernel::Memory::KernelUnmapPhysicalAlias(probe->alias_during_invalidation);
		}
		{
			std::unique_lock<std::mutex> lock(probe->invalidation_mutex);
			if (probe->hold_next_invalidation)
			{
				probe->hold_next_invalidation = false;
				probe->invalidation_entered = true;
				probe->invalidation_condition.notify_all();
				(void)probe->invalidation_condition.wait_for(lock, std::chrono::seconds(5),
				                                            [probe] { return probe->allow_invalidation; });
			}
		}
		for (size_t i = 0; i < range_count; ++i)
		{
			if (probe->reject_invalidation_number == i + 1) { return false; }
		}
		for (size_t i = 0; i < range_count; ++i) { probe->InvalidateGpuRange(ranges[i].vaddr, ranges[i].size); }
		return true;
	}

	static bool ReleaseRange(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingCompletion completion,
	                         void* completion_data)
	{
		auto* probe          = static_cast<GpuMappingPortProbe*>(context);
		if (probe->registration_active.load())
		{
			probe->release_during_registration.store(true);
		}
		{
			std::unique_lock<std::mutex> lock(probe->release_mutex);
			if (probe->hold_next_release)
			{
				probe->hold_next_release = false;
				probe->release_entered = true;
				probe->release_condition.notify_all();
				(void)probe->release_condition.wait_for(lock, std::chrono::seconds(5),
				                                        [probe] { return probe->allow_release; });
			}
		}
		probe->release_vaddr = vaddr;
		probe->release_size  = size;
		probe->release_count += 1;
		probe->release_started_sequence.store(++probe->lifecycle_sequence);
		const bool released = completion(completion_data);
		if (released) { probe->ReleaseGpuRange(vaddr, size); }
		return released;
	}

	static bool ProtectRange(void* context, uint64_t vaddr, uint64_t size, Kernel::Memory::KernelGpuMappingCompletion completion,
	                         void* completion_data)
	{
		auto* probe          = static_cast<GpuMappingPortProbe*>(context);
		probe->protect_vaddr = vaddr;
		probe->protect_size  = size;
		probe->protect_count += 1;
		return completion(completion_data);
	}
};

struct RegistrationTeardownResults
{
	std::mutex              mutex;
	std::condition_variable changed;
	bool                    unmap_started      = false;
	bool                    release_started    = false;
	bool                    unmap_done         = false;
	bool                    release_done       = false;
	int                     unmap_result       = -9999;
	int                     release_result     = -9999;

	void StartedUnmap()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			unmap_started = true;
		}
		changed.notify_all();
	}

	void StartedRelease()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			release_started = true;
		}
		changed.notify_all();
	}

	bool WaitForStarted(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this] { return unmap_started && release_started; });
	}

	void CompleteUnmap(int result)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			unmap_result = result;
			unmap_done = true;
		}
		changed.notify_all();
	}

	void CompleteRelease(int result)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			release_result = result;
			release_done = true;
		}
		changed.notify_all();
	}

	bool WaitForBoth(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this] { return unmap_done && release_done; });
	}
};

struct PendingKernelCall
{
	std::mutex              mutex;
	std::condition_variable changed;
	bool                    started   = false;
	bool                    completed = false;
	int                     result    = -9999;

	void Started()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			started = true;
		}
		changed.notify_all();
	}

	void Complete(int value)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			result    = value;
			completed = true;
		}
		changed.notify_all();
	}

	bool WaitForStarted(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this] { return started; });
	}

	bool WaitForCompletion(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this] { return completed; });
	}
};

int GpuMappingLifecycleContract()
{
	using namespace Kernel::Memory;

	constexpr size_t kSize = 0x4000;
	auto&            port  = GetGpuMappingLifecyclePort();

	size_t available_before = 0;
	Expect(KernelAvailableFlexibleMemorySize(&available_before) == 0, "must query flexible availability before GPU map");
	void* missing_port_mapping = nullptr;
	Expect(KernelMapNamedFlexibleMemory(&missing_port_mapping, kSize, 0x11, 0, "gpu-port-missing") == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "GPU-visible map must reject an uninstalled lifecycle port before allocation");
	Expect(missing_port_mapping == nullptr, "rejected GPU-visible map must not publish a mapping");
	size_t available_after = 0;
	Expect(KernelAvailableFlexibleMemorySize(&available_after) == 0, "must query flexible availability after rejected GPU map");
	Expect(available_after == available_before, "rejected GPU-visible map must not consume flexible memory");

	GpuMappingPortProbe          probe {};
	GpuMappingLifecycleCallbacks callbacks {};
	callbacks.context          = &probe;
	callbacks.register_range   = GpuMappingPortProbe::RegisterRange;
	callbacks.invalidate_ranges = GpuMappingPortProbe::InvalidateRanges;
	callbacks.release_range    = GpuMappingPortProbe::ReleaseRange;
	Expect(!port.Install(callbacks), "lifecycle port must refuse a bundle without the protection callback");
	callbacks.protect_range = GpuMappingPortProbe::ProtectRange;
	Expect(port.Install(callbacks), "complete GPU mapping lifecycle port must install");

	void* mapping = nullptr;
	Expect(KernelMapNamedFlexibleMemory(&mapping, kSize, 0x11, 0, "gpu-port-fake") == 0,
	       "GPU-visible map must succeed with a complete lifecycle port");
	Expect(mapping != nullptr, "GPU-visible map must publish an address");
	Expect(probe.register_count == 1, "GPU-visible map must register exactly once through the lifecycle port");
	Expect(probe.register_vaddr == reinterpret_cast<uint64_t>(mapping) && probe.register_size == kSize,
	       "GPU-visible map must register its exact range");
	Expect(KernelMunmap(reinterpret_cast<uint64_t>(mapping), kSize) == 0,
	       "GPU-visible map must release through the installed lifecycle port");
	Expect(probe.release_count == 1, "GPU-visible unmap must release exactly once through the lifecycle port");
	Expect(probe.release_vaddr == reinterpret_cast<uint64_t>(mapping) && probe.release_size == kSize,
	       "GPU-visible unmap must release its exact range");

	void* owner = nullptr;
	Expect(KernelReserveVirtualRange(&owner, kSize, 0, kSize) == 0 &&
	           KernelMapNamedFlexibleMemory(&owner, kSize, 0x11, 0x10, "gpu-port-protect") == 0,
	       "aligned GPU-visible owner must map");
	const auto owner_vaddr = reinterpret_cast<uint64_t>(owner);
	Expect(KernelMprotect(owner, kSize, 0x3) == 0, "CPU-only writable protection of a GPU-visible owner must succeed");
	Expect(probe.protect_count == 1 && probe.protect_vaddr == owner_vaddr && probe.protect_size == kSize,
	       "making a read-only GPU-visible owner writable must protect its range once through the lifecycle port");
	Expect(Core::VirtualMemory::IsRangeWritable(owner_vaddr, kSize), "completed lifecycle protection must apply the writable rights");
	Expect(KernelMprotect(owner, kSize, 0x12) == 0 && probe.protect_count == 1,
	       "an already writable GPU-visible owner must not need lifecycle protection");
	Expect(KernelMprotect(owner, kSize, 0x11) == 0 && KernelMprotect(owner, kSize, 0x12) == 0 && probe.protect_count == 2,
	       "a GPU-visible owner made read-only again must protect through the lifecycle port when it becomes writable");
	Expect(probe.register_count == 2, "protection changes must not register the retained owner again");
	Expect(KernelMunmap(owner_vaddr, kSize) == 0 && probe.release_count == 2, "protected GPU-visible owner must still release once");

	constexpr uint64_t kGuestPage = 0x4000;
	void* partial_owner_range = nullptr;
	Expect(KernelReserveVirtualRange(&partial_owner_range, 2 * kGuestPage, 0, 2 * kGuestPage) == 0,
	       "single-owner partial-span reservation must succeed");
	const auto partial_owner_base = reinterpret_cast<uint64_t>(partial_owner_range);
	void*      partial_owner      = partial_owner_range;
	Expect(KernelMapNamedFlexibleMemory(&partial_owner, kGuestPage, 0x03, 0x10, "gpu-partial-owner") == 0,
	       "CPU-only flexible owner must map for partial GPU protection validation");
	const auto core_guest_neighbor = partial_owner_base + kGuestPage;
	Expect(Core::VirtualMemory::AllocFixedReplacingOwnedReservation(core_guest_neighbor, kGuestPage,
	                                                                Core::VirtualMemory::Mode::Read),
	       "adjacent Core-owned guest mapping must replace the remaining reservation");
	const auto register_count_before_partial = probe.register_count;
	const auto release_count_before_partial  = probe.release_count;
	int        protection_before_partial     = -1;
	Expect(KernelQueryMemoryProtection(partial_owner, nullptr, nullptr, &protection_before_partial) == 0 &&
	           protection_before_partial == 0x03,
	       "CPU-only owner protection must be queryable before partial GPU promotion");
	Expect(Core::VirtualMemory::IsRangeWritable(partial_owner_base, kGuestPage) &&
	           !Core::VirtualMemory::IsRangeWritable(core_guest_neighbor, kGuestPage),
	       "mixed owner and Core guest range must start with distinct host rights");
	const int partial_span_result = KernelMprotect(partial_owner_range, 2 * kGuestPage, 0x11);
	int       protection_after_partial = -1;
	Expect(partial_span_result == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "GPU protection spanning part of one owner and an adjacent Core guest mapping must reject");
	Expect(KernelQueryMemoryProtection(partial_owner, nullptr, nullptr, &protection_after_partial) == 0 &&
	           protection_after_partial == 0x03,
	       "rejected partial-owner GPU protection must preserve the tracked owner's protection");
	Expect(Core::VirtualMemory::IsRangeWritable(partial_owner_base, kGuestPage) &&
	           !Core::VirtualMemory::IsRangeWritable(core_guest_neighbor, kGuestPage),
	       "rejected partial-owner GPU protection must preserve host rights for both mappings");
	Expect(probe.register_count == register_count_before_partial,
	       "rejected partial-owner GPU protection must not register an unowned combined range");
	Expect(KernelMunmap(partial_owner_base, kGuestPage) == 0,
	       "CPU-only partial-span owner must unmap after rejected promotion");
	Expect(probe.release_count == release_count_before_partial,
	       "CPU-only partial-span owner must not release an unregistered GPU range");
	Expect(KernelMunmap(core_guest_neighbor, kGuestPage) == 0,
	       "adjacent Core guest mapping must release through its remaining reservation record");

	void* adjacent_owners = nullptr;
	Expect(KernelReserveVirtualRange(&adjacent_owners, 2 * kGuestPage, 0, 2 * kGuestPage) == 0,
	       "adjacent GPU-owner reservation must succeed");
	const auto adjacent_base = reinterpret_cast<uint64_t>(adjacent_owners);
	void*      first_owner   = adjacent_owners;
	void*      second_owner  = reinterpret_cast<void*>(adjacent_base + kGuestPage);
	Expect(KernelMapNamedFlexibleMemory(&first_owner, kGuestPage, 0x11, 0x10, "gpu-owner-a") == 0 &&
	           KernelMapNamedFlexibleMemory(&second_owner, kGuestPage, 0x11, 0x10, "gpu-owner-b") == 0,
	       "adjacent GPU-visible flexible owners must map");
	int first_protection = -1;
	int second_protection = -1;
	const int adjacent_protect = KernelMprotect(adjacent_owners, 2 * kGuestPage, 0x12);
	Expect(adjacent_protect == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "GPU protection across multiple owners must reject before changing host rights");
	Expect(KernelQueryMemoryProtection(first_owner, nullptr, nullptr, &first_protection) == 0 &&
	           KernelQueryMemoryProtection(second_owner, nullptr, nullptr, &second_protection) == 0 && first_protection == 0x11 &&
	           second_protection == 0x11,
	       "rejected multi-owner GPU protection must preserve both kernel protection records");
	Expect(!Core::VirtualMemory::IsRangeWritable(adjacent_base, kGuestPage) &&
	           !Core::VirtualMemory::IsRangeWritable(adjacent_base + kGuestPage, kGuestPage),
	       "rejected multi-owner GPU protection must preserve both host protections");
	Expect(probe.register_count == 4, "rejected multi-owner GPU protection must not register a partial range");
	Expect(KernelMunmap(adjacent_base, kGuestPage) == 0 && KernelMunmap(adjacent_base + kGuestPage, kGuestPage) == 0,
	       "adjacent GPU-visible owners must release independently");
	Expect(probe.release_count == 4, "both adjacent GPU-visible owners must release once");

	Config::SetNextGen(true);
	int64_t registration_physical = 0;
	Expect(KernelAllocateMainDirectMemory(kSize, kSize, 12, &registration_physical) == 0,
	       "direct allocation must succeed for registration teardown race validation");
	void* registration_mapping = nullptr;
	Expect(KernelMapDirectMemory(&registration_mapping, kSize, 0x03, 0, registration_physical, kSize) == 0 &&
	           registration_mapping != nullptr,
	       "CPU-only direct mapping must succeed for registration teardown race validation");
	const auto registration_vaddr = reinterpret_cast<uint64_t>(registration_mapping);
	*reinterpret_cast<uint32_t*>(registration_mapping) = kRegistrationAliasMarker;
	const uint32_t registrations_before = probe.register_count;
	const uint32_t releases_before = probe.release_count;
	const uint32_t invalidations_before = probe.invalidate_count;
	probe.HoldNextRegistration();
	int promotion_result = -9999;
	std::thread promoter([&] { promotion_result = KernelMprotect(registration_mapping, kSize, 0x11); });
	const bool registration_held = probe.WaitForHeldRegistration(std::chrono::seconds(2));
	if (!registration_held)
	{
		probe.AllowRegistration();
		promoter.join();
		Die("GPU promotion must reach the held owner registration callback");
	}
	RegistrationTeardownResults teardown_results {};
	std::thread registration_unmapper(
	    [&]
	    {
		    teardown_results.StartedUnmap();
		    teardown_results.CompleteUnmap(KernelMunmap(registration_vaddr, kSize));
	    });
	std::thread registration_releaser(
	    [&]
	    {
		    teardown_results.StartedRelease();
		    teardown_results.CompleteRelease(KernelCheckedReleaseDirectMemory(registration_physical, kSize));
	    });
	const bool teardowns_started = teardown_results.WaitForStarted(std::chrono::seconds(2));
	const bool teardowns_returned_while_registering = teardown_results.WaitForBoth(std::chrono::milliseconds(200));
	probe.AllowRegistration();
	promoter.join();
	registration_unmapper.join();
	registration_releaser.join();
	Expect((teardown_results.unmap_result == 0 || teardown_results.unmap_result == Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
	           (teardown_results.release_result == 0 || teardown_results.release_result == Libs::LibKernel::KERNEL_ERROR_EBUSY) &&
	           (teardown_results.unmap_result == 0 || teardown_results.release_result == 0),
	       "one teardown must claim the owner while the competing pending operation reports EBUSY");
	if (teardown_results.unmap_result == Libs::LibKernel::KERNEL_ERROR_EBUSY)
	{
		Expect(KernelMunmap(registration_vaddr, kSize) == 0,
		       "unmap must succeed when retried after the direct-release claim completes");
	}
	if (teardown_results.release_result == Libs::LibKernel::KERNEL_ERROR_EBUSY)
	{
		Expect(KernelCheckedReleaseDirectMemory(registration_physical, kSize) == 0,
		       "direct release must succeed when retried after the unmap claim completes");
	}
	const uint32_t releases_after_serialized_teardowns = probe.release_count;
	const uint32_t invalidations_after_serialized_teardowns = probe.invalidate_count;
	const bool release_overlapped_registration = probe.release_during_registration.load();
	const bool invalidation_overlapped_registration = probe.invalidate_during_registration.load();
	Expect(teardowns_started, "unmap and direct release threads must reach the lifecycle gate");
	Expect(!teardowns_returned_while_registering,
	       "unmap and direct release must wait behind the in-flight owner registration callback");
	Expect(promotion_result == 0, "owner GPU registration must complete after its callback is released");
	Expect(releases_after_serialized_teardowns == releases_before + 1 &&
	           invalidations_after_serialized_teardowns >= invalidations_before &&
	           invalidations_after_serialized_teardowns <= invalidations_before + 1 &&
	           !release_overlapped_registration && !invalidation_overlapped_registration,
	       "teardown callbacks must run after owner registration and release the mapping exactly once");
	Expect(probe.register_count == registrations_before + 1,
	       "pending owner registration must complete exactly once after its callback is released");
	Expect(probe.alias_during_registration != 0 && probe.registration_alias_backing_matches,
	       "a physical alias must share the owner bytes while GPU registration is still in flight");
	if (invalidations_after_serialized_teardowns != invalidations_before)
	{
		Expect(probe.invalidate_started_sequence.load() > probe.register_completed_sequence.load(),
		       "physical invalidation must follow the completed owner registration");
		Expect(probe.alias_during_invalidation == 0,
		       "a new physical alias must not appear during direct-release invalidation");
	}
	const uint64_t completed_registration_sequence = probe.register_completed_sequence.load();
	Expect(probe.release_started_sequence.load() > completed_registration_sequence,
	       "GPU release must be ordered after owner registration callback completion");

	int64_t pending_physical = 0;
	Expect(KernelAllocateMainDirectMemory(kSize, kSize, 12, &pending_physical) == 0,
	       "direct allocation must succeed for pending-unmap protection validation");
	void* pending_mapping = nullptr;
	Expect(KernelMapDirectMemory(&pending_mapping, kSize, 0x11, 0, pending_physical, kSize) == 0 && pending_mapping != nullptr,
	       "GPU-visible direct mapping must succeed for pending-unmap validation");
	const auto pending_vaddr = reinterpret_cast<uint64_t>(pending_mapping);
	const uint32_t pending_release_count_before = probe.release_count;
	int        pending_unmap_result = Libs::LibKernel::KERNEL_ERROR_EBUSY;
	probe.HoldNextRelease();
	std::thread pending_unmapper([&] { pending_unmap_result = KernelMunmap(pending_vaddr, kSize); });
	const bool release_held = probe.WaitForHeldRelease(std::chrono::seconds(2));
	if (!release_held)
	{
		probe.AllowRelease();
		pending_unmapper.join();
		Die("GPU-visible direct unmap must reach the held release callback");
	}
	int protection_before_pending = -1;
	Expect(KernelQueryMemoryProtection(pending_mapping, nullptr, nullptr, &protection_before_pending) == 0 &&
	           protection_before_pending == 0x11 && !Core::VirtualMemory::IsRangeWritable(pending_vaddr, kSize),
	       "held unmap must leave the original direct mapping readable and non-writable");
	PendingKernelCall pending_protection {};
	std::thread pending_protector(
	    [&]
	    {
		    pending_protection.Started();
		    pending_protection.Complete(KernelMprotect(pending_mapping, kSize, 0x12));
	    });
	const bool mprotect_started = pending_protection.WaitForStarted(std::chrono::seconds(2));
	const bool mprotect_completed_while_release_held = pending_protection.WaitForCompletion(std::chrono::milliseconds(200));
	probe.AllowRelease();
	pending_unmapper.join();
	pending_protector.join();
	Expect(mprotect_started && mprotect_completed_while_release_held,
	       "mprotect must observe the pending unmap without waiting for GPU quiescence");
	Expect(pending_protection.result == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "mprotect must reject the owner while its unmap claim is pending");
	Expect(pending_unmap_result == 0, "held GPU-visible direct unmap must complete after release resumes");
	Expect(KernelCheckedReleaseDirectMemory(pending_physical, kSize) == 0,
	       "pending-unmap direct allocation must release after unmap completes");
	Expect(probe.release_count == pending_release_count_before + 1, "pending GPU-visible direct owner must release exactly once");

	int64_t invalidation_physical = 0;
	Expect(KernelAllocateMainDirectMemory(kSize, kSize, 12, &invalidation_physical) == 0,
	       "direct allocation must succeed for pending-invalidation validation");
	void* invalidation_mapping = nullptr;
	Expect(KernelMapDirectMemory(&invalidation_mapping, kSize, 0x12, 0, invalidation_physical, kSize) == 0 &&
	           invalidation_mapping != nullptr,
	       "GPU-visible direct mapping must succeed for pending-invalidation validation");
	const auto invalidation_vaddr = reinterpret_cast<uint64_t>(invalidation_mapping);
	const uint32_t invalidations_before_pending = probe.invalidate_count;
	probe.HoldNextInvalidation();
	int direct_release_result = -9999;
	std::thread invalidation_releaser(
	    [&] { direct_release_result = KernelCheckedReleaseDirectMemory(invalidation_physical, kSize); });
	const bool invalidation_held = probe.WaitForHeldInvalidation(std::chrono::seconds(2));
	if (!invalidation_held)
	{
		probe.AllowInvalidation();
		invalidation_releaser.join();
		Die("direct release must reach the held GPU invalidation callback");
	}
	PendingKernelCall unmap_during_invalidation {};
	std::thread invalidation_unmapper(
	    [&]
	    {
		    unmap_during_invalidation.Started();
		    unmap_during_invalidation.Complete(KernelMunmap(invalidation_vaddr, kSize));
	    });
	const bool unmap_started_during_invalidation = unmap_during_invalidation.WaitForStarted(std::chrono::seconds(2));
	const bool unmap_completed_during_invalidation =
	    unmap_during_invalidation.WaitForCompletion(std::chrono::milliseconds(200));
	probe.AllowInvalidation();
	invalidation_releaser.join();
	invalidation_unmapper.join();
	Expect(unmap_started_during_invalidation && unmap_completed_during_invalidation,
	       "unmap must observe the pending direct release without waiting for GPU invalidation");
	Expect(unmap_during_invalidation.result == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "unmap must reject a physical owner while direct-release invalidation is pending");
	Expect(direct_release_result == 0 && probe.invalidate_count == invalidations_before_pending + 1,
	       "direct release must finish after its invalidation callback resumes");
	Expect(KernelMunmap(invalidation_vaddr, kSize) == 0,
	       "mapping must remain explicitly unmapable after direct release completes");

	int64_t physical = 0;
	Expect(KernelAllocateMainDirectMemory(kSize, kSize, 12, &physical) == 0,
	       "direct allocation must succeed for physical lifetime validation");
	void* direct_mapping = nullptr;
	Expect(KernelMapDirectMemory(&direct_mapping, kSize, 0x12, 0, physical, kSize) == 0 && direct_mapping != nullptr,
	       "GPU-visible direct mapping must succeed");
	const uint32_t release_count_before_direct_unmap = probe.release_count;
	const uint32_t invalidate_count_before_direct_release = probe.invalidate_count;
	Expect(KernelCheckedReleaseDirectMemory(physical, kSize) == 0,
	       "physical lifetime release must succeed without removing its mapping");
	Expect(probe.invalidate_count == invalidate_count_before_direct_release + 1,
	       "physical lifetime release must invalidate GPU resources exactly once");
	Expect(probe.alias_during_invalidation == 0,
	       "physical alias creation must stay blocked while direct-release invalidation is pending");
	Expect(probe.invalidate_vaddr == reinterpret_cast<uint64_t>(direct_mapping) && probe.invalidate_size == kSize,
	       "physical lifetime release must invalidate the owner mapping");
	Expect(KernelQueryMemoryProtection(direct_mapping, nullptr, nullptr, nullptr) == 0,
	       "physical lifetime release must preserve the guest virtual mapping");
	Expect(KernelMunmap(reinterpret_cast<uint64_t>(direct_mapping), kSize) == 0,
	       "released direct mapping must remain explicitly unmapable");
	Expect(probe.release_count == release_count_before_direct_unmap + 1,
	       "explicit direct unmap must use the mapping release transaction");

	int64_t alias_batch_physical = 0;
	Expect(KernelAllocateMainDirectMemory(kSize, kSize, 12, &alias_batch_physical) == 0,
	       "direct allocation must succeed for overlapping alias invalidation validation");
	void* first_alias = nullptr;
	void* second_alias = nullptr;
	Expect(KernelMapDirectMemory(&first_alias, kSize, 0x12, 0, alias_batch_physical, kSize) == 0 && first_alias != nullptr,
	       "first GPU-visible alias must map the physical allocation");
	Expect(KernelMapDirectMemory(&second_alias, kSize, 0x12, 0, alias_batch_physical, kSize) == 0 && second_alias != nullptr,
	       "second GPU-visible alias must map the same physical allocation");
	const auto first_alias_vaddr  = reinterpret_cast<uint64_t>(first_alias);
	const auto second_alias_vaddr = reinterpret_cast<uint64_t>(second_alias);
	Expect(first_alias_vaddr != second_alias_vaddr, "physical aliases must have distinct guest virtual ranges");
	Expect(probe.HasGpuRange(first_alias_vaddr, kSize) && probe.HasGpuRange(second_alias_vaddr, kSize),
	       "both physical aliases must have live GPU registrations before release");
	const uint32_t invalidations_before_alias_batch = probe.invalidate_count;
	const size_t   invalidated_ranges_before_alias_batch = probe.invalidated_range_count;
	probe.reject_invalidation_number = 2;
	const int rejected_alias_batch_release = KernelCheckedReleaseDirectMemory(alias_batch_physical, kSize);
	probe.reject_invalidation_number = 0;
	Expect(rejected_alias_batch_release == Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "a rejected second alias invalidation must fail physical release with EBUSY");
	Expect(probe.invalidate_count == invalidations_before_alias_batch + 1 &&
	           probe.invalidated_range_count == invalidated_ranges_before_alias_batch + 2,
	       "injected batch rejection must inspect both alias ranges in one invalidation call");
	Expect(probe.HasGpuRange(first_alias_vaddr, kSize) && probe.HasGpuRange(second_alias_vaddr, kSize),
	       "failed physical release must preserve every alias GPU registration");
	const uint32_t invalidations_before_alias_retry = probe.invalidate_count;
	const size_t   invalidated_ranges_before_alias_retry = probe.invalidated_range_count;
	Expect(KernelCheckedReleaseDirectMemory(alias_batch_physical, kSize) == 0,
	       "physical allocation must remain available for a successful release retry");
	Expect(probe.invalidate_count == invalidations_before_alias_retry + 1 &&
	           probe.invalidated_range_count == invalidated_ranges_before_alias_retry + 2,
	       "successful retry must invalidate both physical aliases in one call");
	Expect(!probe.HasGpuRange(first_alias_vaddr, kSize) && !probe.HasGpuRange(second_alias_vaddr, kSize),
	       "successful physical release must retire both alias GPU registrations");
	Expect(KernelMunmap(first_alias_vaddr, kSize) == 0 && KernelMunmap(second_alias_vaddr, kSize) == 0,
	       "both aliases must remain explicitly unmapable after physical release");

	Config::SetNextGen(false);
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	const int init_rc = InitializeMemory(argc, argv);
	if (init_rc != 0)
	{
		return init_rc;
	}
	const int reserve_rc = ReserveVirtualRangeContract();
	if (reserve_rc != 0)
	{
		return reserve_rc;
	}
	return GpuMappingLifecycleContract();
}
