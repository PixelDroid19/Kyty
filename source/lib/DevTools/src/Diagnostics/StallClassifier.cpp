#include "Kyty/DevTools/Diagnostics/StallClassifier.h"

#include "Kyty/DevTools/Diagnostics/Checksum.h"

#include <cstring>

namespace Kyty::DevTools {
namespace {

[[nodiscard]] bool LossPreventsHigh(const LossSnapshot& loss) noexcept
{
	if (loss.writers.aggregate_ring.total != 0u || loss.writers.registration_capacity.total != 0u ||
	    loss.writers.inactive_writer_attempts.total != 0u || loss.unregistered_writers.total != 0u ||
	    loss.skipped_publications.total != 0u || loss.disconnects.total != 0u || loss.rejected_samples.total != 0u)
	{
		return true;
	}
	for (uint32_t i = 0; i < static_cast<uint32_t>(Domain::Count); ++i)
	{
		if (loss.progress.capacity[i].total != 0u || loss.progress.rejected_update[i].total != 0u)
		{
			return true;
		}
	}
	return false;
}

[[nodiscard]] uint64_t ThresholdSuspected(const StallSettings& settings, Domain domain, uint32_t operation) noexcept
{
	for (uint8_t i = 0; i < settings.override_count && i < 16u; ++i)
	{
		const auto& o = settings.overrides[i];
		if (o.domain == domain && o.operation == operation)
		{
			return o.suspected_after_ns;
		}
	}
	return settings.suspected_after_ns;
}

[[nodiscard]] uint64_t ThresholdConfirmed(const StallSettings& settings, Domain domain, uint32_t operation) noexcept
{
	for (uint8_t i = 0; i < settings.override_count && i < 16u; ++i)
	{
		const auto& o = settings.overrides[i];
		if (o.domain == domain && o.operation == operation)
		{
			return o.confirmed_after_ns;
		}
	}
	return settings.confirmed_after_ns;
}

[[nodiscard]] StallCategory ClassifyTerminalProcess(const ProcessStatus& process) noexcept
{
	if (!ValidateProcessStatus(process) || process.error != ProcessStatusError::None)
	{
		return StallCategory::UnknownStall;
	}
	if (process.liveness != ProcessLiveness::Terminated)
	{
		return StallCategory::None;
	}
	switch (process.termination)
	{
		case ProcessTermination::ExitCode: return StallCategory::ProcessExited;
		case ProcessTermination::Signal:
		case ProcessTermination::UnhandledException: return StallCategory::ProcessCrashed;
		case ProcessTermination::OpaquePlatformStatus: return StallCategory::ProcessTerminated;
		default: return StallCategory::UnknownStall;
	}
}

// Guest-thread lanes stay open while the thread lives; they feed the guest
// deadlock rule only, never the busy-lane rules.
[[nodiscard]] bool IsGuestThread(const ProgressSnapshotEntry& entry) noexcept
{
	return entry.key.domain == Domain::GuestThread;
}

[[nodiscard]] bool LaneBusy(const ProgressRecord& record) noexcept
{
	const auto st = static_cast<ProgressState>(record.state);
	return (st == ProgressState::Active || st == ProgressState::Waiting) && record.submitted > record.completed;
}

[[nodiscard]] uint64_t Age(uint64_t since_ns, uint64_t sample_ns) noexcept
{
	return sample_ns > since_ns ? sample_ns - since_ns : 0u;
}

[[nodiscard]] uint64_t LaneAge(const ProgressRecord& record, uint64_t sample_ns, uint64_t not_before_ns) noexcept
{
	return Age(record.last_change_ns > not_before_ns ? record.last_change_ns : not_before_ns, sample_ns);
}

[[nodiscard]] StallCategory CategoryForDomain(Domain domain) noexcept
{
	switch (domain)
	{
		case Domain::GpuQueue: return StallCategory::GpuStall;
		case Domain::CommandProcessor:
		case Domain::Renderer: return StallCategory::CommandProcessorStall;
		case Domain::Presentation:
		case Domain::VideoOut: return StallCategory::PresentationStall;
		case Domain::Hle: return StallCategory::HleStall;
		default: return StallCategory::UnknownStall;
	}
}

// Lower rank is closer to the root cause: a hung GPU queue also keeps the
// command processor and presentation lanes busy, and those keep HLE waits busy.
[[nodiscard]] uint32_t CauseRank(Domain domain) noexcept
{
	switch (domain)
	{
		case Domain::GpuQueue: return 0;
		case Domain::CommandProcessor: return 1;
		case Domain::Renderer: return 2;
		case Domain::Presentation: return 3;
		case Domain::VideoOut: return 4;
		case Domain::Hle: return 5;
		default: return 6;
	}
}

struct StallCandidate
{
	StallCategory                category     = StallCategory::None;
	const ProgressSnapshotEntry* lane         = nullptr;
	uint64_t                     age_ns       = 0;
	uint64_t                     suspected_ns = 0;
	uint64_t                     confirmed_ns = 0;
};

// Picks the busy lane that has not changed for longer than its suspicion
// threshold and is closest to the root cause; older lanes win ties.
[[nodiscard]] StallCandidate FindStalledLane(const ObservationInput& input, const StallSettings& settings,
                                             uint64_t not_before_ns) noexcept
{
	StallCandidate best {};
	for (uint32_t i = 0; i < input.progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto& e = input.progress.entries[i];
		if (IsGuestThread(e) || !LaneBusy(e.record))
		{
			continue;
		}
		const uint64_t age = LaneAge(e.record, input.sample_time_ns, not_before_ns);
		const uint64_t sus = ThresholdSuspected(settings, e.key.domain, e.record.operation);
		if (age < sus)
		{
			continue;
		}
		const bool better = best.lane == nullptr || CauseRank(e.key.domain) < CauseRank(best.lane->key.domain) ||
		                    (CauseRank(e.key.domain) == CauseRank(best.lane->key.domain) && age > best.age_ns);
		if (better)
		{
			best.category     = CategoryForDomain(e.key.domain);
			best.lane         = &e;
			best.age_ns       = age;
			best.suspected_ns = sus;
			best.confirmed_ns = ThresholdConfirmed(settings, e.key.domain, e.record.operation);
		}
	}
	return best;
}

[[nodiscard]] bool AnyBusyLane(const ProgressSnapshot& progress) noexcept
{
	for (uint32_t i = 0; i < progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		if (!IsGuestThread(progress.entries[i]) && LaneBusy(progress.entries[i].record))
		{
			return true;
		}
	}
	return false;
}

// Causal fingerprint: what is stuck, not when it was sampled. A lane that
// completes and stalls again on a later epoch is a new stall.
[[nodiscard]] uint64_t Fingerprint(const StallCandidate& candidate, uint64_t heartbeat_ns) noexcept
{
	struct Compact
	{
		uint32_t version;
		uint16_t category;
		uint16_t reserved;
		uint64_t instance_key;
		uint64_t submitted;
		uint64_t operation;
		uint64_t heartbeat_ns;
	} compact {};
	compact.version  = 1;
	compact.category = static_cast<uint16_t>(candidate.category);
	if (candidate.lane != nullptr)
	{
		compact.instance_key = candidate.lane->record.instance_key;
		compact.submitted    = candidate.lane->record.submitted;
		compact.operation    = candidate.lane->record.operation;
	}
	if (candidate.category == StallCategory::WorkerUnresponsive)
	{
		compact.heartbeat_ns = heartbeat_ns;
	}
	return Crc64Ecma(&compact, sizeof(compact));
}

void PushEvidence(SuspectedEvidence* suspected, StallFact fact) noexcept
{
	if (suspected == nullptr)
	{
		return;
	}
	suspected->evidence_total += 1u;
	if (suspected->evidence_stored < 16u)
	{
		suspected->evidence[suspected->evidence_stored++] = fact;
	} else
	{
		suspected->evidence_truncated = 1u;
	}
}

void PushStalledLanes(const ObservationInput& input, const StallSettings& settings, uint64_t not_before_ns,
                      SuspectedEvidence* suspected) noexcept
{
	for (uint32_t i = 0; i < input.progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto& e = input.progress.entries[i];
		if (IsGuestThread(e) || !LaneBusy(e.record) ||
		    LaneAge(e.record, input.sample_time_ns, not_before_ns) < ThresholdSuspected(settings, e.key.domain, e.record.operation))
		{
			continue;
		}
		PushEvidence(suspected, StallFact {e.key.domain, static_cast<uint16_t>(e.record.operation), e.record.state,
		                                   e.key.instance, e.record.correlation});
	}
}

[[nodiscard]] bool GuestLanesComplete(const LossSnapshot& loss) noexcept
{
	const auto guest = static_cast<uint32_t>(Domain::GuestThread);
	return loss.progress.capacity[guest].total == 0u && loss.progress.rejected_update[guest].total == 0u;
}

// Every guest thread parked in a blocking wait while the command processor has
// had nothing to run: the guest waits for something nothing will signal. A
// running guest thread or a busy command lane rules it out, and so does any
// untracked guest thread, which might be the one still running.
[[nodiscard]] StallCandidate FindGuestDeadlock(const ObservationInput& input, const StallSettings& settings,
                                               uint64_t not_before_ns) noexcept
{
	if (!GuestLanesComplete(input.loss))
	{
		return {};
	}
	StallCandidate best {};
	uint64_t       quiet_ns = UINT64_MAX;
	for (uint32_t i = 0; i < input.progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto&    e   = input.progress.entries[i];
		const uint64_t age = LaneAge(e.record, input.sample_time_ns, not_before_ns);
		if (e.key.domain == Domain::CommandProcessor)
		{
			if (LaneBusy(e.record))
			{
				return {};
			}
			quiet_ns = age < quiet_ns ? age : quiet_ns;
			continue;
		}
		if (!IsGuestThread(e))
		{
			continue;
		}
		if (static_cast<ProgressState>(e.record.state) != ProgressState::Waiting)
		{
			return {};
		}
		quiet_ns = age < quiet_ns ? age : quiet_ns;
		if (best.lane == nullptr || age > best.age_ns)
		{
			best.lane   = &e;
			best.age_ns = age;
		}
	}
	if (best.lane == nullptr)
	{
		return {};
	}
	const auto operation = best.lane->record.operation;
	best.category        = StallCategory::GuestDeadlock;
	best.age_ns          = quiet_ns;
	best.suspected_ns    = ThresholdSuspected(settings, Domain::GuestThread, operation);
	best.confirmed_ns    = ThresholdConfirmed(settings, Domain::GuestThread, operation);
	return best.age_ns >= best.suspected_ns ? best : StallCandidate {};
}

// Evidence code is the wait kind the thread is parked in.
void PushGuestWaits(const ObservationInput& input, SuspectedEvidence* suspected) noexcept
{
	for (uint32_t i = 0; i < input.progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto& e = input.progress.entries[i];
		if (!IsGuestThread(e))
		{
			continue;
		}
		PushEvidence(suspected, StallFact {e.key.domain, static_cast<uint16_t>(e.record.auxiliary[0]), e.record.state,
		                                   e.key.instance, e.record.correlation});
	}
}

} // namespace

bool ValidateStallSettings(const StallSettings& settings) noexcept
{
	if (settings.confirmed_after_ns < settings.suspected_after_ns)
	{
		return false;
	}
	if (settings.override_count > 16u)
	{
		return false;
	}
	for (uint8_t i = 0; i < settings.override_count; ++i)
	{
		const auto& o = settings.overrides[i];
		if (o.domain == Domain::Unknown || o.domain >= Domain::Count || o.operation == 0u)
		{
			return false;
		}
		if (o.confirmed_after_ns < o.suspected_after_ns)
		{
			return false;
		}
		for (uint8_t j = 0; j < i; ++j)
		{
			if (settings.overrides[j].domain == o.domain && settings.overrides[j].operation == o.operation)
			{
				return false;
			}
		}
	}
	return true;
}

StallResult Observe(const ObservationInput& input, const StallSettings& settings, ClassifierState* state) noexcept
{
	StallResult result {};
	result.process = input.process;

	if (state == nullptr || !ValidateStallSettings(settings))
	{
		result.category   = StallCategory::UnknownStall;
		result.confidence = Confidence::Low;
		return result;
	}

	// Status errors never enter high-confidence terminal classification.
	if (input.process.error != ProcessStatusError::None)
	{
		result.category   = StallCategory::UnknownStall;
		result.confidence = Confidence::Low;
		return result;
	}

	const StallCategory terminal = ClassifyTerminalProcess(input.process);
	if (terminal == StallCategory::ProcessExited || terminal == StallCategory::ProcessCrashed ||
	    terminal == StallCategory::ProcessTerminated)
	{
		result.category   = terminal;
		result.confidence = Confidence::High;
		result.terminal   = 1u;
		result.confirmed_ns = input.sample_time_ns;
		state->terminal_finalized = true;
		state->confirmed_ns       = input.sample_time_ns;
		return result;
	}
	if (terminal == StallCategory::UnknownStall && input.process.liveness == ProcessLiveness::Terminated)
	{
		result.category   = StallCategory::UnknownStall;
		result.confidence = Confidence::Low;
		return result;
	}

	const bool loss_blocks_high = LossPreventsHigh(input.loss);

	// A stale heartbeat means the lane data is stale too: the process stopped
	// publishing, so no lane verdict can be trusted.
	StallCandidate candidate {};
	const bool     heartbeat_missing = input.heartbeat_ns == 0u;
	const uint64_t heartbeat_age     = Age(input.heartbeat_ns, input.sample_time_ns);
	const bool     heartbeat_stale   = heartbeat_missing || heartbeat_age >= settings.heartbeat_stale_ns;
	if (state->heartbeat_stale && !heartbeat_stale)
	{
		state->resumed_heartbeat_ns = input.heartbeat_ns;
	}
	state->heartbeat_stale = heartbeat_stale;
	if (heartbeat_stale)
	{
		candidate.category     = StallCategory::WorkerUnresponsive;
		candidate.age_ns       = heartbeat_age;
		candidate.suspected_ns = settings.heartbeat_stale_ns;
		candidate.confirmed_ns = settings.confirmed_after_ns > settings.heartbeat_stale_ns ? settings.confirmed_after_ns
		                                                                                  : settings.heartbeat_stale_ns;
	} else
	{
		candidate = FindStalledLane(input, settings, state->resumed_heartbeat_ns);
		if (candidate.category == StallCategory::None)
		{
			candidate = FindGuestDeadlock(input, settings, state->resumed_heartbeat_ns);
		}
	}
	if (candidate.category == StallCategory::None && !AnyBusyLane(input.progress))
	{
		candidate.category = StallCategory::HealthyIdle;
	}

	const uint64_t fingerprint = Fingerprint(candidate, input.heartbeat_ns);
	if (state->causal_fingerprint != fingerprint)
	{
		state->causal_fingerprint = fingerprint;
		state->first_observed_ns  = input.sample_time_ns;
		state->suspected_ns       = 0;
		state->confirmed_ns       = 0;
		state->suspected          = SuspectedEvidence {};
		state->suspected.causal_fingerprint    = fingerprint;
		state->suspected.category              = candidate.category;
		state->suspected.sample_time_ns        = input.sample_time_ns;
		state->suspected.heartbeat_ns          = input.heartbeat_ns;
		state->suspected.max_loss_monotonic_ns = input.loss.max_loss_monotonic_ns;
		if (candidate.category == StallCategory::GuestDeadlock)
		{
			PushGuestWaits(input, &state->suspected);
		} else if (candidate.category != StallCategory::WorkerUnresponsive)
		{
			PushStalledLanes(input, settings, state->resumed_heartbeat_ns, &state->suspected);
		}
	}
	// Without a heartbeat there is no age to measure; time the observation.
	if (heartbeat_missing)
	{
		candidate.age_ns = Age(state->first_observed_ns, input.sample_time_ns);
	}

	if (candidate.category == StallCategory::HealthyIdle)
	{
		result.category   = StallCategory::HealthyIdle;
		result.confidence = loss_blocks_high ? Confidence::Medium : Confidence::High;
		state->suspected.category   = StallCategory::HealthyIdle;
		state->suspected.confidence = result.confidence;
		return result;
	}

	// Busy lanes younger than their threshold are work in flight, not a stall.
	if (candidate.category == StallCategory::None || candidate.age_ns < candidate.suspected_ns)
	{
		result.category   = StallCategory::None;
		result.confidence = Confidence::Low;
		return result;
	}

	if (state->suspected_ns == 0u)
	{
		state->suspected_ns = input.sample_time_ns;
		state->suspected.confidence = Confidence::Medium;
	}

	result.category     = candidate.category;
	result.suspected_ns = state->suspected_ns;
	result.confidence   = Confidence::Medium;
	result.evidence_total  = state->suspected.evidence_total;
	result.evidence_stored = state->suspected.evidence_stored;
	result.evidence_truncated = state->suspected.evidence_truncated;
	for (uint16_t i = 0; i < state->suspected.evidence_stored && i < 16u; ++i)
	{
		result.evidence[i] = state->suspected.evidence[i];
	}

	if (candidate.age_ns >= candidate.confirmed_ns)
	{
		if (state->confirmed_ns == 0u)
		{
			state->confirmed_ns = input.sample_time_ns;
		}
		result.confirmed_ns = state->confirmed_ns;
		// Without a wait graph a guest deadlock is inferred, not proven.
		const bool inferred = candidate.category == StallCategory::GuestDeadlock;
		result.confidence   = loss_blocks_high || inferred ? Confidence::Medium : Confidence::High;
		// Live stall remains nonterminal — keep sampling.
		result.terminal = 0;
	}

	state->suspected.category   = result.category;
	state->suspected.confidence = result.confidence;
	return result;
}

} // namespace Kyty::DevTools
