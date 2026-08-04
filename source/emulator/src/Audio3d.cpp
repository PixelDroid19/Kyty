#include "Emulator/Audio.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

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
#include <cstring>
#include <iterator>
#include <utility>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

namespace Audio3d {

LIB_NAME("Audio3d", "Audio3d");

namespace Semaphore = LibKernel::Semaphore;

struct Audio3dOpenParameters
{
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
	uint32_t num_beds    = 2;
};

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
	bool                  used                        = false;
	std::atomic_bool      playback_finished           = false;
	int                   audio_out_handles[32]       = {};
	uint32_t              audio_out_handle_count      = 0;
};

constexpr uint32_t MAX_PORTS = 4;

static Audio3dInternal g_ports[MAX_PORTS] = {};

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

	EXIT_NOT_IMPLEMENTED(reserved != 0);

	return OK;
}

void KYTY_SYSV_ABI Audio3dGetDefaultOpenParameters(Audio3dOpenParameters* p)
{
	PRINT_NAME();

	if (p == nullptr)
	{
		return;
	}

	const Audio3dOpenParameters defaults {};
	std::memcpy(p, &defaults, defaults.size);
}

int KYTY_SYSV_ABI Audio3dPortOpen(int user_id, const Audio3dOpenParameters* parameters, uint32_t* id)
{
	PRINT_NAME();

	if (parameters == nullptr || id == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	*id = UINT32_MAX;

	Audio3dOpenParameters effective {};
	effective.size = sizeof(effective);
	switch (parameters->size & ~size_t {7})
	{
		case 0x10:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.buffer_mode = 0;
			break;
		case 0x18:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.max_objects = parameters->max_objects;
			effective.queue_depth = parameters->queue_depth;
			effective.buffer_mode = 1;
			break;
		case 0x20:
			effective.granularity = parameters->granularity;
			effective.rate        = parameters->rate;
			effective.max_objects = parameters->max_objects;
			effective.queue_depth = parameters->queue_depth;
			effective.buffer_mode = parameters->buffer_mode;
			effective.pad         = parameters->pad;
			break;
		case 0x28: effective = *parameters; break;
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
	    effective.max_objects == 0 || effective.queue_depth == 0 || effective.buffer_mode > 2)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	uint32_t port = 0;
	for (; port < MAX_PORTS; port++)
	{
		if (!g_ports[port].used)
		{
			break;
		}
	}

	if (port >= MAX_PORTS)
	{
		return AUDIO3D_ERROR_OUT_OF_RESOURCES;
	}

	g_ports[port].user_id = user_id;
	g_ports[port].params  = effective;
	g_ports[port].used    = true;

	EXIT_IF(g_ports[port].data != nullptr);
	EXIT_IF(g_ports[port].data_mutex != nullptr);
	EXIT_IF(g_ports[port].playback_sema != nullptr);

	g_ports[port].data       = new Audio3dData[effective.queue_depth];
	g_ports[port].data_index = 0;
	g_ports[port].data_mutex = new Core::Mutex;
	g_ports[port].data_delay = (1000000 * static_cast<uint64_t>(effective.granularity)) / 48000;

	for (uint32_t d = 0; d < effective.queue_depth; d++)
	{
		g_ports[port].data[d].state = Audio3dData::State::Empty;
	}

	int result = Semaphore::KernelCreateSema(&g_ports[port].playback_sema, "audio3d_play", 0x01, 0, static_cast<int>(effective.queue_depth),
	                                         nullptr);
	EXIT_NOT_IMPLEMENTED(result != OK);

	g_ports[port].playback_finished = false;
	Core::Thread playback_thread(playback_simulate, &g_ports[port]);
	playback_thread.Detach();

	*id = port;

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortSetAttribute(uint32_t port_id, uint32_t attribute_id, const void* attribute, size_t attribute_size)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(attribute == nullptr);

	KYTY_LOG_DEBUG("\t attribute_id = 0x%" PRIx32 "\n", attribute_id);

	switch (attribute_id)
	{
		case 0x10001:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].late_reverb_level = *static_cast<const float*>(attribute);
			KYTY_LOG_DEBUG("\t late_reverb_level = %f\n", g_ports[port_id].late_reverb_level);
			break;
		case 0x10002:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_radius = *static_cast<const float*>(attribute);
			KYTY_LOG_DEBUG("\t downmix_spread_radius = %f\n", g_ports[port_id].downmix_spread_radius);
			break;
		case 0x10003:
			EXIT_NOT_IMPLEMENTED(attribute_size != 4);
			g_ports[port_id].downmix_spread_height_aware = *static_cast<const int*>(attribute);
			KYTY_LOG_DEBUG("\t downmix_spread_height_aware = %d\n", g_ports[port_id].downmix_spread_height_aware);
			break;
		default: EXIT("unknown attribute: 0x%" PRIx32 "\n", attribute_id);
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetQueueLevel(uint32_t port_id, uint32_t* queue_level, uint32_t* queue_available)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);
	EXIT_NOT_IMPLEMENTED(queue_level == nullptr && queue_available == nullptr);

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
		*queue_level = port->params.queue_depth - empty_num;
	}
	if (queue_available != nullptr)
	{
		*queue_available = empty_num;
	}

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortGetAttributesSupported(uint32_t port_id, uint32_t* capabilities, uint32_t* num_capabilities)
{
	if (num_capabilities == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	if (port_id >= MAX_PORTS || !g_ports[port_id].used)
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}

	static constexpr uint32_t supported[] = {1u, 3u, 9u};
	if (capabilities == nullptr)
	{
		*num_capabilities = std::size(supported);
		return OK;
	}

	const uint32_t count = std::min(*num_capabilities, static_cast<uint32_t>(std::size(supported)));
	std::copy_n(supported, count, capabilities);
	*num_capabilities = count;
	return OK;
}

