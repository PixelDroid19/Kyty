#include "Emulator/Agent/EventRing.h"
#include "Emulator/Agent/FrameScore.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Agent/StallWatch.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/Window.h"
#include "Kyty/UnitTest.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

UT_BEGIN(AgentTools);

using namespace Kyty::Emulator::Agent;

namespace {

void WriteBmp32(const char* path, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
	const uint32_t row_bytes   = width * 4u;
	const uint32_t pixel_bytes = row_bytes * height;
	std::vector<uint8_t> file(54u + pixel_bytes, 0);
	file[0]                    = 'B';
	file[1]                    = 'M';
	const uint32_t file_size   = static_cast<uint32_t>(file.size());
	file[2]                    = static_cast<uint8_t>(file_size);
	file[3]                    = static_cast<uint8_t>(file_size >> 8);
	file[4]                    = static_cast<uint8_t>(file_size >> 16);
	file[5]                    = static_cast<uint8_t>(file_size >> 24);
	file[10]                   = 54;
	file[14]                   = 40;
	file[18]                   = static_cast<uint8_t>(width);
	file[19]                   = static_cast<uint8_t>(width >> 8);
	file[20]                   = static_cast<uint8_t>(width >> 16);
	file[21]                   = static_cast<uint8_t>(width >> 24);
	file[22]                   = static_cast<uint8_t>(height);
	file[23]                   = static_cast<uint8_t>(height >> 8);
	file[24]                   = static_cast<uint8_t>(height >> 16);
	file[25]                   = static_cast<uint8_t>(height >> 24);
	file[26]                   = 1;
	file[28]                   = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			file[off + 0]    = b;
			file[off + 1]    = g;
			file[off + 2]    = r;
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void WriteBmp32HotBlocks(const char* path, uint32_t width, uint32_t height)
{
	const uint32_t row_bytes   = width * 4u;
	const uint32_t pixel_bytes = row_bytes * height;
	std::vector<uint8_t> file(54u + pixel_bytes, 0);
	file[0]                  = 'B';
	file[1]                  = 'M';
	const uint32_t file_size = static_cast<uint32_t>(file.size());
	file[2]                  = static_cast<uint8_t>(file_size);
	file[3]                  = static_cast<uint8_t>(file_size >> 8);
	file[4]                  = static_cast<uint8_t>(file_size >> 16);
	file[5]                  = static_cast<uint8_t>(file_size >> 24);
	file[10]                 = 54;
	file[14]                 = 40;
	file[18]                 = static_cast<uint8_t>(width);
	file[19]                 = static_cast<uint8_t>(width >> 8);
	file[22]                 = static_cast<uint8_t>(height);
	file[23]                 = static_cast<uint8_t>(height >> 8);
	file[26]                 = 1;
	file[28]                 = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			const bool   hot = (x < width / 3u) || (x > (2u * width) / 3u);
			file[off + 0]    = hot ? 20 : static_cast<uint8_t>(40 + (x % 40));
			file[off + 1]    = hot ? 240 : static_cast<uint8_t>(50 + (y % 40));
			file[off + 2]    = hot ? 255 : static_cast<uint8_t>(30 + ((x + y) % 50));
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

} // namespace

TEST(AgentTools, ProtocolParsesRequestAndFormatsResponse)
{
	Request   req {};
	ErrorInfo error {};
	ASSERT_TRUE(ParseRequestLine("{\"id\":7,\"tool\":\"status\",\"args\":{\"last\":3}}", &req, &error));
	EXPECT_EQ(req.id, 7u);
	EXPECT_EQ(req.tool, "status");
	EXPECT_EQ(req.args_json, "{\"last\":3}");

	uint32_t last = 0;
	EXPECT_TRUE(ArgsGetU32(req.args_json, "last", &last));
	EXPECT_EQ(last, 3u);

	const std::string ok = FormatOk(7, "{\"alive\":true}");
	EXPECT_NE(ok.find("\"ok\":true"), std::string::npos);
	EXPECT_NE(ok.find("\"alive\":true"), std::string::npos);

	const std::string err = FormatErr(7, "timeout", "wait timed out");
	EXPECT_NE(err.find("\"ok\":false"), std::string::npos);
	EXPECT_NE(err.find("timeout"), std::string::npos);
}

TEST(AgentTools, PerformanceSnapshotResetUsesIndependentBaseline)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	DebugStatsRecordDraw();
	DebugStatsRecordDispatch();
	DebugStatsRecordAlloc(4096);
	DebugStatsRecordFlip(60.0, 16.667);
	DebugStatsRecordBufferFlush();
	DebugStatsRecordCommandBuffer();
	DebugStatsRecordSubmit();
	DebugStatsRecordSubmit();
	DebugStatsRecordFenceWait(250000);
	DebugStatsRecordFenceWait(750000);
	DebugStatsRecordAcquire(100000);
	DebugStatsRecordAcquire(300000);
	DebugStatsRecordPresent(200000);
	DebugStatsRecordWaitRegMem(400000);
	DebugStatsRecordWaitFlipDone(500000);
	DebugStatsRecordSubmissionComplete();

	const DebugStatsPerformanceSnapshot first = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(first.draws, 1u);
	EXPECT_EQ(first.dispatches, 1u);
	EXPECT_EQ(first.alloc_bytes, 4096u);
	EXPECT_EQ(first.creates, 1u);
	EXPECT_EQ(first.flips, 1u);
	EXPECT_EQ(first.buffer_flushes, 1u);
	EXPECT_EQ(first.command_buffers, 1u);
	EXPECT_EQ(first.submits, 2u);
	EXPECT_EQ(first.fence_waits, 2u);
	EXPECT_EQ(first.fence_wait_ns, 1000000u);
	EXPECT_EQ(first.fence_wait_max_ns, 750000u);
	EXPECT_EQ(first.acquires, 2u);
	EXPECT_EQ(first.acquire_ns, 400000u);
	EXPECT_EQ(first.acquire_max_ns, 300000u);
	EXPECT_EQ(first.presents, 1u);
	EXPECT_EQ(first.present_ns, 200000u);
	EXPECT_EQ(first.present_max_ns, 200000u);
	EXPECT_EQ(first.wait_reg_mem, 1u);
	EXPECT_EQ(first.wait_reg_mem_ns, 400000u);
	EXPECT_EQ(first.wait_reg_mem_max_ns, 400000u);
	EXPECT_EQ(first.wait_flip_done, 1u);
	EXPECT_EQ(first.wait_flip_done_ns, 500000u);
	EXPECT_EQ(first.wait_flip_done_max_ns, 500000u);
	EXPECT_EQ(first.in_flight_current, 1u);
	EXPECT_EQ(first.in_flight_max, 2u);
	EXPECT_EQ(first.live_objects, 1u);

	DebugStatsRecordDraw();
	DebugStatsRecordFree(4096);
	const DebugStatsPerformanceSnapshot second = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(second.draws, 1u);
	EXPECT_EQ(second.dispatches, 0u);
	EXPECT_EQ(second.alloc_bytes, 0u);
	EXPECT_EQ(second.free_bytes, 4096u);
	EXPECT_EQ(second.creates, 0u);
	EXPECT_EQ(second.frees, 1u);
	EXPECT_EQ(second.flips, 0u);
	EXPECT_EQ(second.buffer_flushes, 0u);
	EXPECT_EQ(second.command_buffers, 0u);
	EXPECT_EQ(second.submits, 0u);
	EXPECT_EQ(second.fence_waits, 0u);
	EXPECT_EQ(second.fence_wait_ns, 0u);
	EXPECT_EQ(second.fence_wait_max_ns, 0u);
	EXPECT_EQ(second.acquires, 0u);
	EXPECT_EQ(second.acquire_ns, 0u);
	EXPECT_EQ(second.acquire_max_ns, 0u);
	EXPECT_EQ(second.presents, 0u);
	EXPECT_EQ(second.present_ns, 0u);
	EXPECT_EQ(second.present_max_ns, 0u);
	EXPECT_EQ(second.wait_reg_mem, 0u);
	EXPECT_EQ(second.wait_reg_mem_ns, 0u);
	EXPECT_EQ(second.wait_reg_mem_max_ns, 0u);
	EXPECT_EQ(second.wait_flip_done, 0u);
	EXPECT_EQ(second.wait_flip_done_ns, 0u);
	EXPECT_EQ(second.wait_flip_done_max_ns, 0u);
	EXPECT_EQ(second.in_flight_current, 1u);
	EXPECT_EQ(second.in_flight_max, 1u);
	EXPECT_EQ(second.live_objects, 0u);

	DebugStatsRecordSubmissionComplete();
	const DebugStatsPerformanceSnapshot third = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(third.in_flight_current, 0u);
	EXPECT_EQ(third.in_flight_max, 1u);

	const DebugStatsPerformanceSnapshot fourth = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(fourth.in_flight_current, 0u);
	EXPECT_EQ(fourth.in_flight_max, 0u);
	DebugStatsShutdown();
}

TEST(AgentTools, FrameTimeHistogramIsBoundedAndResettable)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	for (int i = 0; i < 50; ++i)
	{
		DebugStatsRecordFlip(100.0, 10.0);
	}
	for (int i = 0; i < 45; ++i)
	{
		DebugStatsRecordFlip(50.0, 20.0);
	}
	for (int i = 0; i < 4; ++i)
	{
		DebugStatsRecordFlip(16.0, 60.0);
	}
	DebugStatsRecordFlip(1.0, 600.0);

