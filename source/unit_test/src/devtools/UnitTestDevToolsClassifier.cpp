#include "Kyty/DevTools/Diagnostics/Checksum.h"
#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Diagnostics/StallClassifier.h"
#include "Kyty/UnitTest.h"

#include <cstring>
#include <memory>

UT_BEGIN(DevToolsClassifier);

using namespace Kyty::DevTools;

TEST(DevToolsClassifier, CausalFingerprintUsesEcmaCheckValue)
{
	const char* s = "123456789";
	EXPECT_EQ(Crc64Ecma(s, 9), 0x6C40DF5F0B497347ull);
}

TEST(DevToolsClassifier, ProcessStatusCombinationsAreClosed)
{
	ProcessStatus running {};
	running.liveness = ProcessLiveness::Running;
	EXPECT_TRUE(ValidateProcessStatus(running));

	ProcessStatus exited {};
	exited.liveness    = ProcessLiveness::Terminated;
	exited.termination = ProcessTermination::ExitCode;
	exited.code_valid  = 1;
	exited.code        = 0;
	EXPECT_TRUE(ValidateProcessStatus(exited));

	ProcessStatus bad_running = running;
	bad_running.termination   = ProcessTermination::ExitCode;
	EXPECT_FALSE(ValidateProcessStatus(bad_running));

	ProcessStatus opaque {};
	opaque.liveness    = ProcessLiveness::Terminated;
	opaque.termination = ProcessTermination::OpaquePlatformStatus;
	opaque.code_valid  = 0;
	EXPECT_TRUE(ValidateProcessStatus(opaque));
	opaque.code_valid = 1;
	EXPECT_FALSE(ValidateProcessStatus(opaque));

	ProcessStatus err {};
	err.error          = ProcessStatusError::WaitFailed;
	err.platform_error = 1;
	EXPECT_TRUE(ValidateProcessStatus(err));
	err.liveness = ProcessLiveness::Running;
	EXPECT_FALSE(ValidateProcessStatus(err));
}

namespace {

ObservationInput MakeBaseInput(uint64_t sample_ns)
{
	ObservationInput in {};
	in.sample_time_ns = sample_ns;
	in.heartbeat_ns   = sample_ns;
	in.process.liveness = ProcessLiveness::Running;
	return in;
}

void AddHleActive(ObservationInput* in, uint64_t instance, uint64_t submitted, uint64_t last_change)
{
	const uint32_t i = in->progress.count++;
	in->progress.entries[i].key.domain   = Domain::Hle;
	in->progress.entries[i].key.instance = instance;
	uint64_t wire = 0;
	(void)in->progress.entries[i].key.TryPack(&wire);
	in->progress.entries[i].record.instance_key   = wire;
	in->progress.entries[i].record.submitted      = submitted;
	in->progress.entries[i].record.completed      = 0;
	in->progress.entries[i].record.operation      = static_cast<uint32_t>(OperationCode::HleCall);
	in->progress.entries[i].record.state          = static_cast<uint16_t>(ProgressState::Active);
	in->progress.entries[i].record.flags          = static_cast<uint16_t>(ProgressFlag::OperationValid);
	in->progress.entries[i].record.last_change_ns = last_change;
	in->progress.entries[i].record.correlation    = instance;
}

} // namespace

