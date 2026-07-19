#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/CommandProcessorSubmissionSlots.h"
#include "Emulator/Graphics/GpuSubmissionCoordinator.h"
#include "Emulator/Graphics/Objects/LabelSubmissionTracker.h"
#include "Emulator/Graphics/Utils.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

UT_BEGIN(EmulatorGpuSubmissionCoordinator);

using namespace Libs::Graphics;

TEST(EmulatorGpuSubmissionCoordinator, TracksLifecycleWithoutCompletionActions)
{
	GpuSubmissionCoordinator coordinator;
	SubmissionId              id;

	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);

	GpuSubmissionState state = GpuSubmissionState::Completed;
	ASSERT_EQ(coordinator.GetState(id, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Recording);

	ASSERT_EQ(coordinator.MarkSubmitted(id), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.GetState(id, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Submitted);

	ASSERT_EQ(coordinator.MarkCompletedWithoutActions(id), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.GetState(id, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Completed);
}

TEST(EmulatorGpuSubmissionCoordinator, ReportsExactBlockingSubmission)
{
	GpuSubmissionCoordinator coordinator;
	SubmissionId              owner;
	SubmissionId              blocked;
	SubmissionDependency      dependency;

	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(4), 2, &owner, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(coordinator.BeginRecording(GpuQueueId(4), 2, &blocked, &dependency), GpuSubmissionResult::SlotBusy);
	EXPECT_EQ(dependency.producer, owner);
}

TEST(EmulatorGpuSubmissionCoordinator, PreservesMonotonicIdsAfterRetirement)
{
	GpuSubmissionCoordinator coordinator;
	SubmissionId              first;
	SubmissionId              second;

	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(2), 1, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.MarkSubmitted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.MarkCompletedWithoutActions(first), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.RetireCompleted(first), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(2), 1, &second, nullptr), GpuSubmissionResult::Success);

	EXPECT_EQ(first.sequence, 1u);
	EXPECT_EQ(second.sequence, 2u);
}

TEST(EmulatorGpuSubmissionCoordinator, KeepsQueueSequencesIndependent)
{
	GpuSubmissionCoordinator coordinator;
	SubmissionId              graphics;
	SubmissionId              compute;

	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(0), 0, &graphics, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(7), 0, &compute, nullptr), GpuSubmissionResult::Success);

	EXPECT_EQ(graphics.queue, GpuQueueId(0));
	EXPECT_EQ(compute.queue, GpuQueueId(7));
	EXPECT_EQ(graphics.sequence, 1u);
	EXPECT_EQ(compute.sequence, 1u);
	EXPECT_NE(graphics, compute);
}

TEST(EmulatorGpuSubmissionCoordinator, SerializesConcurrentQueuesDeterministically)
{
	constexpr uint64_t cycles = 64;

	GpuSubmissionCoordinator coordinator;
	std::mutex               start_mutex;
	std::condition_variable  start_condition;
	uint32_t                 ready = 0;
	bool                     start = false;

	SubmissionId last[2];
	bool         success[2] = {true, true};

	auto run_queue = [&](uint32_t worker) {
		{
			std::unique_lock<std::mutex> lock(start_mutex);
			ready++;
			start_condition.notify_all();
			start_condition.wait(lock, [&] { return start; });
		}

		for (uint64_t cycle = 0; cycle < cycles; cycle++)
		{
			SubmissionId id;
			const auto   queue = GpuQueueId(worker + 10u);
			if (coordinator.BeginRecording(queue, 0, &id, nullptr) != GpuSubmissionResult::Success ||
			    coordinator.MarkSubmitted(id) != GpuSubmissionResult::Success ||
			    coordinator.MarkCompletedWithoutActions(id) != GpuSubmissionResult::Success ||
			    coordinator.RetireCompleted(id) != GpuSubmissionResult::Success)
			{
				success[worker] = false;
				return;
			}
			last[worker] = id;
		}
	};

	std::thread workers[2] = {std::thread(run_queue, 0), std::thread(run_queue, 1)};
	{
		std::unique_lock<std::mutex> lock(start_mutex);
		start_condition.wait(lock, [&] { return ready == 2; });
		start = true;
	}
	start_condition.notify_all();

	for (auto& worker: workers)
	{
		worker.join();
	}

	EXPECT_TRUE(success[0]);
	EXPECT_TRUE(success[1]);
	EXPECT_EQ(last[0].queue, GpuQueueId(10));
	EXPECT_EQ(last[1].queue, GpuQueueId(11));
	EXPECT_EQ(last[0].sequence, cycles);
	EXPECT_EQ(last[1].sequence, cycles);
}

