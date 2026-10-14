#include "Kyty/UnitTest.h"

#include "Emulator/Graphics/GraphicsOperationTrace.h"
#include "Emulator/Graphics/Shader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

UT_BEGIN(EmulatorGraphicsOperationTrace);

using namespace Libs::Graphics;

// Golden producer contract, byte-identical to GOLDEN_INPUT and GOLDEN_META in
// scripts/tests/test_operation_graph.py.
static const char* const kGoldenInput =
    "{\"schema\":\"kyty_operation_graph_input_v1\",\"records\":["
    "{\"id\":\"op0\",\"kind\":\"draw\",\"spans\":["
    "{\"resource\":\"guest\",\"offset\":4096,\"size\":12,\"access\":\"read\"},"
    "{\"resource\":\"guest\",\"offset\":8192,\"size\":64,\"access\":\"read\"},"
    "{\"resource\":\"guest\",\"offset\":12288,\"size\":32,\"access\":\"read\"},"
    "{\"resource\":\"guest\",\"offset\":12288,\"size\":32,\"access\":\"write\"},"
    "{\"resource\":\"guest\",\"offset\":16384,\"size\":256,\"access\":\"read\"},"
    "{\"resource\":\"guest\",\"offset\":16384,\"size\":256,\"access\":\"write\"}]},"
    "{\"id\":\"op1\",\"kind\":\"dispatch\",\"spans\":["
    "{\"resource\":\"guest\",\"offset\":12288,\"size\":32,\"access\":\"read\"}]}]}";
static const char* const kGoldenMeta =
    "{\"schema\":\"kyty_operation_trace_meta_v1\",\"input_md5\":\"862d69cca795f7de144947efea015b54\","
    "\"operations_seen\":2,\"records_emitted\":2,"
    "\"operations_dropped\":0,\"truncated\":false,"
    "\"not_traced_kinds\":[\"auto_draw\",\"depth_stencil_copy_draw\",\"dma\",\"indirect_dispatch\",\"indirect_draw\","
    "\"resolve_and_copy\"],"
    "\"incomplete_records\":[{\"id\":\"op0\",\"reasons\":[\"color_attachment_extent_unknown\"]}]}";

static void SetBuffer(ShaderBufferResource* resource, uint64_t base, uint32_t stride, uint32_t records)
{
	resource->fields[0] = static_cast<uint32_t>(base & 0xffffffffu);
	resource->fields[1] = static_cast<uint32_t>((base >> 32u) & 0xffffu) | ((stride & 0x3fffu) << 16u);
	resource->fields[2] = records;
}

static OperationTraceRecord OneRead(uint64_t address, uint64_t size)
{
	OperationTraceRecord record;
	record.kind             = OperationTraceKind::Draw;
	record.span_count       = 1;
	record.spans[0]         = {address, size, OperationTraceAccess::Read};
	return record;
}