TEST(DevToolsClassifier, ClassifiesHleStallAfterVirtualThreshold)
{
	ClassifierState state {};
	StallSettings   settings {};
	settings.suspected_after_ns = 5'000'000'000ull;
	settings.confirmed_after_ns = 15'000'000'000ull;

	auto t0 = MakeBaseInput(1'000'000'000ull);
	AddHleActive(&t0, 7, 1, 1'000'000'000ull);
	auto r0 = Observe(t0, settings, &state);
	EXPECT_EQ(r0.category, StallCategory::None); // not yet suspected

	auto t1 = t0;
	t1.sample_time_ns = 1'000'000'000ull + 6'000'000'000ull;
	t1.heartbeat_ns   = t1.sample_time_ns;
	auto r1           = Observe(t1, settings, &state);
	EXPECT_EQ(r1.category, StallCategory::HleStall);
	EXPECT_GE(static_cast<int>(r1.confidence), static_cast<int>(Confidence::Medium));
	EXPECT_EQ(r1.terminal, 0);

	auto t2 = t1;
	t2.sample_time_ns = 1'000'000'000ull + 16'000'000'000ull;
	t2.heartbeat_ns   = t2.sample_time_ns;
	auto r2           = Observe(t2, settings, &state);
	EXPECT_EQ(r2.category, StallCategory::HleStall);
	EXPECT_EQ(r2.confidence, Confidence::High);
	EXPECT_EQ(r2.terminal, 0); // live stall continues sampling
	EXPECT_NE(r2.confirmed_ns, 0u);
}

TEST(DevToolsClassifier, HealthyIdle)
{
	ClassifierState state {};
	StallSettings   settings {};
	auto            in = MakeBaseInput(5'000'000'000ull);
	// Idle progress only.
	in.progress.count = 1;
	in.progress.entries[0].key.domain   = Domain::GuestThread;
	in.progress.entries[0].key.instance = 1;
	uint64_t wire = 0;
	ASSERT_TRUE(in.progress.entries[0].key.TryPack(&wire));
	in.progress.entries[0].record.instance_key = wire;
	in.progress.entries[0].record.state       = static_cast<uint16_t>(ProgressState::Idle);

	auto r = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::HealthyIdle);
	EXPECT_EQ(r.confidence, Confidence::High);
}

TEST(DevToolsClassifier, ProcessExitedAndCrashedAreTerminal)
{
	ClassifierState state {};
	StallSettings   settings {};
	auto            in = MakeBaseInput(10);
	in.process.liveness    = ProcessLiveness::Terminated;
	in.process.termination = ProcessTermination::ExitCode;
	in.process.code_valid  = 1;
	in.process.code        = 0;
	auto r                 = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::ProcessExited);
	EXPECT_EQ(r.terminal, 1);
	EXPECT_EQ(r.confidence, Confidence::High);

	state = {};
	in.process.termination = ProcessTermination::Signal;
	in.process.code        = 11;
	r                      = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::ProcessCrashed);
	EXPECT_EQ(r.terminal, 1);

	state = {};
	in.process.termination = ProcessTermination::OpaquePlatformStatus;
	in.process.code_valid  = 0;
	in.process.code        = 0;
	r                      = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::ProcessTerminated);
}

TEST(DevToolsClassifier, StatusErrorNeverEntersTerminalClassifier)
{
	ClassifierState state {};
	StallSettings   settings {};
	auto            in = MakeBaseInput(10);
	in.process.error          = ProcessStatusError::QueryFailed;
	in.process.platform_error = 5;
	auto r                    = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::UnknownStall);
	EXPECT_EQ(r.confidence, Confidence::Low);
	EXPECT_EQ(r.terminal, 0);
}

TEST(DevToolsClassifier, LossPreventsHighConfidenceOnConfirmedStall)
{
	ClassifierState state {};
	StallSettings   settings {};
	settings.suspected_after_ns = 1'000'000'000ull;
	settings.confirmed_after_ns = 2'000'000'000ull;

	auto t0 = MakeBaseInput(0);
	AddHleActive(&t0, 3, 1, 0);
	(void)Observe(t0, settings, &state);

	auto t1 = t0;
	t1.sample_time_ns = 3'000'000'000ull;
	t1.heartbeat_ns   = t1.sample_time_ns;
	t1.loss.skipped_publications.total = 1;
	auto r                             = Observe(t1, settings, &state);
	EXPECT_EQ(r.category, StallCategory::HleStall);
	EXPECT_NE(r.confidence, Confidence::High);
}

TEST(DevToolsClassifier, CausalKeyIgnoresTimeOnlyResample)
{
	ClassifierState state {};
	StallSettings   settings {};
	settings.suspected_after_ns = 5'000'000'000ull;
	settings.confirmed_after_ns = 15'000'000'000ull;

	auto t0 = MakeBaseInput(100);
	AddHleActive(&t0, 1, 1, 100);
	(void)Observe(t0, settings, &state);
	const uint64_t fp0   = state.causal_fingerprint;
	const uint64_t first = state.first_observed_ns;
	EXPECT_EQ(first, 100u);

	auto t1 = t0;
	t1.sample_time_ns = 1'000'000'000ull;
	t1.heartbeat_ns   = t1.sample_time_ns;
	(void)Observe(t1, settings, &state);
	EXPECT_EQ(state.causal_fingerprint, fp0);
	EXPECT_EQ(state.first_observed_ns, first); // time-only resample does not reset
}

UT_END();
