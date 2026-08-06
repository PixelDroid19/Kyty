#include "GpuMemoryInternal.h"

#include "Kyty/Core/Database.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"
#include "Emulator/Graphics/GpuDirtyPageTracker.h"
#include "Emulator/Graphics/GpuMemoryMaterializationCache.h"
#include "Emulator/Graphics/GpuMemoryRangeQueryCache.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/DepthMeta.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vulkan/vk_enum_string_helper.h>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

uint32_t GpuResources::AddOwner(const String& name)
{
	Core::LockGuard lock(m_mutex);

	Owner n;
	n.name = name;
	n.free = false;

	uint32_t index = 0;
	for (auto& b: m_owners)
	{
		if (b.free)
		{
			b = n;
			return index;
		}
		index++;
	}

	m_owners.Add(n);

	return index;
}

uint32_t GpuResources::AddResource(uint32_t owner_handle, uint64_t memory, size_t size, const String& name, uint32_t type,
                                   uint64_t user_data)
{
	Core::LockGuard lock(m_mutex);

	// Owner handle zero is a valid anonymous-resource scope. Some graphics
	// runtimes register bootstrap allocations before establishing an owner.
	if (owner_handle != 0 && !m_owners.IndexValid(owner_handle)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: owner_handle != 0 && !m_owners.IndexValid(owner_handle) condition ignored (continuing)\n"); }
	if (memory == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: memory == 0 condition ignored (continuing)\n"); }

	Info info;
	info.owner     = owner_handle;
	info.memory    = memory;
	info.free      = false;
	info.name      = name;
	info.size      = size;
	info.type      = type;
	info.user_data = user_data;

	uint32_t index = 0;
	for (auto& i: m_infos)
	{
		if (i.free)
		{
			i = info;
			return index;
		}
		index++;
	}

	m_infos.Add(info);

	return index;
}

void GpuResources::DeleteOwner(uint32_t owner_handle)
{
	Core::LockGuard lock(m_mutex);

	if (owner_handle == 0 && !m_owners.IndexValid(owner_handle))
	{
		for (auto& i: m_infos)
		{
			if (!i.free && i.owner == owner_handle)
			{
				i.free = true;
			}
		}
		return;
	}

	if (!m_owners.IndexValid(owner_handle)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_owners.IndexValid(owner_handle) condition ignored (continuing)\n"); }

	for (auto& i: m_infos)
	{
		if (!i.free && i.owner == owner_handle)
		{
			i.free = true;
		}
	}

	if (m_owners[owner_handle].free) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_owners[owner_handle].free condition ignored (continuing)\n"); }

	m_owners[owner_handle].free = true;
}

void GpuResources::DeleteResources(uint32_t owner_handle)
{
	Core::LockGuard lock(m_mutex);

	if (owner_handle != 0 && !m_owners.IndexValid(owner_handle)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: owner_handle != 0 && !m_owners.IndexValid(owner_handle) condition ignored (continuing)\n"); }

	for (auto& i: m_infos)
	{
		if (!i.free && i.owner == owner_handle)
		{
			i.free = true;
		}
	}
}

void GpuResources::DeleteResource(uint32_t resource_handle)
{
	Core::LockGuard lock(m_mutex);

	if (!m_infos.IndexValid(resource_handle)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: !m_infos.IndexValid(resource_handle) condition ignored (continuing)\n"); }

	if (m_infos[resource_handle].free) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: m_infos[resource_handle].free condition ignored (continuing)\n"); }

	m_infos[resource_handle].free = true;
}

bool GpuResources::FindInfo(uint64_t memory, Info* dst)
{
	EXIT_IF(dst == nullptr);

	Core::LockGuard lock(m_mutex);

	// NOLINTNEXTLINE(readability-use-anyofallof)
	for (const auto& i: m_infos)
	{
		if (!i.free && memory >= i.memory && memory < i.memory + i.size)
		{
			*dst = i;
			return true;
		}
	}

	return false;
}

void GpuMemoryRegisterOwner(uint32_t* owner_handle, const char* name)
{
	EXIT_IF(g_gpu_resources == nullptr);
	EXIT_IF(owner_handle == nullptr);
	EXIT_IF(name == nullptr);

	*owner_handle = g_gpu_resources->AddOwner(String::FromUtf8(name));
}

void GpuMemoryRegisterResource(uint32_t* resource_handle, uint32_t owner_handle, const void* memory, size_t size, const char* name,
                               uint32_t type, uint64_t user_data)
{
	EXIT_IF(g_gpu_resources == nullptr);
	EXIT_IF(resource_handle == nullptr);
	EXIT_IF(name == nullptr);

	*resource_handle =
	    g_gpu_resources->AddResource(owner_handle, reinterpret_cast<uint64_t>(memory), size, String::FromUtf8(name), type, user_data);
}

void GpuMemoryUnregisterAllResourcesForOwner(uint32_t owner_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteResources(owner_handle);
}

void GpuMemoryUnregisterOwnerAndResources(uint32_t owner_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteOwner(owner_handle);
}

void GpuMemoryUnregisterResource(uint32_t resource_handle)
{
	EXIT_IF(g_gpu_resources == nullptr);

	g_gpu_resources->DeleteResource(resource_handle);
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED