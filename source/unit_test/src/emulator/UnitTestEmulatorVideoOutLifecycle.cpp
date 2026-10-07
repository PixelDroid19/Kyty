#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/VideoOutFlipLifecycleGate.h"
#include "Emulator/Graphics/VideoOutHostAccessGate.h"
#include "Emulator/Graphics/VideoOutMaterializationGate.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <utility>

UT_BEGIN(EmulatorVideoOutLifecycle);

using namespace Libs::Graphics;

// These CPU fixtures use the production admission/progress gates, accepted
// registration predicate and completion accounting. The explicit presentation
// checkpoints stand in for the host drawing backend; no GPU completion is
// inferred from elapsed time.

TEST(EmulatorVideoOutLifecycle, CloseBeforeFirstCaptureRetainsAcceptedRegistration)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 1, 1};
	const auto accepted = registered;
	flips.Accept(&owner);
	std::promise<void> consume;
	auto consume_gate = consume.get_future();
	std::atomic_uint32_t presented {0};
	std::atomic_bool closing {false};
	std::thread presenter([&] {
		consume_gate.wait();
		// CaptureRegisteredImageLocked uses this same predicate with the
		// identity retained by the accepted FlipQueue::Request.
		EXPECT_TRUE(registered.CanAccess(true, closing.load(), accepted));
		presented.fetch_add(1);
		flips.Complete(&owner);
	});

	auto drain = host.Drain();
	closing.store(true);
	EXPECT_FALSE(registered.CanAccess(true, true, {}));
	consume.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(presented.load(), 1u);
	EXPECT_FALSE(registered.CanAccess(false, true, accepted));
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, CloseBetweenFirstFlipAndVblankAllowsSecondFlipToDrain)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 3, 7};
	flips.Accept(&owner);
	flips.Accept(&owner);
	std::promise<void> first_completed;
	std::promise<void> resume_vblank;
	auto first = first_completed.get_future();
	auto resume = resume_vblank.get_future();
	std::atomic_uint32_t presented {0};
	std::atomic_bool closing {false};
	std::thread presenter([&] {
		EXPECT_TRUE(registered.CanAccess(true, false, registered));
		presented.fetch_add(1);
		flips.Complete(&owner);
		first_completed.set_value();
		resume.wait();
		{
			// This is the active Window -> VideoOutEndVblank seam between
			// FlipWindow calls. Admission is closed before we resume it.
			auto vblank = host.AcquireProgress();
			EXPECT_TRUE(closing.load());
		}
		EXPECT_TRUE(registered.CanAccess(true, closing.load(), registered));
		presented.fetch_add(1);
		flips.Complete(&owner);
	});

	first.wait();
	auto drain = host.Drain();
	closing.store(true);
	EXPECT_EQ(presented.load(), 1u);
	resume_vblank.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(presented.load(), 2u);
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, CloseWaitsForAlreadyPinnedPresentation)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	VideoOutMaterializationGate materialization;
	int owner = 0;
	const VideoOutRegistrationIdentity registered {&owner, 1, 2};
	flips.Accept(&owner);
	std::promise<void> captured;
	std::promise<void> finish_presentation;
	auto capture_gate = captured.get_future();
	auto finish_gate = finish_presentation.get_future();
	std::atomic_bool presented {false};
	std::thread presenter([&] {
		{
			auto pin = materialization.Acquire();
			EXPECT_TRUE(registered.CanAccess(true, false, registered));
			captured.set_value();
			finish_gate.wait();
			EXPECT_TRUE(registered.CanAccess(true, true, registered));
		}
		presented.store(true);
		flips.Complete(&owner);
	});

	capture_gate.wait();
	auto drain = host.Drain();
	EXPECT_FALSE(presented.load());
	finish_presentation.set_value();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	materialization.WaitUntilIdle();
	EXPECT_TRUE(presented.load());
	presenter.join();
}

TEST(EmulatorVideoOutLifecycle, AdmissionRemainsClosedWhilePresenterCanProgress)
{
	VideoOutHostAccessGate host;
	auto drain = host.Drain();
	std::promise<void> admission_attempted;
	std::promise<void> admission_acquired;
	auto attempted = admission_attempted.get_future();
	auto acquired = admission_acquired.get_future();
	std::thread producer([&] {
		admission_attempted.set_value();
		auto pin = host.Acquire();
		admission_acquired.set_value();
	});
	attempted.wait();
	{
		auto vblank = host.AcquireProgress();
		EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	}
	auto exclusive = drain.Quiesce();
	EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	exclusive.Reset();
	acquired.wait();
	producer.join();
}

TEST(EmulatorVideoOutLifecycle, ExclusiveTeardownWaitsForPresenterProgressPin)
{
	VideoOutHostAccessGate host;
	auto drain = host.Drain();
	auto vblank = host.AcquireProgress();
	std::promise<void> teardown_attempted;
	std::promise<void> teardown_acquired;
	auto attempted = teardown_attempted.get_future();
	auto acquired = teardown_acquired.get_future();
	std::thread closer([&, drain = std::move(drain)]() mutable {
		teardown_attempted.set_value();
		auto exclusive = drain.Quiesce();
		teardown_acquired.set_value();
	});
	attempted.wait();
	EXPECT_EQ(acquired.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
	vblank.Reset();
	acquired.wait();
	closer.join();
}

TEST(EmulatorVideoOutLifecycle, CloseOneOwnerDoesNotRetireAnotherOwnersAcceptedFlip)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int first_owner = 0;
	int second_owner = 0;
	const VideoOutRegistrationIdentity first {&first_owner, 1, 1};
	const VideoOutRegistrationIdentity second {&second_owner, 1, 1};
	flips.Accept(&first_owner);
	flips.Accept(&second_owner);
	auto drain = host.Drain();
	EXPECT_TRUE(first.CanAccess(true, true, first));
	EXPECT_TRUE(second.CanAccess(true, false, second));
	EXPECT_FALSE(second.CanAccess(true, false, first));
	flips.Complete(&first_owner);
	flips.WaitUntilIdle(&first_owner);
	auto exclusive = drain.Quiesce();
	EXPECT_FALSE(first.CanAccess(false, false, first));
	EXPECT_TRUE(second.CanAccess(true, false, second));
	exclusive.Reset();
	{
		auto vblank = host.AcquireProgress();
	}
	flips.Complete(&second_owner);
	flips.WaitUntilIdle(&second_owner);
}

TEST(EmulatorVideoOutLifecycle, ZeroOutstandingCloseAndReopenRejectStaleGenerations)
{
	VideoOutHostAccessGate host;
	VideoOutFlipLifecycleGate flips;
	int owner = 0;
	const VideoOutRegistrationIdentity accepted {&owner, 1, 1};
	auto drain = host.Drain();
	flips.WaitUntilIdle(&owner);
	auto exclusive = drain.Quiesce();
	EXPECT_FALSE(accepted.CanAccess(false, true, accepted));
	const VideoOutRegistrationIdentity reopened {&owner, 2, 1};
	const VideoOutRegistrationIdentity reregistered {&owner, 1, 2};
	EXPECT_FALSE(reopened.CanAccess(true, false, accepted));
	EXPECT_FALSE(reregistered.CanAccess(true, false, accepted));
	EXPECT_TRUE(reopened.CanAccess(true, false, {}));
	EXPECT_TRUE(reopened.CanAccess(true, true, reopened));
	EXPECT_FALSE(reopened.CanAccess(true, true, {}));
}

UT_END();