static std::string ReadFile(const std::filesystem::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

static std::filesystem::path FreshDirectory(const char* name)
{
	std::error_code ec;
	auto            path = std::filesystem::temp_directory_path(ec) / (std::string("kyty-operation-trace-ut-") + name);
	EXPECT_FALSE(ec);
	std::filesystem::remove_all(path, ec);
	std::filesystem::create_directories(path, ec);
	EXPECT_FALSE(ec);
	return path;
}

TEST(EmulatorGraphicsOperationTrace, DisabledWithoutAnAbsoluteDirectory)
{
	OperationTraceCollector unset("");
	OperationTraceCollector relative("relative/dir");
	EXPECT_FALSE(unset.Enabled());
	EXPECT_FALSE(relative.Enabled());
	unset.Append(OneRead(4096, 8));
	relative.Append(OneRead(4096, 8));
	EXPECT_EQ(unset.RecordsEmitted(), 0u);
	EXPECT_EQ(relative.RecordsEmitted(), 0u);
	EXPECT_TRUE(unset.Encode().input.empty());
	EXPECT_FALSE(unset.WriteFiles("run"));
}

TEST(EmulatorGraphicsOperationTrace, HooksAreInertWithTheDefaultEnvironment)
{
	ShaderBindResources bind;
	GraphicsOperationTraceDispatch(bind);
	GraphicsOperationTraceFlush();
	SUCCEED();
}

TEST(EmulatorGraphicsOperationTrace, DrawAndDispatchEncodeToTheGoldenProducerDocument)
{
	ShaderVertexInputInfo vs;
	vs.buffers_num         = 1;
	vs.buffers[0].addr      = 8192;
	vs.buffers[0].stride    = 16;
	vs.buffers[0].num_records = 4;

	ShaderPixelInputInfo ps;
	ps.bind.storage_buffers.buffers_num = 1;
	SetBuffer(&ps.bind.storage_buffers.buffers[0], 12288, 4, 8);
	ps.bind.storage_buffers.usages[0]   = ShaderStorageUsage::ReadWrite;
	ps.bind.storage_buffers.accesses[0] = ShaderStorageAccess::Typed;

	const OperationTraceDepthTarget depth {.address = 16384, .size = 256, .reads = true, .writes = true};
	const auto draw = BuildIndexedDrawRecord(4096, 12, true, vs, ps, 1, depth);

	ShaderBindResources compute;
	compute.storage_buffers.buffers_num = 1;
	SetBuffer(&compute.storage_buffers.buffers[0], 12288, 4, 8);
	compute.storage_buffers.usages[0]   = ShaderStorageUsage::ReadOnly;
	compute.storage_buffers.accesses[0] = ShaderStorageAccess::Raw;
	const auto dispatch = BuildDispatchRecord(compute);

	OperationTraceCollector collector("/absolute/operation-trace");
	collector.Append(draw);
	collector.Append(dispatch);
	const auto docs = collector.Encode();
	EXPECT_EQ(docs.input, std::string(kGoldenInput));
	EXPECT_EQ(docs.meta, std::string(kGoldenMeta));
}

TEST(EmulatorGraphicsOperationTrace, RecordCapTruncatesAndNeverSkipsAnOperation)
{
	OperationTraceCollector collector("/absolute/operation-trace");
	for (uint32_t i = 0; i < OperationTraceLimits::RECORDS_MAX; ++i)
	{
		collector.Append(OneRead(i * 16u, 8));
	}
	EXPECT_EQ(collector.RecordsEmitted(), OperationTraceLimits::RECORDS_MAX);
	EXPECT_FALSE(collector.Truncated());
	collector.Append(OneRead(1ull << 40, 8));
	collector.Append(OneRead(1ull << 41, 8));
	EXPECT_TRUE(collector.Truncated());
	EXPECT_EQ(collector.RecordsEmitted(), OperationTraceLimits::RECORDS_MAX);
	EXPECT_EQ(collector.OperationsDropped(), 2u);
}

TEST(EmulatorGraphicsOperationTrace, OutputBytesAreBoundedBeforeTheSpanTotal)
{
	OperationTraceCollector collector("/absolute/operation-trace");
	for (uint32_t r = 0; r < 2000; ++r)
	{
		OperationTraceRecord record;
		record.kind       = OperationTraceKind::Dispatch;
		record.span_count = OperationTraceRecord::SPANS_MAX;
		for (uint32_t s = 0; s < OperationTraceRecord::SPANS_MAX; ++s)
		{
			const uint64_t index    = static_cast<uint64_t>(r) * OperationTraceRecord::SPANS_MAX + s;
			record.spans[s] = {(index << 30u) + (1ull << 40u), 1, OperationTraceAccess::Read};
		}
		collector.Append(record);
	}
	EXPECT_TRUE(collector.Truncated());
	EXPECT_LE(collector.OutputBytes(), OperationTraceLimits::OUTPUT_BYTES_MAX);
	EXPECT_LE(collector.SpansEmitted(), OperationTraceLimits::SPANS_TOTAL_MAX);
}

TEST(EmulatorGraphicsOperationTrace, OperationWithoutExactSpansIsDroppedAndCounted)
{
	ShaderVertexInputInfo vs;
	ShaderPixelInputInfo  ps;
	const auto record = BuildIndexedDrawRecord(4096, 12, false, vs, ps, 0, {});
	EXPECT_EQ(record.span_count, 0u);
	EXPECT_NE(record.incomplete & OperationTraceIncomplete::IndexTypeUnknown, 0u);
	OperationTraceCollector collector("/absolute/operation-trace");
	collector.Append(record);
	EXPECT_EQ(collector.RecordsEmitted(), 0u);
	EXPECT_EQ(collector.OperationsDropped(), 1u);
}

TEST(EmulatorGraphicsOperationTrace, UnknownSpansAreMarkedNeverGuessed)
{
	ShaderBindResources unknown_usage;
	unknown_usage.storage_buffers.buffers_num = 1;
	SetBuffer(&unknown_usage.storage_buffers.buffers[0], 12288, 4, 8);
	unknown_usage.storage_buffers.usages[0]   = ShaderStorageUsage::Unknown;
	unknown_usage.storage_buffers.accesses[0] = ShaderStorageAccess::Typed;
	const auto usage_record = BuildDispatchRecord(unknown_usage);
	EXPECT_EQ(usage_record.span_count, 0u);
	EXPECT_NE(usage_record.incomplete & OperationTraceIncomplete::StorageUsageUnknown, 0u);

	ShaderBindResources unused;
	unused.storage_buffers.buffers_num = 1;
	SetBuffer(&unused.storage_buffers.buffers[0], 12288, 4, 8);
	unused.storage_buffers.usages[0]   = ShaderStorageUsage::ReadOnly;
	unused.storage_buffers.accesses[0] = ShaderStorageAccess::UnusedMetadata;
	const auto unused_record = BuildDispatchRecord(unused);
	EXPECT_EQ(unused_record.span_count, 0u);
	EXPECT_EQ(unused_record.incomplete, 0u);

	ShaderBindResources textures;
	textures.textures2D.textures_num            = 1;
	textures.textures2D.textures2d_storage_num  = 1;
	const auto texture_record = BuildDispatchRecord(textures);
	EXPECT_NE(texture_record.incomplete & OperationTraceIncomplete::TextureExtentUnknown, 0u);
	EXPECT_NE(texture_record.incomplete & OperationTraceIncomplete::StorageImageExtentUnknown, 0u);

	ShaderBindResources device_address;
	device_address.device_address_used = true;
	EXPECT_NE(BuildDispatchRecord(device_address).incomplete & OperationTraceIncomplete::DeviceAddressResourcesNotSpanned, 0u);
}

TEST(EmulatorGraphicsOperationTrace, ConflictingWritesAndInvalidSpansAreRejectedWithAReason)
{
	ShaderBindResources overlap;
	overlap.storage_buffers.buffers_num = 2;
	SetBuffer(&overlap.storage_buffers.buffers[0], 12288, 4, 8);
	SetBuffer(&overlap.storage_buffers.buffers[1], 12300, 4, 8);
	for (int i = 0; i < 2; ++i)
	{
		overlap.storage_buffers.usages[i]   = ShaderStorageUsage::ReadWrite;
		overlap.storage_buffers.accesses[i] = ShaderStorageAccess::Typed;
	}
	const auto conflict = BuildDispatchRecord(overlap);
	EXPECT_EQ(conflict.span_count, 3u);
	EXPECT_NE(conflict.incomplete & OperationTraceIncomplete::ConflictingWriteSpan, 0u);

	ShaderBindResources invalid;
	invalid.storage_buffers.buffers_num = 1;
	SetBuffer(&invalid.storage_buffers.buffers[0], (1ull << 48) - 4, 1, 8);
	invalid.storage_buffers.usages[0]   = ShaderStorageUsage::ReadOnly;
	invalid.storage_buffers.accesses[0] = ShaderStorageAccess::Raw;
	const auto invalid_record = BuildDispatchRecord(invalid);
	EXPECT_EQ(invalid_record.span_count, 0u);
	EXPECT_NE(invalid_record.incomplete & OperationTraceIncomplete::InvalidSpan, 0u);
}

TEST(EmulatorGraphicsOperationTrace, ReadModifyWriteKeepsReadBeforeWriteAndSpanCountIsCapped)
{
	ShaderBindResources rmw;
	rmw.storage_buffers.buffers_num = 1;
	SetBuffer(&rmw.storage_buffers.buffers[0], 12288, 4, 8);
	rmw.storage_buffers.usages[0]   = ShaderStorageUsage::ReadWrite;
	rmw.storage_buffers.accesses[0] = ShaderStorageAccess::Typed;
	const auto record = BuildDispatchRecord(rmw);
	ASSERT_EQ(record.span_count, 2u);
	EXPECT_EQ(record.spans[0].access, OperationTraceAccess::Read);
	EXPECT_EQ(record.spans[1].access, OperationTraceAccess::Write);

	// 1 index + 16 vertex + 16 read-only VS storage + 16 read-write PS storage (2 spans each) = 65 > 64.
	ShaderVertexInputInfo vs;
	vs.buffers_num                      = 16;
	vs.bind.storage_buffers.buffers_num = 16;
	ShaderPixelInputInfo ps;
	ps.bind.storage_buffers.buffers_num = 16;
	for (int i = 0; i < 16; ++i)
	{
		vs.buffers[i].addr        = 0x100000u + static_cast<uint64_t>(i) * 0x1000u;
		vs.buffers[i].stride      = 1;
		vs.buffers[i].num_records = 16;
		SetBuffer(&vs.bind.storage_buffers.buffers[i], 0x300000u + static_cast<uint64_t>(i) * 0x1000u, 1, 16);
		vs.bind.storage_buffers.usages[i]   = ShaderStorageUsage::ReadOnly;
		vs.bind.storage_buffers.accesses[i] = ShaderStorageAccess::Typed;
		SetBuffer(&ps.bind.storage_buffers.buffers[i], 0x200000u + static_cast<uint64_t>(i) * 0x1000u, 1, 16);
		ps.bind.storage_buffers.usages[i]   = ShaderStorageUsage::ReadWrite;
		ps.bind.storage_buffers.accesses[i] = ShaderStorageAccess::Typed;
	}
	const auto capped = BuildIndexedDrawRecord(0x1000, 4, true, vs, ps, 0, {});
	EXPECT_EQ(capped.span_count, OperationTraceRecord::SPANS_MAX);
	EXPECT_NE(capped.incomplete & OperationTraceIncomplete::SpanLimitReached, 0u);
}

TEST(EmulatorGraphicsOperationTrace, AttachmentsAreRecordedOnlyWithExactExtentAndOtherwiseMarked)
{
	ShaderVertexInputInfo vs;
	ShaderPixelInputInfo  ps;
	const OperationTraceDepthTarget depth {.address = 16384, .size = 256, .reads = false, .writes = true,
	                                       .htile_present = true, .stencil_present = true};
	const auto record = BuildIndexedDrawRecord(4096, 12, true, vs, ps, 2, depth);
	EXPECT_EQ(record.span_count, 2u);
	EXPECT_EQ(record.spans[1].access, OperationTraceAccess::Write);
	EXPECT_NE(record.incomplete & OperationTraceIncomplete::ColorAttachmentExtentUnknown, 0u);
	EXPECT_NE(record.incomplete & OperationTraceIncomplete::HtileMetadataNotRecorded, 0u);
	EXPECT_NE(record.incomplete & OperationTraceIncomplete::StencilAccessNotRecorded, 0u);

	const OperationTraceDepthTarget cleared {.address = 16384, .size = 256, .reads = true, .clear_present = true};
	EXPECT_NE(BuildIndexedDrawRecord(4096, 12, true, vs, ps, 0, cleared).incomplete &
	              OperationTraceIncomplete::AttachmentClearNotRecorded,
	          0u);
}

TEST(EmulatorGraphicsOperationTrace, ExclusiveFilesAreCreatedOnceAndNeverOverwritten)
{
	const auto dir = FreshDirectory("exclusive");
	OperationTraceCollector collector(dir.string());
	collector.Append(OneRead(4096, 8));
	EXPECT_TRUE(collector.WriteFiles("run"));
	const auto input_path = dir / "run.operations.input.json";
	const auto meta_path  = dir / "run.operations.meta.json";
	EXPECT_EQ(ReadFile(input_path), collector.Encode().input);
	std::error_code ec;
	EXPECT_TRUE(std::filesystem::is_regular_file(meta_path, ec));
	EXPECT_FALSE(ec);

	OperationTraceCollector second(dir.string());
	second.Append(OneRead(8192, 8));
	EXPECT_FALSE(second.WriteFiles("run"));
	EXPECT_EQ(ReadFile(input_path), collector.Encode().input);
	std::error_code cleanup;
	std::filesystem::remove_all(dir, cleanup);
}

#if !defined(_WIN32)
TEST(EmulatorGraphicsOperationTrace, ExistingSymbolicLinkIsNeitherFollowedNorReplaced)
{
	const auto dir    = FreshDirectory("symlink");
	const auto victim = dir / "victim.txt";
	std::ofstream(victim) << "victim-original";
	std::error_code ec;
	std::filesystem::create_symlink(victim, dir / "link.operations.input.json", ec);
	ASSERT_FALSE(ec);
	OperationTraceCollector collector(dir.string());
	collector.Append(OneRead(4096, 8));
	EXPECT_FALSE(collector.WriteFiles("link"));
	EXPECT_EQ(ReadFile(victim), std::string("victim-original"));
	std::error_code cleanup;
	std::filesystem::remove_all(dir, cleanup);
}
#endif

UT_END();
