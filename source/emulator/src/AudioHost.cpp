#include "Emulator/AudioHost.h"

#include "Kyty/Core/DbgAssert.h"

#include "Emulator/AudioPcm.h"
#include "Emulator/Host/Clock.h"

#include "SDL.h"

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

namespace {

int InitializeSdlAudio()
{
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (SDL_getenv("SDL_AUDIODRIVER") == nullptr)
	{
		for (int i = 0; i < SDL_GetNumAudioDrivers(); i++)
		{
			if (std::strcmp(SDL_GetAudioDriver(i), "pipewire") == 0)
			{
				if (SDL_AudioInit("pipewire") == 0)
				{
					return 0;
				}
				SDL_AudioQuit();
				break;
			}
		}
	}
#endif
	return SDL_InitSubSystem(SDL_INIT_AUDIO);
}

} // namespace

class HostAudio::Impl
{
public:
	struct PortOut
	{
		bool                 used               = false;
		int                  type               = 0;
		uint32_t             samples_num        = 0;
		uint32_t             freq               = 0;
		Format               format             = Format::Unknown;
		int                  channels_num       = 0;
		int                  volume[8]          = {};
		SDL_AudioDeviceID    device_id          = 0;
		SDL_AudioStream*     conversion_stream  = nullptr;
		AudioPcmFormat       pcm_format         = AudioPcmFormat::Signed16;
		bool                 queue_error_logged = false;
		uint64_t             next_deadline      = 0;
		uint64_t             period_remainder   = 0;
		std::vector<uint8_t> volume_buffer;
		std::vector<uint8_t> device_buffer;
	};

	struct PortIn
	{
		bool     used             = false;
		uint32_t type             = 0;
		uint32_t samples_num      = 0;
		uint32_t freq             = 0;
		Format   format           = Format::Unknown;
		uint64_t last_input_time  = 0;
		uint64_t period_remainder = 0;
	};

	static bool IsValid(const PortOut* ports, Id handle)
	{
		return handle.GetId() >= 0 && handle.GetId() < OUT_PORTS_MAX && ports[handle.GetId()].used;
	}

	static bool IsValid(const PortIn* ports, Id handle)
	{
		return handle.GetId() >= 0 && handle.GetId() < IN_PORTS_MAX && ports[handle.GetId()].used;
	}

	static void CloseOutputPort(PortOut* port)
	{
		EXIT_IF(port == nullptr);
		if (port->conversion_stream != nullptr)
		{
			SDL_FreeAudioStream(port->conversion_stream);
		}
		if (port->device_id != 0)
		{
			SDL_ClearQueuedAudio(port->device_id);
			SDL_CloseAudioDevice(port->device_id);
		}
		*port = PortOut {};
	}

	std::mutex              mutex;
	std::condition_variable state_changed;
	bool                    host_paused   = false;
	bool                    shutting_down = false;
	PortOut                 out_ports[OUT_PORTS_MAX];
	PortIn                  in_ports[IN_PORTS_MAX];
};

std::shared_ptr<HostAudio> HostAudio::Create(std::string* error)
{
	if (InitializeSdlAudio() < 0)
	{
		if (error != nullptr)
		{
			*error = SDL_GetError();
		}
		return {};
	}
	return std::shared_ptr<HostAudio>(new HostAudio(std::make_unique<Impl>()));
}

HostAudio::HostAudio(std::unique_ptr<Impl> impl): m_impl(std::move(impl)) {}

HostAudio::~HostAudio()
{
	Shutdown();
}

