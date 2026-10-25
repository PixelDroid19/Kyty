#include "Emulator/AudioHost.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Threads.h"

#include "Emulator/AudioPcm.h"
#include "Emulator/Host/Clock.h"
#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Audio {

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
		// Bound to its own playback device, which converts to the device format.
		SDL_AudioStream*     stream             = nullptr;
		AudioPcmFormat       pcm_format         = AudioPcmFormat::Signed16;
		bool                 queue_error_logged = false;
		uint64_t             next_deadline      = 0;
		uint64_t             period_remainder   = 0;
		std::vector<uint8_t> volume_buffer;
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
		if (port->stream != nullptr)
		{
			SDL_DestroyAudioStream(port->stream);
		}
		*port = PortOut {};
	}

	Core::Mutex   mutex;
	Core::CondVar state_changed;
	bool          host_paused   = false;
	bool          shutting_down = false;
	PortOut       out_ports[OUT_PORTS_MAX];
	PortIn        in_ports[IN_PORTS_MAX];
};

std::shared_ptr<HostAudio> HostAudio::Create(std::string* error)
{
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
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
	{
		Core::LockGuard lock(m_impl->mutex);
		if (m_impl->shutting_down)
		{
			return;
		}
		m_impl->shutting_down = true;
		m_impl->host_paused   = false;
		m_impl->state_changed.SignalAll();
		for (auto& port: m_impl->out_ports)
		{
			Impl::CloseOutputPort(&port);
		}
	}
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool HostAudio::ShuttingDown()
{
	Core::LockGuard lock(m_impl->mutex);
	return m_impl->shutting_down;
}

HostAudio::Id HostAudio::AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format)
{
	Core::LockGuard lock(m_impl->mutex);
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
			port.pcm_format = (format == Format::Signed16bitMono || format == Format::Signed16bitStereo ||
			                   format == Format::Signed16bit8Ch || format == Format::Signed16bit8ChStd)
			                      ? AudioPcmFormat::Signed16
			                      : AudioPcmFormat::Float32;
			SDL_AudioSpec spec {};
			spec.format   = port.pcm_format == AudioPcmFormat::Signed16 ? SDL_AUDIO_S16 : SDL_AUDIO_F32;
			spec.channels = port.channels_num;
			spec.freq     = static_cast<int>(freq);
			SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(samples_num).c_str());
			port.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
			if (port.stream == nullptr)
			{
				Impl::CloseOutputPort(&port);
				return Id::Invalid();
			}
			if (!m_impl->host_paused && !SDL_ResumeAudioStreamDevice(port.stream))
			{
				Impl::CloseOutputPort(&port);
				return Id::Invalid();
			}
		}
		return Id::Create(id);
	}
	return Id::Invalid();
}

bool HostAudio::AudioOutClose(Id handle)
{
	Core::LockGuard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Impl::IsValid(m_impl->out_ports, handle))
	{
		return false;
	}
	Impl::CloseOutputPort(&m_impl->out_ports[handle.GetId()]);
	return true;
}

bool HostAudio::AudioOutValid(Id handle)
{
	Core::LockGuard lock(m_impl->mutex);
	return !m_impl->shutting_down && Impl::IsValid(m_impl->out_ports, handle);
}

bool HostAudio::AudioOutGetStatus(Id handle, int* type, int* channels_num)
{
	if (type == nullptr || channels_num == nullptr)
	{
		return false;
	}
	Core::LockGuard lock(m_impl->mutex);
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
	Core::LockGuard lock(m_impl->mutex);
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
	Core::LockGuard lock(m_impl->mutex);
	if (m_impl->shutting_down || m_impl->host_paused == paused)
	{
		return;
	}
	m_impl->host_paused = paused;
	for (auto& port: m_impl->out_ports)
	{
		if (port.stream != nullptr && paused)
		{
			SDL_PauseAudioStreamDevice(port.stream);
		} else if (port.stream != nullptr)
		{
			SDL_ResumeAudioStreamDevice(port.stream);
		}
		if (port.used)
		{
			port.next_deadline    = 0;
			port.period_remainder = 0;
		}
	}
	if (!paused)
	{
		m_impl->state_changed.SignalAll();
	}
}

void HostAudio::WaitWhileHostPaused()
{
	Core::LockGuard lock(m_impl->mutex);
	while (m_impl->host_paused && !m_impl->shutting_down)
	{
		Kernel::PthreadInterruptibleWait(&m_impl->state_changed, &m_impl->mutex);
	}
}

bool HostAudio::AudioOutOutputs(const OutputParam* params, uint32_t num, uint32_t* samples_num)
{
	uint64_t   wake_deadline = 0;
	const bool queued        = AudioOutQueue(params, num, samples_num, &wake_deadline);
	{
		Kernel::PthreadInterruptibleScope any_syscall;
		Kyty::Emulator::HostClock::SleepUntil(wake_deadline);
	}
	return queued;
}

bool HostAudio::AudioOutQueue(const OutputParam* params, uint32_t num, uint32_t* samples_num, uint64_t* wake_deadline_us)
{
	if (params == nullptr || samples_num == nullptr || wake_deadline_us == nullptr || num == 0 || num > OUT_PORTS_MAX)
	{
		return false;
	}

	uint64_t wake_deadline = 0;
	bool     all_queues_ok = true;
	{
		Core::LockGuard lock(m_impl->mutex);
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

		*samples_num       = m_impl->out_ports[params[0].handle.GetId()].samples_num;
		const uint64_t now = Kyty::Emulator::HostClock::NowMicroseconds();
		wake_deadline      = now;

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

			if (params[i].data == nullptr || port.stream == nullptr)
			{
				continue;
			}
			if (!AudioPcmApplyChannelVolumes(params[i].data, port.samples_num, static_cast<uint32_t>(port.channels_num), port.pcm_format,
			                                 port.volume, &port.volume_buffer) ||
			    port.volume_buffer.size() > static_cast<size_t>(INT_MAX))
			{
				return false;
			}

			const bool queue_ok =
			    SDL_PutAudioStreamData(port.stream, port.volume_buffer.data(), static_cast<int>(port.volume_buffer.size()));
			if (!queue_ok && !port.queue_error_logged)
			{
				KYTY_LOG_ERROR("Kyty audio output failed: %s\n", SDL_GetError());
				port.queue_error_logged = true;
			}
			all_queues_ok = all_queues_ok && queue_ok;
		}
	}

	*wake_deadline_us = wake_deadline;
	return all_queues_ok;
}

HostAudio::Id HostAudio::AudioInOpen(uint32_t type, uint32_t samples_num, uint32_t freq, Format format)
{
	Core::LockGuard lock(m_impl->mutex);
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

bool HostAudio::AudioInClose(Id handle)
{
	Core::LockGuard lock(m_impl->mutex);
	if (m_impl->shutting_down || !Impl::IsValid(m_impl->in_ports, handle))
	{
		return false;
	}
	m_impl->in_ports[handle.GetId()] = Impl::PortIn {};
	return true;
}

bool HostAudio::AudioInValid(Id handle)
{
	Core::LockGuard lock(m_impl->mutex);
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
		Core::LockGuard lock(m_impl->mutex);
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
	{
		Kernel::PthreadInterruptibleScope any_syscall;
		Kyty::Emulator::HostClock::SleepUntil(deadline);
	}
	return samples;
}

} // namespace Kyty::Libs::Audio

#endif
