#include "Kyty/DevTools/Diagnostics/Checksum.h"
#include "Kyty/DevTools/Protocol/Protocol.h"
#include "Kyty/UnitTest.h"

#include <cstring>
#include <vector>

UT_BEGIN(DevToolsProtocol);

using namespace Kyty::DevTools;

TEST(DevToolsProtocol, Crc64MatchesEcmaCheckValue)
{
	const char* s = "123456789";
	EXPECT_EQ(Crc64Ecma(s, 9), 0x6C40DF5F0B497347ull);
}

TEST(DevToolsProtocol, HeaderMagicMajorMinorAndMappingSize)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.supervisor_pid         = 42;
	init.supervisor_start_token = 7;
	init.requested_mode         = RecordingMode::Full;
	for (int i = 0; i < 16; ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(i + 1);
	}
	MutableMappingView view {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(view, init), ProtocolResult::Ok);
	EXPECT_EQ(ProtocolMagicAt(map.data()), kProtocolMagic);
	EXPECT_EQ(ProtocolMajorAt(map.data()), 1);
	EXPECT_EQ(ProtocolMinorAt(map.data()), 0);
	EXPECT_EQ(kProtocolMappingSize, 0x141000ull);
	EXPECT_EQ(kProtocolHeaderSize, 0x1000u);
	EXPECT_EQ(kProgressBufferAOffset, 0x001000ull);
	EXPECT_EQ(kProgressBufferBOffset, 0x021000ull);
	EXPECT_EQ(kProgressBufferSize, 0x020000ull);
	EXPECT_EQ(kTimelineBufferAOffset, 0x041000ull);
	EXPECT_EQ(kTimelineBufferBOffset, 0x0c1000ull);
	EXPECT_EQ(kEventRecordWireSize, 72u);
	EXPECT_EQ(kTimelineMaxPayloadSize, 0x48000u);
	EXPECT_EQ(static_cast<uint32_t>(RecordingMode::MetricsOnly), 1u);
	EXPECT_EQ(static_cast<uint32_t>(RecordingMode::Full), 2u);
	EXPECT_EQ(static_cast<uint32_t>(LoggingMode::Silent), 1u);
	EXPECT_EQ(static_cast<uint32_t>(ShaderCacheState::PersistentCacheWarm), 3u);
}

TEST(DevToolsProtocol, ProgressPublicationRoundTrips)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.supervisor_pid         = 1;
	init.supervisor_start_token = 2;
	init.requested_mode         = RecordingMode::MetricsOnly;
	MutableMappingView mut {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(mut, init), ProtocolResult::Ok);

	WorkerHandshake hs {};
	hs.accepted_mode = RecordingMode::MetricsOnly;
	hs.logging_mode  = LoggingMode::Silent;
	ASSERT_EQ(PublishWorkerHandshake(mut, hs), ProtocolResult::Ok);
	WorkerHandshake accepted {};
	ASSERT_EQ(AcceptWorkerHandshake(mut, init, &accepted), ProtocolResult::Ok);
	EXPECT_EQ(accepted.accepted_mode, RecordingMode::MetricsOnly);

	ProgressPublication pub {};
	pub.progress.count = 1;
	pub.progress.entries[0].key.domain   = Domain::Hle;
	pub.progress.entries[0].key.instance = 9;
	uint64_t wire = 0;
	ASSERT_TRUE(pub.progress.entries[0].key.TryPack(&wire));
	pub.progress.entries[0].record.instance_key = wire;
	pub.progress.entries[0].record.submitted    = 3;
	pub.progress.entries[0].record.state        = static_cast<uint16_t>(ProgressState::Active);
	pub.gpu_fault.state = GpuFaultState::NotObserved;
	pub.measurement.frame_count = 12;
	pub.writer_loss.aggregate_ring.total = 4;

	ASSERT_EQ(PublishProgress(mut, pub), ProtocolResult::Ok);

	ProgressPublication out {};
	ProtocolReadLossState loss {};
	ConstMappingView      ro {map.data(), map.size()};
	ASSERT_EQ(ReadProgressPublication(ro, &loss, &out), ProtocolResult::Ok);
	EXPECT_EQ(out.progress.count, 1u);
	EXPECT_EQ(out.progress.entries[0].record.submitted, 3u);
	EXPECT_EQ(out.measurement.frame_count, 12u);
	EXPECT_EQ(out.writer_loss.aggregate_ring.total, 4u);
	EXPECT_EQ(out.gpu_fault.state, GpuFaultState::NotObserved);
	EXPECT_EQ(loss.rejected_samples.total, 0u);

	ProtocolHealthSnapshot health {};
	ASSERT_EQ(ReadProtocolHealth(ro, &health), ProtocolResult::Ok);
	EXPECT_EQ(health.aggregate_ring.total, 4u);
}