void HostAudio::Shutdown()
{
	std::unique_lock lock(m_impl->mutex);
	if (m_impl->shutting_down)
	{
		return;
	}
	m_impl->shutting_down = true;
	m_impl->host_paused   = false;
	m_impl->state_changed.notify_all();
	for (auto& port: m_impl->out_ports)
	{
		Impl::CloseOutputPort(&port);
	}
	lock.unlock();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

HostAudio::Id HostAudio::AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format)
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Kyty::Emulator::HostClock::IsPeriodicIntervalValid(samples_num, freq))
	{
		return Id::Invalid();
	}

	for (int id = 0; id < OUT_PORTS_MAX; id++)
	{
		if (m_impl->out_ports[id].used)
		{
			continue;
		}
		auto& port       = m_impl->out_ports[id];
		port.used        = true;
		port.type        = type;
		port.samples_num = samples_num;
		port.freq        = freq;
		port.format      = format;

		switch (format)
		{
			case Format::Signed16bitMono:
			case Format::FloatMono: port.channels_num = 1; break;
			case Format::Signed16bitStereo:
			case Format::FloatStereo: port.channels_num = 2; break;
			case Format::Signed16bit8Ch:
			case Format::Float8Ch:
			case Format::Signed16bit8ChStd:
			case Format::Float8ChStd: port.channels_num = 8; break;
			default: Impl::CloseOutputPort(&port); return Id::Invalid();
		}
		std::fill_n(port.volume, port.channels_num, 32768);

		if (type == 0 || type == 1)
		{
			SDL_AudioSpec desired {};
			SDL_AudioSpec obtained {};
			port.pcm_format  = (format == Format::Signed16bitMono || format == Format::Signed16bitStereo ||
			                    format == Format::Signed16bit8Ch || format == Format::Signed16bit8ChStd)
			                       ? AudioPcmFormat::Signed16
			                       : AudioPcmFormat::Float32;
			desired.freq     = static_cast<int>(freq);
			desired.format   = port.pcm_format == AudioPcmFormat::Signed16 ? AUDIO_S16SYS : AUDIO_F32SYS;
			desired.channels = static_cast<uint8_t>(port.channels_num);
			desired.samples  = static_cast<uint16_t>(samples_num);
			desired.callback = nullptr;
			constexpr int allowed_changes =
			    SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
			port.device_id = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, allowed_changes);
			if (port.device_id == 0 || obtained.format != desired.format)
			{
				Impl::CloseOutputPort(&port);
				return Id::Invalid();
			}
			if (obtained.freq != desired.freq || obtained.channels != desired.channels)
			{
				port.conversion_stream =
				    SDL_NewAudioStream(desired.format, desired.channels, desired.freq, obtained.format, obtained.channels, obtained.freq);
				if (port.conversion_stream == nullptr)
				{
					Impl::CloseOutputPort(&port);
					return Id::Invalid();
				}
				std::fprintf(stderr, "Kyty audio conversion: %d Hz/%u ch/0x%x -> %d Hz/%u ch/0x%x\n", desired.freq, desired.channels,
				             desired.format, obtained.freq, obtained.channels, obtained.format);
			}
			SDL_PauseAudioDevice(port.device_id, m_impl->host_paused ? 1 : 0);
		}
		return Id::Create(id);
	}
	return Id::Invalid();
}

bool HostAudio::AudioOutClose(Id handle)
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Impl::IsValid(m_impl->out_ports, handle))
	{
		return false;
	}
	Impl::CloseOutputPort(&m_impl->out_ports[handle.GetId()]);
	return true;
}

bool HostAudio::AudioOutValid(Id handle)
{
	std::lock_guard lock(m_impl->mutex);
	return !m_impl->shutting_down && Impl::IsValid(m_impl->out_ports, handle);
}

bool HostAudio::AudioOutGetStatus(Id handle, int* type, int* channels_num)
{
	if (type == nullptr || channels_num == nullptr)
	{
		return false;
	}
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Impl::IsValid(m_impl->out_ports, handle))
	{
		return false;
	}
	const auto& port = m_impl->out_ports[handle.GetId()];
	*type            = port.type;
	*channels_num    = port.channels_num;
	return true;
}

bool HostAudio::AudioOutSetVolume(Id handle, uint32_t bitflag, const int* volume)
{
	if (volume == nullptr)
	{
		return false;
	}
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Impl::IsValid(m_impl->out_ports, handle))
	{
		return false;
	}
	auto& port = m_impl->out_ports[handle.GetId()];
	for (int i = 0; i < port.channels_num; i++, bitflag >>= 1u)
	{
		if ((bitflag & 1u) == 0)
		{
			continue;
		}
		int src_index = i;
		if (port.format == Format::Float8ChStd || port.format == Format::Signed16bit8ChStd)
		{
			switch (i)
			{
				case 4: src_index = 6; break;
				case 5: src_index = 7; break;
				case 6: src_index = 4; break;
				case 7: src_index = 5; break;
				default: break;
			}
		}
		port.volume[i] = volume[src_index];
	}
	return true;
}

void HostAudio::SetHostPaused(bool paused)
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || m_impl->host_paused == paused)
	{
		return;
	}
	m_impl->host_paused = paused;
	for (auto& port: m_impl->out_ports)
	{
		if (port.device_id != 0)
		{
			SDL_PauseAudioDevice(port.device_id, paused ? 1 : 0);
		}
		if (port.used)
		{
			port.next_deadline    = 0;
			port.period_remainder = 0;
		}
	}
	if (!paused)
	{
		m_impl->state_changed.notify_all();
	}
}

