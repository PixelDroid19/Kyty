#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuSubmissionCoordinator.h"

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

UT_END();