	const DebugStatsPerformanceSnapshot first = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(first.frame_samples, 100u);
	EXPECT_EQ(first.frame_time_p50_us, 11000u);
	EXPECT_EQ(first.frame_time_p95_us, 21000u);
	EXPECT_EQ(first.frame_time_p99_us, 61000u);
	EXPECT_EQ(first.frame_time_max_us, 600000u);
	EXPECT_EQ(first.frames_over_50ms, 5u);
	EXPECT_EQ(first.frames_over_100ms, 1u);
	EXPECT_EQ(first.frames_over_250ms, 1u);

	const DebugStatsPerformanceSnapshot reset = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(reset.frame_samples, 100u);
	EXPECT_EQ(reset.frame_time_max_us, 600000u);

	const DebugStatsPerformanceSnapshot empty = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(empty.frame_samples, 0u);
	EXPECT_EQ(empty.frame_time_p50_us, 0u);
	EXPECT_EQ(empty.frame_time_p95_us, 0u);
	EXPECT_EQ(empty.frame_time_p99_us, 0u);
	EXPECT_EQ(empty.frame_time_max_us, 0u);
	EXPECT_EQ(empty.frames_over_50ms, 0u);
	EXPECT_EQ(empty.frames_over_100ms, 0u);
	EXPECT_EQ(empty.frames_over_250ms, 0u);
	DebugStatsShutdown();
}