int KYTY_SYSV_ABI Audio3dAudioOutOpen(uint32_t port_id, int user_id, int type, int index, uint32_t len, uint32_t freq, uint32_t param)
{
	if (port_id >= MAX_PORTS || !g_ports[port_id].used)
	{
		return AUDIO3D_ERROR_INVALID_PORT;
	}
	auto& port = g_ports[port_id];
	if (len != port.params.granularity || port.audio_out_handle_count >= std::size(port.audio_out_handles))
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}

	const int handle = AudioOut::AudioOutOpen(user_id, type, index, len, freq, param & 0xffu);
	if (handle < 0)
	{
		return handle;
	}
	port.audio_out_handles[port.audio_out_handle_count++] = handle;
	return handle;
}

int KYTY_SYSV_ABI Audio3dAudioOutOutput(int handle, const void* data)
{
	if (data == nullptr)
	{
		return AUDIO3D_ERROR_INVALID_PARAMETER;
	}
	for (const auto& port: g_ports)
	{
		if (!port.used)
		{
			continue;
		}
		for (uint32_t index = 0; index < port.audio_out_handle_count; ++index)
		{
			if (port.audio_out_handles[index] == handle)
			{
				return AudioOut::AudioOutOutput(handle, data);
			}
		}
	}
	return AUDIO3D_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI Audio3dAudioOutClose(int handle)
{
	for (auto& port: g_ports)
	{
		if (!port.used)
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
			port.audio_out_handles[--port.audio_out_handle_count] = 0;
			return OK;
		}
	}
	return AUDIO3D_ERROR_INVALID_PORT;
}

int KYTY_SYSV_ABI Audio3dPortAdvance(uint32_t port_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	port->data_mutex->Lock();
	{
		uint32_t current_index = port->data_index;
		uint32_t next_index    = (current_index + 1) % port->params.queue_depth;

		if (port->data[current_index].state == Audio3dData::State::Empty)
		{
			port->data[current_index].state = Audio3dData::State::Ready;
		}

		EXIT_NOT_IMPLEMENTED(port->data[current_index].state != Audio3dData::State::Ready);

		port->data_index = next_index;

		KYTY_LOG_DEBUG("\t %u -> %u\n", current_index, next_index);
	}
	port->data_mutex->Unlock();

	return OK;
}

int KYTY_SYSV_ABI Audio3dPortPush(uint32_t port_id, uint32_t blocking)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(port_id >= MAX_PORTS);
	EXIT_NOT_IMPLEMENTED(!g_ports[port_id].used);

	auto* port = &g_ports[port_id];

	EXIT_NOT_IMPLEMENTED(blocking != 1);

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
				Core::Thread::SleepMicro(wait_time);
			}
		}
	}

	return OK;
}

} // namespace Audio3d

} // namespace Kyty::Libs::Audio

#endif // KYTY_EMU_ENABLED
