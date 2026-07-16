#include "Kyty/DevTools/Telemetry/Event.h"
#include "Kyty/DevTools/Telemetry/ThreadInstanceAllocator.h"
#include "Kyty/DevTools/Telemetry/WriterRegistry.h"
#include "Kyty/DevTools/Telemetry/WorkerTelemetry.h"
#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/DevTools/Time/MonotonicClock.h"
#include "Kyty/UnitTest.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

UT_BEGIN(DevToolsEventRing);

using namespace Kyty::DevTools;

TEST(DevToolsEventRing, ThreadInstanceAllocatorStartsAtOneAndNeverReuses)
{
	ThreadInstanceAllocator allocator;
	uint64_t                first  = 0;
	uint64_t                second = 0;
	ASSERT_TRUE(allocator.Allocate(&first));
	ASSERT_TRUE(allocator.Allocate(&second));
	EXPECT_EQ(first, 1u);
	EXPECT_EQ(second, 2u);
	EXPECT_FALSE(allocator.Allocate(nullptr));
}

TEST(DevToolsEventRing, ThreadInstanceAllocatorRejectsProgressRefOverflow)
{
	ThreadInstanceAllocator allocator(ThreadInstanceAllocator::kMaxInstance);
	uint64_t                last = 0;
	ASSERT_TRUE(allocator.Allocate(&last));
	EXPECT_EQ(last, ThreadInstanceAllocator::kMaxInstance);
	EXPECT_FALSE(allocator.Allocate(&last));
}

TEST(DevToolsEventRing, EventRecordHasWireCompatibleFields)
{
	EXPECT_EQ(sizeof(EventRecord), 72u);
	EXPECT_TRUE(std::is_trivially_copyable_v<EventRecord>);
	EXPECT_EQ((WriterKey {7, 11}.Pack()), (uint64_t {11} << 32u) | 7u);
	EXPECT_EQ(static_cast<uint16_t>(Domain::Unknown), 0u);
	EXPECT_EQ(static_cast<uint16_t>(Domain::GuestThread), 1u);
	EXPECT_EQ(static_cast<uint16_t>(Domain::Synchronization), 8u);
	EXPECT_EQ(static_cast<uint16_t>(Domain::Count), 9u);
	EXPECT_EQ(static_cast<uint16_t>(EventId::Unknown), 0u);
	EXPECT_EQ(static_cast<uint16_t>(EventId::ThreadStart), 1u);
	EXPECT_EQ(static_cast<uint16_t>(EventId::WaitEnd), 7u);
	EXPECT_EQ(static_cast<uint16_t>(EventId::Count), 8u);
	EXPECT_EQ(static_cast<uint32_t>(TimelineFlag::TimedWait), 0x40u);
	EXPECT_EQ(static_cast<uint32_t>(ThreadRole::SnapshotPublisher), 11u);
	EXPECT_EQ(static_cast<uint32_t>(ResultCategory::Unsupported), 6u);
	EXPECT_EQ(static_cast<uint32_t>(WaitOutcome::Error), 4u);
	EXPECT_EQ(static_cast<uint32_t>(HleCallKind::WaitEventFlag), 1u);
	EXPECT_EQ(static_cast<uint32_t>(HleCallKind::TriggerEventQueue), 6u);
	EXPECT_EQ(static_cast<uint32_t>(OperationCode::CommandPacket), 0x0302u);
	EXPECT_EQ(static_cast<uint32_t>(OperationCode::RegisterMemoryWait), 0x0804u);
	EXPECT_EQ((static_cast<uint32_t>(OperationCode::CommandPacket) >> 8u) & 0xffu,
	          static_cast<uint16_t>(Domain::CommandProcessor));
	EXPECT_EQ(static_cast<uint32_t>(OperationCode::RegisterMemoryWait) & 0xffff0000u, 0u);
}

