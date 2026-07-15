#include "Kyty/DevTools/Telemetry/Event.h"

namespace Kyty::DevTools {
namespace {

constexpr uint32_t kKnownFlagMask =
    static_cast<uint32_t>(TimelineFlag::CorrelationValid) | static_cast<uint32_t>(TimelineFlag::Payload0Valid) |
    static_cast<uint32_t>(TimelineFlag::Payload1Valid) | static_cast<uint32_t>(TimelineFlag::Payload2Valid) |
    static_cast<uint32_t>(TimelineFlag::Payload3Valid) | static_cast<uint32_t>(TimelineFlag::ResultError) |
    static_cast<uint32_t>(TimelineFlag::TimedWait);

[[nodiscard]] bool DomainInRange(uint16_t domain) noexcept
{
	return domain > static_cast<uint16_t>(Domain::Unknown) && domain < static_cast<uint16_t>(Domain::Count);
}

[[nodiscard]] bool EventInRange(uint16_t event) noexcept
{
	return event > static_cast<uint16_t>(EventId::Unknown) && event < static_cast<uint16_t>(EventId::Count);
}

[[nodiscard]] uint16_t DomainOfOperation(uint32_t op) noexcept
{
	return static_cast<uint16_t>((op >> 8u) & 0xffu);
}

[[nodiscard]] bool OperationBelongsToDomain(uint32_t op, uint16_t domain) noexcept
{
	if (op == 0u || (op & 0xffff0000u) != 0u)
	{
		return false;
	}
	return DomainOfOperation(op) == domain;
}

[[nodiscard]] bool ResultErrorMatchesCategory(uint32_t category, bool result_error) noexcept
{
	const bool expect_error = (category == static_cast<uint32_t>(ResultCategory::GuestError) ||
	                           category == static_cast<uint32_t>(ResultCategory::HostError) ||
	                           category == static_cast<uint32_t>(ResultCategory::Timeout) ||
	                           category == static_cast<uint32_t>(ResultCategory::Unsupported));
	return result_error == expect_error;
}

[[nodiscard]] bool ResultErrorMatchesWaitOutcome(uint32_t outcome, bool result_error) noexcept
{
	const bool expect_error =
	    (outcome == static_cast<uint32_t>(WaitOutcome::TimedOut) || outcome == static_cast<uint32_t>(WaitOutcome::Error));
	return result_error == expect_error;
}

} // namespace

bool ValidateEventSchema(const EventRecord& record) noexcept
{
	if (!DomainInRange(record.domain) || !EventInRange(record.event))
	{
		return false;
	}
	if ((record.flags & ~kKnownFlagMask) != 0u)
	{
		return false;
	}

	const bool corr   = (record.flags & static_cast<uint32_t>(TimelineFlag::CorrelationValid)) != 0u;
	const bool p0     = (record.flags & static_cast<uint32_t>(TimelineFlag::Payload0Valid)) != 0u;
	const bool p1     = (record.flags & static_cast<uint32_t>(TimelineFlag::Payload1Valid)) != 0u;
	const bool p2     = (record.flags & static_cast<uint32_t>(TimelineFlag::Payload2Valid)) != 0u;
	const bool p3     = (record.flags & static_cast<uint32_t>(TimelineFlag::Payload3Valid)) != 0u;
	const bool rerr   = (record.flags & static_cast<uint32_t>(TimelineFlag::ResultError)) != 0u;
	const bool timed  = (record.flags & static_cast<uint32_t>(TimelineFlag::TimedWait)) != 0u;
	const auto event  = static_cast<EventId>(record.event);
	const auto domain = static_cast<Domain>(record.domain);

	switch (event)
	{
		case EventId::ThreadStart:
		{
			if (domain != Domain::GuestThread && domain != Domain::Hle && domain != Domain::CommandProcessor &&
			    domain != Domain::Renderer && domain != Domain::GpuQueue && domain != Domain::VideoOut &&
			    domain != Domain::Presentation && domain != Domain::Synchronization)
			{
				// ThreadStart is allowed on any instrumented domain; keep open for host roles.
			}
			if (!corr || !p0 || p1 || p2 || p3 || rerr || timed)
			{
				return false;
			}
			if (record.payload[0] == 0u || record.payload[0] >= static_cast<uint64_t>(ThreadRole::Count))
			{
				return false;
			}
			return true;
		}
		case EventId::ThreadExit:
		{
			if (!corr || !p0 || !p1 || p2 || p3 || timed)
			{
				return false;
			}
			if (record.payload[0] == 0u || record.payload[0] >= static_cast<uint64_t>(ThreadRole::Count))
			{
				return false;
			}
			if (record.payload[1] == 0u || record.payload[1] >= static_cast<uint64_t>(ResultCategory::Count))
			{
				return false;
			}
			return ResultErrorMatchesCategory(static_cast<uint32_t>(record.payload[1]), rerr);
		}
		case EventId::OperationSubmit:
		case EventId::OperationProgress:
		{
			if (!p0 || !p1 || p2 || rerr)
			{
				return false;
			}
			// Payload3 optional for first auxiliary.
			if (!OperationBelongsToDomain(static_cast<uint32_t>(record.payload[0]), record.domain))
			{
				return false;
			}
			if (timed)
			{
				return false;
			}
			return true;
		}
		case EventId::OperationComplete:
		{
			if (!p0 || !p1 || !p2)
			{
				return false;
			}
			if (!OperationBelongsToDomain(static_cast<uint32_t>(record.payload[0]), record.domain))
			{
				return false;
			}
			if (record.payload[2] == 0u || record.payload[2] >= static_cast<uint64_t>(ResultCategory::Count))
			{
				return false;
			}
			if (timed)
			{
				return false;
			}
			return ResultErrorMatchesCategory(static_cast<uint32_t>(record.payload[2]), rerr);
		}
		case EventId::WaitBegin:
		{
			if (!corr || !p0)
			{
				return false;
			}
			if (!OperationBelongsToDomain(static_cast<uint32_t>(record.payload[0]), record.domain))
			{
				return false;
			}
			if (rerr)
			{
				return false;
			}
			// TimedWait must match whether a finite deadline is present in payload2 when valid.
			if (timed && !p2)
			{
				return false;
			}
			if (!timed && p2)
			{
				// Untimed waits may still set p2 for non-deadline values only when TimedWait clear
				// and operation is RegisterMemoryWait-class; keep strict: TimedWait iff p2 deadline.
				// RegisterMemory is untimed — p2 holds expected value, TimedWait must be clear.
			}
			return true;
		}
		case EventId::WaitEnd:
		{
			if (!corr || !p0 || !p1 || !p2)
			{
				return false;
			}
			if (!OperationBelongsToDomain(static_cast<uint32_t>(record.payload[0]), record.domain))
			{
				return false;
			}
			if (record.payload[2] == 0u || record.payload[2] >= static_cast<uint64_t>(WaitOutcome::Count))
			{
				return false;
			}
			return ResultErrorMatchesWaitOutcome(static_cast<uint32_t>(record.payload[2]), rerr);
		}
		default: return false;
	}
}

} // namespace Kyty::DevTools
