#include "Emulator/Graphics/DebugStats.h"

#ifdef KYTY_EMU_ENABLED

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Sys/SysProcess.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

namespace Kyty::Libs::Graphics {

namespace {

std::atomic<uint64_t> g_draws {0};
std::atomic<uint64_t> g_dispatches {0};
std::atomic<uint64_t> g_alloc_bytes {0};
std::atomic<uint64_t> g_free_bytes {0};
std::atomic<uint64_t> g_create_count {0};
std::atomic<uint64_t> g_free_count {0};
std::atomic<uint64_t> g_live_objects {0};
std::atomic<uint64_t> g_flips {0};
std::atomic<uint64_t> g_buffer_flushes {0};
std::atomic<uint64_t> g_command_buffers {0};
std::atomic<uint64_t> g_submits {0};
std::atomic<uint64_t> g_fence_waits {0};
std::atomic<uint64_t> g_fence_wait_ns {0};
std::atomic<uint64_t> g_fence_wait_max_ns {0};
std::atomic<uint64_t> g_acquires {0};
std::atomic<uint64_t> g_acquire_ns {0};
std::atomic<uint64_t> g_acquire_max_ns {0};
std::atomic<uint64_t> g_presents {0};
std::atomic<uint64_t> g_present_ns {0};
std::atomic<uint64_t> g_present_max_ns {0};
std::atomic<uint64_t> g_wait_reg_mem {0};
std::atomic<uint64_t> g_wait_reg_mem_ns {0};
std::atomic<uint64_t> g_wait_reg_mem_max_ns {0};
std::atomic<uint64_t> g_wait_flip_done {0};
std::atomic<uint64_t> g_wait_flip_done_ns {0};
std::atomic<uint64_t> g_wait_flip_done_max_ns {0};
std::atomic<uint64_t> g_in_flight_current {0};
std::atomic<uint64_t> g_in_flight_max {0};
constexpr size_t      kFrameHistogramMaxMs = 500;
constexpr size_t      kFrameHistogramBins  = kFrameHistogramMaxMs + 1; // Last bin is >= 500 ms.
using FrameHistogram                       = std::array<uint64_t, kFrameHistogramBins>;
std::array<std::atomic<uint64_t>, kFrameHistogramBins> g_frame_histogram {};
std::atomic<uint64_t>                                  g_frame_time_max_us {0};
std::atomic<uint64_t>                                  g_frames_over_50ms {0};
std::atomic<uint64_t>                                  g_frames_over_100ms {0};
std::atomic<uint64_t>                                  g_frames_over_250ms {0};
std::atomic<uint64_t>                                  g_hash_calls {0};
std::atomic<uint64_t>                                  g_hash_bytes {0};
std::atomic<uint64_t>                                  g_hash_ns {0};
std::atomic<uint64_t>                                  g_hash_max_ns {0};
std::atomic<uint64_t>                                  g_detile_calls {0};
std::atomic<uint64_t>                                  g_detile_bytes {0};
std::atomic<uint64_t>                                  g_detile_ns {0};
std::atomic<uint64_t>                                  g_detile_max_ns {0};
std::atomic<uint64_t>                                  g_upload_calls {0};
std::atomic<uint64_t>                                  g_upload_bytes {0};
std::atomic<uint64_t>                                  g_upload_ns {0};
std::atomic<uint64_t>                                  g_upload_max_ns {0};
std::atomic<uint64_t>                                  g_writeback_calls {0};
std::atomic<uint64_t>                                  g_writeback_bytes {0};
std::atomic<uint64_t>                                  g_writeback_ns {0};
std::atomic<uint64_t>                                  g_writeback_max_ns {0};

struct TimedMetric
{
	std::atomic<uint64_t> count {0};
	std::atomic<uint64_t> total_ns {0};
	std::atomic<uint64_t> max_ns {0};
};

struct LookupMetric
{
	std::atomic<uint64_t> hits {0};
	std::atomic<uint64_t> misses {0};
	std::atomic<uint64_t> total_ns {0};
	std::atomic<uint64_t> max_ns {0};
};

struct PipelineCacheCheckpointMetric
{
	std::mutex mutex;
	uint64_t   count           = 0;
	uint64_t   bytes           = 0;
	uint64_t   total_ns        = 0;
	uint64_t   max_ns          = 0;
	uint64_t   written         = 0;
	uint64_t   failed          = 0;
	uint64_t   budget_exceeded = 0;
};

LookupMetric          g_gfx_pipeline_lookup;
LookupMetric          g_compute_pipeline_lookup;
std::atomic<uint64_t> g_pipeline_evictions {0};
PipelineCacheCheckpointMetric g_pipeline_cache_checkpoint;
TimedMetric           g_gfx_pipeline_miss;
TimedMetric           g_compute_pipeline_miss;
TimedMetric           g_shader_ir_input_analysis;
TimedMetric           g_shader_ir_pipeline_miss;
TimedMetric           g_spirv_source;
TimedMetric           g_spirv_compile;
TimedMetric           g_vk_graphics_pipeline_create;
TimedMetric           g_vk_compute_pipeline_create;
std::atomic<uint64_t> g_shader_translation_cache_hits {0};
std::atomic<uint64_t> g_shader_translation_cache_misses {0};
std::atomic<uint64_t> g_shader_translation_cache_evictions {0};
TimedMetric           g_gpu_memory_create;

struct SlowFrameCounters
{
	std::atomic<uint64_t> gfx_pipeline_miss_count {0};
	std::atomic<uint64_t> gfx_pipeline_miss_ns {0};
	std::atomic<uint64_t> compute_pipeline_miss_count {0};
	std::atomic<uint64_t> compute_pipeline_miss_ns {0};
	std::atomic<uint64_t> spirv_compile_count {0};
	std::atomic<uint64_t> spirv_compile_ns {0};
	std::atomic<uint64_t> gpu_memory_create_calls {0};
	std::atomic<uint64_t> gpu_memory_create_ns {0};
	std::atomic<uint64_t> upload_calls {0};
	std::atomic<uint64_t> upload_bytes {0};
	std::atomic<uint64_t> upload_ns {0};
	std::atomic<uint64_t> writeback_calls {0};
	std::atomic<uint64_t> writeback_bytes {0};
	std::atomic<uint64_t> writeback_ns {0};
};

SlowFrameCounters g_slow_frame_totals;
SlowFrameCounters g_slow_frame_baseline;
std::atomic<bool>  g_slow_frame_baseline_valid {false};

struct SlowFrameRing
{
	std::array<DebugStatsSlowFrameRecord, kDebugStatsSlowFrameCapacity> records {};
	uint32_t head    = 0;
	uint32_t size    = 0;
	uint64_t dropped = 0;
};

SlowFrameRing g_slow_frames;
std::mutex    g_slow_frame_mutex;

struct SlowTotalsGate
{
	std::atomic<uint32_t> active_writers {0};
	std::atomic<bool>     capture_pending {false};
};

SlowTotalsGate g_slow_totals_gate;

struct GpuMemoryTypeMetric
{
	std::atomic<uint64_t> cached_reuse {0};
	std::atomic<uint64_t> fast_reuse {0};
	std::atomic<uint64_t> exact_reuse {0};
	std::atomic<uint64_t> covered_reuse {0};
	std::atomic<uint64_t> new_standalone {0};
	std::atomic<uint64_t> new_linked {0};
	std::atomic<uint64_t> new_from_objects {0};
	std::atomic<uint64_t> reclaim_new {0};
	std::atomic<uint64_t> logical_free {0};
	std::atomic<uint64_t> live {0};
	std::atomic<uint64_t> writeback_calls {0};
	std::atomic<uint64_t> writeback_bytes {0};
	std::atomic<uint64_t> writeback_ns {0};
	std::atomic<uint64_t> writeback_max_ns {0};
	std::atomic<uint64_t> hash_calls {0};
	std::atomic<uint64_t> hash_bytes {0};
	std::atomic<uint64_t> hash_ns {0};
	std::atomic<uint64_t> hash_max_ns {0};
	std::atomic<uint64_t> hash_tracked_changed {0};
	std::atomic<uint64_t> hash_tracked_unchanged {0};
	std::atomic<uint64_t> hash_fallback_changed {0};
	std::atomic<uint64_t> hash_fallback_unchanged {0};
};

std::array<GpuMemoryTypeMetric, kDebugStatsGpuMemoryTypeCount> g_gpu_memory_types {};

struct GpuMemorySlowCreateRing
{
	std::array<DebugStatsGpuMemorySlowCreateRecord, kDebugStatsGpuMemorySlowCreateCapacity> records {};
	uint32_t head     = 0;
	uint32_t size     = 0;
	uint64_t dropped  = 0;
	uint64_t next_seq = 1;
};

GpuMemorySlowCreateRing g_gpu_memory_slow_creates;
std::mutex              g_gpu_memory_slow_create_mutex;
thread_local DebugStatsGpuMemorySlowCreateRecord* g_current_gpu_memory_create = nullptr;

std::atomic<uint32_t>                                  g_present_src_w {0};
std::atomic<uint32_t>                                  g_present_src_h {0};
std::atomic<uint32_t>                                  g_present_dst_w {0};
std::atomic<uint32_t>                                  g_present_dst_h {0};
std::atomic<uint32_t>                                  g_present_src_layout {0};

std::atomic<double> g_last_fps {0.0};
std::atomic<double> g_last_frame_ms {0.0};

double   g_window_start_seconds = -1.0;
uint64_t g_window_draws         = 0;
uint64_t g_window_dispatches    = 0;
uint64_t g_window_alloc_bytes   = 0;
uint64_t g_window_creates       = 0;
uint64_t g_window_frees         = 0;
uint64_t g_window_flips         = 0;

DebugStatsSnapshot g_last_snapshot {};

struct PerformanceBaseline
{
	uint64_t time_ms           = 0;
	uint64_t draws             = 0;
	uint64_t dispatches        = 0;
	uint64_t alloc_bytes       = 0;
	uint64_t free_bytes        = 0;
	uint64_t creates           = 0;
	uint64_t frees             = 0;
	uint64_t flips             = 0;
	uint64_t buffer_flushes    = 0;
	uint64_t command_buffers   = 0;
	uint64_t submits           = 0;
	uint64_t fence_waits       = 0;
	uint64_t fence_wait_ns     = 0;
	uint64_t acquires          = 0;
	uint64_t acquire_ns        = 0;
	uint64_t presents          = 0;
	uint64_t present_ns        = 0;
	uint64_t wait_reg_mem      = 0;
	uint64_t wait_reg_mem_ns   = 0;
	uint64_t wait_flip_done    = 0;
	uint64_t wait_flip_done_ns = 0;
};

std::mutex          g_performance_mutex;
PerformanceBaseline g_performance_baseline {};

uint64_t SteadyMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

void UpdateMax(std::atomic<uint64_t>* target, uint64_t value)
{
	uint64_t previous = target->load(std::memory_order_relaxed);
	while (previous < value && !target->compare_exchange_weak(previous, value, std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

void RecordTimed(std::atomic<uint64_t>* count, std::atomic<uint64_t>* total_ns, std::atomic<uint64_t>* max_ns, uint64_t elapsed_ns)
{
	count->fetch_add(1, std::memory_order_relaxed);
	total_ns->fetch_add(elapsed_ns, std::memory_order_relaxed);
	UpdateMax(max_ns, elapsed_ns);
}

void RecordWork(std::atomic<uint64_t>* count, std::atomic<uint64_t>* total_bytes, std::atomic<uint64_t>* total_ns,
                std::atomic<uint64_t>* max_ns, uint64_t bytes, uint64_t elapsed_ns)
{
	count->fetch_add(1, std::memory_order_relaxed);
	total_bytes->fetch_add(bytes, std::memory_order_relaxed);
	total_ns->fetch_add(elapsed_ns, std::memory_order_relaxed);
	UpdateMax(max_ns, elapsed_ns);
}

void ResetTimed(TimedMetric* metric)
{
	metric->count.store(0, std::memory_order_relaxed);
	metric->total_ns.store(0, std::memory_order_relaxed);
	metric->max_ns.store(0, std::memory_order_relaxed);
}

void ResetLookup(LookupMetric* metric)
{
	metric->hits.store(0, std::memory_order_relaxed);
	metric->misses.store(0, std::memory_order_relaxed);
	metric->total_ns.store(0, std::memory_order_relaxed);
	metric->max_ns.store(0, std::memory_order_relaxed);
}

void RecordTimed(TimedMetric* metric, uint64_t elapsed_ns)
{
	RecordTimed(&metric->count, &metric->total_ns, &metric->max_ns, elapsed_ns);
}

uint64_t FrameTimeUs(double frame_time_ms)
{
	if (!std::isfinite(frame_time_ms) || frame_time_ms <= 0.0)
	{
		return 0;
	}
	constexpr double max_us = static_cast<double>(std::numeric_limits<uint64_t>::max());
	const double     us     = frame_time_ms * 1000.0;
	return us >= max_us ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(us + 0.5);
}

class SlowTotalsWriteGuard final
{
public:
	SlowTotalsWriteGuard()
	{
		for (;;)
		{
			while (g_slow_totals_gate.capture_pending.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}

			g_slow_totals_gate.active_writers.fetch_add(1, std::memory_order_acq_rel);
			if (!g_slow_totals_gate.capture_pending.load(std::memory_order_acquire))
			{
				m_active = true;
				return;
			}

			// A capture started between the first check and the increment. Drop
			// the registration and retry so the snapshot sees a stable writer set.
			g_slow_totals_gate.active_writers.fetch_sub(1, std::memory_order_release);
		}
	}
	~SlowTotalsWriteGuard()
	{
		if (m_active)
		{
			g_slow_totals_gate.active_writers.fetch_sub(1, std::memory_order_release);
		}
	}

	KYTY_CLASS_NO_COPY(SlowTotalsWriteGuard);

private:
	bool m_active = false;
};

DebugStatsSlowFrameRecord LoadSlowFrameTotals()
{
	DebugStatsSlowFrameRecord totals {};
	totals.gfx_pipeline_miss_count     = g_slow_frame_totals.gfx_pipeline_miss_count.load(std::memory_order_relaxed);
	totals.gfx_pipeline_miss_ns        = g_slow_frame_totals.gfx_pipeline_miss_ns.load(std::memory_order_relaxed);
	totals.compute_pipeline_miss_count = g_slow_frame_totals.compute_pipeline_miss_count.load(std::memory_order_relaxed);
	totals.compute_pipeline_miss_ns    = g_slow_frame_totals.compute_pipeline_miss_ns.load(std::memory_order_relaxed);
	totals.spirv_compile_count         = g_slow_frame_totals.spirv_compile_count.load(std::memory_order_relaxed);
	totals.spirv_compile_ns            = g_slow_frame_totals.spirv_compile_ns.load(std::memory_order_relaxed);
	totals.gpu_memory_create_calls     = g_slow_frame_totals.gpu_memory_create_calls.load(std::memory_order_relaxed);
	totals.gpu_memory_create_ns        = g_slow_frame_totals.gpu_memory_create_ns.load(std::memory_order_relaxed);
	totals.upload_calls                = g_slow_frame_totals.upload_calls.load(std::memory_order_relaxed);
	totals.upload_bytes                = g_slow_frame_totals.upload_bytes.load(std::memory_order_relaxed);
	totals.upload_ns                   = g_slow_frame_totals.upload_ns.load(std::memory_order_relaxed);
	totals.writeback_calls             = g_slow_frame_totals.writeback_calls.load(std::memory_order_relaxed);
	totals.writeback_bytes             = g_slow_frame_totals.writeback_bytes.load(std::memory_order_relaxed);
	totals.writeback_ns                = g_slow_frame_totals.writeback_ns.load(std::memory_order_relaxed);
	return totals;
}

DebugStatsSlowFrameRecord CaptureSlowFrameTotals()
{
	// All slow-frame fields are atomic, but the gate keeps a snapshot from
	// observing a writer halfway through a correlated update. Writers that
	// race this transition retry before touching the totals.
	g_slow_totals_gate.capture_pending.store(true, std::memory_order_seq_cst);
	while (g_slow_totals_gate.active_writers.load(std::memory_order_acquire) != 0)
	{
		std::this_thread::yield();
	}
	auto totals = LoadSlowFrameTotals();
	g_slow_totals_gate.capture_pending.store(false, std::memory_order_release);
	return totals;
}

void StoreSlowFrameBaseline(const DebugStatsSlowFrameRecord& totals)
{
	g_slow_frame_baseline.gfx_pipeline_miss_count.store(totals.gfx_pipeline_miss_count, std::memory_order_relaxed);
	g_slow_frame_baseline.gfx_pipeline_miss_ns.store(totals.gfx_pipeline_miss_ns, std::memory_order_relaxed);
	g_slow_frame_baseline.compute_pipeline_miss_count.store(totals.compute_pipeline_miss_count, std::memory_order_relaxed);
	g_slow_frame_baseline.compute_pipeline_miss_ns.store(totals.compute_pipeline_miss_ns, std::memory_order_relaxed);
	g_slow_frame_baseline.spirv_compile_count.store(totals.spirv_compile_count, std::memory_order_relaxed);
	g_slow_frame_baseline.spirv_compile_ns.store(totals.spirv_compile_ns, std::memory_order_relaxed);
	g_slow_frame_baseline.gpu_memory_create_calls.store(totals.gpu_memory_create_calls, std::memory_order_relaxed);
	g_slow_frame_baseline.gpu_memory_create_ns.store(totals.gpu_memory_create_ns, std::memory_order_relaxed);
	g_slow_frame_baseline.upload_calls.store(totals.upload_calls, std::memory_order_relaxed);
	g_slow_frame_baseline.upload_bytes.store(totals.upload_bytes, std::memory_order_relaxed);
	g_slow_frame_baseline.upload_ns.store(totals.upload_ns, std::memory_order_relaxed);
	g_slow_frame_baseline.writeback_calls.store(totals.writeback_calls, std::memory_order_relaxed);
	g_slow_frame_baseline.writeback_bytes.store(totals.writeback_bytes, std::memory_order_relaxed);
	g_slow_frame_baseline.writeback_ns.store(totals.writeback_ns, std::memory_order_relaxed);
}

DebugStatsSlowFrameRecord AdvanceSlowFrameBaseline(const DebugStatsSlowFrameRecord& totals, bool* had_baseline)
{
	DebugStatsSlowFrameRecord delta {};
	*had_baseline = g_slow_frame_baseline_valid.load(std::memory_order_relaxed);
	g_slow_frame_baseline_valid.store(true, std::memory_order_relaxed);
#define KYTY_SLOW_FRAME_DELTA(field)                                                                                                \
	delta.field = totals.field - g_slow_frame_baseline.field.load(std::memory_order_relaxed);                                          \
	g_slow_frame_baseline.field.store(totals.field, std::memory_order_relaxed)
	KYTY_SLOW_FRAME_DELTA(gfx_pipeline_miss_count);
	KYTY_SLOW_FRAME_DELTA(gfx_pipeline_miss_ns);
	KYTY_SLOW_FRAME_DELTA(compute_pipeline_miss_count);
	KYTY_SLOW_FRAME_DELTA(compute_pipeline_miss_ns);
	KYTY_SLOW_FRAME_DELTA(spirv_compile_count);
	KYTY_SLOW_FRAME_DELTA(spirv_compile_ns);
	KYTY_SLOW_FRAME_DELTA(gpu_memory_create_calls);
	KYTY_SLOW_FRAME_DELTA(gpu_memory_create_ns);
	KYTY_SLOW_FRAME_DELTA(upload_calls);
	KYTY_SLOW_FRAME_DELTA(upload_bytes);
	KYTY_SLOW_FRAME_DELTA(upload_ns);
	KYTY_SLOW_FRAME_DELTA(writeback_calls);
	KYTY_SLOW_FRAME_DELTA(writeback_bytes);
	KYTY_SLOW_FRAME_DELTA(writeback_ns);
#undef KYTY_SLOW_FRAME_DELTA
	return delta;
}

void RecordFlipCorrelation(uint64_t duration_us, uint64_t flip_seq)
{
	const bool slow = duration_us > 50000;
	if (!slow)
	{
		bool ignored = false;
		AdvanceSlowFrameBaseline(CaptureSlowFrameTotals(), &ignored);
		return;
	}

	bool had_baseline = false;
	auto record       = AdvanceSlowFrameBaseline(CaptureSlowFrameTotals(), &had_baseline);
	if (!had_baseline)
	{
		return;
	}
	record.duration_us = duration_us;
	record.flip_seq    = flip_seq;
	const uint32_t index = (g_slow_frames.head + g_slow_frames.size) % kDebugStatsSlowFrameCapacity;
	if (g_slow_frames.size == kDebugStatsSlowFrameCapacity)
	{
		g_slow_frames.records[g_slow_frames.head] = record;
		g_slow_frames.head = (g_slow_frames.head + 1) % kDebugStatsSlowFrameCapacity;
		g_slow_frames.dropped++;
	} else
	{
		g_slow_frames.records[index] = record;
		g_slow_frames.size++;
	}
}

void CopySlowFrames(bool reset, DebugStatsSlowFrameSnapshot* out)
{
	out->capacity = kDebugStatsSlowFrameCapacity;
	out->size     = g_slow_frames.size;
	out->dropped  = g_slow_frames.dropped;
	for (uint32_t i = 0; i < g_slow_frames.size; ++i)
	{
		out->records[i] = g_slow_frames.records[(g_slow_frames.head + i) % kDebugStatsSlowFrameCapacity];
	}
	if (reset)
	{
		g_slow_frames.head    = 0;
		g_slow_frames.size    = 0;
		g_slow_frames.dropped = 0;
		StoreSlowFrameBaseline(CaptureSlowFrameTotals());
		g_slow_frame_baseline_valid.store(true, std::memory_order_relaxed);
	}
}

uint64_t PercentileUs(const FrameHistogram& histogram, uint64_t samples, uint64_t percentile, uint64_t maximum_us)
{
	if (samples == 0)
	{
		return 0;
	}
	const uint64_t rank       = (samples / 100) * percentile + ((samples % 100) * percentile + 99) / 100;
	uint64_t       cumulative = 0;
	for (size_t i = 0; i < histogram.size(); ++i)
	{
		cumulative += histogram[i];
		if (cumulative >= rank)
		{
			return i == kFrameHistogramMaxMs ? maximum_us : static_cast<uint64_t>(i + 1) * 1000;
		}
	}
	return maximum_us;
}

void AddGpuMemoryCreatePhase(DebugStatsGpuMemorySlowCreateRecord* record, DebugStatsGpuMemoryCreatePhase phase,
	                         uint64_t elapsed_ns, uint64_t bytes)
{
	if (record == nullptr)
	{
		return;
	}
	switch (phase)
	{
		case DebugStatsGpuMemoryCreatePhase::BackingLockWait: record->backing_lock_wait_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::RegistryLockWait: record->registry_lock_wait_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::Classification: record->classification_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::Hash:
			record->hash_calls++;
			record->hash_bytes += bytes;
			record->hash_ns += elapsed_ns;
			break;
		case DebugStatsGpuMemoryCreatePhase::VulkanAllocate:
			record->vulkan_allocate_calls++;
			record->vulkan_allocate_ns += elapsed_ns;
			break;
		case DebugStatsGpuMemoryCreatePhase::VulkanBind:
			record->vulkan_bind_calls++;
			record->vulkan_bind_ns += elapsed_ns;
			break;
		case DebugStatsGpuMemoryCreatePhase::CreateFunc:
			record->create_func_calls++;
			record->create_func_ns += elapsed_ns;
			break;
		case DebugStatsGpuMemoryCreatePhase::UpdateFunc:
			record->update_func_calls++;
			record->update_func_ns += elapsed_ns;
			break;
		case DebugStatsGpuMemoryCreatePhase::DirtyRegister: record->dirty_register_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::DirtyPrepare: record->dirty_prepare_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::DirtyTrack: record->dirty_track_ns += elapsed_ns; break;
		case DebugStatsGpuMemoryCreatePhase::Upload:
			record->upload_calls++;
			record->upload_bytes += bytes;
			record->upload_ns += elapsed_ns;
			break;
	}
}

} // namespace

DebugStatsGpuMemoryCreateTrace::DebugStatsGpuMemoryCreateTrace(uint32_t type_index, uint64_t requested_bytes,
	                                                           uint32_t range_count)
	: m_previous(g_current_gpu_memory_create), m_start(std::chrono::steady_clock::now()), m_type_index(type_index)
{
	m_record.requested_bytes = requested_bytes;
	m_record.range_count     = range_count;
	g_current_gpu_memory_create = &m_record;
}

DebugStatsGpuMemoryCreateTrace::~DebugStatsGpuMemoryCreateTrace()
{
	g_current_gpu_memory_create = m_previous;
	if (m_completed)
	{
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
		DebugStatsRecordGpuMemoryCreate(m_type_index, m_outcome, static_cast<uint64_t>(elapsed), &m_record);
	}
}

void DebugStatsGpuMemoryCreateTrace::AddPhase(DebugStatsGpuMemoryCreatePhase phase, uint64_t elapsed_ns, uint64_t bytes)
{
	AddGpuMemoryCreatePhase(&m_record, phase, elapsed_ns, bytes);
}

void DebugStatsGpuMemoryCreateTrace::SetClassification(uint32_t candidates, uint32_t relation_mask, uint32_t reclaimed,
	                                                   bool create_from_objects)
{
	m_record.overlap_candidates    = candidates;
	m_record.overlap_relation_mask = relation_mask;
	m_record.reclaimed_objects     = reclaimed;
	m_record.create_from_objects   = create_from_objects;
}

void DebugStatsGpuMemoryCreateTrace::Complete(DebugStatsGpuMemoryCreateOutcome outcome)
{
	EXIT_IF(m_completed);
	m_outcome   = outcome;
	m_completed = true;
}

void DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase phase, uint64_t elapsed_ns, uint64_t bytes)
{
	AddGpuMemoryCreatePhase(g_current_gpu_memory_create, phase, elapsed_ns, bytes);
}

void DebugStatsInit()
{
	g_draws.store(0, std::memory_order_relaxed);
	g_dispatches.store(0, std::memory_order_relaxed);
	g_alloc_bytes.store(0, std::memory_order_relaxed);
	g_free_bytes.store(0, std::memory_order_relaxed);
	g_create_count.store(0, std::memory_order_relaxed);
	g_free_count.store(0, std::memory_order_relaxed);
	g_live_objects.store(0, std::memory_order_relaxed);
	g_flips.store(0, std::memory_order_relaxed);
	g_buffer_flushes.store(0, std::memory_order_relaxed);
	g_command_buffers.store(0, std::memory_order_relaxed);
	g_submits.store(0, std::memory_order_relaxed);
	g_fence_waits.store(0, std::memory_order_relaxed);
	g_fence_wait_ns.store(0, std::memory_order_relaxed);
	g_fence_wait_max_ns.store(0, std::memory_order_relaxed);
	g_acquires.store(0, std::memory_order_relaxed);
	g_acquire_ns.store(0, std::memory_order_relaxed);
	g_acquire_max_ns.store(0, std::memory_order_relaxed);
	g_presents.store(0, std::memory_order_relaxed);
	g_present_ns.store(0, std::memory_order_relaxed);
	g_present_max_ns.store(0, std::memory_order_relaxed);
	g_wait_reg_mem.store(0, std::memory_order_relaxed);
	g_wait_reg_mem_ns.store(0, std::memory_order_relaxed);
	g_wait_reg_mem_max_ns.store(0, std::memory_order_relaxed);
	g_wait_flip_done.store(0, std::memory_order_relaxed);
	g_wait_flip_done_ns.store(0, std::memory_order_relaxed);
	g_wait_flip_done_max_ns.store(0, std::memory_order_relaxed);
	g_in_flight_current.store(0, std::memory_order_relaxed);
	g_in_flight_max.store(0, std::memory_order_relaxed);
	for (auto& bin: g_frame_histogram)
	{
		bin.store(0, std::memory_order_relaxed);
	}
	g_frame_time_max_us.store(0, std::memory_order_relaxed);
	g_frames_over_50ms.store(0, std::memory_order_relaxed);
	g_frames_over_100ms.store(0, std::memory_order_relaxed);
	g_frames_over_250ms.store(0, std::memory_order_relaxed);
	g_hash_calls.store(0, std::memory_order_relaxed);
	g_hash_bytes.store(0, std::memory_order_relaxed);
	g_hash_ns.store(0, std::memory_order_relaxed);
	g_hash_max_ns.store(0, std::memory_order_relaxed);
	g_detile_calls.store(0, std::memory_order_relaxed);
	g_detile_bytes.store(0, std::memory_order_relaxed);
	g_detile_ns.store(0, std::memory_order_relaxed);
	g_detile_max_ns.store(0, std::memory_order_relaxed);
	g_upload_calls.store(0, std::memory_order_relaxed);
	g_upload_bytes.store(0, std::memory_order_relaxed);
	g_upload_ns.store(0, std::memory_order_relaxed);
	g_upload_max_ns.store(0, std::memory_order_relaxed);
	g_writeback_calls.store(0, std::memory_order_relaxed);
	g_writeback_bytes.store(0, std::memory_order_relaxed);
	g_writeback_ns.store(0, std::memory_order_relaxed);
	g_writeback_max_ns.store(0, std::memory_order_relaxed);
	ResetLookup(&g_gfx_pipeline_lookup);
	ResetLookup(&g_compute_pipeline_lookup);
	g_pipeline_evictions.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(g_pipeline_cache_checkpoint.mutex);
		g_pipeline_cache_checkpoint.count           = 0;
		g_pipeline_cache_checkpoint.bytes           = 0;
		g_pipeline_cache_checkpoint.total_ns        = 0;
		g_pipeline_cache_checkpoint.max_ns          = 0;
		g_pipeline_cache_checkpoint.written         = 0;
		g_pipeline_cache_checkpoint.failed          = 0;
		g_pipeline_cache_checkpoint.budget_exceeded = 0;
	}
	ResetTimed(&g_gfx_pipeline_miss);
	ResetTimed(&g_compute_pipeline_miss);
	ResetTimed(&g_shader_ir_input_analysis);
	ResetTimed(&g_shader_ir_pipeline_miss);
	ResetTimed(&g_spirv_source);
	ResetTimed(&g_spirv_compile);
	ResetTimed(&g_vk_graphics_pipeline_create);
	ResetTimed(&g_vk_compute_pipeline_create);
	g_shader_translation_cache_hits.store(0, std::memory_order_relaxed);
	g_shader_translation_cache_misses.store(0, std::memory_order_relaxed);
	g_shader_translation_cache_evictions.store(0, std::memory_order_relaxed);
	ResetTimed(&g_gpu_memory_create);
	DebugStatsSlowFrameRecord zero_slow_totals {};
	StoreSlowFrameBaseline(zero_slow_totals);
#define KYTY_RESET_SLOW_FRAME_TOTAL(field) g_slow_frame_totals.field.store(0, std::memory_order_relaxed)
	KYTY_RESET_SLOW_FRAME_TOTAL(gfx_pipeline_miss_count);
	KYTY_RESET_SLOW_FRAME_TOTAL(gfx_pipeline_miss_ns);
	KYTY_RESET_SLOW_FRAME_TOTAL(compute_pipeline_miss_count);
	KYTY_RESET_SLOW_FRAME_TOTAL(compute_pipeline_miss_ns);
	KYTY_RESET_SLOW_FRAME_TOTAL(spirv_compile_count);
	KYTY_RESET_SLOW_FRAME_TOTAL(spirv_compile_ns);
	KYTY_RESET_SLOW_FRAME_TOTAL(gpu_memory_create_calls);
	KYTY_RESET_SLOW_FRAME_TOTAL(gpu_memory_create_ns);
	KYTY_RESET_SLOW_FRAME_TOTAL(upload_calls);
	KYTY_RESET_SLOW_FRAME_TOTAL(upload_bytes);
	KYTY_RESET_SLOW_FRAME_TOTAL(upload_ns);
	KYTY_RESET_SLOW_FRAME_TOTAL(writeback_calls);
	KYTY_RESET_SLOW_FRAME_TOTAL(writeback_bytes);
	KYTY_RESET_SLOW_FRAME_TOTAL(writeback_ns);
#undef KYTY_RESET_SLOW_FRAME_TOTAL
	g_slow_frame_baseline_valid.store(false, std::memory_order_relaxed);
	g_slow_totals_gate.active_writers.store(0, std::memory_order_relaxed);
	g_slow_totals_gate.capture_pending.store(false, std::memory_order_relaxed);
	g_slow_frames.head    = 0;
	g_slow_frames.size    = 0;
	g_slow_frames.dropped = 0;
	for (auto& type: g_gpu_memory_types)
	{
		type.cached_reuse.store(0, std::memory_order_relaxed);
		type.fast_reuse.store(0, std::memory_order_relaxed);
		type.exact_reuse.store(0, std::memory_order_relaxed);
		type.covered_reuse.store(0, std::memory_order_relaxed);
		type.new_standalone.store(0, std::memory_order_relaxed);
		type.new_linked.store(0, std::memory_order_relaxed);
		type.new_from_objects.store(0, std::memory_order_relaxed);
		type.reclaim_new.store(0, std::memory_order_relaxed);
		type.logical_free.store(0, std::memory_order_relaxed);
		type.live.store(0, std::memory_order_relaxed);
		type.writeback_calls.store(0, std::memory_order_relaxed);
		type.writeback_bytes.store(0, std::memory_order_relaxed);
		type.writeback_ns.store(0, std::memory_order_relaxed);
		type.writeback_max_ns.store(0, std::memory_order_relaxed);
		type.hash_calls.store(0, std::memory_order_relaxed);
		type.hash_bytes.store(0, std::memory_order_relaxed);
		type.hash_ns.store(0, std::memory_order_relaxed);
		type.hash_max_ns.store(0, std::memory_order_relaxed);
		type.hash_tracked_changed.store(0, std::memory_order_relaxed);
		type.hash_tracked_unchanged.store(0, std::memory_order_relaxed);
		type.hash_fallback_changed.store(0, std::memory_order_relaxed);
		type.hash_fallback_unchanged.store(0, std::memory_order_relaxed);
	}
	{
		std::lock_guard<std::mutex> lock(g_gpu_memory_slow_create_mutex);
		g_gpu_memory_slow_creates = {};
	}
	g_last_fps.store(0.0, std::memory_order_relaxed);
	g_last_frame_ms.store(0.0, std::memory_order_relaxed);
	g_window_start_seconds = -1.0;
	g_window_draws         = 0;
	g_window_dispatches    = 0;
	g_window_alloc_bytes   = 0;
	g_window_creates       = 0;
	g_window_frees         = 0;
	g_window_flips         = 0;
	g_last_snapshot        = {};
	{
		std::lock_guard<std::mutex> lock(g_performance_mutex);
		g_performance_baseline         = {};
		g_performance_baseline.time_ms = SteadyMs();
	}
	SysProcessSampleReset();
}

void DebugStatsShutdown() {}

double DebugStatsFrameIntervalMs(double current_seconds, double previous_seconds)
{
	if (!std::isfinite(current_seconds) || !std::isfinite(previous_seconds) || previous_seconds <= 0.0 ||
	    current_seconds <= previous_seconds)
	{
		return 0.0;
	}
	return (current_seconds - previous_seconds) * 1000.0;
}

void DebugStatsRecordDraw()
{
	g_draws.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordDispatch()
{
	g_dispatches.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordAlloc(uint64_t bytes)
{
	g_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_create_count.fetch_add(1, std::memory_order_relaxed);
	g_live_objects.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordFree(uint64_t bytes)
{
	g_free_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_free_count.fetch_add(1, std::memory_order_relaxed);
	const uint64_t live = g_live_objects.load(std::memory_order_relaxed);
	if (live > 0)
	{
		g_live_objects.fetch_sub(1, std::memory_order_relaxed);
	}
}

void DebugStatsRecordFlip(double fps, double frame_time_ms)
{
	std::lock_guard<std::mutex> frame_lock(g_slow_frame_mutex);
	const uint64_t flip_seq = g_flips.fetch_add(1, std::memory_order_relaxed) + 1;
	g_last_fps.store(fps, std::memory_order_relaxed);
	g_last_frame_ms.store(frame_time_ms, std::memory_order_relaxed);
	const uint64_t frame_time_us = FrameTimeUs(frame_time_ms);
	RecordFlipCorrelation(frame_time_us, flip_seq);
	if (frame_time_us == 0)
	{
		return;
	}
	const size_t bin = frame_time_us >= kFrameHistogramMaxMs * 1000 ? kFrameHistogramMaxMs : static_cast<size_t>(frame_time_us / 1000);
	g_frame_histogram[bin].fetch_add(1, std::memory_order_relaxed);
	UpdateMax(&g_frame_time_max_us, frame_time_us);
	if (frame_time_us > 50000)
	{
		g_frames_over_50ms.fetch_add(1, std::memory_order_relaxed);
	}
	if (frame_time_us > 100000)
	{
		g_frames_over_100ms.fetch_add(1, std::memory_order_relaxed);
	}
	if (frame_time_us > 250000)
	{
		g_frames_over_250ms.fetch_add(1, std::memory_order_relaxed);
	}
}

void DebugStatsRecordBufferFlush()
{
	g_buffer_flushes.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordCommandBuffer()
{
	g_command_buffers.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordSubmit()
{
	g_submits.fetch_add(1, std::memory_order_relaxed);
	const uint64_t current = g_in_flight_current.fetch_add(1, std::memory_order_relaxed) + 1;
	UpdateMax(&g_in_flight_max, current);
}

void DebugStatsRecordSubmissionComplete()
{
	uint64_t current = g_in_flight_current.load(std::memory_order_relaxed);
	while (current != 0 &&
	       !g_in_flight_current.compare_exchange_weak(current, current - 1, std::memory_order_relaxed, std::memory_order_relaxed))
	{
	}
}

void DebugStatsRecordFenceWait(uint64_t elapsed_ns)
{
	RecordTimed(&g_fence_waits, &g_fence_wait_ns, &g_fence_wait_max_ns, elapsed_ns);
}

void DebugStatsRecordAcquire(uint64_t elapsed_ns)
{
	RecordTimed(&g_acquires, &g_acquire_ns, &g_acquire_max_ns, elapsed_ns);
}

void DebugStatsRecordPresent(uint64_t elapsed_ns)
{
	RecordTimed(&g_presents, &g_present_ns, &g_present_max_ns, elapsed_ns);
}

void DebugStatsRecordWaitRegMem(uint64_t elapsed_ns)
{
	RecordTimed(&g_wait_reg_mem, &g_wait_reg_mem_ns, &g_wait_reg_mem_max_ns, elapsed_ns);
}

void DebugStatsRecordWaitFlipDone(uint64_t elapsed_ns)
{
	RecordTimed(&g_wait_flip_done, &g_wait_flip_done_ns, &g_wait_flip_done_max_ns, elapsed_ns);
}

void DebugStatsRecordHash(uint64_t bytes, uint64_t elapsed_ns)
{
	RecordWork(&g_hash_calls, &g_hash_bytes, &g_hash_ns, &g_hash_max_ns, bytes, elapsed_ns);
}

void DebugStatsRecordDetile(uint64_t bytes, uint64_t elapsed_ns)
{
	RecordWork(&g_detile_calls, &g_detile_bytes, &g_detile_ns, &g_detile_max_ns, bytes, elapsed_ns);
}

void DebugStatsRecordUpload(uint64_t bytes, uint64_t elapsed_ns)
{
	RecordWork(&g_upload_calls, &g_upload_bytes, &g_upload_ns, &g_upload_max_ns, bytes, elapsed_ns);
	DebugStatsGpuMemoryCreateTrace::AddCurrentPhase(DebugStatsGpuMemoryCreatePhase::Upload, elapsed_ns, bytes);
	SlowTotalsWriteGuard totals_write;
	g_slow_frame_totals.upload_calls.fetch_add(1, std::memory_order_relaxed);
	g_slow_frame_totals.upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_slow_frame_totals.upload_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

void DebugStatsRecordWriteBack(uint64_t bytes, uint64_t elapsed_ns)
{
	RecordWork(&g_writeback_calls, &g_writeback_bytes, &g_writeback_ns, &g_writeback_max_ns, bytes, elapsed_ns);
	SlowTotalsWriteGuard totals_write;
	g_slow_frame_totals.writeback_calls.fetch_add(1, std::memory_order_relaxed);
	g_slow_frame_totals.writeback_bytes.fetch_add(bytes, std::memory_order_relaxed);
	g_slow_frame_totals.writeback_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

void DebugStatsRecordPipelineLookup(DebugStatsPipelineKind kind, bool hit, uint64_t elapsed_ns)
{
	auto* metric = kind == DebugStatsPipelineKind::Graphics ? &g_gfx_pipeline_lookup : &g_compute_pipeline_lookup;
	(hit ? metric->hits : metric->misses).fetch_add(1, std::memory_order_relaxed);
	metric->total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
	UpdateMax(&metric->max_ns, elapsed_ns);
}

void DebugStatsRecordPipelineEviction()
{
	g_pipeline_evictions.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordPipelineCacheCheckpoint(DebugStatsPipelineCacheCheckpointOutcome outcome, uint64_t attempted_bytes,
                                             uint64_t elapsed_ns)
{
	std::lock_guard<std::mutex> lock(g_pipeline_cache_checkpoint.mutex);
	g_pipeline_cache_checkpoint.count++;
	g_pipeline_cache_checkpoint.bytes += attempted_bytes;
	g_pipeline_cache_checkpoint.total_ns += elapsed_ns;
	g_pipeline_cache_checkpoint.max_ns = std::max(g_pipeline_cache_checkpoint.max_ns, elapsed_ns);
	switch (outcome)
	{
		case DebugStatsPipelineCacheCheckpointOutcome::Written:
			g_pipeline_cache_checkpoint.written++;
			break;
		case DebugStatsPipelineCacheCheckpointOutcome::Failed:
			g_pipeline_cache_checkpoint.failed++;
			break;
		case DebugStatsPipelineCacheCheckpointOutcome::BudgetExceeded:
			g_pipeline_cache_checkpoint.budget_exceeded++;
			break;
	}
}

void DebugStatsRecordPipelineMiss(DebugStatsPipelineKind kind, uint64_t elapsed_ns)
{
	RecordTimed(kind == DebugStatsPipelineKind::Graphics ? &g_gfx_pipeline_miss : &g_compute_pipeline_miss, elapsed_ns);
	SlowTotalsWriteGuard totals_write;
	auto& count = kind == DebugStatsPipelineKind::Graphics ? g_slow_frame_totals.gfx_pipeline_miss_count
	                                                      : g_slow_frame_totals.compute_pipeline_miss_count;
	auto& total = kind == DebugStatsPipelineKind::Graphics ? g_slow_frame_totals.gfx_pipeline_miss_ns
	                                                      : g_slow_frame_totals.compute_pipeline_miss_ns;
	count.fetch_add(1, std::memory_order_relaxed);
	total.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

void DebugStatsRecordShaderIrParse(DebugStatsShaderParseKind kind, uint64_t elapsed_ns)
{
	RecordTimed(kind == DebugStatsShaderParseKind::InputAnalysis ? &g_shader_ir_input_analysis : &g_shader_ir_pipeline_miss, elapsed_ns);
}

void DebugStatsRecordSpirvSource(uint64_t elapsed_ns)
{
	RecordTimed(&g_spirv_source, elapsed_ns);
}

void DebugStatsRecordSpirvCompile(uint64_t elapsed_ns)
{
	RecordTimed(&g_spirv_compile, elapsed_ns);
	SlowTotalsWriteGuard totals_write;
	g_slow_frame_totals.spirv_compile_count.fetch_add(1, std::memory_order_relaxed);
	g_slow_frame_totals.spirv_compile_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
}

void DebugStatsRecordVkPipelineCreate(DebugStatsPipelineKind kind, uint64_t elapsed_ns)
{
	RecordTimed(kind == DebugStatsPipelineKind::Graphics ? &g_vk_graphics_pipeline_create : &g_vk_compute_pipeline_create, elapsed_ns);
}

void DebugStatsRecordShaderTranslationCache(bool hit, bool evicted)
{
	(hit ? g_shader_translation_cache_hits : g_shader_translation_cache_misses).fetch_add(1, std::memory_order_relaxed);
	if (evicted)
	{
		g_shader_translation_cache_evictions.fetch_add(1, std::memory_order_relaxed);
	}
}

void DebugStatsRecordGpuMemoryCreate(uint32_t type_index, DebugStatsGpuMemoryCreateOutcome outcome, uint64_t elapsed_ns,
	                                 const DebugStatsGpuMemorySlowCreateRecord* detail)
{
	if (type_index >= kDebugStatsGpuMemoryTypeCount)
	{
		return;
	}

	RecordTimed(&g_gpu_memory_create, elapsed_ns);
	{
		SlowTotalsWriteGuard totals_write;
		g_slow_frame_totals.gpu_memory_create_calls.fetch_add(1, std::memory_order_relaxed);
		g_slow_frame_totals.gpu_memory_create_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
	}
	auto& type = g_gpu_memory_types[type_index];
	switch (outcome)
	{
		case DebugStatsGpuMemoryCreateOutcome::CachedReuse: type.cached_reuse.fetch_add(1, std::memory_order_relaxed); break;
		case DebugStatsGpuMemoryCreateOutcome::FastReuse: type.fast_reuse.fetch_add(1, std::memory_order_relaxed); break;
		case DebugStatsGpuMemoryCreateOutcome::ExactReuse: type.exact_reuse.fetch_add(1, std::memory_order_relaxed); break;
		case DebugStatsGpuMemoryCreateOutcome::CoveredReuse: type.covered_reuse.fetch_add(1, std::memory_order_relaxed); break;
		case DebugStatsGpuMemoryCreateOutcome::NewStandalone:
			type.new_standalone.fetch_add(1, std::memory_order_relaxed);
			type.live.fetch_add(1, std::memory_order_relaxed);
			break;
		case DebugStatsGpuMemoryCreateOutcome::NewLinked:
			type.new_linked.fetch_add(1, std::memory_order_relaxed);
			type.live.fetch_add(1, std::memory_order_relaxed);
			break;
		case DebugStatsGpuMemoryCreateOutcome::NewFromObjects:
			type.new_from_objects.fetch_add(1, std::memory_order_relaxed);
			type.live.fetch_add(1, std::memory_order_relaxed);
			break;
		case DebugStatsGpuMemoryCreateOutcome::ReclaimNew:
			type.reclaim_new.fetch_add(1, std::memory_order_relaxed);
			type.live.fetch_add(1, std::memory_order_relaxed);
			break;
	}

	if (detail != nullptr && elapsed_ns > kDebugStatsGpuMemorySlowCreateThresholdNs)
	{
		std::lock_guard<std::mutex> lock(g_gpu_memory_slow_create_mutex);
		auto record       = *detail;
		record.seq        = g_gpu_memory_slow_creates.next_seq++;
		record.duration_us = elapsed_ns / 1000u + (elapsed_ns % 1000u != 0u ? 1u : 0u);
		record.type_index = type_index;
		record.outcome    = outcome;
		const uint32_t index =
		    (g_gpu_memory_slow_creates.head + g_gpu_memory_slow_creates.size) % kDebugStatsGpuMemorySlowCreateCapacity;
		if (g_gpu_memory_slow_creates.size == kDebugStatsGpuMemorySlowCreateCapacity)
		{
			g_gpu_memory_slow_creates.records[g_gpu_memory_slow_creates.head] = record;
			g_gpu_memory_slow_creates.head =
			    (g_gpu_memory_slow_creates.head + 1u) % kDebugStatsGpuMemorySlowCreateCapacity;
			g_gpu_memory_slow_creates.dropped++;
		} else
		{
			g_gpu_memory_slow_creates.records[index] = record;
			g_gpu_memory_slow_creates.size++;
		}
	}
}

void DebugStatsRecordGpuMemoryFree(uint32_t type_index)
{
	if (type_index >= kDebugStatsGpuMemoryTypeCount)
	{
		return;
	}

	auto& type = g_gpu_memory_types[type_index];
	type.logical_free.fetch_add(1, std::memory_order_relaxed);
	uint64_t live = type.live.load(std::memory_order_relaxed);
	while (live > 0 && !type.live.compare_exchange_weak(live, live - 1, std::memory_order_relaxed))
	{
	}
}

void DebugStatsRecordGpuMemoryWriteBack(uint32_t type_index, uint64_t bytes, uint64_t elapsed_ns)
{
	DebugStatsRecordWriteBack(bytes, elapsed_ns);
	if (type_index >= kDebugStatsGpuMemoryTypeCount)
	{
		return;
	}
	auto& type = g_gpu_memory_types[type_index];
	RecordWork(&type.writeback_calls, &type.writeback_bytes, &type.writeback_ns, &type.writeback_max_ns, bytes, elapsed_ns);
}

void DebugStatsRecordGpuMemoryHash(uint32_t type_index, uint64_t bytes, uint64_t elapsed_ns)
{
	DebugStatsRecordHash(bytes, elapsed_ns);
	if (type_index >= kDebugStatsGpuMemoryTypeCount)
	{
		return;
	}
	auto& type = g_gpu_memory_types[type_index];
	RecordWork(&type.hash_calls, &type.hash_bytes, &type.hash_ns, &type.hash_max_ns, bytes, elapsed_ns);
}

void DebugStatsRecordGpuMemoryHashComparison(uint32_t type_index, bool tracked, bool changed)
{
	if (type_index >= kDebugStatsGpuMemoryTypeCount)
	{
		return;
	}
	auto& type = g_gpu_memory_types[type_index];
	auto& counter = tracked ? (changed ? type.hash_tracked_changed : type.hash_tracked_unchanged)
	                        : (changed ? type.hash_fallback_changed : type.hash_fallback_unchanged);
	counter.fetch_add(1, std::memory_order_relaxed);
}

void DebugStatsRecordPresentSource(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t src_layout)
{
	g_present_src_w.store(src_w, std::memory_order_relaxed);
	g_present_src_h.store(src_h, std::memory_order_relaxed);
	g_present_dst_w.store(dst_w, std::memory_order_relaxed);
	g_present_dst_h.store(dst_h, std::memory_order_relaxed);
	g_present_src_layout.store(src_layout, std::memory_order_relaxed);
}

DebugStatsSnapshot DebugStatsTick(double now_seconds)
{
	if (g_window_start_seconds < 0.0)
	{
		g_window_start_seconds = now_seconds;
		g_window_draws         = g_draws.load(std::memory_order_relaxed);
		g_window_dispatches    = g_dispatches.load(std::memory_order_relaxed);
		g_window_alloc_bytes   = g_alloc_bytes.load(std::memory_order_relaxed);
		g_window_creates       = g_create_count.load(std::memory_order_relaxed);
		g_window_frees         = g_free_count.load(std::memory_order_relaxed);
		g_window_flips         = g_flips.load(std::memory_order_relaxed);
		return g_last_snapshot;
	}

	const double elapsed = now_seconds - g_window_start_seconds;
	if (elapsed < 1.0)
	{
		return g_last_snapshot;
	}

	const uint64_t draws_now      = g_draws.load(std::memory_order_relaxed);
	const uint64_t dispatches_now = g_dispatches.load(std::memory_order_relaxed);
	const uint64_t alloc_now      = g_alloc_bytes.load(std::memory_order_relaxed);
	const uint64_t creates_now    = g_create_count.load(std::memory_order_relaxed);
	const uint64_t frees_now      = g_free_count.load(std::memory_order_relaxed);
	const uint64_t flips_now      = g_flips.load(std::memory_order_relaxed);

	const double draws_delta      = static_cast<double>(draws_now - g_window_draws);
	const double dispatches_delta = static_cast<double>(dispatches_now - g_window_dispatches);
	const double alloc_delta      = static_cast<double>(alloc_now - g_window_alloc_bytes);
	const double flips_delta      = static_cast<double>(flips_now - g_window_flips);

	DebugStatsSnapshot snap {};
	snap.fps                = g_last_fps.load(std::memory_order_relaxed);
	snap.frame_time_ms      = g_last_frame_ms.load(std::memory_order_relaxed);
	snap.flip_per_sec       = flips_delta / elapsed;
	snap.draws_per_sec      = draws_delta / elapsed;
	snap.dispatches_per_sec = dispatches_delta / elapsed;
	snap.draws_per_frame    = (flips_delta > 0.0) ? (draws_delta / flips_delta) : 0.0;
	snap.alloc_mib_per_sec  = (alloc_delta / (1024.0 * 1024.0)) / elapsed;
	snap.live_objects       = g_live_objects.load(std::memory_order_relaxed);
	snap.creates_window     = creates_now - g_window_creates;
	snap.frees_window       = frees_now - g_window_frees;

	const SysProcessSample host = SysProcessSampleNow();
	snap.host_sample_ok         = host.valid;
	snap.cpu_percent            = host.cpu_percent;
	snap.heap_mib               = static_cast<double>(host.heap_bytes) / (1024.0 * 1024.0);
	snap.present_src_w          = g_present_src_w.load(std::memory_order_relaxed);
	snap.present_src_h          = g_present_src_h.load(std::memory_order_relaxed);
	snap.present_dst_w          = g_present_dst_w.load(std::memory_order_relaxed);
	snap.present_dst_h          = g_present_dst_h.load(std::memory_order_relaxed);
	snap.present_src_layout     = g_present_src_layout.load(std::memory_order_relaxed);

	g_last_snapshot        = snap;
	g_window_start_seconds = now_seconds;
	g_window_draws         = draws_now;
	g_window_dispatches    = dispatches_now;
	g_window_alloc_bytes   = alloc_now;
	g_window_creates       = creates_now;
	g_window_frees         = frees_now;
	g_window_flips         = flips_now;

	return g_last_snapshot;
}

DebugStatsPerformanceSnapshot DebugStatsGetPerformanceSnapshot(bool reset)
{
	const uint64_t now_ms            = SteadyMs();
	const uint64_t draws             = g_draws.load(std::memory_order_relaxed);
	const uint64_t dispatches        = g_dispatches.load(std::memory_order_relaxed);
	const uint64_t alloc_bytes       = g_alloc_bytes.load(std::memory_order_relaxed);
	const uint64_t free_bytes        = g_free_bytes.load(std::memory_order_relaxed);
	const uint64_t creates           = g_create_count.load(std::memory_order_relaxed);
	const uint64_t frees             = g_free_count.load(std::memory_order_relaxed);
	uint64_t       flips             = 0;
	const uint64_t buffer_flushes    = g_buffer_flushes.load(std::memory_order_relaxed);
	const uint64_t command_buffers   = g_command_buffers.load(std::memory_order_relaxed);
	const uint64_t submits           = g_submits.load(std::memory_order_relaxed);
	const uint64_t fence_waits       = g_fence_waits.load(std::memory_order_relaxed);
	const uint64_t fence_wait_ns     = g_fence_wait_ns.load(std::memory_order_relaxed);
	const uint64_t acquires          = g_acquires.load(std::memory_order_relaxed);
	const uint64_t acquire_ns        = g_acquire_ns.load(std::memory_order_relaxed);
	const uint64_t presents          = g_presents.load(std::memory_order_relaxed);
	const uint64_t present_ns        = g_present_ns.load(std::memory_order_relaxed);
	const uint64_t wait_reg_mem      = g_wait_reg_mem.load(std::memory_order_relaxed);
	const uint64_t wait_reg_mem_ns   = g_wait_reg_mem_ns.load(std::memory_order_relaxed);
	const uint64_t wait_flip_done    = g_wait_flip_done.load(std::memory_order_relaxed);
	const uint64_t wait_flip_done_ns = g_wait_flip_done_ns.load(std::memory_order_relaxed);

	std::lock_guard<std::mutex> lock(g_performance_mutex);
	if (g_performance_baseline.time_ms == 0)
	{
		g_performance_baseline.time_ms = now_ms;
	}

	DebugStatsPerformanceSnapshot snapshot {};
	snapshot.interval_ms     = now_ms - g_performance_baseline.time_ms;
	snapshot.draws           = draws - g_performance_baseline.draws;
	snapshot.dispatches      = dispatches - g_performance_baseline.dispatches;
	snapshot.alloc_bytes     = alloc_bytes - g_performance_baseline.alloc_bytes;
	snapshot.free_bytes      = free_bytes - g_performance_baseline.free_bytes;
	snapshot.creates         = creates - g_performance_baseline.creates;
	snapshot.frees           = frees - g_performance_baseline.frees;
	snapshot.buffer_flushes  = buffer_flushes - g_performance_baseline.buffer_flushes;
	snapshot.command_buffers = command_buffers - g_performance_baseline.command_buffers;
	snapshot.submits         = submits - g_performance_baseline.submits;
	snapshot.fence_waits     = fence_waits - g_performance_baseline.fence_waits;
	snapshot.fence_wait_ns   = fence_wait_ns - g_performance_baseline.fence_wait_ns;
	snapshot.fence_wait_max_ns =
	    reset ? g_fence_wait_max_ns.exchange(0, std::memory_order_relaxed) : g_fence_wait_max_ns.load(std::memory_order_relaxed);
	snapshot.acquires   = acquires - g_performance_baseline.acquires;
	snapshot.acquire_ns = acquire_ns - g_performance_baseline.acquire_ns;
	snapshot.acquire_max_ns =
	    reset ? g_acquire_max_ns.exchange(0, std::memory_order_relaxed) : g_acquire_max_ns.load(std::memory_order_relaxed);
	snapshot.presents   = presents - g_performance_baseline.presents;
	snapshot.present_ns = present_ns - g_performance_baseline.present_ns;
	snapshot.present_max_ns =
	    reset ? g_present_max_ns.exchange(0, std::memory_order_relaxed) : g_present_max_ns.load(std::memory_order_relaxed);
	snapshot.present_src_w      = g_present_src_w.load(std::memory_order_relaxed);
	snapshot.present_src_h      = g_present_src_h.load(std::memory_order_relaxed);
	snapshot.present_dst_w      = g_present_dst_w.load(std::memory_order_relaxed);
	snapshot.present_dst_h      = g_present_dst_h.load(std::memory_order_relaxed);
	snapshot.present_src_layout = g_present_src_layout.load(std::memory_order_relaxed);
	snapshot.wait_reg_mem    = wait_reg_mem - g_performance_baseline.wait_reg_mem;
	snapshot.wait_reg_mem_ns = wait_reg_mem_ns - g_performance_baseline.wait_reg_mem_ns;
	snapshot.wait_reg_mem_max_ns =
	    reset ? g_wait_reg_mem_max_ns.exchange(0, std::memory_order_relaxed) : g_wait_reg_mem_max_ns.load(std::memory_order_relaxed);
	snapshot.wait_flip_done    = wait_flip_done - g_performance_baseline.wait_flip_done;
	snapshot.wait_flip_done_ns = wait_flip_done_ns - g_performance_baseline.wait_flip_done_ns;
	snapshot.wait_flip_done_max_ns =
	    reset ? g_wait_flip_done_max_ns.exchange(0, std::memory_order_relaxed) : g_wait_flip_done_max_ns.load(std::memory_order_relaxed);
	snapshot.in_flight_current = g_in_flight_current.load(std::memory_order_relaxed);
	snapshot.in_flight_max     = reset ? g_in_flight_max.exchange(snapshot.in_flight_current, std::memory_order_relaxed)
	                                   : g_in_flight_max.load(std::memory_order_relaxed);

	FrameHistogram frame_histogram {};
	{
		std::lock_guard<std::mutex> frame_lock(g_slow_frame_mutex);
		flips          = g_flips.load(std::memory_order_relaxed);
		snapshot.flips = flips - g_performance_baseline.flips;
		for (size_t i = 0; i < frame_histogram.size(); ++i)
		{
			frame_histogram[i] = reset ? g_frame_histogram[i].exchange(0, std::memory_order_relaxed)
			                           : g_frame_histogram[i].load(std::memory_order_relaxed);
			snapshot.frame_samples += frame_histogram[i];
		}
		snapshot.frame_time_max_us =
		    reset ? g_frame_time_max_us.exchange(0, std::memory_order_relaxed) : g_frame_time_max_us.load(std::memory_order_relaxed);
		snapshot.frames_over_50ms =
		    reset ? g_frames_over_50ms.exchange(0, std::memory_order_relaxed) : g_frames_over_50ms.load(std::memory_order_relaxed);
		snapshot.frames_over_100ms =
		    reset ? g_frames_over_100ms.exchange(0, std::memory_order_relaxed) : g_frames_over_100ms.load(std::memory_order_relaxed);
		snapshot.frames_over_250ms =
		    reset ? g_frames_over_250ms.exchange(0, std::memory_order_relaxed) : g_frames_over_250ms.load(std::memory_order_relaxed);
		CopySlowFrames(reset, &snapshot.slow_frames);
	}
	snapshot.frame_time_p50_us = PercentileUs(frame_histogram, snapshot.frame_samples, 50, snapshot.frame_time_max_us);
	snapshot.frame_time_p95_us = PercentileUs(frame_histogram, snapshot.frame_samples, 95, snapshot.frame_time_max_us);
	snapshot.frame_time_p99_us = PercentileUs(frame_histogram, snapshot.frame_samples, 99, snapshot.frame_time_max_us);

	auto take_window = [reset](std::atomic<uint64_t>& value)
	{ return reset ? value.exchange(0, std::memory_order_relaxed) : value.load(std::memory_order_relaxed); };
	snapshot.hash_calls       = take_window(g_hash_calls);
	snapshot.hash_bytes       = take_window(g_hash_bytes);
	snapshot.hash_ns          = take_window(g_hash_ns);
	snapshot.hash_max_ns      = take_window(g_hash_max_ns);
	snapshot.detile_calls     = take_window(g_detile_calls);
	snapshot.detile_bytes     = take_window(g_detile_bytes);
	snapshot.detile_ns        = take_window(g_detile_ns);
	snapshot.detile_max_ns    = take_window(g_detile_max_ns);
	snapshot.upload_calls     = take_window(g_upload_calls);
	snapshot.upload_bytes     = take_window(g_upload_bytes);
	snapshot.upload_ns        = take_window(g_upload_ns);
	snapshot.upload_max_ns    = take_window(g_upload_max_ns);
	snapshot.writeback_calls  = take_window(g_writeback_calls);
	snapshot.writeback_bytes  = take_window(g_writeback_bytes);
	snapshot.writeback_ns     = take_window(g_writeback_ns);
	snapshot.writeback_max_ns = take_window(g_writeback_max_ns);

	auto take_timed = [&take_window](TimedMetric& metric, uint64_t* count, uint64_t* total_ns, uint64_t* max_ns)
	{
		*count    = take_window(metric.count);
		*total_ns = take_window(metric.total_ns);
		*max_ns   = take_window(metric.max_ns);
	};
	auto take_lookup = [&take_window](LookupMetric& metric, uint64_t* hits, uint64_t* misses, uint64_t* total_ns, uint64_t* max_ns)
	{
		*hits     = take_window(metric.hits);
		*misses   = take_window(metric.misses);
		*total_ns = take_window(metric.total_ns);
		*max_ns   = take_window(metric.max_ns);
	};
	take_lookup(g_gfx_pipeline_lookup, &snapshot.gfx_pipeline_lookup_hits, &snapshot.gfx_pipeline_lookup_misses,
	            &snapshot.gfx_pipeline_lookup_ns, &snapshot.gfx_pipeline_lookup_max_ns);
	take_lookup(g_compute_pipeline_lookup, &snapshot.compute_pipeline_lookup_hits, &snapshot.compute_pipeline_lookup_misses,
	            &snapshot.compute_pipeline_lookup_ns, &snapshot.compute_pipeline_lookup_max_ns);
	snapshot.pipeline_evictions = take_window(g_pipeline_evictions);
	{
		std::lock_guard<std::mutex> checkpoint_lock(g_pipeline_cache_checkpoint.mutex);
		snapshot.pipeline_cache_checkpoint_count           = g_pipeline_cache_checkpoint.count;
		snapshot.pipeline_cache_checkpoint_bytes           = g_pipeline_cache_checkpoint.bytes;
		snapshot.pipeline_cache_checkpoint_ns              = g_pipeline_cache_checkpoint.total_ns;
		snapshot.pipeline_cache_checkpoint_max_ns          = g_pipeline_cache_checkpoint.max_ns;
		snapshot.pipeline_cache_checkpoint_written         = g_pipeline_cache_checkpoint.written;
		snapshot.pipeline_cache_checkpoint_failed          = g_pipeline_cache_checkpoint.failed;
		snapshot.pipeline_cache_checkpoint_budget_exceeded = g_pipeline_cache_checkpoint.budget_exceeded;
		if (reset)
		{
			g_pipeline_cache_checkpoint.count           = 0;
			g_pipeline_cache_checkpoint.bytes           = 0;
			g_pipeline_cache_checkpoint.total_ns        = 0;
			g_pipeline_cache_checkpoint.max_ns          = 0;
			g_pipeline_cache_checkpoint.written         = 0;
			g_pipeline_cache_checkpoint.failed          = 0;
			g_pipeline_cache_checkpoint.budget_exceeded = 0;
		}
	}
	take_timed(g_gfx_pipeline_miss, &snapshot.gfx_pipeline_miss_count, &snapshot.gfx_pipeline_miss_ns,
	           &snapshot.gfx_pipeline_miss_max_ns);
	take_timed(g_compute_pipeline_miss, &snapshot.compute_pipeline_miss_count, &snapshot.compute_pipeline_miss_ns,
	           &snapshot.compute_pipeline_miss_max_ns);
	take_timed(g_shader_ir_input_analysis, &snapshot.shader_ir_input_analysis_count, &snapshot.shader_ir_input_analysis_ns,
	           &snapshot.shader_ir_input_analysis_max_ns);
	take_timed(g_shader_ir_pipeline_miss, &snapshot.shader_ir_pipeline_miss_count, &snapshot.shader_ir_pipeline_miss_ns,
	           &snapshot.shader_ir_pipeline_miss_max_ns);
	take_timed(g_spirv_source, &snapshot.spirv_source_count, &snapshot.spirv_source_ns, &snapshot.spirv_source_max_ns);
	take_timed(g_spirv_compile, &snapshot.spirv_compile_count, &snapshot.spirv_compile_ns, &snapshot.spirv_compile_max_ns);
	take_timed(g_vk_graphics_pipeline_create, &snapshot.vk_graphics_pipeline_create_count, &snapshot.vk_graphics_pipeline_create_ns,
	           &snapshot.vk_graphics_pipeline_create_max_ns);
	take_timed(g_vk_compute_pipeline_create, &snapshot.vk_compute_pipeline_create_count, &snapshot.vk_compute_pipeline_create_ns,
	           &snapshot.vk_compute_pipeline_create_max_ns);
	snapshot.shader_translation_cache_hits      = take_window(g_shader_translation_cache_hits);
	snapshot.shader_translation_cache_misses    = take_window(g_shader_translation_cache_misses);
	snapshot.shader_translation_cache_evictions = take_window(g_shader_translation_cache_evictions);
	take_timed(g_gpu_memory_create, &snapshot.gpu_memory_create_calls, &snapshot.gpu_memory_create_ns,
	           &snapshot.gpu_memory_create_max_ns);
	for (uint32_t i = 0; i < kDebugStatsGpuMemoryTypeCount; ++i)
	{
		auto& src = g_gpu_memory_types[i];
		auto& dst = snapshot.gpu_memory_types[i];
		dst.cached_reuse     = take_window(src.cached_reuse);
		dst.fast_reuse       = take_window(src.fast_reuse);
		dst.exact_reuse      = take_window(src.exact_reuse);
		dst.covered_reuse    = take_window(src.covered_reuse);
		dst.new_standalone   = take_window(src.new_standalone);
		dst.new_linked       = take_window(src.new_linked);
		dst.new_from_objects = take_window(src.new_from_objects);
		dst.reclaim_new      = take_window(src.reclaim_new);
		dst.logical_free     = take_window(src.logical_free);
		dst.live             = src.live.load(std::memory_order_relaxed);
		dst.writeback_calls  = take_window(src.writeback_calls);
		dst.writeback_bytes  = take_window(src.writeback_bytes);
		dst.writeback_ns     = take_window(src.writeback_ns);
		dst.writeback_max_ns = take_window(src.writeback_max_ns);
		dst.hash_calls              = take_window(src.hash_calls);
		dst.hash_bytes              = take_window(src.hash_bytes);
		dst.hash_ns                 = take_window(src.hash_ns);
		dst.hash_max_ns             = take_window(src.hash_max_ns);
		dst.hash_tracked_changed    = take_window(src.hash_tracked_changed);
		dst.hash_tracked_unchanged  = take_window(src.hash_tracked_unchanged);
		dst.hash_fallback_changed   = take_window(src.hash_fallback_changed);
		dst.hash_fallback_unchanged = take_window(src.hash_fallback_unchanged);
	}
	{
		std::lock_guard<std::mutex> slow_create_lock(g_gpu_memory_slow_create_mutex);
		auto& out     = snapshot.gpu_memory_slow_creates;
		out.capacity  = kDebugStatsGpuMemorySlowCreateCapacity;
		out.size      = g_gpu_memory_slow_creates.size;
		out.dropped   = g_gpu_memory_slow_creates.dropped;
		for (uint32_t i = 0; i < out.size; ++i)
		{
			out.records[i] = g_gpu_memory_slow_creates.records[
			    (g_gpu_memory_slow_creates.head + i) % kDebugStatsGpuMemorySlowCreateCapacity];
		}
		if (reset)
		{
			g_gpu_memory_slow_creates.head    = 0;
			g_gpu_memory_slow_creates.size    = 0;
			g_gpu_memory_slow_creates.dropped = 0;
		}
	}

	snapshot.live_objects  = g_live_objects.load(std::memory_order_relaxed);
	snapshot.fps           = g_last_fps.load(std::memory_order_relaxed);
	snapshot.frame_time_ms = g_last_frame_ms.load(std::memory_order_relaxed);

	if (reset)
	{
		g_performance_baseline = {now_ms,          draws,          dispatches,       alloc_bytes,     free_bytes, creates,
		                          frees,           flips,          buffer_flushes,   command_buffers, submits,    fence_waits,
		                          fence_wait_ns,   acquires,       acquire_ns,       presents,        present_ns, wait_reg_mem,
		                          wait_reg_mem_ns, wait_flip_done, wait_flip_done_ns};
	}
	return snapshot;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
