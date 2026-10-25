#include "Emulator/DevTools/Runtime.h"

#include "Emulator/Ports/ProgressPort.h"

#include "Kyty/DevTools/Transport/Bootstrap.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Kyty::Emulator::DevTools {
namespace {

using Ports::ProgressLane;

// Matches the supervisor's default sample period so every sample sees a
// fresh heartbeat and current lane states.
constexpr std::chrono::milliseconds kPublishPeriod {250};

// Host threads only bump counters in a slot they own; the publisher thread
// turns counter changes into progress records. The hot path takes no lock,
// reads no clock and writes no line shared with another writer.
// Guest threads take one slot each, so the pool covers the guest-thread
// budget plus the host lanes.
constexpr uint32_t kMaxLaneSlots = 512u;

enum class SlotOwner: uint32_t
{
	Free     = 0,
	Claimed  = 1,
	Released = 2
};

struct alignas(64) LaneSlot
{
	std::atomic<uint32_t> owner {static_cast<uint32_t>(SlotOwner::Free)};
	ProgressLane          lane = ProgressLane::Count;
	// Written by the owning thread only. Every begin and every compile phase
	// change bumps `begun`; `ended == begun` means the lane is idle.
	std::atomic<uint64_t> begun {0};
	std::atomic<uint64_t> ended {0};
	std::atomic<uint32_t> operation {0};
	// Guest-thread lanes: the blocking wait the thread is parked in, 0 while
	// it runs.
	std::atomic<uint32_t> wait {0};
	// Publisher-only.
	Kyty::DevTools::ProgressToken token {};
	uint64_t                      seen_begun = 0;
	uint64_t                      seen_ended = 0;
	bool                          registered = false;
	bool                          refused    = false;
};

struct RuntimeState
{
	Kyty::DevTools::WorkerSession session {};
	bool                         prepared        = false;
	bool                         shutdown_result = true;
	std::thread                  publisher {};
	std::mutex                   publisher_mutex {};
	std::condition_variable      publisher_wake {};
	bool                         publisher_stop     = false;
	uint64_t                     next_lane_instance = 1;
	LaneSlot                     slots[kMaxLaneSlots] {};
	// Claims refused because the pool was full, per lane kind; the publisher
	// forwards them as progress loss.
	std::atomic<uint32_t>        unclaimed[static_cast<uint32_t>(ProgressLane::Count)] {};
	uint32_t                     reported_unclaimed[static_cast<uint32_t>(ProgressLane::Count)] {};
	// Slots handed back to the pool; a thread refused a slot rescans only
	// after it changes.
	std::atomic<uint64_t>        freed_slots {0};
};

RuntimeState& GetState() noexcept
{
	static RuntimeState state {};
	return state;
}

struct LaneDescriptor
{
	Kyty::DevTools::Domain        domain;
	Kyty::DevTools::OperationCode operation;
};

[[nodiscard]] LaneDescriptor DescribeLane(ProgressLane lane) noexcept
{
	using Kyty::DevTools::Domain;
	using Kyty::DevTools::OperationCode;
	switch (lane)
	{
		case ProgressLane::GraphicsCommands:
		case ProgressLane::ComputeCommands: return {Domain::CommandProcessor, OperationCode::CommandSubmit};
		case ProgressLane::FenceWait: return {Domain::GpuQueue, OperationCode::FenceWait};
		case ProgressLane::Present: return {Domain::Presentation, OperationCode::PresentationPresent};
		case ProgressLane::GuestThread: return {Domain::GuestThread, OperationCode::ExecutionBoundary};
		default: return {Domain::Unknown, OperationCode::Unknown};
	}
}

[[nodiscard]] Kyty::DevTools::OperationCode WaitOperation(Ports::GuestWait wait) noexcept
{
	using Kyty::DevTools::OperationCode;
	switch (wait)
	{
		case Ports::GuestWait::Mutex: return OperationCode::MutexWait;
		case Ports::GuestWait::Cond: return OperationCode::CondWait;
		case Ports::GuestWait::Semaphore: return OperationCode::SemaphoreWait;
		case Ports::GuestWait::EventFlag: return OperationCode::EventFlagWait;
		case Ports::GuestWait::EventQueue: return OperationCode::EventQueueWait;
		case Ports::GuestWait::Join: return OperationCode::ThreadJoin;
		case Ports::GuestWait::Rwlock: return OperationCode::RwlockWait;
		case Ports::GuestWait::Address: return OperationCode::AddressWait;
		default: return OperationCode::Unknown;
	}
}

[[nodiscard]] bool IsCommandLane(ProgressLane lane) noexcept
{
	return lane == ProgressLane::GraphicsCommands || lane == ProgressLane::ComputeCommands;
}

struct ThreadLane
{
	LaneSlot* slot          = nullptr;
	uint32_t  depth         = 0;
	uint32_t  phase_depth   = 0;
	uint32_t  wait_depth    = 0;
	bool      claim_refused = false;
	uint64_t  refused_at    = 0;
};

// One slot per (host thread, lane kind), handed back when the thread exits so
// short-lived threads do not exhaust the pool.
struct ThreadLanes
{
	ThreadLane lanes[static_cast<uint32_t>(ProgressLane::Count)] {};

