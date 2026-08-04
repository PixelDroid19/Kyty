#include "Kyty/Core/Core.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/GpuMappingLifecycle.h"
#include "Emulator/Kernel/Memory.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstdio>
#include <cstdlib>

using namespace Kyty;

namespace {

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
	auto* memory  = Libs::LibKernel::Memory::MemorySubsystem::Instance();

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
	Expect(record->vaddr == reinterpret_cast<uint64_t>(&Libs::LibKernel::Memory::KernelReserveVirtualRange),
	       "7oxv3PPCumo must resolve to KernelReserveVirtualRange");

	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(nullptr, kGuestPage, 0, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "null address pointer must be rejected");

	void* addr = nullptr;
	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage - 1, 0, 0) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "length must be guest-page aligned");
	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage, 0, 0x6000) == Libs::LibKernel::KERNEL_ERROR_EINVAL,
	       "alignment must be a guest-page-sized power of two");

	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, kGuestPage * 2, 0, kGuestPage * 2) == 0,
	       "valid reservation must succeed");
	Expect(addr != nullptr, "successful reservation must publish an address");
	Expect((reinterpret_cast<uintptr_t>(addr) & (kGuestPage * 2 - 1)) == 0, "reservation must honor alignment");
	Expect(Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(addr), kGuestPage * 2) == 0,
	       "reserved range must be releasable through KernelMunmap");

	void* fixed_addr = addr;
	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&fixed_addr, kGuestPage * 2, 0x10 | 0x80, kGuestPage * 2) == 0,
	       "fixed no-overwrite reservation must succeed at a free aligned address");
	Expect(fixed_addr == addr, "fixed reservation must preserve the requested address");
	void* duplicate_addr = fixed_addr;
	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&duplicate_addr, kGuestPage * 2, 0x10 | 0x80, kGuestPage * 2) ==
	           Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "fixed no-overwrite reservation must reject an occupied range");
	Expect(Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(fixed_addr), kGuestPage * 2) == 0,
	       "fixed reservation must be releasable");

	void* large_addr = nullptr;
	Expect(Libs::LibKernel::Memory::KernelReserveVirtualRange(&large_addr, UINT64_C(0x8000000000), 0, UINT64_C(0x200000)) == 0,
	       "observed 512 GiB reservation contract must fit the guest virtual window");
	Expect(Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(large_addr), UINT64_C(0x8000000000)) == 0,
	       "large reservation must be releasable");
	return 0;
}

struct GpuMappingPortProbe
{
	uint64_t register_vaddr = 0;
	uint64_t register_size  = 0;
	uint64_t release_vaddr  = 0;
	uint64_t release_size   = 0;
	uint32_t register_count = 0;
	uint32_t release_count  = 0;

	static void RegisterRange(void* context, uint64_t vaddr, uint64_t size)
	{
		auto* probe            = static_cast<GpuMappingPortProbe*>(context);
		probe->register_vaddr  = vaddr;
		probe->register_size   = size;
		probe->register_count += 1;
	}

	static bool ReleaseRange(void* context, uint64_t vaddr, uint64_t size,
	                         Libs::LibKernel::Memory::KernelGpuMappingCompletion completion, void* completion_data)
	{
		auto* probe           = static_cast<GpuMappingPortProbe*>(context);
		probe->release_vaddr  = vaddr;
		probe->release_size   = size;
		probe->release_count += 1;
		return completion(completion_data);
	}
};

int GpuMappingLifecycleContract()
{
	using namespace Libs::LibKernel::Memory;

	constexpr size_t kSize = 0x4000;
	auto&            port  = GetGpuMappingLifecyclePort();

	size_t available_before = 0;
	Expect(KernelAvailableFlexibleMemorySize(&available_before) == 0, "must query flexible availability before GPU map");
	void* missing_port_mapping = nullptr;
	Expect(KernelMapNamedFlexibleMemory(&missing_port_mapping, kSize, 0x11, 0, "gpu-port-missing") ==
	           Libs::LibKernel::KERNEL_ERROR_EBUSY,
	       "GPU-visible map must reject an uninstalled lifecycle port before allocation");
	Expect(missing_port_mapping == nullptr, "rejected GPU-visible map must not publish a mapping");
	size_t available_after = 0;
	Expect(KernelAvailableFlexibleMemorySize(&available_after) == 0, "must query flexible availability after rejected GPU map");
	Expect(available_after == available_before, "rejected GPU-visible map must not consume flexible memory");

	GpuMappingPortProbe          probe {};
	GpuMappingLifecycleCallbacks callbacks {};
	callbacks.context        = &probe;
	callbacks.register_range = GpuMappingPortProbe::RegisterRange;
	callbacks.release_range  = GpuMappingPortProbe::ReleaseRange;
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