TEST(AgentTools, ResourceWorkTelemetryTracksOnlyRecordedOperations)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	DebugStatsRecordHash(64, 100);
	DebugStatsRecordHash(128, 300);
	DebugStatsRecordDetile(256, 400);
	DebugStatsRecordUpload(512, 500);
	DebugStatsRecordWriteBack(1024, 600);

	const DebugStatsPerformanceSnapshot first = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(first.hash_calls, 2u);
	EXPECT_EQ(first.hash_bytes, 192u);
	EXPECT_EQ(first.hash_ns, 400u);
	EXPECT_EQ(first.hash_max_ns, 300u);
	EXPECT_EQ(first.detile_calls, 1u);
	EXPECT_EQ(first.detile_bytes, 256u);
	EXPECT_EQ(first.detile_ns, 400u);
	EXPECT_EQ(first.detile_max_ns, 400u);
	EXPECT_EQ(first.upload_calls, 1u);
	EXPECT_EQ(first.upload_bytes, 512u);
	EXPECT_EQ(first.upload_ns, 500u);
	EXPECT_EQ(first.upload_max_ns, 500u);
	EXPECT_EQ(first.writeback_calls, 1u);
	EXPECT_EQ(first.writeback_bytes, 1024u);
	EXPECT_EQ(first.writeback_ns, 600u);
	EXPECT_EQ(first.writeback_max_ns, 600u);

	const DebugStatsPerformanceSnapshot empty = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(empty.hash_calls, 0u);
	EXPECT_EQ(empty.hash_bytes, 0u);
	EXPECT_EQ(empty.hash_ns, 0u);
	EXPECT_EQ(empty.hash_max_ns, 0u);
	EXPECT_EQ(empty.detile_calls, 0u);
	EXPECT_EQ(empty.detile_bytes, 0u);
	EXPECT_EQ(empty.detile_ns, 0u);
	EXPECT_EQ(empty.detile_max_ns, 0u);
	EXPECT_EQ(empty.upload_calls, 0u);
	EXPECT_EQ(empty.upload_bytes, 0u);
	EXPECT_EQ(empty.upload_ns, 0u);
	EXPECT_EQ(empty.upload_max_ns, 0u);
	EXPECT_EQ(empty.writeback_calls, 0u);
	EXPECT_EQ(empty.writeback_bytes, 0u);
	EXPECT_EQ(empty.writeback_ns, 0u);
	EXPECT_EQ(empty.writeback_max_ns, 0u);
	DebugStatsShutdown();
}

