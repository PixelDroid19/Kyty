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

TEST(DevToolsProtocol, WorkerBootstrapValidatesNonceAndRequestedMode)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.supervisor_start_token = 9;
	init.requested_mode         = RecordingMode::Full;
	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(0x80u + i);
	}
	MutableMappingView mut {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(mut, init), ProtocolResult::Ok);

	RecordingMode requested = RecordingMode::MetricsOnly;
	EXPECT_EQ(ReadWorkerBootstrap({map.data(), map.size()}, init.nonce, &requested), ProtocolResult::Ok);
	EXPECT_EQ(requested, RecordingMode::Full);

	uint8_t wrong_nonce[16] = {};
	EXPECT_EQ(ReadWorkerBootstrap({map.data(), map.size()}, wrong_nonce, &requested), ProtocolResult::Rejected);
	EXPECT_EQ(ReadWorkerBootstrap({nullptr, 0}, init.nonce, &requested), ProtocolResult::InvalidArgument);
}

TEST(DevToolsProtocol, WorkerHandshakeUsesVersionOneWireFields)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.supervisor_pid         = 42;
	init.supervisor_start_token = 77;
	init.requested_mode         = RecordingMode::Full;
	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		init.nonce[i] = static_cast<uint8_t>(0xa0u + i);
	}
	MutableMappingView view {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(view, init), ProtocolResult::Ok);

	for (uint32_t i = 0; i < sizeof(init.nonce); ++i)
	{
		EXPECT_EQ(map[0x020u + i], init.nonce[i]);
	}
	uint64_t parent_pid = 0;
	std::memcpy(&parent_pid, map.data() + 0x030u, sizeof(parent_pid));
	EXPECT_EQ(parent_pid, init.supervisor_pid);
	uint32_t state = 0;
	std::memcpy(&state, map.data() + kProtocolHandshakeStateOffset, sizeof(state));
	EXPECT_EQ(state, static_cast<uint32_t>(HandshakeState::ParentReady));

	WorkerHandshake hs {};
	hs.worker_pid           = 99;
	hs.worker_start_token   = 101;
	hs.accepted_mode        = RecordingMode::Full;
	hs.logging_mode         = LoggingMode::Console;
	hs.shader_cache_state   = ShaderCacheState::PersistentCacheWarm;
	hs.dirty                = 1;
	hs.validation_enabled   = 1;
	hs.resolution_width     = 1920;
	hs.resolution_height    = 1080;
	hs.capabilities[0]      = 1;
	std::memcpy(hs.revision, "0123456789abcdef0123456789abcdef01234567", 40);
	ASSERT_EQ(PublishWorkerHandshake(view, hs), ProtocolResult::Ok);

	uint64_t worker_pid = 0;
	uint64_t worker_start_token = 0;
	std::memcpy(&worker_pid, map.data() + 0x038u, sizeof(worker_pid));
	std::memcpy(&worker_start_token, map.data() + 0x040u, sizeof(worker_start_token));
	EXPECT_EQ(worker_pid, hs.worker_pid);
	EXPECT_EQ(worker_start_token, hs.worker_start_token);
	EXPECT_EQ(map[0x080u], static_cast<uint8_t>('0'));
	EXPECT_EQ(map[0x0a8u], static_cast<uint8_t>(1));
	EXPECT_EQ(map[0x0c0u], static_cast<uint8_t>(LoggingMode::Console));
	EXPECT_EQ(map[0x0c4u], static_cast<uint8_t>(ShaderCacheState::PersistentCacheWarm));
	EXPECT_EQ(map[0x0ccu], static_cast<uint8_t>(0x80));
	EXPECT_EQ(map[0x0d0u], static_cast<uint8_t>(0x38));
	std::memcpy(&state, map.data() + kProtocolHandshakeStateOffset, sizeof(state));
	EXPECT_EQ(state, static_cast<uint32_t>(HandshakeState::WorkerReady));

	WorkerHandshake accepted {};
	ASSERT_EQ(AcceptWorkerHandshake({map.data(), map.size()}, init, &accepted), ProtocolResult::Ok);
	EXPECT_EQ(accepted.worker_pid, hs.worker_pid);
	EXPECT_EQ(accepted.worker_start_token, hs.worker_start_token);
	EXPECT_EQ(accepted.logging_mode, hs.logging_mode);
	EXPECT_EQ(accepted.shader_cache_state, hs.shader_cache_state);
	EXPECT_EQ(accepted.dirty, hs.dirty);
	EXPECT_EQ(accepted.validation_enabled, hs.validation_enabled);
	EXPECT_EQ(accepted.resolution_width, hs.resolution_width);
	EXPECT_EQ(accepted.resolution_height, hs.resolution_height);
	EXPECT_EQ(accepted.capabilities[0], hs.capabilities[0]);
	EXPECT_EQ(std::memcmp(accepted.revision, hs.revision, sizeof(hs.revision)), 0);
	ASSERT_EQ(CloseWorkerHandshake(view), ProtocolResult::Ok);
	WorkerHandshake closing_accepted {};
	EXPECT_EQ(AcceptWorkerHandshake(view, init, &closing_accepted), ProtocolResult::Ok);

	for (uint32_t i = 0x0acu; i < 0x0b0u; ++i)
	{
		EXPECT_EQ(map[i], 0u);
	}
	for (uint32_t i = 0x0dcu; i < 0x100u; ++i)
	{
		EXPECT_EQ(map[i], 0u);
	}
}