bool HostAudio::AudioOutOutputs(const OutputParam* params, uint32_t num, uint32_t* samples_num)
{
	if (params == nullptr || samples_num == nullptr || num == 0 || num > OUT_PORTS_MAX)
	{
		return false;
	}

	std::unique_lock lock(m_impl->mutex);
	m_impl->state_changed.wait(lock, [this] { return !m_impl->host_paused || m_impl->shutting_down; });
	if (m_impl->shutting_down)
	{
		return false;
	}
	for (uint32_t i = 0; i < num; i++)
	{
		if (!Impl::IsValid(m_impl->out_ports, params[i].handle))
		{
			return false;
		}
	}

	*samples_num                 = m_impl->out_ports[params[0].handle.GetId()].samples_num;
	const uint64_t now           = Kyty::Emulator::HostClock::NowMicroseconds();
	uint64_t       wake_deadline = now;
	bool           all_queues_ok = true;

	for (uint32_t i = 0; i < num; i++)
	{
		auto&          port          = m_impl->out_ports[params[i].handle.GetId()];
		const uint64_t resync_window = (4'000'000ull * port.samples_num) / port.freq;
		if (port.next_deadline == 0 || (now > port.next_deadline && now - port.next_deadline > resync_window))
		{
			port.next_deadline    = now;
			port.period_remainder = 0;
		}
		port.next_deadline +=
		    Kyty::Emulator::HostClock::NextPeriodicIntervalMicroseconds(port.samples_num, port.freq, &port.period_remainder);
		wake_deadline = std::max(wake_deadline, port.next_deadline);

		if (params[i].data == nullptr || port.device_id == 0)
		{
			continue;
		}
		if (!AudioPcmApplyChannelVolumes(params[i].data, port.samples_num, static_cast<uint32_t>(port.channels_num), port.pcm_format,
		                                 port.volume, &port.volume_buffer) ||
		    port.volume_buffer.size() > static_cast<size_t>(INT_MAX))
		{
			return false;
		}

		const void* queue_data = port.volume_buffer.data();
		size_t      queue_size = port.volume_buffer.size();
		bool        queue_ok   = true;
		if (port.conversion_stream != nullptr)
		{
			if (SDL_AudioStreamPut(port.conversion_stream, queue_data, static_cast<int>(queue_size)) != 0)
			{
				queue_ok = false;
			}
			const int available = queue_ok ? SDL_AudioStreamAvailable(port.conversion_stream) : -1;
			if (available < 0)
			{
				queue_ok = false;
			} else if (available == 0)
			{
				continue;
			} else
			{
				port.device_buffer.resize(static_cast<size_t>(available));
				const int converted = SDL_AudioStreamGet(port.conversion_stream, port.device_buffer.data(), available);
				if (converted < 0)
				{
					queue_ok = false;
				} else
				{
					queue_data = port.device_buffer.data();
					queue_size = static_cast<size_t>(converted);
				}
			}
		}
		if (queue_ok && queue_size != 0 && SDL_QueueAudio(port.device_id, queue_data, static_cast<uint32_t>(queue_size)) != 0)
		{
			queue_ok = false;
		}
		if (!queue_ok && !port.queue_error_logged)
		{
			std::fprintf(stderr, "Kyty audio output failed: %s\n", SDL_GetError());
			port.queue_error_logged = true;
		}
		all_queues_ok = all_queues_ok && queue_ok;
	}

	lock.unlock();
	Kyty::Emulator::HostClock::SleepUntil(wake_deadline);
	return all_queues_ok;
}

HostAudio::Id HostAudio::AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format)
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Kyty::Emulator::HostClock::IsPeriodicIntervalValid(samples_num, freq))
	{
		return Id::Invalid();
	}
	for (int id = 0; id < IN_PORTS_MAX; id++)
	{
		if (!m_impl->in_ports[id].used)
		{
			auto& port       = m_impl->in_ports[id];
			port.used        = true;
			port.type        = type;
			port.samples_num = samples_num;
			port.freq        = freq;
			port.format      = format;
			return Id::Create(id);
		}
	}
	return Id::Invalid();
}

bool HostAudio::AudioInValid(Id handle)
{
	std::lock_guard lock(m_impl->mutex);
	return !m_impl->shutting_down && Impl::IsValid(m_impl->in_ports, handle);
}

uint32_t HostAudio::AudioInInput(Id handle, void* dest)
{
	if (dest == nullptr)
	{
		return 0;
	}
	uint32_t samples  = 0;
	uint32_t freq     = 0;
	uint64_t deadline = 0;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->shutting_down || !Impl::IsValid(m_impl->in_ports, handle))
		{
			return 0;
		}
		auto& port         = m_impl->in_ports[handle.GetId()];
		samples            = port.samples_num;
		freq               = port.freq;
		const uint64_t now = Kyty::Emulator::HostClock::NowMicroseconds();
		const uint64_t next =
		    port.last_input_time + Kyty::Emulator::HostClock::NextPeriodicIntervalMicroseconds(samples, freq, &port.period_remainder);
		if (next < now)
		{
			deadline              = now;
			port.period_remainder = 0;
		} else
		{
			deadline = next;
		}
		port.last_input_time = deadline;
	}
	Kyty::Emulator::HostClock::SleepUntil(deadline);
	return samples;
}

} // namespace Kyty::Libs::Audio

#endif
