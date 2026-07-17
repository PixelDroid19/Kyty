#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_EVENTRING_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_EVENTRING_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

inline constexpr uint32_t kAgentEventRingCapacity = 512u;
inline constexpr uint32_t kAgentEventMessageMax   = 192u;

enum class EventKind: uint8_t
{
	Info    = 1,
	Warn    = 2,
	Error   = 3,
	Fatal   = 4,
	Capture = 5,
	Input   = 6,
	Present = 7,
};

struct EventRecord
{
	uint64_t  seq                                = 0;
	uint64_t  t_ms                               = 0;
	EventKind kind                               = EventKind::Info;
	char      code[32]                           = {};
	char      message[kAgentEventMessageMax]     = {};
};

// Snapshot of ring capacity/overflow (current observability, not guest state).
struct EventRingStats
{
	uint32_t capacity     = kAgentEventRingCapacity;
	uint64_t size         = 0; // events currently retained (≤ capacity)
	uint64_t next_seq     = 0; // last assigned sequence (monotonic)
	uint64_t total_pushed = 0; // all-time pushes
	uint64_t dropped      = 0; // overwritten/dropped due to capacity
	bool     overflowed   = false;
};

class EventRing
{
public:
	EventRing() = default;

	void Push(EventKind kind, const char* code, const char* message);
	[[nodiscard]] uint64_t Size() const;
	[[nodiscard]] uint64_t NextSeq() const;
	[[nodiscard]] EventRingStats GetStats() const;
	// Copies up to max_out newest-first events with seq > after_seq. Returns count.
	uint32_t CopySince(uint64_t after_seq, EventRecord* out, uint32_t max_out) const;
	[[nodiscard]] bool LastOfKind(EventKind kind, EventRecord* out) const;
	[[nodiscard]] bool LastError(EventRecord* out) const;

	// Test helper: clear ring state (integration / unit tests only).
	void ResetForTests();

	static EventRing& Instance();

private:
	mutable std::mutex m_mutex;
	EventRecord        m_records[kAgentEventRingCapacity] {};
	uint64_t           m_seq          = 0;
	uint64_t           m_count        = 0;
	uint64_t           m_start_ms     = 0;
	uint64_t           m_total_pushed = 0;
	uint64_t           m_dropped      = 0;
};

const char* EventKindName(EventKind kind);
bool        EventKindFromName(const char* name, EventKind* out);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_EVENTRING_H_ */
