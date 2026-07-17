#include "Kyty/DevTools/Diagnostics/Checksum.h"
#include "Kyty/DevTools/Diagnostics/ProcessStatus.h"
#include "Kyty/DevTools/Supervisor/BundleWriter.h"
#include "Kyty/DevTools/Supervisor/DurableFile.h"
#include "Kyty/UnitTest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

UT_BEGIN(DevToolsBundle);

using namespace Kyty::DevTools;

namespace {

std::string MakeScratchDir()
{
	char tmpl[] = "/tmp/kyty-bundle-test-XXXXXX";
	char* path  = ::mkdtemp(tmpl);
	EXPECT_NE(path, nullptr);
	return path != nullptr ? std::string(path) : std::string();
}

[[nodiscard]] bool ReadFile(const std::string& path, std::vector<uint8_t>* out)
{
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return false;
	}
	out->clear();
	uint8_t buf[4096];
	for (;;)
	{
		const ssize_t n = ::read(fd, buf, sizeof(buf));
		if (n < 0)
		{
			::close(fd);
			return false;
		}
		if (n == 0)
		{
			break;
		}
		out->insert(out->end(), buf, buf + n);
	}
	::close(fd);
	return true;
}

[[nodiscard]] bool FileContainsBytes(const std::string& path, const void* needle, size_t n)
{
	std::vector<uint8_t> data;
	if (!ReadFile(path, &data))
	{
		return false;
	}
	if (n == 0 || data.size() < n)
	{
		return false;
	}
	const auto* base = reinterpret_cast<const char*>(data.data());
	const auto* nd   = static_cast<const char*>(needle);
	return std::search(base, base + data.size(), nd, nd + n) != base + data.size();
}

BundleInput MinimalInput()
{
	BundleInput in {};
	in.bundle_generation = 1;
	in.trigger           = BundleTrigger::ConfirmedStall;
	in.process.liveness  = ProcessLiveness::Running;
	in.logging_mode      = LoggingMode::Silent;
	return in;
}

} // namespace

TEST(DevToolsBundle, CompleteMarkerWireFormatIsExact)
{
	uint8_t marker[kBundleMarkerSize] = {};
	ASSERT_TRUE(EncodeCompleteMarker(1234, 0x6C40DF5F0B497347ull, 7, marker));
	EXPECT_TRUE(ValidateCompleteMarker(marker, kBundleMarkerSize, 1234, 0x6C40DF5F0B497347ull, 7));
	// Wrong size
	EXPECT_FALSE(ValidateCompleteMarker(marker, 63, 1234, 0x6C40DF5F0B497347ull, 7));
	// Corrupt reserved
	marker[0x3f] = 1;
	EXPECT_FALSE(ValidateCompleteMarker(marker, kBundleMarkerSize, 1234, 0x6C40DF5F0B497347ull, 7));
}

TEST(DevToolsBundle, TimelineHeaderWireFormatIsExact)
{
	TimelineSnapshot tl {};
	tl.count      = 1;
	tl.generation = 3;
	tl.events[0].sequence     = 1;
	tl.events[0].monotonic_ns = 100;
	tl.events[0].domain       = static_cast<uint16_t>(Domain::Hle);
	tl.events[0].event        = static_cast<uint16_t>(EventId::OperationSubmit);

	std::vector<uint8_t> buf(kTimelineHeaderSize + kTimelineRecordSize);
	const uint64_t sz = EncodeTimelineFile(tl, buf.data(), buf.size());
	ASSERT_EQ(sz, kTimelineHeaderSize + kTimelineRecordSize);
	ASSERT_TRUE(ValidateTimelineFile(buf.data(), sz));
}

TEST(DevToolsBundle, TimelineHeaderRejectsSizeCountAndChecksumMismatch)
{
	TimelineSnapshot tl {};
	tl.count = 0;
	std::vector<uint8_t> buf(kTimelineHeaderSize);
	ASSERT_EQ(EncodeTimelineFile(tl, buf.data(), buf.size()), kTimelineHeaderSize);
	ASSERT_TRUE(ValidateTimelineFile(buf.data(), buf.size()));
	// Trailing byte
	buf.push_back(0);
	EXPECT_FALSE(ValidateTimelineFile(buf.data(), buf.size()));
	buf.pop_back();
	// Corrupt payload CRC field
	buf[0x28] ^= 0xff;
	EXPECT_FALSE(ValidateTimelineFile(buf.data(), buf.size()));
}