TEST(AgentTools, PipelineShaderTelemetrySeparatesExactOperationBoundaries)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Graphics, true, 100);
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Graphics, false, 300);
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Compute, true, 200);
	DebugStatsRecordPipelineLookup(DebugStatsPipelineKind::Compute, false, 400);
	DebugStatsRecordPipelineEviction();
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Graphics, 500);
	DebugStatsRecordPipelineMiss(DebugStatsPipelineKind::Compute, 600);
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::InputAnalysis, 700);
	DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind::PipelineMiss, 800);
	DebugStatsRecordSpirvSource(900);
	DebugStatsRecordSpirvCompile(1000);
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Graphics, 1100);
	DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind::Compute, 1200);
	DebugStatsRecordShaderTranslationCache(true, false);
	DebugStatsRecordShaderTranslationCache(false, true);

	const DebugStatsPerformanceSnapshot first = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(first.gfx_pipeline_lookup_hits, 1u);
	EXPECT_EQ(first.gfx_pipeline_lookup_misses, 1u);
	EXPECT_EQ(first.gfx_pipeline_lookup_ns, 400u);
	EXPECT_EQ(first.gfx_pipeline_lookup_max_ns, 300u);
	EXPECT_EQ(first.compute_pipeline_lookup_hits, 1u);
	EXPECT_EQ(first.compute_pipeline_lookup_misses, 1u);
	EXPECT_EQ(first.compute_pipeline_lookup_ns, 600u);
	EXPECT_EQ(first.compute_pipeline_lookup_max_ns, 400u);
	EXPECT_EQ(first.pipeline_evictions, 1u);
	EXPECT_EQ(first.gfx_pipeline_miss_count, 1u);
	EXPECT_EQ(first.gfx_pipeline_miss_ns, 500u);
	EXPECT_EQ(first.gfx_pipeline_miss_max_ns, 500u);
	EXPECT_EQ(first.compute_pipeline_miss_count, 1u);
	EXPECT_EQ(first.compute_pipeline_miss_ns, 600u);
	EXPECT_EQ(first.compute_pipeline_miss_max_ns, 600u);
	EXPECT_EQ(first.shader_ir_input_analysis_count, 1u);
	EXPECT_EQ(first.shader_ir_input_analysis_ns, 700u);
	EXPECT_EQ(first.shader_ir_input_analysis_max_ns, 700u);
	EXPECT_EQ(first.shader_ir_pipeline_miss_count, 1u);
	EXPECT_EQ(first.shader_ir_pipeline_miss_ns, 800u);
	EXPECT_EQ(first.shader_ir_pipeline_miss_max_ns, 800u);
	EXPECT_EQ(first.spirv_source_count, 1u);
	EXPECT_EQ(first.spirv_source_ns, 900u);
	EXPECT_EQ(first.spirv_source_max_ns, 900u);
	EXPECT_EQ(first.spirv_compile_count, 1u);
	EXPECT_EQ(first.spirv_compile_ns, 1000u);
	EXPECT_EQ(first.spirv_compile_max_ns, 1000u);
	EXPECT_EQ(first.vk_graphics_pipeline_create_count, 1u);
	EXPECT_EQ(first.vk_graphics_pipeline_create_ns, 1100u);
	EXPECT_EQ(first.vk_graphics_pipeline_create_max_ns, 1100u);
	EXPECT_EQ(first.vk_compute_pipeline_create_count, 1u);
	EXPECT_EQ(first.vk_compute_pipeline_create_ns, 1200u);
	EXPECT_EQ(first.vk_compute_pipeline_create_max_ns, 1200u);
	EXPECT_EQ(first.shader_translation_cache_hits, 1u);
	EXPECT_EQ(first.shader_translation_cache_misses, 1u);
	EXPECT_EQ(first.shader_translation_cache_evictions, 1u);

	const DebugStatsPerformanceSnapshot empty = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(empty.gfx_pipeline_lookup_hits, 0u);
	EXPECT_EQ(empty.gfx_pipeline_lookup_misses, 0u);
	EXPECT_EQ(empty.gfx_pipeline_lookup_ns, 0u);
	EXPECT_EQ(empty.gfx_pipeline_lookup_max_ns, 0u);
	EXPECT_EQ(empty.compute_pipeline_lookup_hits, 0u);
	EXPECT_EQ(empty.compute_pipeline_lookup_misses, 0u);
	EXPECT_EQ(empty.compute_pipeline_lookup_ns, 0u);
	EXPECT_EQ(empty.compute_pipeline_lookup_max_ns, 0u);
	EXPECT_EQ(empty.pipeline_evictions, 0u);
	EXPECT_EQ(empty.gfx_pipeline_miss_count, 0u);
	EXPECT_EQ(empty.compute_pipeline_miss_count, 0u);
	EXPECT_EQ(empty.shader_ir_input_analysis_count, 0u);
	EXPECT_EQ(empty.shader_ir_pipeline_miss_count, 0u);
	EXPECT_EQ(empty.spirv_source_count, 0u);
	EXPECT_EQ(empty.spirv_compile_count, 0u);
	EXPECT_EQ(empty.vk_graphics_pipeline_create_count, 0u);
	EXPECT_EQ(empty.vk_compute_pipeline_create_count, 0u);
	EXPECT_EQ(empty.shader_translation_cache_hits, 0u);
	EXPECT_EQ(empty.shader_translation_cache_misses, 0u);
	EXPECT_EQ(empty.shader_translation_cache_evictions, 0u);
	DebugStatsShutdown();
}

