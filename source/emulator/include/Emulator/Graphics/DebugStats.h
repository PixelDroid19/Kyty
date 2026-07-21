#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <array>
#include <chrono>
#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

constexpr uint32_t kDebugStatsGpuMemoryTypeCount = 9;
constexpr uint32_t kDebugStatsSlowFrameCapacity  = 64;
constexpr uint32_t kDebugStatsGpuMemorySlowCreateCapacity = 64;
constexpr uint64_t kDebugStatsGpuMemorySlowCreateThresholdNs = 5'000'000;

enum class DebugStatsGpuMemoryCreateOutcome
{
	CachedReuse,
	FastReuse,
	ExactReuse,
	CoveredReuse,
	NewStandalone,
	NewLinked,
	NewFromObjects,
	ReclaimNew
};

// Bounded host-only diagnostics for one CreateObject operation. No guest
// addresses or object identifiers are retained.
struct DebugStatsGpuMemorySlowCreateRecord
{
	uint64_t seq                      = 0;
	uint64_t duration_us              = 0;
	uint32_t type_index               = 0;
	DebugStatsGpuMemoryCreateOutcome outcome = DebugStatsGpuMemoryCreateOutcome::FastReuse;
	uint64_t requested_bytes          = 0;
	uint32_t range_count              = 0;
	uint64_t backing_lock_wait_ns     = 0;
	uint64_t registry_lock_wait_ns    = 0;
	uint64_t classification_ns        = 0;
	uint32_t overlap_candidates       = 0;
	uint32_t overlap_relation_mask    = 0;
	uint32_t reclaimed_objects        = 0;
	bool     create_from_objects      = false;
	uint64_t hash_calls               = 0;
	uint64_t hash_bytes               = 0;
	uint64_t hash_ns                  = 0;
	uint64_t vulkan_allocate_calls    = 0;
	uint64_t vulkan_allocate_ns       = 0;
	uint64_t vulkan_bind_calls        = 0;
	uint64_t vulkan_bind_ns           = 0;
	uint64_t create_func_calls        = 0;
	uint64_t create_func_ns           = 0;
	uint64_t update_func_calls        = 0;
	uint64_t update_func_ns           = 0;
	uint64_t dirty_register_ns        = 0;
	uint64_t dirty_prepare_ns         = 0;
	uint64_t dirty_track_ns           = 0;
	uint64_t upload_calls             = 0;
	uint64_t upload_bytes             = 0;
	uint64_t upload_ns                = 0;
};

struct DebugStatsGpuMemorySlowCreateSnapshot
{
	uint32_t capacity = kDebugStatsGpuMemorySlowCreateCapacity;
	uint32_t size     = 0;
	uint64_t dropped  = 0;
	std::array<DebugStatsGpuMemorySlowCreateRecord, kDebugStatsGpuMemorySlowCreateCapacity> records {};
};

enum class DebugStatsGpuMemoryCreatePhase
{
	BackingLockWait,
	RegistryLockWait,
	Classification,
	Hash,
	VulkanAllocate,
	VulkanBind,
	CreateFunc,
	UpdateFunc,
	DirtyRegister,
	DirtyPrepare,
	DirtyTrack,
	Upload
};

class DebugStatsGpuMemoryCreateTrace final
{
public:
	DebugStatsGpuMemoryCreateTrace(uint32_t type_index, uint64_t requested_bytes, uint32_t range_count);
	~DebugStatsGpuMemoryCreateTrace();

	KYTY_CLASS_NO_COPY(DebugStatsGpuMemoryCreateTrace);

	void AddPhase(DebugStatsGpuMemoryCreatePhase phase, uint64_t elapsed_ns, uint64_t bytes = 0);
	void SetClassification(uint32_t candidates, uint32_t relation_mask, uint32_t reclaimed, bool create_from_objects);
	void Complete(DebugStatsGpuMemoryCreateOutcome outcome);

	static void AddCurrentPhase(DebugStatsGpuMemoryCreatePhase phase, uint64_t elapsed_ns, uint64_t bytes = 0);

private:
	DebugStatsGpuMemorySlowCreateRecord* m_previous = nullptr;
	DebugStatsGpuMemorySlowCreateRecord  m_record {};
	std::chrono::steady_clock::time_point m_start;
	uint32_t                             m_type_index = 0;
	DebugStatsGpuMemoryCreateOutcome     m_outcome    = DebugStatsGpuMemoryCreateOutcome::FastReuse;
	bool                                 m_completed  = false;
};

