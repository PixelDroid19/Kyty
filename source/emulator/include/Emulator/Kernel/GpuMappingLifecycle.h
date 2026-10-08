#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_GPUMAPPINGLIFECYCLE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_GPUMAPPINGLIFECYCLE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Kernel::Memory {

// Kernel-owned mapping access state. Graphics translates the lifecycle events
// rather than exposing its internal GpuMemoryMode through Kernel memory APIs.
enum class KernelGpuMappingAccessMode : uint8_t
{
	NoAccess,
	Read,
	Write,
	ReadWrite,
};

// Physical mappings view the kernel's shared backing, whose population
// KernelQueryPhysicalPopulation reports; flexible ones are private anonymous memory.
enum class KernelGpuMappingBacking : uint8_t
{
	Physical,
	Flexible,
};

using KernelGpuMappingCompletion      = bool (*)(void*);
using KernelGpuMappingRegisterRange   = void (*)(void* context, uint64_t vaddr, uint64_t size, KernelGpuMappingBacking backing);
using KernelGpuMappingInvalidateRange = bool (*)(void* context, uint64_t vaddr, uint64_t size);
using KernelGpuMappingReleaseRange    = bool (*)(void* context, uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion,
                                                 void* completion_data);
// Runs `completion` (Kernel's protection change) with GPU submissions
// quiesced; only when it returns true are the GPU's copies of the range
// dropped, before submissions resume. Returns the completion's result.
using KernelGpuMappingProtectRange    = bool (*)(void* context, uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion,
                                                 void* completion_data);

struct GpuMappingLifecycleCallbacks
{
	void*                           context          = nullptr;
	KernelGpuMappingRegisterRange   register_range   = nullptr;
	KernelGpuMappingInvalidateRange invalidate_range = nullptr;
	KernelGpuMappingReleaseRange    release_range    = nullptr;
	KernelGpuMappingProtectRange    protect_range    = nullptr;
};

// A complete callback bundle is installed once and remains process-lifetime.
// Partial or replacement bundles are rejected. RegisterRange and ProtectRange
// return false before installation without running anything, so Kernel can
// reject a GPU-visible map or protection change before mutating guest memory.
// Callback invocation is outside the port lock, so adapters may re-enter
// Kernel without deadlocking the registry.
class GpuMappingLifecyclePort
{
public:
	[[nodiscard]] bool Install(const GpuMappingLifecycleCallbacks& callbacks);
	[[nodiscard]] bool IsInstalled() const;

	[[nodiscard]] bool RegisterRange(uint64_t vaddr, uint64_t size, KernelGpuMappingBacking backing);
	[[nodiscard]] bool InvalidateRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool ReleaseRange(uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion, void* completion_data);
	[[nodiscard]] bool ProtectRange(uint64_t vaddr, uint64_t size, KernelGpuMappingCompletion completion, void* completion_data);

private:
	mutable std::mutex           m_mutex;
	GpuMappingLifecycleCallbacks m_callbacks {};
};

GpuMappingLifecyclePort& GetGpuMappingLifecyclePort();

} // namespace Kyty::Kernel::Memory

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_GPUMAPPINGLIFECYCLE_H_ */