TEST(AgentTools, GpuMemoryTelemetryRecordsOneBoundedOutcomePerCreateCall)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	DebugStatsRecordGpuMemoryCreate(0, DebugStatsGpuMemoryCreateOutcome::CachedReuse, 50);
	DebugStatsRecordGpuMemoryCreate(1, DebugStatsGpuMemoryCreateOutcome::FastReuse, 100);
	DebugStatsRecordGpuMemoryCreate(2, DebugStatsGpuMemoryCreateOutcome::ExactReuse, 200);
	DebugStatsRecordGpuMemoryCreate(3, DebugStatsGpuMemoryCreateOutcome::CoveredReuse, 300);
	DebugStatsRecordGpuMemoryCreate(4, DebugStatsGpuMemoryCreateOutcome::NewStandalone, 400);
	DebugStatsRecordGpuMemoryCreate(5, DebugStatsGpuMemoryCreateOutcome::NewLinked, 500);
	DebugStatsRecordGpuMemoryCreate(6, DebugStatsGpuMemoryCreateOutcome::NewFromObjects, 600);
	DebugStatsRecordGpuMemoryCreate(7, DebugStatsGpuMemoryCreateOutcome::ReclaimNew, 700);
	DebugStatsRecordGpuMemoryFree(7);
	DebugStatsRecordGpuMemoryWriteBack(5, 4096, 250);
	DebugStatsRecordGpuMemoryHash(6, 8192, 300);
	DebugStatsRecordGpuMemoryHashComparison(6, true, true);
	DebugStatsRecordGpuMemoryHashComparison(6, true, false);
	DebugStatsRecordGpuMemoryHashComparison(6, false, true);
	DebugStatsRecordGpuMemoryHashComparison(6, false, false);

	const DebugStatsPerformanceSnapshot peek = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(peek.gpu_memory_types[5].writeback_max_ns, 250u);
	EXPECT_EQ(peek.gpu_memory_types[6].hash_max_ns, 300u);

	const DebugStatsPerformanceSnapshot first = DebugStatsGetPerformanceSnapshot(true);
	EXPECT_EQ(first.gpu_memory_create_calls, 8u);
	EXPECT_EQ(first.gpu_memory_create_ns, 2850u);
	EXPECT_EQ(first.gpu_memory_create_max_ns, 700u);
	EXPECT_EQ(first.gpu_memory_types[0].cached_reuse, 1u);
	EXPECT_EQ(first.gpu_memory_types[1].fast_reuse, 1u);
	EXPECT_EQ(first.gpu_memory_types[2].exact_reuse, 1u);
	EXPECT_EQ(first.gpu_memory_types[3].covered_reuse, 1u);
	EXPECT_EQ(first.gpu_memory_types[4].new_standalone, 1u);
	EXPECT_EQ(first.gpu_memory_types[5].new_linked, 1u);
	EXPECT_EQ(first.gpu_memory_types[6].new_from_objects, 1u);
	EXPECT_EQ(first.gpu_memory_types[7].reclaim_new, 1u);
	EXPECT_EQ(first.gpu_memory_types[7].logical_free, 1u);
	EXPECT_EQ(first.gpu_memory_types[7].live, 0u);
	EXPECT_EQ(first.gpu_memory_types[5].writeback_calls, 1u);
	EXPECT_EQ(first.gpu_memory_types[5].writeback_bytes, 4096u);
	EXPECT_EQ(first.gpu_memory_types[5].writeback_ns, 250u);
	EXPECT_EQ(first.gpu_memory_types[5].writeback_max_ns, 250u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_calls, 1u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_bytes, 8192u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_ns, 300u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_max_ns, 300u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_tracked_changed, 1u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_tracked_unchanged, 1u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_fallback_changed, 1u);
	EXPECT_EQ(first.gpu_memory_types[6].hash_fallback_unchanged, 1u);
	for (uint32_t i = 8; i < kDebugStatsGpuMemoryTypeCount; ++i)
	{
		EXPECT_EQ(first.gpu_memory_types[i].cached_reuse, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].fast_reuse, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].exact_reuse, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].covered_reuse, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].new_standalone, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].new_linked, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].new_from_objects, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].reclaim_new, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].logical_free, 0u);
		EXPECT_EQ(first.gpu_memory_types[i].live, 0u);
	}

	const DebugStatsPerformanceSnapshot empty = DebugStatsGetPerformanceSnapshot(false);
	EXPECT_EQ(empty.gpu_memory_create_calls, 0u);
	EXPECT_EQ(empty.gpu_memory_create_ns, 0u);
	EXPECT_EQ(empty.gpu_memory_create_max_ns, 0u);
	for (uint32_t i = 0; i < kDebugStatsGpuMemoryTypeCount; ++i)
	{
		EXPECT_EQ(empty.gpu_memory_types[i].cached_reuse, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].fast_reuse, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].exact_reuse, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].covered_reuse, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].new_standalone, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].new_linked, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].new_from_objects, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].reclaim_new, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].logical_free, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].live, first.gpu_memory_types[i].live);
		EXPECT_EQ(empty.gpu_memory_types[i].hash_tracked_changed, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].hash_tracked_unchanged, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].hash_fallback_changed, 0u);
		EXPECT_EQ(empty.gpu_memory_types[i].hash_fallback_unchanged, 0u);
	}
	DebugStatsShutdown();
}

