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

void AddLane(ObservationInput* in, Domain domain, OperationCode operation, uint64_t instance, uint64_t submitted,
             uint64_t last_change)
{
	const uint32_t i = in->progress.count++;
	in->progress.entries[i].key.domain   = domain;
	in->progress.entries[i].key.instance = instance;
	uint64_t wire = 0;
	(void)in->progress.entries[i].key.TryPack(&wire);
	in->progress.entries[i].record.instance_key   = wire;
	in->progress.entries[i].record.submitted      = submitted;
	in->progress.entries[i].record.operation      = static_cast<uint32_t>(operation);
	in->progress.entries[i].record.state          = static_cast<uint16_t>(ProgressState::Active);
	in->progress.entries[i].record.flags          = static_cast<uint16_t>(ProgressFlag::OperationValid);
	in->progress.entries[i].record.last_change_ns = last_change;
}

void AddGuestThread(ObservationInput* in, uint64_t instance, OperationCode wait, uint64_t last_change)
{
	AddLane(in, Domain::GuestThread, OperationCode::ExecutionBoundary, instance, 3, last_change);
	auto& record        = in->progress.entries[in->progress.count - 1u].record;
	record.state        = static_cast<uint16_t>(wait == OperationCode::Unknown ? ProgressState::Active : ProgressState::Waiting);
	record.auxiliary[0] = static_cast<uint64_t>(wait);
	record.flags |= static_cast<uint16_t>(ProgressFlag::Auxiliary0Valid);
}

void AddIdleCommandLane(ObservationInput* in, uint64_t instance, uint64_t last_change)
{
	AddLane(in, Domain::CommandProcessor, OperationCode::CommandSubmit, instance, 50, last_change);
	auto& record     = in->progress.entries[in->progress.count - 1u].record;
	record.completed = 50;
	record.state     = static_cast<uint16_t>(ProgressState::Idle);
}

} // namespace