TEST(DevToolsEventRing, EventSchemaRejectsInvalidFlagAndDomainCombinations)
{
	EventRecord valid {};
	valid.domain      = static_cast<uint16_t>(Domain::Hle);
	valid.event       = static_cast<uint16_t>(EventId::OperationSubmit);
	valid.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	              static_cast<uint32_t>(TimelineFlag::Payload0Valid) | static_cast<uint32_t>(TimelineFlag::Payload1Valid) |
	              static_cast<uint32_t>(TimelineFlag::Payload3Valid);
	valid.correlation = (uint64_t {1} << 56u) | 7u;
	valid.payload[0]  = static_cast<uint32_t>(OperationCode::HleCall);
	valid.payload[1]  = 11;
	valid.payload[3]  = static_cast<uint32_t>(HleCallKind::WaitEventFlag) | (uint64_t {1} << 32u);
	EXPECT_TRUE(ValidateEventSchema(valid));

	auto wrong_domain       = valid;
	wrong_domain.payload[0] = static_cast<uint32_t>(OperationCode::CommandPacket);
	EXPECT_FALSE(ValidateEventSchema(wrong_domain));

	auto reserved_flag = valid;
	reserved_flag.flags |= 0x80u;
	EXPECT_FALSE(ValidateEventSchema(reserved_flag));
}

TEST(DevToolsEventRing, ResultErrorFlagsAreEventSpecific)
{
	// ThreadExit: ResultError exactly for GuestError/HostError/Timeout/Unsupported.
	EventRecord exit {};
	exit.domain      = static_cast<uint16_t>(Domain::GuestThread);
	exit.event       = static_cast<uint16_t>(EventId::ThreadExit);
	exit.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	             static_cast<uint32_t>(TimelineFlag::Payload0Valid) | static_cast<uint32_t>(TimelineFlag::Payload1Valid);
	exit.correlation = 1;
	exit.payload[0]  = static_cast<uint32_t>(ThreadRole::MainGuest);
	exit.payload[1]  = static_cast<uint32_t>(ResultCategory::Success);
	EXPECT_TRUE(ValidateEventSchema(exit));
	exit.flags |= static_cast<uint32_t>(TimelineFlag::ResultError);
	EXPECT_FALSE(ValidateEventSchema(exit));

	exit.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	             static_cast<uint32_t>(TimelineFlag::Payload0Valid) | static_cast<uint32_t>(TimelineFlag::Payload1Valid) |
	             static_cast<uint32_t>(TimelineFlag::ResultError);
	exit.payload[1]  = static_cast<uint32_t>(ResultCategory::Timeout);
	EXPECT_TRUE(ValidateEventSchema(exit));
	exit.flags &= ~static_cast<uint32_t>(TimelineFlag::ResultError);
	EXPECT_FALSE(ValidateEventSchema(exit));

	// WaitEnd: ResultError exactly for TimedOut/Error.
	EventRecord wait {};
	wait.domain      = static_cast<uint16_t>(Domain::Synchronization);
	wait.event       = static_cast<uint16_t>(EventId::WaitEnd);
	wait.flags       = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	             static_cast<uint32_t>(TimelineFlag::Payload0Valid) | static_cast<uint32_t>(TimelineFlag::Payload1Valid) |
	             static_cast<uint32_t>(TimelineFlag::Payload2Valid);
	wait.correlation = 2;
	wait.payload[0]  = static_cast<uint32_t>(OperationCode::EventFlagWait);
	wait.payload[1]  = 9;
	wait.payload[2]  = static_cast<uint32_t>(WaitOutcome::Satisfied);
	EXPECT_TRUE(ValidateEventSchema(wait));
	wait.flags |= static_cast<uint32_t>(TimelineFlag::ResultError);
	EXPECT_FALSE(ValidateEventSchema(wait));
	wait.payload[2] = static_cast<uint32_t>(WaitOutcome::Error);
	EXPECT_TRUE(ValidateEventSchema(wait));
}