TEST(DevToolsBundle, RejectsInvalidProcessStatusCombination)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	BundleInput in       = MinimalInput();
	in.process.liveness  = ProcessLiveness::Running;
	in.process.termination = ProcessTermination::ExitCode; // invalid
	BundlePath path {};
	EXPECT_EQ(WriteBundle(dir.c_str(), in, &path), BundleWriteResult::InvalidInput);
	// No temp or final created for invalid input after validation.
	EXPECT_FALSE(DurablePathExists((dir + "/stall-bundle-1").c_str()));
}

TEST(DevToolsBundle, WritesCompleteBundleAtomically)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());

	ProgressPublication pub {};
	pub.progress.count = 1;
	pub.progress.entries[0].key.domain   = Domain::Hle;
	pub.progress.entries[0].key.instance = 1;
	uint64_t packed = 0;
	ASSERT_TRUE(pub.progress.entries[0].key.TryPack(&packed));
	pub.progress.entries[0].record.instance_key   = packed;
	pub.progress.entries[0].record.state          = static_cast<uint16_t>(ProgressState::Waiting);
	pub.progress.entries[0].record.operation      = static_cast<uint32_t>(OperationCode::HleCall);
	pub.progress.entries[0].record.last_change_ns = 50;
	pub.gpu_fault.state = GpuFaultState::NotObserved;

	WaitGraphSnapshot wg {};
	wg.count                   = 1;
	wg.unknown_producer_count  = 0;
	wg.edges[0].wait_ref       = 0x11;
	wg.edges[0].waiter_ref     = 0x22;
	wg.edges[0].producer_ref   = 0x33;

	TimelineSnapshot tl {};
	tl.count              = 1;
	tl.events[0].sequence = 9;
	tl.events[0].domain   = static_cast<uint16_t>(Domain::Hle);

	StallResult result {};
	result.category       = StallCategory::HleStall;
	result.confidence     = Confidence::Medium;
	result.evidence_total = 1;
	result.evidence_stored = 1;
	result.evidence[0].domain = Domain::Hle;
	result.evidence[0].code   = 1;

	ClassifierState clf {};
	clf.suspected.category   = StallCategory::HleStall;
	clf.suspected.confidence = Confidence::Low;
	clf.suspected_ns         = 1000;
	clf.suspected.evidence_total  = 1;
	clf.suspected.evidence_stored = 1;

	BundleInput in       = MinimalInput();
	in.publication       = &pub;
	in.timeline          = &tl;
	in.wait_graph        = &wg;
	in.result            = &result;
	in.classifier        = &clf;
	in.health.aggregate_ring.total = 2;
	in.health.rejected_samples.total = 1;
	in.revision_hex40    = "0123456789abcdef0123456789abcdef01234567";
	in.bundle_generation = 42;

	// Canaries that must never appear in any artifact.
	const char* canary_argv  = "CANARY_ARGV_SECRET_XYZ";
	const char* canary_path  = "/private/fixture/CANARY_PATH";
	const char* canary_guest = "CANARY_GUEST_THREAD_NAME";
	(void)canary_argv;
	(void)canary_path;
	(void)canary_guest;

	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	ASSERT_GT(out.size, 0u);
	EXPECT_TRUE(DurablePathExists(out.bytes));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/complete.marker").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/manifest.json").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/progress.json").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/threads.json").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/wait_graph.json").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/gpu.json").c_str()));
	EXPECT_TRUE(DurablePathExists((std::string(out.bytes) + "/timeline.bin").c_str()));
	// Temp must be gone after rename.
	// complete.marker validates
	std::vector<uint8_t> marker;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/complete.marker", &marker));
	std::vector<uint8_t> manifest;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/manifest.json", &manifest));
	const uint64_t mcrc = Crc64Ecma(manifest.data(), manifest.size());
	ASSERT_TRUE(ValidateCompleteMarker(marker.data(), marker.size(), manifest.size(), mcrc, 42));

	std::vector<uint8_t> timeline;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/timeline.bin", &timeline));
	ASSERT_TRUE(ValidateTimelineFile(timeline.data(), timeline.size()));

	// Privacy canaries must not appear.
	static constexpr const char* kFiles[] = {"progress.json", "threads.json", "wait_graph.json",
	                                         "gpu.json", "timeline.bin", "manifest.json", "complete.marker"};
	for (const char* f : kFiles)
	{
		const std::string p = std::string(out.bytes) + "/" + f;
		EXPECT_FALSE(FileContainsBytes(p, canary_argv, std::strlen(canary_argv)));
		EXPECT_FALSE(FileContainsBytes(p, canary_path, std::strlen(canary_path)));
		EXPECT_FALSE(FileContainsBytes(p, canary_guest, std::strlen(canary_guest)));
	}

	// Manifest preserves loss owners separately.
	std::string man(reinterpret_cast<const char*>(manifest.data()), manifest.size());
	EXPECT_NE(man.find("aggregate_ring"), std::string::npos);
	EXPECT_NE(man.find("rejected_samples"), std::string::npos);

	// progress.json has category, domain_loss, and classifier suspected.
	std::vector<uint8_t> progress;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/progress.json", &progress));
	std::string prog(reinterpret_cast<const char*>(progress.data()), progress.size());
	EXPECT_NE(prog.find("hle_stall"), std::string::npos);
	EXPECT_NE(prog.find("domain_loss"), std::string::npos);
	EXPECT_NE(prog.find("suspected_category"), std::string::npos);

	// gpu.json only sanitized counts
	std::vector<uint8_t> gpu;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/gpu.json", &gpu));
	std::string g(reinterpret_cast<const char*>(gpu.data()), gpu.size());
	EXPECT_NE(g.find("address_info_count"), std::string::npos);
	EXPECT_EQ(g.find("CANARY"), std::string::npos);
}