TEST(EmulatorGpuSubmissionCoordinator, RejectsUnexpectedActionsBeforeCompletion)
{
	GpuSubmissionCoordinator coordinator;
	SubmissionId              id;

	ASSERT_EQ(coordinator.BeginRecording(GpuQueueId(0), 0, &id, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.AddCompletionAction(id, GpuCompletionPhase::Notify, 42), GpuSubmissionResult::Success);
	ASSERT_EQ(coordinator.MarkSubmitted(id), GpuSubmissionResult::Success);

	EXPECT_EQ(coordinator.MarkCompletedWithoutActions(id), GpuSubmissionResult::CompletionActionsPending);

	GpuSubmissionState state = GpuSubmissionState::Recording;
	ASSERT_EQ(coordinator.GetState(id, &state), GpuSubmissionResult::Success);
	EXPECT_EQ(state, GpuSubmissionState::Submitted);
}

TEST(EmulatorGpuSubmissionCoordinator, RotatesCommandProcessorSlotsWithExactMonotonicIdentity)
{
	GpuSubmissionCoordinator        coordinator;
	CommandProcessorSubmissionSlots slots(&coordinator, GpuQueueId(3));

	for (uint32_t slot = 0; slot < CommandProcessorSubmissionSlots::SlotCount; slot++)
	{
		SubmissionId id;
		ASSERT_EQ(slots.BeginRecording(slot, &id, nullptr), GpuSubmissionResult::Success);
		EXPECT_EQ(id.queue, GpuQueueId(3));
		EXPECT_EQ(id.sequence, static_cast<uint64_t>(slot) + 1);
		ASSERT_EQ(slots.MarkSubmitted(slot), GpuSubmissionResult::Success);
		ASSERT_EQ(slots.MarkCompletedWithoutActionsAndRetire(slot), GpuSubmissionResult::Success);
	}

	SubmissionId reused;
	ASSERT_EQ(slots.BeginRecording(0, &reused, nullptr), GpuSubmissionResult::Success);
	EXPECT_EQ(reused.queue, GpuQueueId(3));
	EXPECT_EQ(reused.sequence, 5u);
}

TEST(EmulatorGpuSubmissionCoordinator, CompletesOnlyTheSubmissionOwnedByTheFenceSlot)
{
	GpuSubmissionCoordinator        coordinator;
	CommandProcessorSubmissionSlots slots(&coordinator, GpuQueueId(7));
	SubmissionId                    first;
	SubmissionId                    second;

	ASSERT_EQ(slots.BeginRecording(0, &first, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(slots.BeginRecording(1, &second, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(slots.MarkSubmitted(0), GpuSubmissionResult::Success);
	ASSERT_EQ(slots.MarkSubmitted(1), GpuSubmissionResult::Success);
	ASSERT_EQ(slots.MarkCompletedWithoutActionsAndRetire(1), GpuSubmissionResult::Success);

	GpuSubmissionState first_state = GpuSubmissionState::Recording;
	ASSERT_EQ(coordinator.GetState(first, &first_state), GpuSubmissionResult::Success);
	EXPECT_EQ(first_state, GpuSubmissionState::Submitted);

	GpuSubmissionState second_state = GpuSubmissionState::Recording;
	EXPECT_EQ(coordinator.GetState(second, &second_state), GpuSubmissionResult::UnknownSubmission);
}

TEST(EmulatorGpuSubmissionCoordinator, RejectsCommandProcessorSlotLifecycleWithoutAnActiveIdentity)
{
	GpuSubmissionCoordinator        coordinator;
	CommandProcessorSubmissionSlots slots(&coordinator, GpuQueueId(0));

	EXPECT_EQ(slots.MarkSubmitted(0), GpuSubmissionResult::UnknownSubmission);
	EXPECT_EQ(slots.MarkCompletedWithoutActionsAndRetire(0), GpuSubmissionResult::UnknownSubmission);
	EXPECT_EQ(slots.BeginRecording(CommandProcessorSubmissionSlots::SlotCount, nullptr, nullptr), GpuSubmissionResult::InvalidArgument);
}

TEST(EmulatorGpuSubmissionCoordinator, RotatesToRecordingSlotBeforeCompletionCallbackFlush)
{
	GpuSubmissionCoordinator        coordinator;
	CommandProcessorSubmissionSlots slots(&coordinator, GpuQueueId(8));
	SubmissionId                    submitted;
	SubmissionId                    callback_recording;

	ASSERT_EQ(slots.BeginRecording(0, &submitted, nullptr), GpuSubmissionResult::Success);
	ASSERT_EQ(slots.MarkSubmitted(0), GpuSubmissionResult::Success);
	// A synchronous completion callback flushing before rotation would submit
	// the same slot twice, which is the strict runtime failure this guards.
	EXPECT_EQ(slots.MarkSubmitted(0), GpuSubmissionResult::InvalidTransition);
	ASSERT_EQ(slots.CompleteAndRetireThenBeginRecording(0, 1, &callback_recording, nullptr), GpuSubmissionResult::Success);

	EXPECT_EQ(callback_recording.queue, GpuQueueId(8));
	EXPECT_EQ(callback_recording.sequence, 2u);
	EXPECT_EQ(slots.MarkSubmitted(1), GpuSubmissionResult::Success);
}

TEST(EmulatorGpuSubmissionCoordinator, LabelCompletionSelectsOnlyTheExactSubmission)
{
	LabelSubmissionTracker tracker;
	const SubmissionId     first {GpuQueueId(2), 11};
	const SubmissionId     second {GpuQueueId(2), 12};
	Vector<LabelSubmissionCompletion> completed;

	ASSERT_EQ(tracker.Bind(101, first), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.Bind(102, second), LabelSubmissionResult::Success);

	ASSERT_EQ(tracker.TakeCompleted(first, &completed), LabelSubmissionResult::Success);
	ASSERT_EQ(completed.Size(), 1u);
	EXPECT_EQ(completed.At(0).token, 101u);
	EXPECT_EQ(completed.At(0).kind, LabelSubmissionCompletionKind::Keep);
	EXPECT_TRUE(tracker.IsBound(102));
}

TEST(EmulatorGpuSubmissionCoordinator, LabelCompletionDestroysOnlyAnActiveDeletedLabel)
{
	LabelSubmissionTracker tracker;
	const SubmissionId     first {GpuQueueId(4), 21};
	const SubmissionId     second {GpuQueueId(4), 22};
	Vector<LabelSubmissionCompletion> completed;

	ASSERT_EQ(tracker.Bind(201, first), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.Bind(202, second), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.MarkDeleted(201), LabelSubmissionResult::Success);

	ASSERT_EQ(tracker.TakeCompleted(first, &completed), LabelSubmissionResult::Success);
	ASSERT_EQ(completed.Size(), 1u);
	EXPECT_EQ(completed.At(0).token, 201u);
	EXPECT_EQ(completed.At(0).kind, LabelSubmissionCompletionKind::Destroy);
	EXPECT_TRUE(tracker.IsBound(202));
}

TEST(EmulatorGpuSubmissionCoordinator, LabelCompletionAllowsExactReuseAfterPriorCompletion)
{
	LabelSubmissionTracker tracker;
	const SubmissionId     first {GpuQueueId(6), 31};
	const SubmissionId     second {GpuQueueId(6), 32};
	Vector<LabelSubmissionCompletion> completed;

	ASSERT_EQ(tracker.Bind(301, first), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.TakeCompleted(first, &completed), LabelSubmissionResult::Success);
	ASSERT_EQ(completed.Size(), 1u);

	completed.Clear();
	ASSERT_EQ(tracker.Bind(301, second), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.TakeCompleted(first, &completed), LabelSubmissionResult::Success);
	EXPECT_TRUE(completed.IsEmpty());
	EXPECT_TRUE(tracker.IsBound(301));

	ASSERT_EQ(tracker.TakeCompleted(second, &completed), LabelSubmissionResult::Success);
	ASSERT_EQ(completed.Size(), 1u);
	EXPECT_EQ(completed.At(0).token, 301u);
}

TEST(EmulatorGpuSubmissionCoordinator, LabelCompletionKeepsDistinctLabelsAtAReusedGuestAddressIndependent)
{
	LabelSubmissionTracker tracker;
	const SubmissionId     old_write {GpuQueueId(1), 41};
	const SubmissionId     new_write {GpuQueueId(1), 42};
	Vector<LabelSubmissionCompletion> completed;

	// Tokens identify Label objects, not guest addresses. Two Label objects may
	// legitimately target the same guest fence word in consecutive submissions.
	ASSERT_EQ(tracker.Bind(401, old_write), LabelSubmissionResult::Success);
	ASSERT_EQ(tracker.Bind(402, new_write), LabelSubmissionResult::Success);

	ASSERT_EQ(tracker.TakeCompleted(old_write, &completed), LabelSubmissionResult::Success);
	ASSERT_EQ(completed.Size(), 1u);
	EXPECT_EQ(completed.At(0).token, 401u);
	EXPECT_TRUE(tracker.IsBound(402));
}

UT_END();
