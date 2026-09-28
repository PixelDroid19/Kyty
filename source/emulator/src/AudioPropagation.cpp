#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Audio.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio::AudioPropagation {

LIB_NAME("AudioPropagation", "AudioPropagation");

namespace {

constexpr uint32_t kSystemOptionId = 0x010107d5;
constexpr uint32_t kSystemMemoryId = 0x010107d4;
constexpr size_t   kMaxSystems     = 32;
constexpr size_t   kMaxRooms       = 4096;

struct SystemWorkspace
{
	SystemOption option;
	uint64_t     handle = 0;
};

struct SystemRecord
{
	uint64_t     handle = 0;
	SystemOption option;
};

struct RoomRecord
{
	uint64_t handle = 0;
	uint64_t system = 0;
};

static_assert(sizeof(StructDescriptor) == 0x10);
static_assert(sizeof(SystemOption) == 0x38);
static_assert(sizeof(SystemMemory) == 0x30);
static_assert(offsetof(SystemMemory, cpu_memory_size) == 0x18);
static_assert(sizeof(SystemWorkspace) == 0x40);

std::mutex                            g_system_mutex;
std::array<SystemRecord, kMaxSystems> g_systems {};
std::array<RoomRecord, kMaxRooms>     g_rooms {};
uint64_t                              g_next_room_handle = 1;

bool ReadGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyFromGuest(destination, reinterpret_cast<uint64_t>(source), size);
}

bool WriteGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(destination), source, size);
}

bool WritableGuest(const void* pointer, size_t size)
{
	return pointer != nullptr && size != 0 && Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(pointer), size);
}

bool ReadOption(const SystemOption* guest_option, SystemOption* option)
{
	if (!ReadGuest(option, guest_option, sizeof(*option)))
	{
		return false;
	}
	if (option->desc.id != kSystemOptionId || option->desc.size != sizeof(*option) || option->desc.pad != 0 || option->pad != 0 ||
	    option->max_sources == 0 || option->max_materials == 0 || option->max_raycasts == 0 || !std::isfinite(option->speed_of_sound) ||
	    option->speed_of_sound <= 0 || !std::isfinite(option->max_distance) || option->max_distance <= 0)
	{
		return false;
	}
	if (option->flags != 0)
	{
		EXIT("unsupported audio propagation option flags: 0x%08x\n", option->flags);
	}
	return true;
}

bool ReadMemory(const SystemMemory* guest_memory, SystemMemory* memory)
{
	return ReadGuest(memory, guest_memory, sizeof(*memory)) && memory->desc.id == kSystemMemoryId && memory->desc.size == sizeof(*memory) &&
	       memory->desc.pad == 0;
}

} // namespace

int KYTY_SYSV_ABI SystemQueryMemory(const SystemOption* option, SystemMemory* memory)
{
	PRINT_NAME();
	SystemOption option_value {};
	SystemMemory memory_value {};
	if (!ReadOption(option, &option_value) || !ReadMemory(memory, &memory_value))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	// This CPU-only backend keeps its system identity and options in the
	// caller-owned work memory. No GPU work memory is needed for this mode.
	memory_value.cpu_memory_size = sizeof(SystemWorkspace);
	memory_value.gpu_memory_size = 0;
	return WriteGuest(memory, &memory_value, sizeof(memory_value)) ? 0 : LibKernel::KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI SystemCreate(const SystemOption* option, const SystemMemory* memory, uint64_t* system_out)
{
	PRINT_NAME();
	SystemOption option_value {};
	SystemMemory memory_value {};
	if (!ReadOption(option, &option_value) || !ReadMemory(memory, &memory_value) ||
	    memory_value.cpu_memory_size < sizeof(SystemWorkspace) || (reinterpret_cast<uintptr_t>(memory_value.cpu_memory) & 0xfu) != 0 ||
	    !WritableGuest(memory_value.cpu_memory, sizeof(SystemWorkspace)) || !WritableGuest(system_out, sizeof(*system_out)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}

	const uint64_t  handle = reinterpret_cast<uint64_t>(memory_value.cpu_memory);
	std::lock_guard lock(g_system_mutex);
	SystemRecord*   free_record = nullptr;
	for (auto& record: g_systems)
	{
		if (record.handle == handle)
		{
			return LibKernel::KERNEL_ERROR_EINVAL;
		}
		if (record.handle == 0 && free_record == nullptr)
		{
			free_record = &record;
		}
	}
	if (free_record == nullptr)
	{
		return LibKernel::KERNEL_ERROR_ENOMEM;
	}
	const SystemWorkspace workspace {.option = option_value, .handle = handle};
	if (!WriteGuest(memory_value.cpu_memory, &workspace, sizeof(workspace)) || !WriteGuest(system_out, &handle, sizeof(handle)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	*free_record = SystemRecord {.handle = handle, .option = option_value};
	return 0;
}

int KYTY_SYSV_ABI SystemDestroy(uint64_t system)
{
	PRINT_NAME();
	std::lock_guard lock(g_system_mutex);
	for (auto& record: g_systems)
	{
		if (record.handle == system && system != 0)
		{
			record = {};
			for (auto& room: g_rooms)
			{
				if (room.system == system)
				{
					room = {};
				}
			}
			return 0;
		}
	}
	return LibKernel::KERNEL_ERROR_EINVAL;
}

int KYTY_SYSV_ABI RoomCreate(uint64_t system, uint64_t* room_out)
{
	PRINT_NAME();
	if (!WritableGuest(room_out, sizeof(*room_out)))
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	std::lock_guard lock(g_system_mutex);
	bool            live_system = false;
	for (const auto& record: g_systems)
	{
		live_system |= record.handle == system && system != 0;
	}
	if (!live_system)
	{
		return LibKernel::KERNEL_ERROR_EINVAL;
	}
	for (auto& room: g_rooms)
	{
		if (room.handle == 0)
		{
			if (g_next_room_handle == 0)
			{
				return LibKernel::KERNEL_ERROR_ENOMEM;
			}
			const uint64_t handle = g_next_room_handle++;
			if (!WriteGuest(room_out, &handle, sizeof(handle)))
			{
				return LibKernel::KERNEL_ERROR_EINVAL;
			}
			room = RoomRecord {.handle = handle, .system = system};
			return 0;
		}
	}
	return LibKernel::KERNEL_ERROR_ENOMEM;
}

int KYTY_SYSV_ABI RoomDestroy(uint64_t system, uint64_t room_handle)
{
	PRINT_NAME();
	std::lock_guard lock(g_system_mutex);
	for (auto& room: g_rooms)
	{
		if (room.handle == room_handle && room_handle != 0 && room.system == system)
		{
			room = {};
			return 0;
		}
	}
	return LibKernel::KERNEL_ERROR_EINVAL;
}

} // namespace Kyty::Libs::Audio::AudioPropagation

#endif // KYTY_EMU_ENABLED