TEST(DevToolsEventRing, MonotonicConversionIsChecked)
{
	uint64_t out = 0;
	EXPECT_TRUE(MonotonicFromPosixTimespec(1, 500, &out));
	EXPECT_EQ(out, 1000000000ull + 500ull);
	EXPECT_FALSE(MonotonicFromPosixTimespec(-1, 0, &out));
	EXPECT_FALSE(MonotonicFromPosixTimespec(0, 1000000000ll, &out));
	EXPECT_FALSE(MonotonicFromPosixTimespec(0, 0, nullptr));

	// Overflow on sec * 1e9.
	const int64_t huge_sec = static_cast<int64_t>(std::numeric_limits<uint64_t>::max() / 1000000000ull) + 1;
	EXPECT_FALSE(MonotonicFromPosixTimespec(huge_sec, 0, &out));

	EXPECT_TRUE(MonotonicFromWindowsCounter(10, 5, &out));
	EXPECT_EQ(out, 2ull * 1000000000ull);
	// 3/2 * 1e9 = 1.5e9 with remainder path: q=1 r=1 → 1e9 + 5e8 = 1.5e9
	EXPECT_TRUE(MonotonicFromWindowsCounter(3, 2, &out));
	EXPECT_EQ(out, 1500000000ull);
	EXPECT_FALSE(MonotonicFromWindowsCounter(1, 0, &out));
	EXPECT_FALSE(MonotonicFromWindowsCounter(1, 1, nullptr));

	// Live clock advances (smoke).
	const uint64_t a = MonotonicNowNs();
	const uint64_t b = MonotonicNowNs();
	EXPECT_GE(b, a);
}

namespace {

EventRecord MakeThreadStart(uint64_t mono_ns, ThreadRole role)
{
	EventRecord r {};
	r.monotonic_ns = mono_ns;
	r.domain       = static_cast<uint16_t>(Domain::GuestThread);
	r.event        = static_cast<uint16_t>(EventId::ThreadStart);
	r.flags        = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	          static_cast<uint32_t>(TimelineFlag::Payload0Valid);
	r.correlation  = 1;
	r.payload[0]   = static_cast<uint32_t>(role);
	// Deliberately garbage sequence/writer_key — registry must overwrite.
	r.sequence   = 0xdeadbeefull;
	r.writer_key = 0xcafebabeull;
	return r;
}

EventRecord MakeThreadExit(uint64_t mono_ns, ThreadRole role)
{
	EventRecord r {};
	r.monotonic_ns = mono_ns;
	r.domain       = static_cast<uint16_t>(Domain::GuestThread);
	r.event        = static_cast<uint16_t>(EventId::ThreadExit);
	r.flags        = static_cast<uint32_t>(TimelineFlag::CorrelationValid) |
	          static_cast<uint32_t>(TimelineFlag::Payload0Valid) | static_cast<uint32_t>(TimelineFlag::Payload1Valid);
	r.correlation  = 1;
	r.payload[0]   = static_cast<uint32_t>(role);
	r.payload[1]   = static_cast<uint32_t>(ResultCategory::Success);
	return r;
}

} // namespace

// WriterRegistry embeds 512 * 256 EventRecords (~9+ MiB). Heap-allocate in tests.
TEST(DevToolsEventRing, ReservesActivatesAndDrainsWriter)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken token {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::MainGuest, 1, &token));
	EXPECT_EQ(token.slot, 0u);
	EXPECT_EQ(token.generation, 1u);

	// Reserved cannot record.
	EXPECT_FALSE(registry->TryRecord(token, MakeThreadStart(100, ThreadRole::MainGuest)));
	const auto inactive_after_reserved = registry->SnapshotLoss();
	EXPECT_GE(inactive_after_reserved.inactive_writer_attempts.total, 1u);

	ASSERT_TRUE(registry->Activate(token));
	ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(100, ThreadRole::MainGuest)));
	ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(200, ThreadRole::MainGuest)));

	EventRecord drained[8] = {};
	const uint32_t n       = registry->Drain(drained, 8);
	ASSERT_EQ(n, 2u);
	EXPECT_EQ(drained[0].monotonic_ns, 100u);
	EXPECT_EQ(drained[1].monotonic_ns, 200u);
	EXPECT_EQ(drained[0].sequence, 1u);
	EXPECT_EQ(drained[1].sequence, 2u);
	EXPECT_EQ(drained[0].writer_key, (WriterKey {token.slot, token.generation}.Pack()));
	// Caller sequence/key ignored.
	EXPECT_NE(drained[0].sequence, 0xdeadbeefull);

	registry->Close(token, MakeThreadExit(300, ThreadRole::MainGuest));
	EventRecord exit_buf[4] = {};
	const uint32_t n_exit   = registry->Drain(exit_buf, 4);
	EXPECT_GE(n_exit, 1u);

	// Failed create: abandon reserved.
	TelemetryWriterToken abandoned {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::GuestPthread, 2, &abandoned));
	registry->Abandon(abandoned);
	EXPECT_FALSE(registry->Activate(abandoned));
}