TEST(AgentTools, GpuMemoryPerformanceJsonUsesTheSharedStableSchema)
{
	Libs::Graphics::DebugStatsPerformanceSnapshot snapshot {};
	snapshot.gpu_memory_create_calls                = 2;
	snapshot.gpu_memory_create_ns                   = 300;
	snapshot.gpu_memory_create_max_ns               = 200;
	snapshot.gpu_memory_types[3].reclaim_new        = 1;
	snapshot.gpu_memory_types[3].logical_free       = 1;
	snapshot.gpu_memory_types[3].live               = 7;
	snapshot.gpu_memory_types[4].fast_reuse         = 9;
	snapshot.gpu_memory_types[4].cached_reuse       = 11;
	snapshot.gpu_memory_types[4].writeback_calls    = 3;
	snapshot.gpu_memory_types[4].writeback_bytes    = 8192;
	snapshot.gpu_memory_types[4].hash_calls         = 5;
	snapshot.gpu_memory_types[4].hash_bytes         = 16384;
	snapshot.gpu_memory_types[4].hash_tracked_changed   = 2;
	snapshot.gpu_memory_types[4].hash_fallback_unchanged = 3;

	std::string json;
	AppendGpuMemoryPerformanceJson(snapshot, &json);

	EXPECT_NE(json.find(R"("gpu_memory":{"create_calls":2,"create_ns":300,"create_max_ns":200)"), std::string::npos);
	EXPECT_NE(json.find(R"("type":"index_buffer")"), std::string::npos);
	EXPECT_NE(json.find(R"("reclaim_new":1,"logical_free":1,"live":7)"), std::string::npos);
	EXPECT_NE(json.find(R"("type":"vertex_buffer","cached_reuse":11,"fast_reuse":9)"), std::string::npos);
	EXPECT_NE(json.find(R"("writeback_calls":3,"writeback_bytes":8192)"), std::string::npos);
	EXPECT_NE(json.find(R"("hash_calls":5,"hash_bytes":16384)"), std::string::npos);
	EXPECT_NE(json.find(R"("hash_tracked_changed":2)"), std::string::npos);
	EXPECT_NE(json.find(R"("hash_fallback_unchanged":3)"), std::string::npos);
	EXPECT_EQ(json.find("PPSA"), std::string::npos);
}

TEST(AgentTools, PerformanceSnapshotReportsPresentSourceAndDestination)
{
	using namespace Kyty::Libs::Graphics;

	DebugStatsInit();
	DebugStatsRecordPresentSource(3840, 2160, 1280, 720, 7);

	const DebugStatsPerformanceSnapshot snapshot = DebugStatsGetPerformanceSnapshot(false);

	EXPECT_EQ(snapshot.present_src_w, 3840u);
	EXPECT_EQ(snapshot.present_src_h, 2160u);
	EXPECT_EQ(snapshot.present_dst_w, 1280u);
	EXPECT_EQ(snapshot.present_dst_h, 720u);
	EXPECT_EQ(snapshot.present_src_layout, 7u);
	DebugStatsShutdown();
}

TEST(AgentTools, ProtocolRejectsUnknownShape)
{
	Request   req {};
	ErrorInfo error {};
	EXPECT_FALSE(ParseRequestLine("{\"tool\":\"status\"}", &req, &error));
	EXPECT_EQ(error.code, "malformed");
}

