#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuSubmissionTracker.h"

#include <cstdint>
#include <vector>

UT_BEGIN(EmulatorGpuSubmissionTracker);

using namespace Libs::Graphics;

namespace {

struct RecordedAction
{
	GpuCompletionPhase phase = GpuCompletionPhase::WriteBack;
	uint64_t           token = 0;
};

class RecordingSink final: public GpuCompletionActionSink
{
public:
	void Execute(GpuCompletionPhase phase, uint64_t token) override { actions.push_back({phase, token}); }

	std::vector<RecordedAction> actions;
};

} // namespace

TEST(EmulatorGpuSubmissionTracker, AllocatesMonotonicIdsPerQueue)
{
	GpuSubmissionTracker tracker;
	SubmissionId         gfx_first;
	SubmissionId         gfx_second;
	SubmissionId         compute_first;

	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &gfx_first, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.MarkSubmitted(gfx_first), GpuSubmissionResult::Success);
	RecordingSink sink;
	EXPECT_EQ(tracker.MarkCompleted(gfx_first, &sink), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &gfx_second, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(1), 0, &compute_first, nullptr), GpuSubmissionResult::Success);

	EXPECT_EQ(gfx_first.queue, GpuQueueId(0));
	EXPECT_EQ(gfx_first.sequence, 1u);
	EXPECT_EQ(gfx_second.sequence, 2u);
	EXPECT_EQ(compute_first.queue, GpuQueueId(1));
	EXPECT_EQ(compute_first.sequence, 1u);
	EXPECT_NE(gfx_second, compute_first);
}

TEST(EmulatorGpuSubmissionTracker, ReportsExactSlotOwnerUntilCompletion)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;
	SubmissionId         blocked;
	SubmissionDependency dependency;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(3), 2, &first, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(3), 2, &blocked, &dependency), GpuSubmissionResult::SlotBusy);
	EXPECT_EQ(dependency.producer, first);

	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	RecordingSink sink;
	ASSERT_EQ(tracker.MarkCompleted(first, &sink), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(3), 2, &blocked, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(blocked.sequence, 2u);
}

TEST(EmulatorGpuSubmissionTracker, RejectsInvalidStateTransitionsStructurally)
{
	GpuSubmissionTracker tracker;
	SubmissionId         id;
	RecordingSink        sink;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.MarkCompleted(id, &sink), GpuSubmissionResult::InvalidTransition);
	EXPECT_EQ(tracker.MarkSubmitted(id), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.MarkSubmitted(id), GpuSubmissionResult::InvalidTransition);

	const SubmissionId unknown {GpuQueueId(0), 999};
	EXPECT_EQ(tracker.MarkSubmitted(unknown), GpuSubmissionResult::UnknownSubmission);
	EXPECT_EQ(tracker.MarkCompleted(unknown, &sink), GpuSubmissionResult::UnknownSubmission);
}

TEST(EmulatorGpuSubmissionTracker, ExecutesActionsOnceInCompletionPhaseOrder)
{
	GpuSubmissionTracker tracker;
	SubmissionId         id;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::Notify, 30), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::GuestStore, 20), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::WriteBack, 10), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::WriteBack, 11), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::Notify, 31), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(id), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::GuestStore, 21), GpuSubmissionResult::SubmissionFrozen);

	RecordingSink sink;
	EXPECT_EQ(tracker.MarkCompleted(id, &sink), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.MarkCompleted(id, &sink), GpuSubmissionResult::AlreadyCompleted);
	ASSERT_EQ(sink.actions.size(), 5u);
	EXPECT_EQ(sink.actions[0].phase, GpuCompletionPhase::WriteBack);
	EXPECT_EQ(sink.actions[0].token, 10u);
	EXPECT_EQ(sink.actions[1].phase, GpuCompletionPhase::WriteBack);
	EXPECT_EQ(sink.actions[1].token, 11u);
	EXPECT_EQ(sink.actions[2].phase, GpuCompletionPhase::GuestStore);
	EXPECT_EQ(sink.actions[2].token, 20u);
	EXPECT_EQ(sink.actions[3].phase, GpuCompletionPhase::Notify);
	EXPECT_EQ(sink.actions[3].token, 30u);
	EXPECT_EQ(sink.actions[4].phase, GpuCompletionPhase::Notify);
	EXPECT_EQ(sink.actions[4].token, 31u);
}

TEST(EmulatorGpuSubmissionTracker, CompletesOnlyTheRequestedSubmission)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;
	SubmissionId         second;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 1, &second, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(first, GpuCompletionPhase::Notify, 1), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.AddCompletionAction(second, GpuCompletionPhase::Notify, 2), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(second), GpuSubmissionResult::Success);

	RecordingSink sink;
	ASSERT_EQ(tracker.MarkCompleted(first, &sink), GpuSubmissionResult::Success);
	ASSERT_EQ(sink.actions.size(), 1u);
	EXPECT_EQ(sink.actions[0].token, 1u);
	GpuSubmissionState state = GpuSubmissionState::Recording;
	EXPECT_EQ(tracker.GetState(second, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Submitted);
}

TEST(EmulatorGpuSubmissionTracker, FindsNewestPendingProducerWhenValueMatchesWait)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;
	SubmissionId         second;
	SubmissionDependency dependency;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(first, 0x1000, 8, 0x10), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 1, &second, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(second, 0x1000, 8, 0x20), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(second), GpuSubmissionResult::Success);

	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 8, 0x20, UINT64_MAX, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(dependency.producer, second);
	EXPECT_EQ(tracker.FindPendingProducer(0x2000, 8, 0x10, UINT64_MAX, &dependency), GpuSubmissionResult::ProducerNotFound);
}