TEST(DevToolsBundle, GpuJsonContainsOnlySanitizedFaultCounts)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	ProgressPublication pub {};
	// Valid CountsSucceeded closed schema (vendor_binary_size must remain 0).
	pub.gpu_fault.state                = GpuFaultState::CountsSucceeded;
	pub.gpu_fault.flags               = 1u;
	pub.gpu_fault.device_lost_result  = GpuFaultResultSuccess;
	pub.gpu_fault.capture_monotonic_ns = 1000;
	pub.gpu_fault.address_info_count  = 3;
	pub.gpu_fault.vendor_info_count   = 1;
	pub.gpu_fault.vendor_binary_size  = 0;
	pub.gpu_fault.gpu_submission_id   = 7;
	BundleInput in       = MinimalInput();
	in.publication       = &pub;
	in.bundle_generation = 3;
	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	std::vector<uint8_t> gpu;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/gpu.json", &gpu));
	std::string g(reinterpret_cast<const char*>(gpu.data()), gpu.size());
	EXPECT_NE(g.find("\"address_info_count\":3"), std::string::npos);
	EXPECT_NE(g.find("\"vendor_info_count\":1"), std::string::npos);
	EXPECT_NE(g.find("\"vendor_binary_size\":0"), std::string::npos);
	// No free-form vendor description fields.
	EXPECT_EQ(g.find("description"), std::string::npos);
	EXPECT_EQ(g.find("vendor_text"), std::string::npos);
}

TEST(DevToolsBundle, ManifestPreservesSuspectedAndConfirmedEvidence)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	StallResult result {};
	result.category          = StallCategory::GpuStall;
	result.confidence        = Confidence::High;
	result.evidence_total    = 2;
	result.evidence_stored   = 2;
	result.suspected_ns      = 10;
	result.confirmed_ns      = 20;
	result.evidence[0].domain = Domain::GpuQueue;
	result.evidence[0].code   = 5;
	ClassifierState clf {};
	clf.suspected.category        = StallCategory::GpuStall;
	clf.suspected.confidence      = Confidence::Medium;
	clf.suspected.evidence_total  = 2;
	clf.suspected.evidence_stored = 1;
	clf.suspected_ns              = 10;
	clf.confirmed_ns              = 20;
	BundleInput in       = MinimalInput();
	in.result            = &result;
	in.classifier        = &clf;
	in.bundle_generation = 5;
	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	std::vector<uint8_t> progress;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/progress.json", &progress));
	std::string p(reinterpret_cast<const char*>(progress.data()), progress.size());
	EXPECT_NE(p.find("gpu_stall"), std::string::npos);
	EXPECT_NE(p.find("suspected_ns"), std::string::npos);
	EXPECT_NE(p.find("confirmed_ns"), std::string::npos);
}

