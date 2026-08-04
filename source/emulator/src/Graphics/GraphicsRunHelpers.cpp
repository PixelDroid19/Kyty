#include "GraphicsRunInternal.h"

#include "Emulator/Config.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

[[nodiscard]] const char* gpu_submission_result_name(GpuSubmissionResult result)
{
	switch (result)
	{
		case GpuSubmissionResult::Success: return "Success";
		case GpuSubmissionResult::InvalidArgument: return "InvalidArgument";
		case GpuSubmissionResult::UnknownSubmission: return "UnknownSubmission";
		case GpuSubmissionResult::InvalidTransition: return "InvalidTransition";
		case GpuSubmissionResult::SubmissionFrozen: return "SubmissionFrozen";
		case GpuSubmissionResult::SlotBusy: return "SlotBusy";
		case GpuSubmissionResult::ProducerNotFound: return "ProducerNotFound";
		case GpuSubmissionResult::ProducerValueMismatch: return "ProducerValueMismatch";
		case GpuSubmissionResult::AlreadyCompleted: return "AlreadyCompleted";
		case GpuSubmissionResult::CompletionActionsPending: return "CompletionActionsPending";
	}
	return "UnknownResult";
}

void require_submission_success(GpuSubmissionResult result, const char* operation, int queue, uint32_t slot)
{
	if (result != GpuSubmissionResult::Success)
	{
		EXIT("GPU submission lifecycle failed: operation=%s queue=%d slot=%" PRIu32 " result=%s\n", operation, queue, slot,
		     gpu_submission_result_name(result));
	}
}

[[nodiscard]] const char* gpu_submission_publication_result_name(GpuSubmissionPublicationResult result)
{
	switch (result)
	{
		case GpuSubmissionPublicationResult::Success: return "Success";
		case GpuSubmissionPublicationResult::InvalidArgument: return "InvalidArgument";
		case GpuSubmissionPublicationResult::UnknownSubmission: return "UnknownSubmission";
		case GpuSubmissionPublicationResult::InvalidTransition: return "InvalidTransition";
		case GpuSubmissionPublicationResult::NotReady: return "NotReady";
	}
	return "UnknownResult";
}

void require_publication_success(GpuSubmissionPublicationResult result, const char* operation, int queue, SubmissionId submission)
{
	if (result != GpuSubmissionPublicationResult::Success)
	{
		EXIT("GPU submission publication failed: operation=%s queue=%d sequence=%" PRIu64 " result=%s\n", operation, queue,
		     submission.sequence, gpu_submission_publication_result_name(result));
	}
}

uint64_t SuspendedWaitTimeoutMs()
{
	static const uint64_t timeout_ms = [] {
		const char* value = std::getenv("KYTY_WAIT_TIMEOUT_MS");
		if (value == nullptr || *value == '\0')
		{
			return uint64_t {1000};
		}
		char* end = nullptr;
		const auto parsed = std::strtoull(value, &end, 10);
		if (end == value || *end != '\0')
		{
			return uint64_t {1000};
		}
		return static_cast<uint64_t>(parsed);
	}();
	return timeout_ms;
}

uint64_t SuspendedWaitNowNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
