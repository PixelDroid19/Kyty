#include "Emulator/Audio3d.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Audio.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Kernel/Semaphore.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = Kernel::Semaphore;

constexpr int AUDIO3D_ERROR_INVALID_PARAMETER = static_cast<int32_t>(0x80ea0004u);
constexpr int AUDIO3D_ERROR_INVALID_PORT      = static_cast<int32_t>(0x80ea0002u);
constexpr int AUDIO3D_ERROR_OUT_OF_RESOURCES  = static_cast<int32_t>(0x80ea0006u);

struct Audio3dData
{
	enum class State
	{
		Empty,
		Ready,
		Play
	};

	std::atomic<State> state = State::Empty;
};

struct Audio3dInternal
{
	Audio3dData*          data                        = nullptr;
	Core::Mutex*          data_mutex                  = nullptr;
	uint64_t              data_delay                  = 0;
	Semaphore::KernelSema playback_sema               = nullptr;
	Audio3dOpenParameters params                      = {};
	int                   user_id                     = 0;
	float                 late_reverb_level           = 0.0f;
	float                 downmix_spread_radius       = 2.0f;
	int                   downmix_spread_height_aware = 0;
	uint32_t              data_index                  = 0;
	std::atomic_bool      used                        = false;
	std::atomic_bool      playback_finished           = false;
	int                   audio_out_handles[32]       = {};
	size_t                audio_out_buffer_bytes[32]   = {};
	uint32_t              audio_out_handle_count      = 0;
};

constexpr uint32_t MAX_PORTS = 4;
constexpr size_t   MAX_QUEUE_ALLOCATION_BYTES = 8u * 1024u * 1024u;
// Bound the owned copy made before passing guest audio to the host backend.
constexpr size_t   MAX_OUTPUT_SNAPSHOT_BYTES  = 8u * 1024u * 1024u;

static Audio3dInternal g_ports[MAX_PORTS] = {};
static std::mutex      g_ports_mutex;

static bool IsGuestRangeReadable(const void* pointer, size_t size)
{
	return pointer != nullptr && size != 0 &&
	       Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(pointer), static_cast<uint64_t>(size));
}

static bool IsGuestRangeWritable(const void* pointer, size_t size)
{
	return pointer != nullptr && size != 0 &&
	       Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(pointer), static_cast<uint64_t>(size));
}

static bool CopyFromGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyFromGuest(destination, reinterpret_cast<uint64_t>(source), static_cast<uint64_t>(size));
}

static bool CopyToGuest(void* destination, const void* source, size_t size)
{
	return destination != nullptr && source != nullptr && size != 0 &&
	       Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(destination), source, static_cast<uint64_t>(size));
}

static bool IsValidPort(uint32_t port_id)
{
	if (port_id >= MAX_PORTS)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d invalid port id %u\n", port_id);
		return false;
	}
	std::lock_guard lock(g_ports_mutex);
	if (!g_ports[port_id].used.load(std::memory_order_acquire))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d invalid port id %u\n", port_id);
		return false;
	}
	return true;
}

static bool AudioOutBufferSize(uint32_t parameter, uint32_t samples, size_t* size_out)
{
	if (size_out == nullptr)
	{
		return false;
	}

	uint32_t channels = 0;
	uint32_t sample_bytes = 0;
	switch (parameter & 0xffu)
	{
		case 0: channels = 1; sample_bytes = 2; break;
		case 1: channels = 2; sample_bytes = 2; break;
		case 2: channels = 8; sample_bytes = 2; break;
		case 3: channels = 1; sample_bytes = 4; break;
		case 4: channels = 2; sample_bytes = 4; break;
		case 5: channels = 8; sample_bytes = 4; break;
		case 6: channels = 8; sample_bytes = 2; break;
		case 7: channels = 8; sample_bytes = 4; break;
		default: return false;
	}

	const uint64_t bytes = static_cast<uint64_t>(samples) * channels * sample_bytes;
	if constexpr (sizeof(size_t) < sizeof(uint64_t))
	{
		if (bytes > std::numeric_limits<size_t>::max())
		{
			return false;
		}
	}
	*size_out = static_cast<size_t>(bytes);
	return true;
}

