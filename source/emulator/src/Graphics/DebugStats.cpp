#include "Emulator/Graphics/DebugStats.h"

#ifdef KYTY_EMU_ENABLED

#include "Kyty/Sys/SysProcess.h"

#include <atomic>

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
std::atomic<uint32_t> g_present_src_w {0};
std::atomic<uint32_t> g_present_src_h {0};
std::atomic<uint32_t> g_present_dst_w {0};
std::atomic<uint32_t> g_present_dst_h {0};
std::atomic<uint32_t> g_present_src_layout {0};

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

} // namespace

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
	SysProcessSampleReset();
}

void DebugStatsShutdown() {}

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
	g_flips.fetch_add(1, std::memory_order_relaxed);
	g_last_fps.store(fps, std::memory_order_relaxed);
	g_last_frame_ms.store(frame_time_ms, std::memory_order_relaxed);
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
	snap.fps               = g_last_fps.load(std::memory_order_relaxed);
	snap.frame_time_ms     = g_last_frame_ms.load(std::memory_order_relaxed);
	snap.flip_per_sec      = flips_delta / elapsed;
	snap.draws_per_sec     = draws_delta / elapsed;
	snap.dispatches_per_sec = dispatches_delta / elapsed;
	snap.draws_per_frame   = (flips_delta > 0.0) ? (draws_delta / flips_delta) : 0.0;
	snap.alloc_mib_per_sec = (alloc_delta / (1024.0 * 1024.0)) / elapsed;
	snap.live_objects      = g_live_objects.load(std::memory_order_relaxed);
	snap.creates_window    = creates_now - g_window_creates;
	snap.frees_window      = frees_now - g_window_frees;

	const SysProcessSample host = SysProcessSampleNow();
	snap.host_sample_ok         = host.valid;
	snap.cpu_percent            = host.cpu_percent;
	snap.heap_mib               = static_cast<double>(host.heap_bytes) / (1024.0 * 1024.0);
	snap.present_src_w          = g_present_src_w.load(std::memory_order_relaxed);
	snap.present_src_h          = g_present_src_h.load(std::memory_order_relaxed);
	snap.present_dst_w          = g_present_dst_w.load(std::memory_order_relaxed);
	snap.present_dst_h          = g_present_dst_h.load(std::memory_order_relaxed);
	snap.present_src_layout     = g_present_src_layout.load(std::memory_order_relaxed);

	g_last_snapshot          = snap;
	g_window_start_seconds   = now_seconds;
	g_window_draws           = draws_now;
	g_window_dispatches      = dispatches_now;
	g_window_alloc_bytes     = alloc_now;
	g_window_creates         = creates_now;
	g_window_frees           = frees_now;
	g_window_flips           = flips_now;

	return g_last_snapshot;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
