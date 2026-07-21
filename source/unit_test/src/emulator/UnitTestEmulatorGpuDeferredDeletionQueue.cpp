#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GpuDeferredDeletionQueue.h"

#include <vector>

UT_BEGIN(EmulatorGpuDeferredDeletionQueue);

using namespace Libs::Graphics;

TEST(EmulatorGpuDeferredDeletionQueue, WaitsForEveryQueueHighWater)
{
	GpuDeferredDeletionQueue queue;
	std::vector<int>         destroyed;

	EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(0), 3}, SubmissionId {GpuQueueId(2), 7}}, [&destroyed]() { destroyed.push_back(1); }),
	          GpuDeferredDeletionResult::Success);

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 3}), GpuDeferredDeletionResult::Success);
	EXPECT_TRUE(destroyed.empty());
	EXPECT_EQ(queue.PendingCount(), 1u);

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(2), 6}), GpuDeferredDeletionResult::Success);
	EXPECT_TRUE(destroyed.empty());

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(2), 7}), GpuDeferredDeletionResult::Success);
	ASSERT_EQ(destroyed.size(), 1u);
	EXPECT_EQ(destroyed[0], 1);
	EXPECT_EQ(queue.PendingCount(), 0u);
}

TEST(EmulatorGpuDeferredDeletionQueue, PreservesStableOrder)
{
	GpuDeferredDeletionQueue queue;
	std::vector<int>         destroyed;

	EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(1), 5}}, [&destroyed]() { destroyed.push_back(1); }),
	          GpuDeferredDeletionResult::Success);
	EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(0), 1}}, [&destroyed]() { destroyed.push_back(2); }),
	          GpuDeferredDeletionResult::Success);

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 1}), GpuDeferredDeletionResult::Success);
	EXPECT_TRUE(destroyed.empty());

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(1), 5}), GpuDeferredDeletionResult::Success);
	ASSERT_EQ(destroyed.size(), 2u);
	EXPECT_EQ(destroyed[0], 1);
	EXPECT_EQ(destroyed[1], 2);
}

TEST(EmulatorGpuDeferredDeletionQueue, CompletionIsIdempotentAndReentrant)
{
	GpuDeferredDeletionQueue queue;
	std::vector<int>         destroyed;

	EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(0), 2}},
	                        [&queue, &destroyed]()
	                        {
		                        destroyed.push_back(1);
		                        EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(1), 4}}, [&destroyed]() { destroyed.push_back(2); }),
		                                  GpuDeferredDeletionResult::Success);
		                        EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(1), 4}), GpuDeferredDeletionResult::Success);
	                        }),
	          GpuDeferredDeletionResult::Success);

	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 2}), GpuDeferredDeletionResult::Success);
	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 2}), GpuDeferredDeletionResult::Success);
	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 1}), GpuDeferredDeletionResult::Success);

	ASSERT_EQ(destroyed.size(), 2u);
	EXPECT_EQ(destroyed[0], 1);
	EXPECT_EQ(destroyed[1], 2);
}

TEST(EmulatorGpuDeferredDeletionQueue, RejectsInvalidInput)
{
	GpuDeferredDeletionQueue queue;

	EXPECT_EQ(queue.Enqueue({SubmissionId {GpuQueueId(0), 0}}, []() {}), GpuDeferredDeletionResult::InvalidArgument);
	EXPECT_EQ(queue.Enqueue({}, {}), GpuDeferredDeletionResult::InvalidArgument);
	EXPECT_EQ(queue.CompleteSubmission(SubmissionId {GpuQueueId(0), 0}), GpuDeferredDeletionResult::InvalidArgument);
	EXPECT_EQ(queue.PendingCount(), 0u);
}