TEST(AgentTools, EventRingKeepsNewestAndLastError)
{
	EventRing ring;
	ring.Push(EventKind::Info, "boot", "hello");
	ring.Push(EventKind::Error, "boom", "failed");
	ring.Push(EventKind::Capture, "capture_ok", "/tmp/a.bmp");

	EventRecord err {};
	ASSERT_TRUE(ring.LastError(&err));
	EXPECT_STREQ(err.code, "boom");

	EventRecord out[4] {};
	const uint32_t count = ring.CopySince(0, out, 4);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(out[0].kind, EventKind::Capture);
}

TEST(AgentTools, EventRingOverflowStaysBounded)
{
	EventRing ring;
	for (uint32_t i = 0; i < kAgentEventRingCapacity + 32u; ++i)
	{
		char code[32];
		std::snprintf(code, sizeof(code), "c%u", i);
		ring.Push(EventKind::Info, code, "x");
	}
	EXPECT_EQ(ring.Size(), kAgentEventRingCapacity);

	EventRecord newest[1] {};
	ASSERT_EQ(ring.CopySince(0, newest, 1), 1u);
	EXPECT_STREQ(newest[0].code, "c543");
}

TEST(AgentTools, AgentPadOverlayMergesButtonsAndAxes)
{
	Kyty::Libs::Controller::AgentPadClear();
	uint32_t button = 0;
	ASSERT_TRUE(Kyty::Libs::Controller::AgentPadButtonFromName("cross", &button));
	Kyty::Libs::Controller::AgentPadSetButton(button, true);

	Kyty::Libs::Controller::Axis axis {};
	ASSERT_TRUE(Kyty::Libs::Controller::AgentPadAxisFromName("left_x", &axis));
	Kyty::Libs::Controller::AgentPadSetAxis(axis, 200);

	uint32_t buttons = 0;
	uint8_t  axes[6] {};
	Kyty::Libs::Controller::AgentPadGetState(&buttons, axes);
	EXPECT_EQ(buttons & Kyty::Libs::Controller::PAD_BUTTON_CROSS, Kyty::Libs::Controller::PAD_BUTTON_CROSS);
	EXPECT_EQ(axes[0], 200);

	Kyty::Libs::Controller::AgentPadClear();
	Kyty::Libs::Controller::AgentPadGetState(&buttons, axes);
	EXPECT_EQ(buttons, 0u);
}

TEST(AgentTools, AgentPadTapIsReleasePressReleaseOnGuestSamples)
{
	using Kyty::Libs::Controller::AgentPadApplyReadStateSample;
	using Kyty::Libs::Controller::AgentPadClear;
	using Kyty::Libs::Controller::AgentPadGetReadStats;
	using Kyty::Libs::Controller::AgentPadScheduleTap;
	using Kyty::Libs::Controller::AgentPadReadStats;
	using Kyty::Libs::Controller::PAD_BUTTON_CROSS;

	AgentPadClear();
	ASSERT_TRUE(AgentPadScheduleTap(PAD_BUTTON_CROSS));

	AgentPadReadStats stats {};
	AgentPadGetReadStats(&stats);
	EXPECT_TRUE(stats.tap_pending);
	EXPECT_EQ(stats.delivered_taps, 0u);

	uint32_t sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);

	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, PAD_BUTTON_CROSS);

	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);

	AgentPadGetReadStats(&stats);
	EXPECT_FALSE(stats.tap_pending);
	EXPECT_EQ(stats.delivered_taps, 1u);

	// PadRead-style samples must not advance a new tap by themselves.
	ASSERT_TRUE(AgentPadScheduleTap(PAD_BUTTON_CROSS));
	sample = 0;
	AgentPadApplyReadStateSample(&sample); // release
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);
	AgentPadClear();
}

TEST(AgentTools, CaptureRequestFailsCleanlyWithoutWindow)
{
	Kyty::Libs::Graphics::WindowPresentStats stats {};
	ASSERT_TRUE(Kyty::Libs::Graphics::WindowGetPresentStats(&stats));
	EXPECT_FALSE(stats.graphic_ready);
	EXPECT_FALSE(stats.capture_ready);

	Kyty::Libs::Graphics::WindowNativeCaptureResult error {};
	uint64_t                                        request_id = 0;
	EXPECT_FALSE(Kyty::Libs::Graphics::WindowRequestNativeCapture(&request_id, &error));
	EXPECT_EQ(error.error_code, "not_ready");
}