	~ThreadLanes()
	{
		for (auto& lane: lanes)
		{
			if (lane.slot != nullptr)
			{
				lane.slot->owner.store(static_cast<uint32_t>(SlotOwner::Released), std::memory_order_release);
			}
		}
	}
};

thread_local ThreadLanes t_lanes;

[[nodiscard]] LaneSlot* ClaimSlot(ProgressLane lane, ThreadLane* owned) noexcept
{
	auto&          state = GetState();
	const uint64_t freed = state.freed_slots.load(std::memory_order_acquire);
	if (owned->claim_refused && owned->refused_at == freed)
	{
		state.unclaimed[static_cast<uint32_t>(lane)].fetch_add(1u, std::memory_order_relaxed);
		return nullptr;
	}
	for (auto& slot: state.slots)
	{
		auto expected = static_cast<uint32_t>(SlotOwner::Free);
		if (slot.owner.load(std::memory_order_relaxed) == expected &&
		    slot.owner.compare_exchange_strong(expected, static_cast<uint32_t>(SlotOwner::Claimed), std::memory_order_acquire))
		{
			slot.lane            = lane;
			owned->claim_refused = false;
			return &slot;
		}
	}
	// Reported as progress loss so the supervisor never claims high
	// confidence while a lane is invisible.
	owned->claim_refused = true;
	owned->refused_at    = freed;
	state.unclaimed[static_cast<uint32_t>(lane)].fetch_add(1u, std::memory_order_relaxed);
	return nullptr;
}

[[nodiscard]] ThreadLane* OwnedLane(ProgressLane lane) noexcept
{
	const auto index = static_cast<uint32_t>(lane);
	if (index >= static_cast<uint32_t>(ProgressLane::Count))
	{
		return nullptr;
	}
	// A thread left without a slot counts the loss on every begin, so it
	// stays reported for as long as its lane is invisible, and rescans the
	// pool once a slot was freed.
	auto& owned = t_lanes.lanes[index];
	if (owned.slot == nullptr)
	{
		owned.slot = ClaimSlot(lane, &owned);
	}
	return owned.slot != nullptr ? &owned : nullptr;
}

void BumpBegun(LaneSlot* slot, Kyty::DevTools::OperationCode operation) noexcept
{
	slot->operation.store(static_cast<uint32_t>(operation), std::memory_order_relaxed);
	slot->begun.store(slot->begun.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

void BeginLane(ProgressLane lane)
{
	auto* owned = OwnedLane(lane);
	if (owned == nullptr || owned->depth++ != 0u)
	{
		return;
	}
	BumpBegun(owned->slot, DescribeLane(lane).operation);
}

void EndLane(ProgressLane lane)
{
	const auto index = static_cast<uint32_t>(lane);
	if (index >= static_cast<uint32_t>(ProgressLane::Count))
	{
		return;
	}
	auto& owned = t_lanes.lanes[index];
	if (owned.slot == nullptr || owned.depth == 0u || --owned.depth != 0u)
	{
		return;
	}
	owned.phase_depth = 0;
	if (lane == ProgressLane::GuestThread)
	{
		// The thread left guest code for good; its lane closes.
		owned.wait_depth = 0;
		owned.slot->owner.store(static_cast<uint32_t>(SlotOwner::Released), std::memory_order_release);
		owned.slot = nullptr;
		return;
	}
	owned.slot->ended.store(owned.slot->begun.load(std::memory_order_relaxed), std::memory_order_release);
}

// A command phase changes the operation of the command lane already open on
// this thread; phases outside a command batch (background precompile) block
// nothing the supervisor tracks.
[[nodiscard]] ThreadLane* OpenCommandLane() noexcept
{
	for (auto lane: {ProgressLane::GraphicsCommands, ProgressLane::ComputeCommands})
	{
		auto& owned = t_lanes.lanes[static_cast<uint32_t>(lane)];
		if (owned.slot != nullptr && owned.depth != 0u)
		{
			return &owned;
		}
	}
	return nullptr;
}

[[nodiscard]] Kyty::DevTools::OperationCode PhaseOperation(Ports::CommandPhase phase) noexcept
{
	return phase == Ports::CommandPhase::RegisterWait ? Kyty::DevTools::OperationCode::RegisterMemoryWait
	                                                  : Kyty::DevTools::OperationCode::PipelineCompile;
}

// Phases do not interleave: a compile runs under the run lock, which a
// register wait releases. Only the outermost phase names the operation.
void BeginCommandPhase(Ports::CommandPhase phase)
{
	auto* owned = OpenCommandLane();
	if (owned != nullptr && owned->phase_depth++ == 0u)
	{
		BumpBegun(owned->slot, PhaseOperation(phase));
	}
}

void EndCommandPhase()
{
	auto* owned = OpenCommandLane();
	if (owned != nullptr && owned->phase_depth != 0u && --owned->phase_depth == 0u)
	{
		BumpBegun(owned->slot, Kyty::DevTools::OperationCode::CommandSubmit);
	}
}

[[nodiscard]] ThreadLane* OpenGuestLane() noexcept
{
	auto& owned = t_lanes.lanes[static_cast<uint32_t>(ProgressLane::GuestThread)];
	return owned.slot != nullptr && owned.depth != 0u ? &owned : nullptr;
}

void SetGuestWait(LaneSlot* slot, uint32_t wait) noexcept
{
	slot->wait.store(wait, std::memory_order_relaxed);
	slot->begun.store(slot->begun.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

void BeginGuestWait(Ports::GuestWait wait)
{
	auto* owned = OpenGuestLane();
	if (owned != nullptr && owned->wait_depth++ == 0u)
	{
		SetGuestWait(owned->slot, static_cast<uint32_t>(WaitOperation(wait)));
	}
}

void EndGuestWait()
{
	auto* owned = OpenGuestLane();
	if (owned != nullptr && owned->wait_depth != 0u && --owned->wait_depth == 0u)
	{
		SetGuestWait(owned->slot, 0u);
	}
}

void ResetSlot(LaneSlot* slot) noexcept
{
	slot->begun.store(0, std::memory_order_relaxed);
	slot->ended.store(0, std::memory_order_relaxed);
	slot->operation.store(0, std::memory_order_relaxed);
	slot->wait.store(0, std::memory_order_relaxed);
	slot->token      = {};
	slot->seen_begun = 0;
	slot->seen_ended = 0;
	slot->registered = false;
	slot->refused    = false;
	slot->owner.store(static_cast<uint32_t>(SlotOwner::Free), std::memory_order_release);
	GetState().freed_slots.fetch_add(1u, std::memory_order_release);
}

void RegisterSlot(RuntimeState* state, LaneSlot* slot) noexcept
{
	const auto domain = DescribeLane(slot->lane).domain;
	const Kyty::DevTools::ProgressKey key {domain, state->next_lane_instance++};
	// A refused registration is counted as progress loss by the registry; do
	// not retry it every period.
	slot->registered = state->session.RegisterProgress(key, &slot->token);
	slot->refused    = !slot->registered;
}

[[nodiscard]] Kyty::DevTools::ProgressUpdate DescribeUpdate(const LaneSlot& slot, uint64_t epoch) noexcept
{
	using Kyty::DevTools::ProgressFlag;
	using Kyty::DevTools::ProgressState;
	const auto operation = static_cast<Kyty::DevTools::OperationCode>(slot.operation.load(std::memory_order_relaxed));
	Kyty::DevTools::ProgressUpdate update {};
	update.epoch     = epoch;
	update.operation = operation;
	update.state     = operation == Kyty::DevTools::OperationCode::FenceWait || operation == Kyty::DevTools::OperationCode::RegisterMemoryWait
	                       ? ProgressState::Waiting
	                       : ProgressState::Active;
	update.flags     = static_cast<uint16_t>(ProgressFlag::OperationValid);
	if (slot.lane == ProgressLane::GuestThread)
	{
		const uint32_t wait = slot.wait.load(std::memory_order_relaxed);
		update.state        = wait != 0u ? ProgressState::Waiting : ProgressState::Active;
		update.auxiliary[0] = wait;
		update.flags |= static_cast<uint16_t>(ProgressFlag::Auxiliary0Valid);
	}
	return update;
}

// Publishes what the owning thread did since the previous period. Lane age is
// therefore exact to one publish period, far below every stall threshold.
void SampleSlot(RuntimeState* state, LaneSlot* slot) noexcept
{
	const uint64_t begun = slot->begun.load(std::memory_order_acquire);
	const uint64_t ended = std::min(slot->ended.load(std::memory_order_acquire), begun);
	if (begun != slot->seen_begun)
	{
		(void)state->session.SubmitProgress(slot->token, DescribeUpdate(*slot, begun));
		slot->seen_begun = begun;
	}
	// A guest thread's lane stays open until the thread exits.
	if (slot->lane != ProgressLane::GuestThread && ended == begun && ended != slot->seen_ended)
	{
		(void)state->session.CompleteProgress(slot->token, ended);
		slot->seen_ended = ended;
	}
}

void ReportUnclaimed(RuntimeState* state) noexcept
{
	for (uint32_t kind = 0; kind < static_cast<uint32_t>(ProgressLane::Count); ++kind)
	{
		const uint32_t unclaimed = state->unclaimed[kind].load(std::memory_order_relaxed);
		if (unclaimed != state->reported_unclaimed[kind])
		{
			state->session.NoteUntrackedProgress(DescribeLane(static_cast<ProgressLane>(kind)).domain);
			state->reported_unclaimed[kind] = unclaimed;
		}
	}
}

void SampleLanes(RuntimeState* state) noexcept
{
	ReportUnclaimed(state);
	for (auto& slot: state->slots)
	{
		const auto owner = static_cast<SlotOwner>(slot.owner.load(std::memory_order_acquire));
		if (owner == SlotOwner::Free)
		{
			continue;
		}
		// `lane` is written after the claim and published by the owner's first
		// begin; an unbegun slot has nothing to register yet.
		if (!slot.registered && !slot.refused && slot.begun.load(std::memory_order_acquire) != 0u)
		{
			RegisterSlot(state, &slot);
		}
		if (slot.registered)
		{
			SampleSlot(state, &slot);
		}
		if (owner == SlotOwner::Released)
		{
			if (slot.registered)
			{
				state->session.CloseProgress(slot.token);
			}
			ResetSlot(&slot);
		}
	}
}

void PublisherLoop() noexcept
{
	auto&                        state = GetState();
	std::unique_lock<std::mutex> lock(state.publisher_mutex);
	while (!state.publisher_wake.wait_for(lock, kPublishPeriod, [&state] { return state.publisher_stop; }))
	{
		lock.unlock();
		SampleLanes(&state);
		// A failed publication leaves the heartbeat stale; the supervisor
		// reports that as an unresponsive worker, which is the truth.
		(void)state.session.Publish();
		lock.lock();
	}
}

void StartPublisher(RuntimeState* state) noexcept
{
	state->publisher_stop = false;
	state->publisher      = std::thread(PublisherLoop);
	Ports::ProgressPort::Install({&BeginLane, &EndLane, &BeginCommandPhase, &EndCommandPhase, &BeginGuestWait, &EndGuestWait});
}

void StopPublisher(RuntimeState* state) noexcept
{
	if (!state->publisher.joinable())
	{
		return;
	}
	Ports::ProgressPort::Install({});
	{
		std::lock_guard<std::mutex> lock(state->publisher_mutex);
		state->publisher_stop = true;
	}
	state->publisher_wake.notify_all();
	state->publisher.join();
}

} // namespace

Kyty::DevTools::WorkerSessionResult PrepareFromBootstrap(
    const Kyty::DevTools::WorkerTelemetryOptions& options) noexcept
{
	auto& state = GetState();
	if (state.prepared)
	{
		return Kyty::DevTools::WorkerSessionResult::AlreadyActive;
	}

	const auto result = state.session.StartFromBootstrap(std::getenv(Kyty::DevTools::kBootstrapEnvName), options);
	if (result == Kyty::DevTools::WorkerSessionResult::MissingBootstrap ||
	    result == Kyty::DevTools::WorkerSessionResult::Attached)
	{
		state.prepared        = true;
		state.shutdown_result = true;
	}
	// The supervisor treats a stale heartbeat as an unresponsive worker, so
	// publication starts with the session, before any long initialization.
	if (result == Kyty::DevTools::WorkerSessionResult::Attached)
	{
		StartPublisher(&state);
	}
	return result;
}

bool Initialize() noexcept
{
	return GetState().prepared;
}

bool Shutdown() noexcept
{
	auto& state = GetState();
	if (!state.prepared)
	{
		return state.shutdown_result;
	}
	StopPublisher(&state);
	const bool result = state.session.Stop();
	state.prepared        = false;
	state.shutdown_result = result;
	return result;
}

bool Active() noexcept
{
	return GetState().session.Active();
}

} // namespace Kyty::Emulator::DevTools