TEST(EmulatorGpuSubmissionTracker, NewerProducerShadowsOlderMatchingValue)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;
	SubmissionId         second;
	SubmissionDependency dependency;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(first, 0x1000, 4, 1), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 1, &second, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(second, 0x1000, 4, 2), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(second), GpuSubmissionResult::Success);

	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 4, 1, UINT32_MAX, &dependency), GpuSubmissionResult::ProducerValueMismatch);
	EXPECT_EQ(dependency.producer, second);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 4, 2, UINT32_MAX, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(dependency.producer, second);
}

TEST(EmulatorGpuSubmissionTracker, MatchesOnlyMaskedBytesCoveredByProducerRange)
{
	GpuSubmissionTracker tracker;
	SubmissionId         id;
	SubmissionDependency dependency;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(id, 0x1004, 4, 0xaabbccdd), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(id), GpuSubmissionResult::Success);

	const uint64_t covered_mask = 0xffffffff00000000ull;
	const uint64_t covered_ref  = 0xaabbccdd00000000ull;
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 8, covered_ref, covered_mask, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(dependency.producer, id);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 8, covered_ref, UINT64_MAX, &dependency),
	          GpuSubmissionResult::ProducerValueMismatch);
	EXPECT_EQ(dependency.producer, id);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 8, 0x1122334400000000ull, covered_mask, &dependency),
	          GpuSubmissionResult::ProducerValueMismatch);
	EXPECT_EQ(dependency.producer, id);
}

TEST(EmulatorGpuSubmissionTracker, CompletionKeepsProducerUntilGuestPublicationRetiresIt)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;
	SubmissionId         second;
	SubmissionDependency dependency;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(first, 0x1000, 4, 1), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 1, &second, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.RegisterProducer(second, 0x1000, 4, 2), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkSubmitted(second), GpuSubmissionResult::Success);

	RecordingSink sink;
	ASSERT_EQ(tracker.MarkCompleted(second, &sink), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 4, 2, UINT32_MAX, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(dependency.producer, second);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 4, 1, UINT32_MAX, &dependency),
	          GpuSubmissionResult::ProducerValueMismatch);
	EXPECT_EQ(dependency.producer, second);

	ASSERT_EQ(tracker.RetireCompleted(second), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 4, 1, UINT32_MAX, &dependency), GpuSubmissionResult::Success);
	EXPECT_EQ(dependency.producer, first);
}

TEST(EmulatorGpuSubmissionTracker, RetiresOnlyCompletedSubmissionWithoutRecyclingId)
{
	GpuSubmissionTracker tracker;
	SubmissionId         first;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(2), 4, &first, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.RetireCompleted(first), GpuSubmissionResult::InvalidTransition);
	ASSERT_EQ(tracker.MarkSubmitted(first), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.RetireCompleted(first), GpuSubmissionResult::InvalidTransition);

	RecordingSink sink;
	ASSERT_EQ(tracker.MarkCompleted(first, &sink), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.RetireCompleted(first), GpuSubmissionResult::Success);

	GpuSubmissionState state = GpuSubmissionState::Recording;
	EXPECT_EQ(tracker.GetState(first, &state), GpuSubmissionResult::UnknownSubmission);
	EXPECT_EQ(tracker.RetireCompleted(first), GpuSubmissionResult::UnknownSubmission);

	SubmissionId second;
	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(2), 4, &second, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(second.sequence, 2u);
}

TEST(EmulatorGpuSubmissionTracker, ReportsWhetherCompletionActionsExistWithoutChangingState)
{
	GpuSubmissionTracker tracker;
	SubmissionId         id;
	bool                 has_actions = true;

	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.HasCompletionActions(id, &has_actions), GpuSubmissionResult::Success);
	EXPECT_FALSE(has_actions);

	ASSERT_EQ(tracker.AddCompletionAction(id, GpuCompletionPhase::Notify, 1), GpuSubmissionResult::Success);
	ASSERT_EQ(tracker.HasCompletionActions(id, &has_actions), GpuSubmissionResult::Success);
	EXPECT_TRUE(has_actions);

	GpuSubmissionState state = GpuSubmissionState::Completed;
	ASSERT_EQ(tracker.GetState(id, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Recording);

	EXPECT_EQ(tracker.HasCompletionActions(id, nullptr), GpuSubmissionResult::InvalidArgument);
	EXPECT_EQ(tracker.HasCompletionActions({GpuQueueId(0), 999}, &has_actions), GpuSubmissionResult::UnknownSubmission);
}

TEST(EmulatorGpuSubmissionTracker, RejectsInvalidArgumentsWithoutCreatingState)
{
	GpuSubmissionTracker tracker;
	SubmissionId         id;

	EXPECT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, nullptr, nullptr), GpuSubmissionResult::InvalidArgument);
	ASSERT_EQ(tracker.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(tracker.AddCompletionAction(id, static_cast<GpuCompletionPhase>(99), 1), GpuSubmissionResult::InvalidArgument);
	EXPECT_EQ(tracker.RegisterProducer(id, UINT64_MAX - 1, 4, 1), GpuSubmissionResult::InvalidArgument);
	EXPECT_EQ(tracker.RegisterProducer(id, 0x1000, 0, 1), GpuSubmissionResult::InvalidArgument);
	EXPECT_EQ(tracker.RegisterProducer(id, 0x1000, 9, 1), GpuSubmissionResult::InvalidArgument);
	EXPECT_EQ(tracker.FindPendingProducer(0x1000, 8, 1, UINT64_MAX, nullptr), GpuSubmissionResult::InvalidArgument);
}

UT_END();