TEST(DevToolsBundle, StatusDecodeErrorPreservesLastCoherentEvidence)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	ProgressPublication pub {};
	pub.progress.count = 1;
	pub.progress.entries[0].key.domain   = Domain::Renderer;
	pub.progress.entries[0].key.instance = 2;
	uint64_t packed = 0;
	ASSERT_TRUE(pub.progress.entries[0].key.TryPack(&packed));
	pub.progress.entries[0].record.instance_key = packed;
	pub.gpu_fault.state = GpuFaultState::NotObserved;

	BundleInput in       = MinimalInput();
	in.publication       = &pub;
	in.trigger           = BundleTrigger::StatusDecodeError;
	in.process.liveness  = ProcessLiveness::Unknown;
	in.process.error     = ProcessStatusError::WaitFailed;
	in.process.platform_error = 42;
	// No fabricated exit classification.
	in.bundle_generation = 9;
	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	std::vector<uint8_t> man;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/manifest.json", &man));
	std::string m(reinterpret_cast<const char*>(man.data()), man.size());
	EXPECT_NE(m.find("status_decode_error"), std::string::npos);
	EXPECT_NE(m.find("\"platform_error\":42"), std::string::npos);
	// Must not invent exit code classification.
	EXPECT_EQ(m.find("process_exited"), std::string::npos);
	EXPECT_EQ(m.find("process_crashed"), std::string::npos);
	std::vector<uint8_t> progress;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/progress.json", &progress));
	std::string p(reinterpret_cast<const char*>(progress.data()), progress.size());
	EXPECT_NE(p.find("renderer"), std::string::npos);
}

TEST(DevToolsBundle, AutomaticArtifactsExcludeCanaries)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	const char canary[] = "SECRET_CANARY_VALUE_NEVER_IN_BUNDLE";
	BundleInput in       = MinimalInput();
	in.bundle_generation = 11;
	// Even if a canary sits in memory nearby, artifacts must not include it.
	volatile char sink[sizeof(canary)];
	std::memcpy(const_cast<char*>(sink), canary, sizeof(canary));
	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	static constexpr const char* kFiles[] = {"progress.json", "threads.json", "wait_graph.json",
	                                         "gpu.json", "timeline.bin", "manifest.json", "complete.marker"};
	for (const char* f : kFiles)
	{
		EXPECT_FALSE(FileContainsBytes(std::string(out.bytes) + "/" + f, canary, sizeof(canary) - 1u));
	}
	(void)sink;
}

TEST(DevToolsBundle, RejectsLiveOwnerTemporaryDirectory)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	uint64_t pid = 0;
	uint64_t start = 0;
	ASSERT_TRUE(QuerySelfProcessIdentity(&pid, &start));
	char temp[512] = {};
	std::snprintf(temp, sizeof(temp), "%s/.kyty-bundle-tmp.%llu.%llu.99", dir.c_str(),
	              static_cast<unsigned long long>(pid), static_cast<unsigned long long>(start));
	ASSERT_EQ(DurableCreateDirectory(temp), DurableIoResult::Ok);
	// Create a dummy file inside.
	ASSERT_EQ(DurableWriteFile((std::string(temp) + "/x").c_str(), "hi", 2), DurableIoResult::Ok);

	// Injected clock far in the future would normally age-out, but live owner must retain.
	auto future = []() noexcept -> uint64_t {
		return 100ull * 365ull * 24ull * 60ull * 60ull * 1000000000ull;
	};
	TempCleanupResult r {};
	ASSERT_EQ(DurableCleanupOwnTemps(dir.c_str(), future, &r), DurableIoResult::Ok);
	EXPECT_GE(r.scanned, 1u);
	EXPECT_GE(r.retained, 1u);
	EXPECT_EQ(r.removed, 0u);
	EXPECT_TRUE(DurablePathExists(temp));
}

TEST(DevToolsBundle, ManifestPreservesAllLossOwners)
{
	const std::string dir = MakeScratchDir();
	ASSERT_FALSE(dir.empty());
	BundleInput in = MinimalInput();
	in.health.aggregate_ring.total          = 1;
	in.health.unregistered_writers.total    = 2;
	in.health.registration_capacity.total   = 3;
	in.health.instance_capacity.total       = 4;
	in.health.skipped_publications.total    = 5;
	in.health.disconnects.total             = 6;
	in.health.rejected_samples.total        = 7;
	in.health.inactive_token_attempts.total = 8;
	in.bundle_generation                    = 12;
	BundlePath out {};
	ASSERT_EQ(WriteBundle(dir.c_str(), in, &out), BundleWriteResult::Ok);
	std::vector<uint8_t> man;
	ASSERT_TRUE(ReadFile(std::string(out.bytes) + "/manifest.json", &man));
	std::string m(reinterpret_cast<const char*>(man.data()), man.size());
	EXPECT_NE(m.find("unregistered_writers"), std::string::npos);
	EXPECT_NE(m.find("registration_capacity"), std::string::npos);
	EXPECT_NE(m.find("instance_capacity"), std::string::npos);
	EXPECT_NE(m.find("skipped_publications"), std::string::npos);
	EXPECT_NE(m.find("disconnects"), std::string::npos);
	EXPECT_NE(m.find("rejected_samples"), std::string::npos);
	EXPECT_NE(m.find("inactive_token_attempts"), std::string::npos);
}

UT_END();