static void playback_simulate(void* arg)
{
	auto* port = static_cast<Audio3dInternal*>(arg);
	EXIT_IF(port == nullptr);
	EXIT_IF(port->data_mutex == nullptr);
	EXIT_IF(port->data == nullptr);

	for (;;)
	{
		int result = Semaphore::KernelWaitSema(port->playback_sema, 1, nullptr);

		if (result != OK)
		{
			break;
		}

		Audio3dData* play_data = nullptr;

		port->data_mutex->Lock();
		{
			for (uint32_t i = 0; i < port->params.queue_depth; i++)
			{
				uint32_t index = (port->data_index + i) % port->params.queue_depth;

				if (port->data[index].state == Audio3dData::State::Play)
				{
					play_data = &port->data[index];
					break;
				}
			}
		}
		port->data_mutex->Unlock();

		EXIT_IF(play_data == nullptr);

		if (play_data != nullptr)
		{
			// TODO(): Audio output is not yet implemented, so simulate audio delay
			Core::Thread::SleepMicro(port->data_delay);
			play_data->state = Audio3dData::State::Empty;
		}
	}

	port->playback_finished = true;
}

int KYTY_SYSV_ABI Audio3dInitialize(int64_t reserved)
{
	PRINT_NAME();

	if (reserved != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p)
{
	PRINT_NAME();

	const Audio3dOpenParameters defaults {};
	if (!CopyToGuest(p, &defaults, defaults.size))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d default-parameter output is not writable guest memory\n");
		return;
	}
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id)
{
	PRINT_NAME();

	if (!IsGuestRangeWritable(id, sizeof(*id)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d port id output is not writable guest memory\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	size_t parameters_size = 0;
	if (!CopyFromGuest(&parameters_size, parameters, sizeof(parameters_size)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d open parameters are not readable guest memory\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	size_t parameters_copy_size = 0;
	switch (parameters_size & ~size_t {7})
	{
		case 0x10: parameters_copy_size = 0x10; break;
		case 0x18: parameters_copy_size = 0x18; break;
		case 0x20: parameters_copy_size = 0x20; break;
		case 0x28: parameters_copy_size = 0x28; break;
		default: return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	Audio3dOpenParameters requested {};
	if (!CopyFromGuest(&requested, parameters, parameters_copy_size) ||
	    (requested.size & ~size_t {7}) != (parameters_size & ~size_t {7}))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d open parameters cross an invalid guest range\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	Audio3dOpenParameters effective {};
	effective.size = sizeof(effective);
	switch (requested.size & ~size_t {7})
	{
		case 0x10:
			effective.granularity = requested.granularity;
			effective.rate        = requested.rate;
			effective.buffer_mode = 0;
			break;
		case 0x18:
			effective.granularity = requested.granularity;
			effective.rate        = requested.rate;
			effective.max_objects = requested.max_objects;
			effective.queue_depth = requested.queue_depth;
			effective.buffer_mode = 1;
			break;
		case 0x20:
			effective.granularity = requested.granularity;
			effective.rate        = requested.rate;
			effective.max_objects = requested.max_objects;
			effective.queue_depth = requested.queue_depth;
			effective.buffer_mode = requested.buffer_mode;
			effective.pad         = requested.pad;
			break;
		case 0x28: effective = requested; break;
		default: return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	KYTY_LOG_DEBUG("\t user_id     = %d\n", user_id);
	KYTY_LOG_DEBUG("\t granularity = %u\n", effective.granularity);
	KYTY_LOG_DEBUG("\t rate        = %u\n", effective.rate);
	KYTY_LOG_DEBUG("\t max_objects = %u\n", effective.max_objects);
	KYTY_LOG_DEBUG("\t queue_depth = %u\n", effective.queue_depth);
	KYTY_LOG_DEBUG("\t buffer_mode = %u\n", effective.buffer_mode);
	KYTY_LOG_DEBUG("\t num_beds    = %u\n", effective.num_beds);

	if ((user_id != 255 && user_id != 1) || effective.rate != 0 || effective.granularity < 0x100 || (effective.granularity & 0xffu) != 0 ||
	    effective.max_objects == 0 || effective.queue_depth == 0 || effective.queue_depth > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
	    static_cast<size_t>(effective.queue_depth) > std::numeric_limits<size_t>::max() / sizeof(Audio3dData) ||
	    effective.buffer_mode > 2)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	const size_t queue_allocation_bytes = static_cast<size_t>(effective.queue_depth) * sizeof(Audio3dData);
	if (queue_allocation_bytes > MAX_QUEUE_ALLOCATION_BYTES)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d queue allocation exceeds the host resource budget\n");
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	std::lock_guard port_lock(g_ports_mutex);
	uint32_t port = 0;
	for (; port < MAX_PORTS; port++)
	{
		if (!g_ports[port].used.load(std::memory_order_acquire))
		{
			break;
		}
	}

	if (port >= MAX_PORTS)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	std::unique_ptr<Audio3dData[]> data(new (std::nothrow) Audio3dData[effective.queue_depth]);
	std::unique_ptr<Core::Mutex>   data_mutex(new (std::nothrow) Core::Mutex);
	if (data == nullptr || data_mutex == nullptr)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	Semaphore::KernelSema playback_sema = nullptr;
	const int             sema_result = Semaphore::KernelCreateSema(&playback_sema, "audio3d_play", 0x01, 0,
	                                                                 static_cast<int>(effective.queue_depth), nullptr);
	if (sema_result != OK)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d playback semaphore creation failed (%d)\n", sema_result);
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	const uint32_t port_id = port;
	if (!CopyToGuest(id, &port_id, sizeof(port_id)))
	{
		const int cleanup_result = Semaphore::KernelDeleteSema(playback_sema);
		if (cleanup_result != OK)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d failed to delete unpublished playback semaphore (%d)\n", cleanup_result);
		}
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	auto& opened_port             = g_ports[port];
	opened_port.data              = data.release();
	opened_port.data_index        = 0;
	opened_port.data_mutex        = data_mutex.release();
	opened_port.data_delay        = (1000000 * static_cast<uint64_t>(effective.granularity)) / 48000;
	opened_port.playback_sema     = playback_sema;
	opened_port.user_id           = user_id;
	opened_port.params            = effective;
	opened_port.playback_finished = false;
	opened_port.used.store(true, std::memory_order_release);
	Core::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size)
{
	PRINT_NAME();

	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (attribute_size != sizeof(uint32_t))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d attribute pointer or size is invalid\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	uint32_t attribute_value = 0;
	if (!CopyFromGuest(&attribute_value, attribute, sizeof(attribute_value)))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d attribute is not readable guest memory\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	KYTY_LOG_DEBUG("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id)
	{
		case 0x10001:
			std::memcpy(&g_ports[port_id].late_reverb_level, &attribute_value, sizeof(float));
			KYTY_LOG_DEBUG("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			std::memcpy(&g_ports[port_id].downmix_spread_radius, &attribute_value, sizeof(float));
			KYTY_LOG_DEBUG("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			std::memcpy(&g_ports[port_id].downmix_spread_height_aware, &attribute_value, sizeof(int));
			KYTY_LOG_DEBUG("\t downmix_spread_height_aware = %d\n", g_ports[port_id].downmix_spread_height_aware);
			break;
		default:
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d attribute id 0x%" PRIx32 " is unsupported\n", attribute_id);
			return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available)
{
	PRINT_NAME();

	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (queue_level == nullptr && queue_available == nullptr)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d queue outputs are null\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if ((queue_level != nullptr && !IsGuestRangeWritable(queue_level, sizeof(*queue_level))) ||
	    (queue_available != nullptr && !IsGuestRangeWritable(queue_available, sizeof(*queue_available))))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d queue output is not writable guest memory\n");
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	auto* port = &g_ports[port_id];

	uint32_t empty_num = 0;

	port->data_mutex->Lock();
	{
		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Empty)
			{
				empty_num++;
			} else
			{
				break;
			}
		}
	}
	port->data_mutex->Unlock();

	EXIT_IF(empty_num > port->params.queue_depth);

	KYTY_LOG_DEBUG("\t queue_available = %u\n", empty_num);

	if (queue_level != nullptr)
	{
		const uint32_t level = port->params.queue_depth - empty_num;
		if (!CopyToGuest(queue_level, &level, sizeof(level)))
		{
			return AUDIO3D_ERROR_INVALID_PARAMETER;
		}
	}
	if (queue_available != nullptr)
	{
		if (!CopyToGuest(queue_available, &empty_num, sizeof(empty_num)))
		{
			return AUDIO3D_ERROR_INVALID_PARAMETER;
		}
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetAttributesSupported(uint32_t port_id, uint32_t* capabilities, uint32_t* num_capabilities)
{
	PRINT_NAME();

	if (num_capabilities == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	static constexpr uint32_t supported[] = {1u, 3u, 9u};
	if (capabilities == nullptr)
	{
		const uint32_t count = std::size(supported);
		return CopyToGuest(num_capabilities, &count, sizeof(count)) ? OK : AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	if (!IsGuestRangeWritable(num_capabilities, sizeof(*num_capabilities)))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	uint32_t requested_count = 0;
	if (!CopyFromGuest(&requested_count, num_capabilities, sizeof(requested_count)))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	const uint32_t count = std::min(requested_count, static_cast<uint32_t>(std::size(supported)));
	if (count != 0 && !IsGuestRangeWritable(capabilities, static_cast<size_t>(count) * sizeof(*capabilities)))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if (count != 0 && !CopyToGuest(capabilities, supported, static_cast<size_t>(count) * sizeof(*capabilities)))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if (!CopyToGuest(num_capabilities, &count, sizeof(count)))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	return OK;
}

int KYTY_SYSV_ABI Audio3dAudioOutOpen(uint32_t port_id, int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param)
{
	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	std::lock_guard lock(g_ports_mutex);
	auto&           port = g_ports[port_id];
	if (!port.used.load(std::memory_order_acquire))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (len != port.params.granularity || port.audio_out_handle_count >= std::size(port.audio_out_handles))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	const int handle = AudioOut::AudioOutOpen(user_id, type, index, len, freq, param & 0xffu);
	if (handle < 0)
	{
		return handle;
	}
	size_t buffer_bytes = 0;
	if (!AudioOutBufferSize(param, len, &buffer_bytes))
	{
		(void)AudioOut::AudioOutClose(handle);
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}
	port.audio_out_handles[port.audio_out_handle_count++]        = handle;
	port.audio_out_buffer_bytes[port.audio_out_handle_count - 1] = buffer_bytes;
	return handle;
}

int KYTY_SYSV_ABI Audio3dAudioOutOutput(int handle, const void* data)
{
	if (data == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	size_t buffer_bytes = 0;
	{
		std::lock_guard lock(g_ports_mutex);
		for (const auto& port: g_ports)
		{
			if (!port.used.load(std::memory_order_acquire))
			{
				continue;
			}
			for (uint32_t index = 0; index < port.audio_out_handle_count; ++index)
			{
				if (port.audio_out_handles[index] == handle)
				{
					buffer_bytes = port.audio_out_buffer_bytes[index];
					break;
				}
			}
			if (buffer_bytes != 0)
			{
				break;
			}
		}
	}
	if (buffer_bytes == 0)
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	if (buffer_bytes > MAX_OUTPUT_SNAPSHOT_BYTES)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}
	std::unique_ptr<uint8_t, decltype(&std::free)> snapshot(static_cast<uint8_t*>(std::malloc(buffer_bytes)), &std::free);
	if (snapshot == nullptr)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}
	if (!CopyFromGuest(snapshot.get(), data, buffer_bytes))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	return AudioOut::AudioOutOutput(handle, snapshot.get());
}

int KYTY_SYSV_ABI Audio3dAudioOutClose(int handle)
{
	std::lock_guard lock(g_ports_mutex);
	for (auto& port: g_ports)
	{
		if (!port.used.load(std::memory_order_acquire))
		{
			continue;
		}
		for (uint32_t index = 0; index < port.audio_out_handle_count; ++index)
		{
			if (port.audio_out_handles[index] != handle)
			{
				continue;
			}

			const int result = AudioOut::AudioOutClose(handle);
			if (result != OK)
			{
				return result;
			}

			std::move(port.audio_out_handles + index + 1, port.audio_out_handles + port.audio_out_handle_count,
			          port.audio_out_handles + index);
			std::move(port.audio_out_buffer_bytes + index + 1, port.audio_out_buffer_bytes + port.audio_out_handle_count,
			          port.audio_out_buffer_bytes + index);
			port.audio_out_handles[--port.audio_out_handle_count] = 0;
			port.audio_out_buffer_bytes[port.audio_out_handle_count] = 0;
			return OK;
		}
	}
	return AUDIO3D_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id)
{
	PRINT_NAME();

	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty)
		{
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		if (port->data[current_index].state != Audio3dData::State::Ready) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

		port->data_index = next_index;

		KYTY_LOG_DEBUG("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking)
{
	PRINT_NAME();

	if (!IsValidPort(port_id))
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	auto* port = &g_ports[port_id];

	if (blocking != 1)
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: Audio3d blocking=%u treated as 1 (continuing)\n", blocking);
	}

	KYTY_LOG_DEBUG("\t blocking = %u\n", blocking);

	int          data_num   = 0;
	Audio3dData* first_data = nullptr;

	port->data_mutex->Lock();
	{
		first_data = port->data + port->data_index;

		for (uint32_t i = 0; i < port->params.queue_depth; i++)
		{
			uint32_t index = (port->data_index + i) % port->params.queue_depth;

			if (port->data[index].state == Audio3dData::State::Ready)
			{
				port->data[index].state = Audio3dData::State::Play;
				data_num++;
			}
		}
	}
	port->data_mutex->Unlock();

	KYTY_LOG_DEBUG("\t push num = %d\n", data_num);

	if (data_num > 0)
	{
		Semaphore::KernelSignalSema(port->playback_sema, data_num);

		if (blocking == 1)
		{
			auto wait_time = port->data_delay / 8;
			while (first_data->state != Audio3dData::State::Empty)
			{
				Kernel::PthreadInterruptibleScope any_syscall;
				Core::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

} // namespace Audio3d

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