TEST(DevToolsProtocol, RejectsCorruptProgressCrc)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.requested_mode = RecordingMode::Full;
	MutableMappingView mut {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(mut, init), ProtocolResult::Ok);

	ProgressPublication pub {};
	pub.gpu_fault.state = GpuFaultState::NotObserved;
	ASSERT_EQ(PublishProgress(mut, pub), ProtocolResult::Ok);

	// Flip a body byte after CRC was sealed.
	map[static_cast<size_t>(kProgressBufferBOffset + 40)] ^= 0xffu;

	ProgressPublication out {};
	ProtocolReadLossState loss {};
	ConstMappingView      ro {map.data(), map.size()};
	EXPECT_EQ(ReadProgressPublication(ro, &loss, &out), ProtocolResult::Corrupt);
	EXPECT_GE(loss.rejected_samples.total, 1u);
}

TEST(DevToolsProtocol, TimelineRoundTripAndHealthControls)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.requested_mode = RecordingMode::Full;
	MutableMappingView mut {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(mut, init), ProtocolResult::Ok);

	TimelineSnapshot tl {};
	tl.generation = 5;
	tl.count      = 2;
	tl.events[0].sequence     = 1;
	tl.events[0].monotonic_ns = 10;
	tl.events[0].domain       = static_cast<uint16_t>(Domain::Hle);
	tl.events[0].event        = static_cast<uint16_t>(EventId::OperationSubmit);
	tl.events[1].sequence     = 2;
	tl.events[1].monotonic_ns = 20;
	tl.events[1].domain       = static_cast<uint16_t>(Domain::Hle);
	tl.events[1].event        = static_cast<uint16_t>(EventId::OperationComplete);
	ASSERT_EQ(PublishTimeline(mut, tl), ProtocolResult::Ok);

	TimelineSnapshot out {};
	ProtocolReadLossState loss {};
	ConstMappingView      ro {map.data(), map.size()};
	ASSERT_EQ(ReadTimeline(ro, &loss, &out), ProtocolResult::Ok);
	EXPECT_EQ(out.count, 2u);
	EXPECT_EQ(out.generation, 5u);
	EXPECT_EQ(out.events[1].monotonic_ns, 20u);

	GlobalLossCounter skip {3, 99};
	ASSERT_EQ(PublishWorkerControl(mut, ControlCell::SkippedPublications, skip), ProtocolResult::Ok);
	// Parent owns rejected samples — child publish rejected.
	EXPECT_EQ(PublishWorkerControl(mut, ControlCell::RejectedSamples, skip), ProtocolResult::Rejected);

	ProtocolHealthSnapshot health {};
	ASSERT_EQ(ReadProtocolHealth(ro, &health), ProtocolResult::Ok);
	EXPECT_EQ(health.skipped_publications.total, 3u);
}

TEST(DevToolsProtocol, GpuFaultValidationIsClosed)
{
	GpuFaultSnapshot ok {};
	ok.state = GpuFaultState::NotObserved;
	EXPECT_TRUE(ValidateGpuFaultSnapshot(ok));
	ok.vendor_binary_size = 1;
	EXPECT_FALSE(ValidateGpuFaultSnapshot(ok));

	GpuFaultSnapshot fail {};
	fail.state                = GpuFaultState::CountsFailed;
	fail.flags               = 1;
	fail.device_lost_result  = GpuFaultResultDeviceLost;
	fail.capture_monotonic_ns = 1;
	EXPECT_TRUE(ValidateGpuFaultSnapshot(fail));
	fail.device_lost_result = 0;
	EXPECT_FALSE(ValidateGpuFaultSnapshot(fail));
}

UT_END();
