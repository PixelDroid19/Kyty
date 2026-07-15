#include "Emulator/Agent/EventRing.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {
namespace {

uint64_t NowMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

void CopyBounded(char* dst, size_t dst_size, const char* src)
{
	if (dst == nullptr || dst_size == 0)
	{
		return;
	}
	if (src == nullptr)
	{
		dst[0] = '\0';
		return;
	}
	std::snprintf(dst, dst_size, "%s", src);
}

} // namespace

EventRing& EventRing::Instance()
{
	static EventRing ring;
	return ring;
}

const char* EventKindName(EventKind kind)
{
	switch (kind)
	{
		case EventKind::Info: return "info";
		case EventKind::Warn: return "warn";
		case EventKind::Error: return "error";
		case EventKind::Fatal: return "fatal";
		case EventKind::Capture: return "capture";
		case EventKind::Input: return "input";
		case EventKind::Present: return "present";
	}
	return "info";
}

bool EventKindFromName(const char* name, EventKind* out)
{
	if (name == nullptr || out == nullptr)
	{
		return false;
	}
	if (std::strcmp(name, "info") == 0)
	{
		*out = EventKind::Info;
		return true;
	}
	if (std::strcmp(name, "warn") == 0)
	{
		*out = EventKind::Warn;
		return true;
	}
	if (std::strcmp(name, "error") == 0)
	{
		*out = EventKind::Error;
		return true;
	}
	if (std::strcmp(name, "fatal") == 0)
	{
		*out = EventKind::Fatal;
		return true;
	}
	if (std::strcmp(name, "capture") == 0)
	{
		*out = EventKind::Capture;
		return true;
	}
	if (std::strcmp(name, "input") == 0)
	{
		*out = EventKind::Input;
		return true;
	}
	if (std::strcmp(name, "present") == 0)
	{
		*out = EventKind::Present;
		return true;
	}
	return false;
}

void EventRing::Push(EventKind kind, const char* code, const char* message)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_start_ms == 0)
	{
		m_start_ms = NowMs();
	}

	const uint64_t index = m_count % kAgentEventRingCapacity;
	auto&          rec   = m_records[index];
	rec.seq              = ++m_seq;
	rec.t_ms             = NowMs() - m_start_ms;
	rec.kind             = kind;
	CopyBounded(rec.code, sizeof(rec.code), code != nullptr ? code : "");
	CopyBounded(rec.message, sizeof(rec.message), message != nullptr ? message : "");
	++m_count;
}

uint64_t EventRing::Size() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_count < kAgentEventRingCapacity ? m_count : kAgentEventRingCapacity;
}

uint64_t EventRing::NextSeq() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_seq;
}

uint32_t EventRing::CopySince(uint64_t after_seq, EventRecord* out, uint32_t max_out) const
{
	if (out == nullptr || max_out == 0)
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	const uint64_t              available = m_count < kAgentEventRingCapacity ? m_count : kAgentEventRingCapacity;
	uint32_t                    written   = 0;

	for (uint64_t i = 0; i < available && written < max_out; ++i)
	{
		const uint64_t logical = available - 1 - i;
		const uint64_t index =
		    (m_count < kAgentEventRingCapacity) ? logical : ((m_count - available + logical) % kAgentEventRingCapacity);
		const auto& rec = m_records[index];
		if (rec.seq <= after_seq)
		{
			continue;
		}
		out[written++] = rec;
	}
	return written;
}

bool EventRing::LastOfKind(EventKind kind, EventRecord* out) const
{
	if (out == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	const uint64_t              available = m_count < kAgentEventRingCapacity ? m_count : kAgentEventRingCapacity;
	for (uint64_t i = 0; i < available; ++i)
	{
		const uint64_t logical = available - 1 - i;
		const uint64_t index =
		    (m_count < kAgentEventRingCapacity) ? logical : ((m_count - available + logical) % kAgentEventRingCapacity);
		const auto& rec = m_records[index];
		if (rec.kind == kind)
		{
			*out = rec;
			return true;
		}
	}
	return false;
}

bool EventRing::LastError(EventRecord* out) const
{
	if (out == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	const uint64_t              available = m_count < kAgentEventRingCapacity ? m_count : kAgentEventRingCapacity;
	for (uint64_t i = 0; i < available; ++i)
	{
		const uint64_t logical = available - 1 - i;
		const uint64_t index =
		    (m_count < kAgentEventRingCapacity) ? logical : ((m_count - available + logical) % kAgentEventRingCapacity);
		const auto& rec = m_records[index];
		if (rec.kind == EventKind::Error || rec.kind == EventKind::Fatal)
		{
			*out = rec;
			return true;
		}
	}
	return false;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