TEST(DevToolsClassifier, LongCompileIsJudgedByCompileThreshold)
{
	ClassifierState state {};
	StallSettings   settings {};
	settings.overrides[0]   = {Domain::CommandProcessor, static_cast<uint32_t>(OperationCode::PipelineCompile), 60'000'000'000ull,
	                           180'000'000'000ull};
	settings.override_count = 1;

	// A 40 s synchronous compile is far past the command threshold but is work.
	auto compiling = MakeBaseInput(100'000'000'000ull);
	AddLane(&compiling, Domain::CommandProcessor, OperationCode::PipelineCompile, 1, 8, 60'000'000'000ull);
	EXPECT_EQ(Observe(compiling, settings, &state).category, StallCategory::None);

	auto stuck = MakeBaseInput(250'000'000'000ull);
	AddLane(&stuck, Domain::CommandProcessor, OperationCode::PipelineCompile, 1, 8, 60'000'000'000ull);
	const auto r = Observe(stuck, settings, &state);
	EXPECT_EQ(r.category, StallCategory::CommandProcessorStall);
	EXPECT_NE(r.confirmed_ns, 0u);
	ASSERT_EQ(r.evidence_stored, 1u);
	EXPECT_EQ(r.evidence[0].code, static_cast<uint16_t>(OperationCode::PipelineCompile));
}

TEST(DevToolsClassifier, EveryGuestThreadParkedWithNoCommandWorkIsAGuestDeadlock)
{
	ClassifierState state {};
	StallSettings   settings {};

	auto in = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&in, 1, OperationCode::CondWait, 70'000'000'000ull);
	AddGuestThread(&in, 2, OperationCode::MutexWait, 75'000'000'000ull);
	AddIdleCommandLane(&in, 3, 78'000'000'000ull);
	const auto r = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::GuestDeadlock);
	EXPECT_NE(r.confirmed_ns, 0u);
	// Inferred from parked threads, not from a wait cycle.
	EXPECT_EQ(r.confidence, Confidence::Medium);
	ASSERT_EQ(r.evidence_stored, 2u);
	EXPECT_EQ(r.evidence[0].code, static_cast<uint16_t>(OperationCode::CondWait));
	EXPECT_EQ(r.evidence[1].code, static_cast<uint16_t>(OperationCode::MutexWait));
}

TEST(DevToolsClassifier, RunningGuestThreadRulesOutGuestDeadlock)
{
	ClassifierState state {};
	StallSettings   settings {};

	// One thread runs guest code (a load, a spin, a sleep); parked peers are fine.
	auto in = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&in, 1, OperationCode::CondWait, 70'000'000'000ull);
	AddGuestThread(&in, 2, OperationCode::Unknown, 70'000'000'000ull);
	AddIdleCommandLane(&in, 3, 70'000'000'000ull);
	EXPECT_EQ(Observe(in, settings, &state).category, StallCategory::HealthyIdle);
}

TEST(DevToolsClassifier, CommandWorkRulesOutGuestDeadlock)
{
	ClassifierState state {};
	StallSettings   settings {};

	// Guest threads wait on a batch that is still compiling.
	auto busy = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&busy, 1, OperationCode::EventQueueWait, 70'000'000'000ull);
	AddLane(&busy, Domain::CommandProcessor, OperationCode::PipelineCompile, 2, 9, 98'000'000'000ull);
	EXPECT_EQ(Observe(busy, settings, &state).category, StallCategory::None);

	// A present still in flight can wake threads parked on the flip.
	auto presenting = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&presenting, 1, OperationCode::EventQueueWait, 70'000'000'000ull);
	AddIdleCommandLane(&presenting, 2, 70'000'000'000ull);
	AddLane(&presenting, Domain::Presentation, OperationCode::PresentationPresent, 3, 9, 98'000'000'000ull);
	EXPECT_EQ(Observe(presenting, settings, &state).category, StallCategory::None);

	// Batches that keep completing are progress the guest waits on.
	auto recent = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&recent, 1, OperationCode::EventQueueWait, 70'000'000'000ull);
	AddIdleCommandLane(&recent, 2, 99'900'000'000ull);
	EXPECT_EQ(Observe(recent, settings, &state).category, StallCategory::HealthyIdle);
}

TEST(DevToolsClassifier, CommandLaneWaitingOnGuestMemoryIsGuestPaced)
{
	ClassifierState state {};
	StallSettings   settings {};

	// A WAIT_REG_MEM far past the command threshold while the guest still runs.
	auto loading = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&loading, 1, OperationCode::Unknown, 99'000'000'000ull);
	AddLane(&loading, Domain::CommandProcessor, OperationCode::RegisterMemoryWait, 2, 9, 40'000'000'000ull);
	EXPECT_EQ(Observe(loading, settings, &state).category, StallCategory::None);

	// The guest parks too: nothing will write the label.
	auto parked = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&parked, 1, OperationCode::CondWait, 60'000'000'000ull);
	AddLane(&parked, Domain::CommandProcessor, OperationCode::RegisterMemoryWait, 2, 9, 40'000'000'000ull);
	const auto r = Observe(parked, settings, &state);
	EXPECT_EQ(r.category, StallCategory::GuestDeadlock);
	ASSERT_EQ(r.evidence_stored, 2u);
	EXPECT_EQ(r.evidence[1].code, static_cast<uint16_t>(OperationCode::RegisterMemoryWait));
}

TEST(DevToolsClassifier, UntrackedGuestThreadRulesOutGuestDeadlock)
{
	ClassifierState state {};
	StallSettings   settings {};

	// A guest thread without a lane might be the one still running.
	auto in = MakeBaseInput(100'000'000'000ull);
	AddGuestThread(&in, 1, OperationCode::CondWait, 70'000'000'000ull);
	AddIdleCommandLane(&in, 2, 70'000'000'000ull);
	in.loss.progress.capacity[static_cast<uint32_t>(Domain::GuestThread)].total = 1;
	EXPECT_EQ(Observe(in, settings, &state).category, StallCategory::HealthyIdle);
}

TEST(DevToolsClassifier, BusyLaneYoungerThanThresholdIsNotAStall)
{
	ClassifierState state {};
	StallSettings   settings {};

	// Work in flight for 4 s on a 5 s threshold, sampled repeatedly.
	auto in = MakeBaseInput(100'000'000'000ull);
	AddLane(&in, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 40, 96'000'000'000ull);
	for (int i = 0; i < 100; ++i)
	{
		const auto r = Observe(in, settings, &state);
		EXPECT_EQ(r.category, StallCategory::None);
		EXPECT_EQ(r.confirmed_ns, 0u);
	}
}

TEST(DevToolsClassifier, StuckLaneIsFoundAmongProgressingLanes)
{
	ClassifierState state {};
	StallSettings   settings {};

	// The CP lane has not moved for 20 s while presentation keeps advancing.
	auto in = MakeBaseInput(100'000'000'000ull);
	AddLane(&in, Domain::Presentation, OperationCode::PresentationPresent, 1, 9000, 99'990'000'000ull);
	AddLane(&in, Domain::CommandProcessor, OperationCode::CommandSubmit, 2, 77, 80'000'000'000ull);
	const auto r = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::CommandProcessorStall);
	EXPECT_EQ(r.confidence, Confidence::High);
	EXPECT_NE(r.confirmed_ns, 0u);
	ASSERT_EQ(r.evidence_stored, 1u);
	EXPECT_EQ(r.evidence[0].domain, Domain::CommandProcessor);
	EXPECT_EQ(r.evidence[0].instance, 2u);
}

TEST(DevToolsClassifier, HungGpuQueueOutranksTheLanesWaitingOnIt)
{
	ClassifierState state {};
	StallSettings   settings {};

	auto in = MakeBaseInput(100'000'000'000ull);
	AddLane(&in, Domain::Hle, OperationCode::HleCall, 1, 3, 70'000'000'000ull);
	AddLane(&in, Domain::CommandProcessor, OperationCode::CommandSubmit, 2, 77, 75'000'000'000ull);
	AddLane(&in, Domain::GpuQueue, OperationCode::FenceWait, 3, 12, 80'000'000'000ull);
	const auto r = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::GpuStall);
	EXPECT_EQ(r.evidence_stored, 3u);
}

TEST(DevToolsClassifier, StaleHeartbeatIsUnresponsiveEvenWithBusyLanes)
{
	ClassifierState state {};
	StallSettings   settings {};

	// The worker stopped publishing 16 s ago; its last lane data is not evidence.
	auto in         = MakeBaseInput(100'000'000'000ull);
	in.heartbeat_ns = 84'000'000'000ull;
	AddLane(&in, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 5, 83'000'000'000ull);
	const auto r = Observe(in, settings, &state);
	EXPECT_EQ(r.category, StallCategory::WorkerUnresponsive);
	EXPECT_NE(r.confirmed_ns, 0u);
	EXPECT_EQ(r.evidence_stored, 0u);
}

TEST(DevToolsClassifier, PausedWorkerDoesNotAgeItsLanes)
{
	ClassifierState state {};
	StallSettings   settings {};

	// A batch was in flight when the whole process was paused for 20 s.
	auto paused         = MakeBaseInput(70'000'000'000ull);
	paused.heartbeat_ns = 50'000'000'000ull;
	AddLane(&paused, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 9, 49'900'000'000ull);
	ASSERT_EQ(Observe(paused, settings, &state).category, StallCategory::WorkerUnresponsive);

	// Publication resumes before the batch moves: the pause is not lane age.
	auto resumed = MakeBaseInput(70'500'000'000ull);
	AddLane(&resumed, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 9, 49'900'000'000ull);
	EXPECT_EQ(Observe(resumed, settings, &state).category, StallCategory::None);

	// The same batch still stuck 16 s after the resume is a stall.
	auto stuck = MakeBaseInput(86'600'000'000ull);
	AddLane(&stuck, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 9, 49'900'000'000ull);
	const auto r = Observe(stuck, settings, &state);
	EXPECT_EQ(r.category, StallCategory::CommandProcessorStall);
	EXPECT_NE(r.confirmed_ns, 0u);
}

TEST(DevToolsClassifier, CompletedLaneStartsANewStall)
{
	ClassifierState state {};
	StallSettings   settings {};

	auto first = MakeBaseInput(100'000'000'000ull);
	AddLane(&first, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 5, 80'000'000'000ull);
	ASSERT_EQ(Observe(first, settings, &state).category, StallCategory::CommandProcessorStall);
	const uint64_t first_stall = state.causal_fingerprint;

	// The same lane completed epoch 5 and has now been stuck on epoch 6.
	auto second = MakeBaseInput(140'000'000'000ull);
	AddLane(&second, Domain::CommandProcessor, OperationCode::CommandSubmit, 1, 6, 120'000'000'000ull);
	ASSERT_EQ(Observe(second, settings, &state).category, StallCategory::CommandProcessorStall);
	EXPECT_NE(state.causal_fingerprint, first_stall);
}

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

	// Heartbeat 0 is the protocol's "no heartbeat" value; start at a real time.
	auto t0 = MakeBaseInput(1'000'000'000ull);
	AddHleActive(&t0, 3, 1, 1'000'000'000ull);
	(void)Observe(t0, settings, &state);

	auto t1 = t0;
	t1.sample_time_ns = 4'000'000'000ull;
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