struct DebugStatsGpuMemoryTypeSnapshot
{
	uint64_t cached_reuse     = 0;
	uint64_t fast_reuse       = 0;
	uint64_t exact_reuse      = 0;
	uint64_t covered_reuse    = 0;
	uint64_t new_standalone   = 0;
	uint64_t new_linked       = 0;
	uint64_t new_from_objects = 0;
	uint64_t reclaim_new      = 0;
	uint64_t logical_free     = 0;
	uint64_t live             = 0;
	uint64_t writeback_calls  = 0;
	uint64_t writeback_bytes  = 0;
	uint64_t writeback_ns     = 0;
	uint64_t writeback_max_ns = 0;
	uint64_t hash_calls              = 0;
	uint64_t hash_bytes              = 0;
	uint64_t hash_ns                 = 0;
	uint64_t hash_max_ns             = 0;
	uint64_t hash_tracked_changed    = 0;
	uint64_t hash_tracked_unchanged  = 0;
	uint64_t hash_fallback_changed   = 0;
	uint64_t hash_fallback_unchanged = 0;
};

// Host-only diagnostic counters. Never gate guest correctness on these values.
struct DebugStatsSnapshot
{
	double   fps                = 0.0;
	double   flip_per_sec       = 0.0;
	double   frame_time_ms      = 0.0;
	double   draws_per_sec      = 0.0;
	double   draws_per_frame    = 0.0;
	double   dispatches_per_sec = 0.0;
	double   alloc_mib_per_sec  = 0.0;
	uint64_t live_objects       = 0;
	uint64_t creates_window     = 0;
	uint64_t frees_window       = 0;
	double   cpu_percent        = 0.0;
	double   heap_mib           = 0.0;
	bool     host_sample_ok     = false;
	// Last VideoOut → swapchain blit source (guest display buffer).
	uint32_t present_src_w      = 0;
	uint32_t present_src_h      = 0;
	uint32_t present_dst_w      = 0;
	uint32_t present_dst_h      = 0;
	uint32_t present_src_layout = 0; // VkImageLayout as uint
};

// Bounded temporal correlation captured at a slow VideoOut flip. These deltas
// identify work observed between adjacent flips; they do not establish that
// any recorded operation caused the slow frame.
struct DebugStatsSlowFrameRecord
{
	uint64_t duration_us                 = 0;
	uint64_t flip_seq                    = 0;
	uint64_t gfx_pipeline_miss_count     = 0;
	uint64_t gfx_pipeline_miss_ns        = 0;
	uint64_t compute_pipeline_miss_count = 0;
	uint64_t compute_pipeline_miss_ns    = 0;
	uint64_t spirv_compile_count         = 0;
	uint64_t spirv_compile_ns            = 0;
	uint64_t gpu_memory_create_calls     = 0;
	uint64_t gpu_memory_create_ns        = 0;
	uint64_t upload_calls                = 0;
	uint64_t upload_bytes                = 0;
	uint64_t upload_ns                   = 0;
	uint64_t writeback_calls             = 0;
	uint64_t writeback_bytes             = 0;
	uint64_t writeback_ns                = 0;
};

struct DebugStatsSlowFrameSnapshot
{
	uint32_t capacity = kDebugStatsSlowFrameCapacity;
	uint32_t size     = 0;
	uint64_t dropped  = 0;
	std::array<DebugStatsSlowFrameRecord, kDebugStatsSlowFrameCapacity> records {};
};