TEST(DevToolsEventRing, TryRecordOverwritesCallerSequenceAndWriterKey)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken token {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::GraphicsRender, 42, &token));
	ASSERT_TRUE(registry->Activate(token));
	EventRecord r = MakeThreadStart(50, ThreadRole::GraphicsRender);
	r.sequence    = 999;
	r.writer_key  = 888;
	ASSERT_TRUE(registry->TryRecord(token, r));
	EventRecord out[1] = {};
	ASSERT_EQ(registry->Drain(out, 1), 1u);
	EXPECT_EQ(out[0].sequence, 1u);
	EXPECT_EQ(out[0].writer_key, (WriterKey {token.slot, token.generation}.Pack()));
}

TEST(DevToolsEventRing, DropsWhenRingFullAndCountsLoss)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken token {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::MainGuest, 7, &token));
	ASSERT_TRUE(registry->Activate(token));

	for (uint32_t i = 0; i < EventRing::kCapacity; ++i)
	{
		ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(1000u + i, ThreadRole::MainGuest))) << i;
	}
	// 257th attempt drops.
	EXPECT_FALSE(registry->TryRecord(token, MakeThreadStart(1000u + EventRing::kCapacity, ThreadRole::MainGuest)));
	const auto loss = registry->SnapshotLoss();
	EXPECT_EQ(loss.writer[token.slot].total, 1u);
	EXPECT_EQ(loss.writer[token.slot].last_attempted_sequence, EventRing::kCapacity + 1u);
	EXPECT_EQ(loss.aggregate_ring.total, 1u);

	// Drain frees space; sequences continue (process-lifetime attempted counter).
	EventRecord buf[EventRing::kCapacity] = {};
	EXPECT_EQ(registry->Drain(buf, EventRing::kCapacity), EventRing::kCapacity);
	ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(9000, ThreadRole::MainGuest)));
	EventRecord one[1] = {};
	ASSERT_EQ(registry->Drain(one, 1), 1u);
	EXPECT_EQ(one[0].sequence, EventRing::kCapacity + 2u);
}

TEST(DevToolsEventRing, RegistrationCapacityIs512)
{
	auto registry = std::make_unique<WriterRegistry>();
	std::vector<TelemetryWriterToken> tokens;
	tokens.reserve(WriterRegistry::kWriterSlots);
	for (uint32_t i = 0; i < WriterRegistry::kWriterSlots; ++i)
	{
		TelemetryWriterToken t {};
		ASSERT_TRUE(registry->Reserve(ThreadRole::GuestPthread, static_cast<uint64_t>(i) + 1u, &t)) << i;
		tokens.push_back(t);
	}
	TelemetryWriterToken overflow {};
	EXPECT_FALSE(registry->Reserve(ThreadRole::GuestPthread, 99999, &overflow));
	const auto loss = registry->SnapshotLoss();
	EXPECT_GE(loss.registration_capacity.total, 1u);

	// Abandon one and reserve again.
	registry->Abandon(tokens[0]);
	TelemetryWriterToken again {};
	EXPECT_TRUE(registry->Reserve(ThreadRole::MainGuest, 100001, &again));
}

