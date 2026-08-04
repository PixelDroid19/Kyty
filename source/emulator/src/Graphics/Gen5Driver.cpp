#include "Emulator/Graphics/Graphics.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace Gen5Driver {

LIB_NAME("Graphics5Driver", "Graphics5Driver");

static Core::Mutex     g_resource_registration_mutex;
static Vector<uint32_t> g_registered_resources;
static std::atomic<uint64_t> g_tf_ring_base {0};
static std::atomic<uint32_t> g_tf_ring_size {0};
static std::atomic<uint64_t> g_hs_offchip_value0 {0};
static std::atomic<uint64_t> g_hs_offchip_value1 {0};
static std::atomic<uint64_t> g_hs_offchip_value2 {0};

struct Packet
{
	uint32_t* addr;
	uint32_t  dw_num;
	uint8_t   pad[4];
};

int KYTY_SYSV_ABI GraphicsDriverQueryResourceRegistrationUserMemoryRequirements(size_t* size, uint32_t max_resources,
                                                                                 uint32_t max_owners)
{
	if (size == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	constexpr size_t header_bytes       = 256;
	constexpr size_t resource_bytes     = 64;
	constexpr size_t owner_bytes        = 64;
	constexpr size_t required_alignment = 64;
	constexpr size_t max_size = std::numeric_limits<size_t>::max();
	auto checked_add_scaled = [](size_t current, uint32_t count, size_t stride, size_t* result) {
		if (count != 0 && stride > (std::numeric_limits<size_t>::max() - current) / static_cast<size_t>(count))
		{
			return false;
		}
		*result = current + static_cast<size_t>(count) * stride;
		return true;
	};

	size_t unaligned = header_bytes;
	if (!checked_add_scaled(unaligned, max_resources, resource_bytes, &unaligned) ||
	    !checked_add_scaled(unaligned, max_owners, owner_bytes, &unaligned) || unaligned > max_size - (required_alignment - 1))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*size = (unaligned + required_alignment - 1) & ~(required_alignment - 1);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverInitResourceRegistration(void* memory, size_t size, uint32_t max_owners)
{
	if (memory == nullptr || size == 0 || max_owners == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<void*>    registration_memory {nullptr};
	static std::atomic<size_t>   registration_size {0};
	static std::atomic<uint32_t> registration_max_owners {0};
	registration_memory.store(memory, std::memory_order_release);
	registration_size.store(size, std::memory_order_release);
	registration_max_owners.store(max_owners, std::memory_order_release);
	return OK;
}

// Handle reserved for the driver's default resource owner. Named owners are
// allocated above it so their handles never collide with the default.
static constexpr uint32_t GRAPHICS_DEFAULT_OWNER = 1;

int KYTY_SYSV_ABI GraphicsDriverRegisterDefaultOwner(uint32_t options)
{
	if (options != 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<bool> default_owner_registered {false};
	default_owner_registered.store(true, std::memory_order_release);
	return OK;
}

// sce::Agc::ResourceRegistration::getDefaultOwner(unsigned int*): returns the
// handle of the driver's default owner through the output pointer.
int KYTY_SYSV_ABI GraphicsDriverGetDefaultOwner(uint32_t* owner)
{
	if (owner == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*owner = GRAPHICS_DEFAULT_OWNER;
	return OK;
}

// Maximum length, including the terminator, accepted for a registered resource
// name. Names passed to resource registration are bounded by this value.
static constexpr uint32_t GRAPHICS_RESOURCE_NAME_MAX = 256;

// sce::Agc::ResourceRegistration::getMaxNameLength(unsigned int*).
int KYTY_SYSV_ABI GraphicsDriverGetResourceRegistrationMaxNameLength(uint32_t* max_length)
{
	if (max_length == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	*max_length = GRAPHICS_RESOURCE_NAME_MAX;
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverRegisterOwner(uint32_t* owner, const char* name)
{
	if (owner == nullptr || name == nullptr || name[0] == '\0')
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<uint32_t> next_owner {GRAPHICS_DEFAULT_OWNER + 1};
	*owner = next_owner.fetch_add(1, std::memory_order_relaxed);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverRegisterResource(uint32_t* resource, uint32_t owner, const void* base, uint64_t size, const char* name,
                                                 uint32_t /*type*/, uint64_t /*user_data*/)
{
	if (resource == nullptr || owner == 0 || base == nullptr || size == 0 || name == nullptr || name[0] == '\0')
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	static std::atomic<uint32_t> next_resource {1};
	*resource = next_resource.fetch_add(1, std::memory_order_relaxed);
	{
		Core::LockGuard lock(g_resource_registration_mutex);
		g_registered_resources.Add(*resource);
	}
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverUnregisterResource(uint32_t resource)
{
	if (resource == 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	Core::LockGuard lock(g_resource_registration_mutex);
	const auto      index = g_registered_resources.Find(resource);
	if (!g_registered_resources.IndexValid(index))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	g_registered_resources.RemoveAt(index);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSetTFRing(const volatile void* base, uint32_t size)
{
	PRINT_NAME();
	g_tf_ring_base.store(reinterpret_cast<uint64_t>(base), std::memory_order_release);
	g_tf_ring_size.store(size, std::memory_order_release);
			KYTY_LOG_DEBUG("\t base = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(base));
			KYTY_LOG_DEBUG("\t size = 0x%08" PRIx32 "\n", size);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSetHsOffchipParam(uint64_t value0, uint64_t value1, uint64_t value2)
{
	PRINT_NAME();
	g_hs_offchip_value0.store(value0, std::memory_order_release);
	g_hs_offchip_value1.store(value1, std::memory_order_release);
	g_hs_offchip_value2.store(value2, std::memory_order_release);
			KYTY_LOG_DEBUG("\t value0 = 0x%016" PRIx64 "\n", value0);
			KYTY_LOG_DEBUG("\t value1 = 0x%016" PRIx64 "\n", value1);
			KYTY_LOG_DEBUG("\t value2 = 0x%016" PRIx64 "\n", value2);
	return OK;
}

static int SubmitDcbBuffer(uint32_t* address, uint32_t size_in_dwords)
{
	if (size_in_dwords == 0)
	{
		return OK;
	}
	if (address == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	GraphicsDbgDumpDcb("d", size_in_dwords, address);
	GraphicsRunSubmit(address, size_in_dwords, nullptr, 0, GraphicsSubmissionCompletion::QueuedGraphicsInterrupt);
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitDcb(const Packet* packet)
{
	PRINT_NAME();

	if (packet == nullptr || packet->pad[0] != 0)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

			KYTY_LOG_DEBUG("\t addr   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet->addr));
			KYTY_LOG_DEBUG("\t dw_num = 0x%08" PRIx32 "\n", packet->dw_num);

	return SubmitDcbBuffer(packet->addr, packet->dw_num);
}

int KYTY_SYSV_ABI GraphicsDriverSubmitMultiDcbs(uint32_t* const* dcb_gpu_addrs, const uint32_t* dcb_sizes_in_dwords,
                                                uint32_t count)
{
	PRINT_NAME();

	if (count == 0)
	{
		return OK;
	}
	if (dcb_gpu_addrs == nullptr || dcb_sizes_in_dwords == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	for (uint32_t i = 0; i < count; i++)
	{
		const int result = SubmitDcbBuffer(dcb_gpu_addrs[i], dcb_sizes_in_dwords[i]);
		if (result != OK)
		{
			return result;
		}
	}
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverSubmitAcb(uint32_t queue, const Packet* packet)
{
	PRINT_NAME();
			KYTY_LOG_DEBUG("\t queue  = 0x%08" PRIx32 "\n", queue);
			KYTY_LOG_DEBUG("\t packet = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet));

	if (packet == nullptr)
	{
		return OK;
	}

			KYTY_LOG_DEBUG("\t acb    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(packet->addr));
			KYTY_LOG_DEBUG("\t dw_num = 0x%08" PRIx32 "\n", packet->dw_num);

	// Queue-indexed compute submit is not fully modeled yet. Execute the ACB
	// through the existing command processor so WaitRegMem/ReleaseMem packets
	// still complete guest labels rather than stalling on an empty GPU path.
	(void)queue;
	if (packet->addr != nullptr && packet->dw_num != 0)
	{
		GraphicsDbgDumpDcb("a", packet->dw_num, packet->addr);
		GraphicsRunSubmit(packet->addr, packet->dw_num, nullptr, 0, GraphicsSubmissionCompletion::None);
	}
	return OK;
}

int KYTY_SYSV_ABI GraphicsDriverAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, int id, void* udata)
{
	PRINT_NAME();
	if (eq == nullptr)
	{
		return LibKernel::KERNEL_ERROR_EBADF;
	}
	return GraphicsRenderAddEqEvent(eq, id, udata);
}

} // namespace Gen5Driver

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
