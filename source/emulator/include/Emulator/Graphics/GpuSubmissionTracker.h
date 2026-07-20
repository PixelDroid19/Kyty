#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONTRACKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONTRACKER_H_

#include <cstdint>
#include <vector>

namespace Kyty::Libs::Graphics {

class GpuQueueId
{
public:
	explicit constexpr GpuQueueId(uint32_t value): m_value(value) {}

	[[nodiscard]] constexpr uint32_t Value() const { return m_value; }

	friend constexpr bool operator==(GpuQueueId lhs, GpuQueueId rhs) { return lhs.m_value == rhs.m_value; }
	friend constexpr bool operator!=(GpuQueueId lhs, GpuQueueId rhs) { return !(lhs == rhs); }

private:
	uint32_t m_value = 0;
};

struct SubmissionId
{
	GpuQueueId queue {0};
	uint64_t   sequence = 0;
};

[[nodiscard]] constexpr bool operator==(const SubmissionId& lhs, const SubmissionId& rhs)
{
	return lhs.queue == rhs.queue && lhs.sequence == rhs.sequence;
}

[[nodiscard]] constexpr bool operator!=(const SubmissionId& lhs, const SubmissionId& rhs)
{
	return !(lhs == rhs);
}

struct SubmissionDependency
{
	SubmissionId producer;
};

enum class GpuSubmissionState : uint8_t
{
	Recording,
	Submitted,
	Completed,
};

enum class GpuCompletionPhase : uint8_t
{
	WriteBack,
	GuestStore,
	Notify,
};

enum class GpuSubmissionResult : uint8_t
{
	Success,
	InvalidArgument,
	UnknownSubmission,
	InvalidTransition,
	SubmissionFrozen,
	SlotBusy,
	ProducerNotFound,
	ProducerValueMismatch,
	AlreadyCompleted,
	CompletionActionsPending,
};

class GpuCompletionActionSink
{
public:
	virtual ~GpuCompletionActionSink() = default;

	virtual void Execute(GpuCompletionPhase phase, uint64_t token) = 0;
};

// Pure submission state machine. The caller owns synchronization and translates
// opaque completion tokens into runtime actions.
class GpuSubmissionTracker
{
public:
	GpuSubmissionTracker()  = default;
	~GpuSubmissionTracker() = default;

	GpuSubmissionTracker(const GpuSubmissionTracker&)            = delete;
	GpuSubmissionTracker& operator=(const GpuSubmissionTracker&) = delete;
	GpuSubmissionTracker(GpuSubmissionTracker&&)                 = delete;
	GpuSubmissionTracker& operator=(GpuSubmissionTracker&&)      = delete;

	GpuSubmissionResult BeginRecording(GpuQueueId queue, uint32_t slot, SubmissionId* id, SubmissionDependency* blocking_dependency);
	GpuSubmissionResult AddCompletionAction(SubmissionId id, GpuCompletionPhase phase, uint64_t token);
	GpuSubmissionResult RegisterProducer(SubmissionId id, uint64_t address, uint32_t size_bytes, uint64_t value);
	GpuSubmissionResult MarkSubmitted(SubmissionId id);
	GpuSubmissionResult MarkCompleted(SubmissionId id, GpuCompletionActionSink* sink);
	GpuSubmissionResult RetireCompleted(SubmissionId id);
	GpuSubmissionResult GetState(SubmissionId id, GpuSubmissionState* state) const;
	GpuSubmissionResult HasCompletionActions(SubmissionId id, bool* has_actions) const;
	GpuSubmissionResult FindPendingProducer(uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask,
	                                        SubmissionDependency* dependency) const;

private:
	struct CompletionAction
	{
		GpuCompletionPhase phase = GpuCompletionPhase::WriteBack;
		uint64_t           token = 0;
	};

	struct Producer
	{
		uint64_t address            = 0;
		uint32_t size_bytes         = 0;
		uint64_t value              = 0;
		uint64_t registration_order = 0;
	};

	struct Submission
	{
		SubmissionId                  id;
		uint32_t                      slot  = 0;
		GpuSubmissionState            state = GpuSubmissionState::Recording;
		std::vector<CompletionAction> actions;
		std::vector<Producer>         producers;
	};

	struct QueueSequence
	{
		GpuQueueId queue {0};
		uint64_t   next = 1;
	};

	Submission*       FindSubmission(SubmissionId id);
	const Submission* FindSubmission(SubmissionId id) const;
	static bool       IsValidPhase(GpuCompletionPhase phase);
	static bool       IsValidRange(uint64_t address, uint32_t size_bytes);
	static bool       ProducerTouchesMaskedBits(const Producer& producer, uint64_t address, uint32_t size_bytes, uint64_t mask);
	static bool       ProducerMatches(const Producer& producer, uint64_t address, uint32_t size_bytes, uint64_t reference, uint64_t mask);

	std::vector<Submission>    m_submissions;
	std::vector<QueueSequence> m_queue_sequences;
	uint64_t                   m_next_producer_registration = 1;
};

} // namespace Kyty::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GPUSUBMISSIONTRACKER_H_ */