struct DebugStatsPerformanceSnapshot
{
	uint64_t interval_ms           = 0;
	uint64_t draws                 = 0;
	uint64_t dispatches            = 0;
	uint64_t alloc_bytes           = 0;
	uint64_t free_bytes            = 0;
	uint64_t creates               = 0;
	uint64_t frees                 = 0;
	uint64_t flips                 = 0;
	uint64_t buffer_flushes        = 0;
	uint64_t command_buffers       = 0;
	uint64_t submits               = 0;
	uint64_t fence_waits           = 0;
	uint64_t fence_wait_ns         = 0;
	uint64_t fence_wait_max_ns     = 0;
	uint64_t acquires              = 0;
	uint64_t acquire_ns            = 0;
	uint64_t acquire_max_ns        = 0;
	uint64_t presents              = 0;
	uint64_t present_ns            = 0;
	uint64_t present_max_ns        = 0;
	uint32_t present_src_w         = 0;
	uint32_t present_src_h         = 0;
	uint32_t present_dst_w         = 0;
	uint32_t present_dst_h         = 0;
	uint32_t present_src_layout    = 0;
	uint64_t wait_reg_mem          = 0;
	uint64_t wait_reg_mem_ns       = 0;
	uint64_t wait_reg_mem_max_ns   = 0;
	uint64_t wait_flip_done        = 0;
	uint64_t wait_flip_done_ns     = 0;
	uint64_t wait_flip_done_max_ns = 0;
	uint64_t in_flight_current     = 0;
	uint64_t in_flight_max         = 0;
	uint64_t frame_samples         = 0;
	uint64_t frame_time_p50_us     = 0;
	uint64_t frame_time_p95_us     = 0;
	uint64_t frame_time_p99_us     = 0;
	uint64_t frame_time_max_us     = 0;
	uint64_t frames_over_50ms      = 0;
	uint64_t frames_over_100ms     = 0;
	uint64_t frames_over_250ms     = 0;
	DebugStatsSlowFrameSnapshot slow_frames {};
	uint64_t hash_calls            = 0;
	uint64_t hash_bytes            = 0;
	uint64_t hash_ns               = 0;
	uint64_t hash_max_ns           = 0;
	uint64_t detile_calls          = 0;
	uint64_t detile_bytes          = 0;
	uint64_t detile_ns             = 0;
	uint64_t detile_max_ns         = 0;
	uint64_t upload_calls          = 0;
	uint64_t upload_bytes          = 0;
	uint64_t upload_ns             = 0;
	uint64_t upload_max_ns         = 0;
	uint64_t writeback_calls       = 0;
	uint64_t writeback_bytes       = 0;
	uint64_t writeback_ns          = 0;
	uint64_t writeback_max_ns      = 0;
	uint64_t gfx_pipeline_lookup_hits           = 0;
	uint64_t gfx_pipeline_lookup_misses         = 0;
	uint64_t gfx_pipeline_lookup_ns             = 0;
	uint64_t gfx_pipeline_lookup_max_ns         = 0;
	uint64_t compute_pipeline_lookup_hits       = 0;
	uint64_t compute_pipeline_lookup_misses     = 0;
	uint64_t compute_pipeline_lookup_ns         = 0;
	uint64_t compute_pipeline_lookup_max_ns     = 0;
	uint64_t pipeline_evictions                 = 0;
	uint64_t pipeline_cache_checkpoint_count           = 0;
	uint64_t pipeline_cache_checkpoint_bytes           = 0;
	uint64_t pipeline_cache_checkpoint_ns              = 0;
	uint64_t pipeline_cache_checkpoint_max_ns          = 0;
	uint64_t pipeline_cache_checkpoint_written         = 0;
	uint64_t pipeline_cache_checkpoint_failed          = 0;
	uint64_t pipeline_cache_checkpoint_budget_exceeded = 0;
	// Inclusive miss latency: parse + SPIR-V + Vulkan pipeline creation.
	uint64_t gfx_pipeline_miss_count            = 0;
	uint64_t gfx_pipeline_miss_ns               = 0;
	uint64_t gfx_pipeline_miss_max_ns           = 0;
	uint64_t compute_pipeline_miss_count        = 0;
	uint64_t compute_pipeline_miss_ns           = 0;
	uint64_t compute_pipeline_miss_max_ns       = 0;
	uint64_t shader_ir_input_analysis_count     = 0;
	uint64_t shader_ir_input_analysis_ns        = 0;
	uint64_t shader_ir_input_analysis_max_ns    = 0;
	uint64_t shader_ir_pipeline_miss_count      = 0;
	uint64_t shader_ir_pipeline_miss_ns         = 0;
	uint64_t shader_ir_pipeline_miss_max_ns     = 0;
	uint64_t spirv_source_count                 = 0;
	uint64_t spirv_source_ns                    = 0;
	uint64_t spirv_source_max_ns                = 0;
	uint64_t spirv_compile_count                = 0;
	uint64_t spirv_compile_ns                   = 0;
	uint64_t spirv_compile_max_ns               = 0;
	uint64_t vk_graphics_pipeline_create_count  = 0;
	uint64_t vk_graphics_pipeline_create_ns     = 0;
	uint64_t vk_graphics_pipeline_create_max_ns = 0;
	uint64_t vk_compute_pipeline_create_count   = 0;
	uint64_t vk_compute_pipeline_create_ns      = 0;
	uint64_t vk_compute_pipeline_create_max_ns  = 0;
	uint64_t shader_translation_cache_hits      = 0;
	uint64_t shader_translation_cache_misses    = 0;
	uint64_t shader_translation_cache_evictions = 0;
	uint64_t gpu_memory_create_calls            = 0;
	uint64_t gpu_memory_create_ns               = 0;
	uint64_t gpu_memory_create_max_ns           = 0;
	std::array<DebugStatsGpuMemoryTypeSnapshot, kDebugStatsGpuMemoryTypeCount> gpu_memory_types {};
	DebugStatsGpuMemorySlowCreateSnapshot gpu_memory_slow_creates {};
	uint64_t live_objects          = 0;
	double   fps                   = 0.0;
	double   frame_time_ms         = 0.0;
};

enum class DebugStatsPipelineKind
{
	Graphics,
	Compute
};

