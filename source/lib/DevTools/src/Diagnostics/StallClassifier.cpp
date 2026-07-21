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

struct ActiveLane
{
	Domain   domain      = Domain::Unknown;
	uint64_t instance    = 0;
	uint64_t correlation = 0;
	uint32_t operation   = 0;
	uint64_t last_change = 0;
	bool     waiting     = false;
	bool     active      = false;
};

[[nodiscard]] bool FindStalledHle(const ProgressSnapshot& progress, ActiveLane* out) noexcept
{
	if (out == nullptr)
	{
		return false;
	}
	for (uint32_t i = 0; i < progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto& e = progress.entries[i];
		if (e.key.domain != Domain::Hle)
		{
			continue;
		}
		const auto st = static_cast<ProgressState>(e.record.state);
		if ((st == ProgressState::Active || st == ProgressState::Waiting) && e.record.submitted > e.record.completed)
		{
			out->domain      = Domain::Hle;
			out->instance    = e.key.instance;
			out->correlation = e.record.correlation;
			out->operation   = e.record.operation;
			out->last_change = e.record.last_change_ns;
			out->waiting     = (st == ProgressState::Waiting);
			out->active      = true;
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool AnyActiveProgress(const ProgressSnapshot& progress) noexcept
{
	for (uint32_t i = 0; i < progress.count && i < MaxProgressSnapshotEntries; ++i)
	{
		const auto st = static_cast<ProgressState>(progress.entries[i].record.state);
		if (st == ProgressState::Active || st == ProgressState::Waiting)
		{
			return true;
		}
	}
	return false;
}

[[nodiscard]] uint64_t FingerprintInput(const ObservationInput& input, StallCategory category) noexcept
{
	// Causal fingerprint: category + process + progress identity (no sample times).
	struct Compact
	{
		uint16_t category;
		uint8_t  liveness;
		uint8_t  termination;
		uint8_t  error;
		uint8_t  code_valid;
		uint32_t code;
		uint32_t progress_count;
		uint64_t keys[32];
		uint64_t submitted[32];
		uint64_t completed[32];
		uint32_t operations[32];
		uint16_t states[32];
	} compact {};
	compact.category       = static_cast<uint16_t>(category);
	compact.liveness       = static_cast<uint8_t>(input.process.liveness);
	compact.termination    = static_cast<uint8_t>(input.process.termination);
	compact.error          = static_cast<uint8_t>(input.process.error);
	compact.code_valid     = input.process.code_valid;
	compact.code           = input.process.code;
	const uint32_t n       = (input.progress.count < 32u) ? input.progress.count : 32u;
	compact.progress_count = n;
	for (uint32_t i = 0; i < n; ++i)
	{
		const auto& e    = input.progress.entries[i];
		compact.keys[i]  = e.record.instance_key;
		compact.submitted[i]  = e.record.submitted;
		compact.completed[i]  = e.record.completed;
		compact.operations[i] = e.record.operation;
		compact.states[i]     = e.record.state;
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
	const bool heartbeat_stale =
	    (input.heartbeat_ns == 0u) ||
	    (input.sample_time_ns > input.heartbeat_ns && (input.sample_time_ns - input.heartbeat_ns) >= settings.heartbeat_stale_ns);

	ActiveLane hle {};
	const bool hle_stalled = FindStalledHle(input.progress, &hle);

	StallCategory candidate = StallCategory::HealthyIdle;
	if (heartbeat_stale && !AnyActiveProgress(input.progress))
	{
		candidate = StallCategory::WorkerUnresponsive;
	} else if (hle_stalled)
	{
		candidate = StallCategory::HleStall;
	} else if (!AnyActiveProgress(input.progress))
	{
		candidate = StallCategory::HealthyIdle;
	} else
	{
		candidate = StallCategory::UnknownStall;
	}

	const uint64_t fingerprint = FingerprintInput(input, candidate);
	if (state->causal_fingerprint != fingerprint)
	{
		state->causal_fingerprint = fingerprint;
		state->first_observed_ns  = input.sample_time_ns;
		state->suspected_ns       = 0;
		state->confirmed_ns       = 0;
		state->suspected          = SuspectedEvidence {};
		state->suspected.causal_fingerprint    = fingerprint;
		state->suspected.category              = candidate;
		state->suspected.sample_time_ns        = input.sample_time_ns;
		state->suspected.heartbeat_ns          = input.heartbeat_ns;
		state->suspected.max_loss_monotonic_ns = input.loss.max_loss_monotonic_ns;
		if (hle_stalled)
		{
			PushEvidence(&state->suspected, StallFact {Domain::Hle, 1, 0, hle.instance, hle.correlation});
		}
	}

	const uint64_t elapsed =
	    (input.sample_time_ns >= state->first_observed_ns) ? (input.sample_time_ns - state->first_observed_ns) : 0u;

	uint64_t sus_ns = settings.suspected_after_ns;
	uint64_t conf_ns = settings.confirmed_after_ns;
	if (hle_stalled)
	{
		sus_ns  = ThresholdSuspected(settings, Domain::Hle, hle.operation);
		conf_ns = ThresholdConfirmed(settings, Domain::Hle, hle.operation);
	}

	if (candidate == StallCategory::HealthyIdle)
	{
		result.category   = StallCategory::HealthyIdle;
		result.confidence = loss_blocks_high ? Confidence::Medium : Confidence::High;
		state->suspected.category   = StallCategory::HealthyIdle;
		state->suspected.confidence = result.confidence;
		return result;
	}

	if (elapsed < sus_ns)
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

	result.category     = candidate;
	result.suspected_ns = state->suspected_ns;
	result.confidence   = Confidence::Medium;
	result.evidence_total  = state->suspected.evidence_total;
	result.evidence_stored = state->suspected.evidence_stored;
	result.evidence_truncated = state->suspected.evidence_truncated;
	for (uint16_t i = 0; i < state->suspected.evidence_stored && i < 16u; ++i)
	{
		result.evidence[i] = state->suspected.evidence[i];
	}

	if (elapsed >= conf_ns)
	{
		if (state->confirmed_ns == 0u)
		{
			state->confirmed_ns = input.sample_time_ns;
		}
		result.confirmed_ns = state->confirmed_ns;
		result.confidence   = loss_blocks_high ? Confidence::Medium : Confidence::High;
		// Live stall remains nonterminal — keep sampling.
		result.terminal = 0;
	}

	state->suspected.category   = result.category;
	state->suspected.confidence = result.confidence;
	return result;
}

} // namespace Kyty::DevTools