TEST(DevToolsProtocol, WorkerHandshakePublishesRejectedTerminalState)
{
	std::vector<uint8_t> map(static_cast<size_t>(kProtocolMappingSize), 0);
	ParentProtocolInit   init {};
	init.requested_mode = RecordingMode::MetricsOnly;
	MutableMappingView mut {map.data(), map.size()};
	ASSERT_EQ(InitializeProtocolOwner(mut, init), ProtocolResult::Ok);

	WorkerHandshake hs {};
	hs.worker_pid         = 123;
	hs.worker_start_token = 456;
	hs.accepted_mode      = RecordingMode::MetricsOnly;
	ASSERT_EQ(PublishWorkerHandshake(mut, hs), ProtocolResult::Ok);

	ASSERT_EQ(RejectWorkerHandshake(mut), ProtocolResult::Ok);
	uint32_t state = 0;
	std::memcpy(&state, map.data() + kProtocolHandshakeStateOffset, sizeof(state));
	EXPECT_EQ(state, static_cast<uint32_t>(HandshakeState::WorkerRejected));
	EXPECT_EQ(RejectWorkerHandshake(mut), ProtocolResult::Rejected);
	WorkerHandshake rejected_accepted {};
	EXPECT_EQ(AcceptWorkerHandshake(mut, init, &rejected_accepted), ProtocolResult::Rejected);
	EXPECT_EQ(CloseWorkerHandshake(mut), ProtocolResult::Rejected);
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
	hs.worker_pid    = 123;
	hs.worker_start_token = 456;
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
	uint64_t publication_heartbeat = 0;
	EXPECT_EQ(ReadPublicationHeartbeat({map.data(), map.size()}, &publication_heartbeat), ProtocolResult::Rejected);

	ASSERT_EQ(PublishProgress(mut, pub), ProtocolResult::Ok);
	std::memcpy(&publication_heartbeat, map.data() + kProtocolPublicationHeartbeatOffset,
	            sizeof(publication_heartbeat));
	EXPECT_NE(publication_heartbeat, 0u);
	uint64_t read_heartbeat = 0;
	ASSERT_EQ(ReadPublicationHeartbeat({map.data(), map.size()}, &read_heartbeat), ProtocolResult::Ok);
	EXPECT_EQ(read_heartbeat, publication_heartbeat);

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