TEST(DevToolsEventRing, DrainMergesWritersDeterministically)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken a {};
	TelemetryWriterToken b {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::MainGuest, 1, &a));
	ASSERT_TRUE(registry->Reserve(ThreadRole::GuestPthread, 2, &b));
	ASSERT_TRUE(registry->Activate(a));
	ASSERT_TRUE(registry->Activate(b));
	// Writer B earlier time, writer A later — order by monotonic_ns first.
	ASSERT_TRUE(registry->TryRecord(a, MakeThreadStart(300, ThreadRole::MainGuest)));
	ASSERT_TRUE(registry->TryRecord(b, MakeThreadStart(100, ThreadRole::GuestPthread)));
	ASSERT_TRUE(registry->TryRecord(b, MakeThreadStart(200, ThreadRole::GuestPthread)));

	EventRecord out[4] = {};
	ASSERT_EQ(registry->Drain(out, 4), 3u);
	EXPECT_EQ(out[0].monotonic_ns, 100u);
	EXPECT_EQ(out[1].monotonic_ns, 200u);
	EXPECT_EQ(out[2].monotonic_ns, 300u);
	EXPECT_EQ(out[0].writer_key, (WriterKey {b.slot, b.generation}.Pack()));
}

TEST(DevToolsEventRing, TimelineHistoryRetainsAcrossEmptyPublications)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken token {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::MainGuest, 3, &token));
	ASSERT_TRUE(registry->Activate(token));
	ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(10, ThreadRole::MainGuest)));
	EventRecord tmp[2] = {};
	ASSERT_EQ(registry->Drain(tmp, 2), 1u);
	EXPECT_EQ(registry->History().Size(), 1u);

	// Empty drain does not clear history.
	EXPECT_EQ(registry->Drain(tmp, 2), 0u);
	EXPECT_EQ(registry->History().Size(), 1u);

	EventRecord newest[2] = {};
	EXPECT_EQ(registry->History().SnapshotNewest(newest, 2), 1u);
	EXPECT_EQ(newest[0].monotonic_ns, 10u);
}

TEST(DevToolsEventRing, SnapshotInventoryIsIndependentOfTimelineRetention)
{
	auto registry = std::make_unique<WriterRegistry>();
	TelemetryWriterToken token {};
	ASSERT_TRUE(registry->Reserve(ThreadRole::SnapshotPublisher, 55, &token));
	ASSERT_TRUE(registry->Activate(token));
	ASSERT_TRUE(registry->TryRecord(token, MakeThreadStart(1, ThreadRole::SnapshotPublisher)));
	EventRecord tmp[4] = {};
	(void)registry->Drain(tmp, 4);

	WriterInventorySnapshot inv {};
	ASSERT_TRUE(registry->SnapshotInventory(&inv));
	EXPECT_NE(inv.inventory_generation, 0u);
	EXPECT_EQ(inv.entries[token.slot].state, WriterState::Active);
	EXPECT_EQ(inv.entries[token.slot].role, ThreadRole::SnapshotPublisher);
	EXPECT_EQ(inv.entries[token.slot].diagnostic_thread_instance, 55u);
	EXPECT_EQ(inv.entries[token.slot].writer_key, (WriterKey {token.slot, token.generation}.Pack()));
}

