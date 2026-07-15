#include "Kyty/DevTools/Telemetry/Event.h"
#include "Kyty/DevTools/Time/MonotonicClock.h"
#include "Kyty/UnitTest.h"

#include <cstdint>
#include <limits>
#include <type_traits>

UT_BEGIN(DevToolsEventRing);

using namespace Kyty::DevTools;

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

UT_END();