enum class DebugStatsShaderParseKind
{
	InputAnalysis,
	PipelineMiss
};

enum class DebugStatsPipelineCacheCheckpointOutcome
{
	Written,
	Failed,
	BudgetExceeded
};

void DebugStatsInit();
void DebugStatsShutdown();

[[nodiscard]] double DebugStatsFrameIntervalMs(double current_seconds, double previous_seconds);

void DebugStatsRecordDraw();
void DebugStatsRecordDispatch();
void DebugStatsRecordAlloc(uint64_t bytes);
void DebugStatsRecordFree(uint64_t bytes);
void DebugStatsRecordFlip(double fps, double frame_time_ms);
void DebugStatsRecordBufferFlush();
void DebugStatsRecordCommandBuffer();
void DebugStatsRecordSubmit();
void DebugStatsRecordSubmissionComplete();
void DebugStatsRecordFenceWait(uint64_t elapsed_ns);
void DebugStatsRecordAcquire(uint64_t elapsed_ns);
void DebugStatsRecordPresent(uint64_t elapsed_ns);
void DebugStatsRecordWaitRegMem(uint64_t elapsed_ns);
void DebugStatsRecordWaitFlipDone(uint64_t elapsed_ns);
void DebugStatsRecordHash(uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordDetile(uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordUpload(uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordWriteBack(uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordPipelineLookup(DebugStatsPipelineKind kind, bool hit, uint64_t elapsed_ns);
void DebugStatsRecordPipelineEviction();
void DebugStatsRecordPipelineCacheCheckpoint(DebugStatsPipelineCacheCheckpointOutcome outcome, uint64_t attempted_bytes,
                                             uint64_t elapsed_ns);
void DebugStatsRecordPipelineMiss(DebugStatsPipelineKind kind, uint64_t elapsed_ns);
void DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind kind, uint64_t elapsed_ns);
void DebugStatsRecordSpirvSource(uint64_t elapsed_ns);
void DebugStatsRecordSpirvCompile(uint64_t elapsed_ns);
void DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind kind, uint64_t elapsed_ns);
void DebugStatsRecordShaderTranslationCache(bool hit, bool evicted);
void DebugStatsRecordGpuMemoryCreate(uint32_t type_index, DebugStatsGpuMemoryCreateOutcome outcome, uint64_t elapsed_ns,
	                                 const DebugStatsGpuMemorySlowCreateRecord* detail = nullptr);
void DebugStatsRecordGpuMemoryFree(uint32_t type_index);
void DebugStatsRecordGpuMemoryWriteBack(uint32_t type_index, uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordGpuMemoryHash(uint32_t type_index, uint64_t bytes, uint64_t elapsed_ns);
void DebugStatsRecordGpuMemoryHashComparison(uint32_t type_index, bool tracked, bool changed);

// Host-only wall-time scope. Construct only after an operation's validation
// gates pass so rejected/skipped work is never counted.
class DebugStatsScopedWork final
{
public:
	using Recorder = void (*)(uint64_t bytes, uint64_t elapsed_ns);

	DebugStatsScopedWork(Recorder recorder, uint64_t bytes): m_recorder(recorder), m_bytes(bytes), m_start(std::chrono::steady_clock::now())
	{
	}
	~DebugStatsScopedWork()
	{
		const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
		m_recorder(m_bytes, static_cast<uint64_t>(elapsed_ns));
	}

	KYTY_CLASS_NO_COPY(DebugStatsScopedWork);

private:
	Recorder                              m_recorder;
	uint64_t                              m_bytes;
	std::chrono::steady_clock::time_point m_start;
};

class DebugStatsScopedTimer final
{
public:
	using Recorder = void (*)(uint64_t elapsed_ns);

	explicit DebugStatsScopedTimer(Recorder recorder): m_recorder(recorder), m_start(std::chrono::steady_clock::now()) {}
	~DebugStatsScopedTimer()
	{
		const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
		m_recorder(static_cast<uint64_t>(elapsed_ns));
	}

	KYTY_CLASS_NO_COPY(DebugStatsScopedTimer);

private:
	Recorder                              m_recorder;
	std::chrono::steady_clock::time_point m_start;
};

// Call from WindowDrawBuffer with the guest display image and swapchain extent.
void DebugStatsRecordPresentSource(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t src_layout);

// Refresh one-second rates and host CPU/RSS. Call from the window/present thread.
DebugStatsSnapshot DebugStatsTick(double now_seconds);

// Returns counters relative to an agent-owned baseline. Reset advances that
// baseline after taking the snapshot and never mutates the overlay window.
DebugStatsPerformanceSnapshot DebugStatsGetPerformanceSnapshot(bool reset);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_ */