TEST(EmulatorGpuDeferredDeletionQueue, ResourceHighWaterKeepsNewestUsePerQueue)
{
	GpuSubmissionHighWater uses;

	EXPECT_EQ(uses.RecordUse(SubmissionId {GpuQueueId(2), 4}), GpuDeferredDeletionResult::Success);
	EXPECT_EQ(uses.RecordUse(SubmissionId {GpuQueueId(0), 8}), GpuDeferredDeletionResult::Success);
	EXPECT_EQ(uses.RecordUse(SubmissionId {GpuQueueId(2), 3}), GpuDeferredDeletionResult::Success);
	EXPECT_EQ(uses.RecordUse(SubmissionId {GpuQueueId(2), 9}), GpuDeferredDeletionResult::Success);

	ASSERT_EQ(uses.Dependencies().size(), 2u);
	EXPECT_EQ(uses.Dependencies()[0], (SubmissionId {GpuQueueId(2), 9}));
	EXPECT_EQ(uses.Dependencies()[1], (SubmissionId {GpuQueueId(0), 8}));
	SubmissionId latest;
	ASSERT_TRUE(uses.LatestForQueue(GpuQueueId(2), &latest));
	EXPECT_EQ(latest, (SubmissionId {GpuQueueId(2), 9}));
	EXPECT_FALSE(uses.LatestForQueue(GpuQueueId(7), &latest));
	EXPECT_FALSE(uses.LatestForQueue(GpuQueueId(0), nullptr));
	EXPECT_EQ(uses.RecordUse(SubmissionId {GpuQueueId(0), 0}), GpuDeferredDeletionResult::InvalidArgument);
}

TEST(EmulatorGpuDeferredDeletionQueue, ReportsDependencyCompletionWithoutDrainingFutureTasks)
{
	GpuDeferredDeletionQueue       queue;
	const std::vector<SubmissionId> dependencies {{GpuQueueId(1), 3}, {GpuQueueId(2), 5}};
	int                            calls = 0;

	ASSERT_EQ(queue.Enqueue(dependencies, [&calls] { calls++; }), GpuDeferredDeletionResult::Success);
	EXPECT_FALSE(queue.AreDependenciesComplete(dependencies));

	ASSERT_EQ(queue.CompleteSubmission({GpuQueueId(1), 3}), GpuDeferredDeletionResult::Success);
	EXPECT_FALSE(queue.AreDependenciesComplete(dependencies));
	EXPECT_EQ(calls, 0);

	ASSERT_EQ(queue.CompleteSubmission({GpuQueueId(2), 5}), GpuDeferredDeletionResult::Success);
	EXPECT_TRUE(queue.AreDependenciesComplete(dependencies));
	EXPECT_EQ(calls, 1);
}

TEST(EmulatorGpuDeferredDeletionQueue, PublicationAllowsOnlyCurrentFenceWithoutDrainingDestruction)
{
	GpuDeferredDeletionQueue        queue;
	const SubmissionId              publishing {GpuQueueId(1), 3};
	const SubmissionId              other_queue {GpuQueueId(2), 5};
	const std::vector<SubmissionId> dependencies {publishing, other_queue};
	std::vector<int>                events;

	ASSERT_EQ(queue.Enqueue(dependencies, [&events] { events.push_back(4); }), GpuDeferredDeletionResult::Success);
	ASSERT_EQ(queue.CompleteSubmission(other_queue), GpuDeferredDeletionResult::Success);

	EXPECT_FALSE(queue.AreDependenciesComplete(dependencies));
	EXPECT_FALSE(queue.AreDependenciesCompleteForPublication(dependencies, SubmissionId {GpuQueueId(1), 2}));
	EXPECT_FALSE(queue.AreDependenciesCompleteForPublication(dependencies, SubmissionId {GpuQueueId(1), 4}));
	ASSERT_TRUE(queue.AreDependenciesCompleteForPublication(dependencies, publishing));
	EXPECT_TRUE(events.empty());
	EXPECT_EQ(queue.PendingCount(), 1u);

	events.push_back(1); // WriteBack.
	events.push_back(2); // EOP store.
	events.push_back(3); // Interrupt/flip notification.
	EXPECT_EQ(queue.PendingCount(), 1u);

	ASSERT_EQ(queue.CompleteSubmission(publishing), GpuDeferredDeletionResult::Success);
	ASSERT_EQ(events.size(), 4u);
	EXPECT_EQ(events[0], 1);
	EXPECT_EQ(events[1], 2);
	EXPECT_EQ(events[2], 3);
	EXPECT_EQ(events[3], 4);
	EXPECT_EQ(queue.PendingCount(), 0u);
}

UT_END();