TEST(DevToolsEventRing, WorkerTelemetryPublishesHandshakeAndLifecycle)
{
	std::vector<uint8_t> mapping(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit init {};
	init.supervisor_start_token = 17;
	init.requested_mode         = RecordingMode::Full;
	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(i + 1u);
	}
	ASSERT_EQ(InitializeProtocolOwner({mapping.data(), mapping.size()}, init), ProtocolResult::Ok);

	WorkerTelemetryOptions options {};
	options.worker_pid                 = 123;
	options.worker_start_token         = 456;
	options.requested_mode             = RecordingMode::Full;
	options.logging_mode               = LoggingMode::Silent;
	options.diagnostic_thread_instance = 99;

	WorkerTelemetry telemetry;
	ASSERT_TRUE(telemetry.Start({mapping.data(), mapping.size()}, options));
	EXPECT_TRUE(telemetry.Active());

	WorkerHandshake handshake {};
	ASSERT_EQ(AcceptWorkerHandshake({mapping.data(), mapping.size()}, init, &handshake), ProtocolResult::Ok);
	EXPECT_EQ(handshake.accepted_mode, RecordingMode::Full);

	ProtocolReadLossState loss {};
	TimelineSnapshot timeline {};
	ASSERT_EQ(ReadTimeline({mapping.data(), mapping.size()}, &loss, &timeline), ProtocolResult::Ok);
	ASSERT_EQ(timeline.count, 1u);
	EXPECT_EQ(timeline.events[0].event, static_cast<uint16_t>(EventId::ThreadStart));

	ASSERT_TRUE(telemetry.Stop());
	EXPECT_FALSE(telemetry.Active());
	uint32_t handshake_state = 0;
	std::memcpy(&handshake_state, mapping.data() + kProtocolHandshakeStateOffset, sizeof(handshake_state));
	EXPECT_EQ(handshake_state, static_cast<uint32_t>(HandshakeState::WorkerClosing));
	ASSERT_EQ(ReadTimeline({mapping.data(), mapping.size()}, &loss, &timeline), ProtocolResult::Ok);
	ASSERT_EQ(timeline.count, 2u);
	EXPECT_EQ(timeline.events[0].event, static_cast<uint16_t>(EventId::ThreadExit));
	EXPECT_EQ(timeline.events[1].event, static_cast<uint16_t>(EventId::ThreadStart));
}

TEST(DevToolsEventRing, WorkerTelemetryAllocatesThreadInstanceWhenUnset)
{
	std::vector<uint8_t> mapping(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit init {};
	init.requested_mode = RecordingMode::Full;
	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(0x40u + i);
	}
	ASSERT_EQ(InitializeProtocolOwner({mapping.data(), mapping.size()}, init), ProtocolResult::Ok);

	WorkerTelemetryOptions options {};
	options.worker_pid         = 123;
	options.worker_start_token = 456;
	options.requested_mode = RecordingMode::Full;

	WorkerTelemetry telemetry;
	ASSERT_TRUE(telemetry.Start({mapping.data(), mapping.size()}, options));

	ProtocolReadLossState loss {};
	TimelineSnapshot timeline {};
	ASSERT_EQ(ReadTimeline({mapping.data(), mapping.size()}, &loss, &timeline), ProtocolResult::Ok);
	ASSERT_EQ(timeline.count, 1u);
	EXPECT_EQ(timeline.events[0].correlation, 1u);

	ASSERT_TRUE(telemetry.Stop());
}

TEST(DevToolsEventRing, WorkerTelemetryRejectsInvalidMappingWithoutActivation)
{
	WorkerTelemetry telemetry;
	WorkerTelemetryOptions options {};
	EXPECT_FALSE(telemetry.Start({}, options));
	EXPECT_FALSE(telemetry.Active());
}

TEST(DevToolsEventRing, WorkerTelemetryMetricsOnlyPublishesNoTimeline)
{
	std::vector<uint8_t> mapping(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit init {};
	init.requested_mode = RecordingMode::MetricsOnly;
	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(0xa0u + i);
	}
	ASSERT_EQ(InitializeProtocolOwner({mapping.data(), mapping.size()}, init), ProtocolResult::Ok);

	WorkerTelemetryOptions options {};
	options.worker_pid         = 123;
	options.worker_start_token = 456;
	options.requested_mode             = RecordingMode::MetricsOnly;

	WorkerTelemetry telemetry;
	ASSERT_TRUE(telemetry.Start({mapping.data(), mapping.size()}, options));

	ProtocolReadLossState loss {};
	ProgressPublication progress {};
	ASSERT_EQ(ReadProgressPublication({mapping.data(), mapping.size()}, &loss, &progress), ProtocolResult::Ok);
	EXPECT_EQ(progress.progress.count, 0u);
	TimelineSnapshot timeline {};
	EXPECT_EQ(ReadTimeline({mapping.data(), mapping.size()}, &loss, &timeline), ProtocolResult::Rejected);

	ASSERT_TRUE(telemetry.Stop());
	EXPECT_EQ(ReadTimeline({mapping.data(), mapping.size()}, &loss, &timeline), ProtocolResult::Rejected);
}

UT_END();