TEST(AgentTools, StallWatchClassifiesPresentHang)
{
	StallSample start {};
	start.graphic_ready    = true;
	start.frame            = 2594;
	start.present          = 100;
	start.fps              = 11.0;
	start.ms_since_present = 100;
	start.ms_since_frame   = 100;

	StallSample end = start;
	end.fps              = 0.9;
	end.ms_since_present = 8000;
	end.ms_since_frame   = 8000;

	StallWatchArgs args {};
	args.window_ms        = 10000;
	args.present_stall_ms = 5000;
	args.frame_stall_ms   = 5000;
	args.min_fps          = 2.0;

	const StallWatchResult result = ClassifyStall(start, end, args);
	EXPECT_FALSE(result.healthy);
	EXPECT_EQ(result.code, StallCode::PresentStalled);
	EXPECT_STREQ(result.code_name, "present_stalled");
}

TEST(AgentTools, StallWatchHealthyWhenPresentAdvances)
{
	StallSample start {};
	start.graphic_ready = true;
	start.frame         = 100;
	start.present       = 50;
	start.fps           = 12.0;

	StallSample end = start;
	end.frame   = 220;
	end.present = 170;
	end.fps     = 11.5;

	StallWatchArgs args {};
	const StallWatchResult result = ClassifyStall(start, end, args);
	EXPECT_TRUE(result.healthy);
	EXPECT_EQ(result.code, StallCode::Healthy);
}

TEST(AgentTools, PhaseHintClassifiesLoadingInteractiveAndStalled)
{
	EXPECT_EQ(ClassifyPhaseHint(false, 0, 0.0, 0), PhaseHint::NotReady);
	EXPECT_EQ(ClassifyPhaseHint(true, 3, 30.0, 16), PhaseHint::Booting);
	EXPECT_EQ(ClassifyPhaseHint(true, 200, 2.0, 100), PhaseHint::Loading);
	EXPECT_EQ(ClassifyPhaseHint(true, 200, 12.0, 16), PhaseHint::Interactive);
	EXPECT_EQ(ClassifyPhaseHint(true, 200, 12.0, 5000), PhaseHint::Stalled);
	EXPECT_STREQ(PhaseHintName(PhaseHint::Loading), "loading");
	EXPECT_STREQ(PhaseHintName(PhaseHint::Interactive), "interactive");
}

TEST(AgentTools, FrameScoreFlagsHotCorruption)
{
	const char* path = "/tmp/kyty_agent_score_hot.bmp";
	WriteBmp32HotBlocks(path, 128, 96);
	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::HotCorruption);
	EXPECT_STREQ(metrics.verdict_name, "hot_corruption");
	EXPECT_GE(metrics.hot_block_ratio, 0.08);
	const std::string json = FrameScoreToJson(metrics, path);
	EXPECT_NE(json.find("\"healthy\":false"), std::string::npos);
	EXPECT_NE(json.find("hot_corruption"), std::string::npos);
	std::remove(path);
}

TEST(AgentTools, FrameScoreFlagsWhiteWorld)
{
	const char* path = "/tmp/kyty_agent_score_white.bmp";
	WriteBmp32(path, 96, 64, 252, 252, 252);
	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::WhiteWorld);
	EXPECT_GE(metrics.white_ratio, 0.35);
	std::remove(path);
}

TEST(AgentTools, FrameScoreAcceptsVariedGameplayLikeFrame)
{
	const char*    path      = "/tmp/kyty_agent_score_ok.bmp";
	const uint32_t width     = 96;
	const uint32_t height    = 64;
	const uint32_t row_bytes = width * 4u;
	std::vector<uint8_t> file(54u + row_bytes * height, 0);
	file[0]                  = 'B';
	file[1]                  = 'M';
	const uint32_t file_size = static_cast<uint32_t>(file.size());
	file[2]                  = static_cast<uint8_t>(file_size);
	file[3]                  = static_cast<uint8_t>(file_size >> 8);
	file[4]                  = static_cast<uint8_t>(file_size >> 16);
	file[5]                  = static_cast<uint8_t>(file_size >> 24);
	file[10]                 = 54;
	file[14]                 = 40;
	file[18]                 = static_cast<uint8_t>(width);
	file[22]                 = static_cast<uint8_t>(height);
	file[26]                 = 1;
	file[28]                 = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			file[off + 0]    = static_cast<uint8_t>((x * 3 + y * 5) % 180);
			file[off + 1]    = static_cast<uint8_t>((x * 7 + y * 2) % 160);
			file[off + 2]    = static_cast<uint8_t>((x * 11 + y * 13) % 140);
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	out.close();

	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::Healthy);
	EXPECT_NE(FrameScoreToJson(metrics, path).find("\"healthy\":true"), std::string::npos);
	std::remove(path);
}

UT_END();
