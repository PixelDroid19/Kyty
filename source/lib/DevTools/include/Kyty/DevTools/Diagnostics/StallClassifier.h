#ifndef KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_STALLCLASSIFIER_H_
#define KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_STALLCLASSIFIER_H_

#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Telemetry/Progress.h"
#include "Kyty/DevTools/Telemetry/WriterRegistry.h"

#include <cstdint>

namespace Kyty::DevTools {

enum class StallCategory: uint16_t
{
	None                   = 0,
	HealthyIdle            = 1,
	HleStall               = 2,
	GuestDeadlock          = 3,
	CommandProcessorStall  = 4,
	GpuStall               = 5,
	PresentationStall      = 6,
	WorkerUnresponsive     = 7,
	ProcessExited          = 8,
	ProcessCrashed         = 9,
	ProcessTerminated      = 10,
	UnknownStall           = 11
};

enum class Confidence: uint8_t
{
	Low    = 0,
	Medium = 1,
	High   = 2
};

struct StallFact
{
	Domain   domain      = Domain::Unknown;
	uint16_t code        = 0;
	uint32_t flags       = 0;
	uint64_t instance    = 0;
	uint64_t correlation = 0;
};

struct WaitEdge
{
	uint64_t wait_ref     = 0;
	uint64_t waiter_ref   = 0;
	uint64_t producer_ref = 0;
};

struct WaitGraphSnapshot
{
	WaitEdge edges[512] {};
	uint32_t count                   = 0;
	uint32_t unknown_producer_count  = 0;
	uint32_t rejected_reference_count = 0;
	uint32_t reserved                = 0;
};

struct LossSnapshot
{
	WriterLossSnapshot   writers {};
	ProgressLossSnapshot progress {};
	GlobalLossCounter    unregistered_writers {};
	GlobalLossCounter    skipped_publications {};
	GlobalLossCounter    disconnects {};
	GlobalLossCounter    rejected_samples {};
	uint64_t             max_loss_monotonic_ns = 0;
};

struct ObservationInput
{
	ProgressSnapshot  progress {};
	WaitGraphSnapshot wait_graph {};
	LossSnapshot      loss {};
	ProcessStatus     process {};
	uint64_t          heartbeat_ns  = 0;
	uint64_t          sample_time_ns = 0;
};

struct OperationThreshold
{
	Domain   domain            = Domain::Unknown;
	uint32_t operation         = 0;
	uint64_t suspected_after_ns = 0;
	uint64_t confirmed_after_ns = 0;
};

struct StallSettings
{
	uint64_t           suspected_after_ns = 5'000'000'000ull;
	uint64_t           confirmed_after_ns = 15'000'000'000ull;
	uint64_t           heartbeat_stale_ns = 5'000'000'000ull;
	OperationThreshold overrides[16]      = {};
	uint8_t            override_count     = 0;
};

struct SuspectedEvidence
{
	uint64_t      causal_fingerprint     = 0;
	uint64_t      sample_time_ns         = 0;
	uint64_t      heartbeat_ns           = 0;
	uint64_t      max_loss_monotonic_ns  = 0;
	StallCategory category               = StallCategory::None;
	Confidence    confidence             = Confidence::Low;
	uint16_t      evidence_total         = 0;
	uint16_t      evidence_stored        = 0;
	uint16_t      contradiction_total    = 0;
	uint16_t      contradiction_stored   = 0;
	uint8_t       evidence_truncated     = 0;
	uint8_t       contradiction_truncated = 0;
	uint8_t       reserved[6]            = {};
	StallFact     evidence[16]           = {};
	StallFact     contradictions[16]     = {};
};

struct ClassifierState
{
	uint64_t          causal_fingerprint = 0;
	uint64_t          first_observed_ns  = 0;
	uint64_t          suspected_ns       = 0;
	uint64_t          confirmed_ns       = 0;
	bool              terminal_finalized = false;
	SuspectedEvidence suspected {};
};

struct StallResult
{
	StallCategory category               = StallCategory::None;
	Confidence    confidence             = Confidence::Low;
	uint8_t       terminal               = 0;
	uint16_t      evidence_total         = 0;
	uint16_t      evidence_stored        = 0;
	uint16_t      contradiction_total    = 0;
	uint16_t      contradiction_stored   = 0;
	uint8_t       evidence_truncated     = 0;
	uint8_t       contradiction_truncated = 0;
	uint8_t       reserved[5]            = {};
	uint64_t      suspected_ns           = 0;
	uint64_t      confirmed_ns           = 0;
	ProcessStatus process {};
	StallFact     evidence[16] {};
};

[[nodiscard]] bool ValidateStallSettings(const StallSettings& settings) noexcept;

// Pure classifier: advances only on caller-supplied sample_time_ns (no wall sleeps).
[[nodiscard]] StallResult Observe(const ObservationInput& input, const StallSettings& settings,
                                  ClassifierState* state) noexcept;

} // namespace Kyty::DevTools

#endif /* KYTY_DEVTOOLS_INCLUDE_KYTY_DEVTOOLS_DIAGNOSTICS_STALLCLASSIFIER_H_ */
