#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// Host-only diagnostic counters. Never gate guest correctness on these values.
struct DebugStatsSnapshot
{
	double   fps              = 0.0;
	double   flip_per_sec     = 0.0;
	double   frame_time_ms    = 0.0;
	double   draws_per_sec    = 0.0;
	double   draws_per_frame  = 0.0;
	double   dispatches_per_sec = 0.0;
	double   alloc_mib_per_sec = 0.0;
	uint64_t live_objects     = 0;
	uint64_t creates_window   = 0;
	uint64_t frees_window     = 0;
	double   cpu_percent      = 0.0;
	double   heap_mib         = 0.0;
	bool     host_sample_ok   = false;
	// Last VideoOut → swapchain blit source (guest display buffer).
	uint32_t present_src_w    = 0;
	uint32_t present_src_h    = 0;
	uint32_t present_dst_w    = 0;
	uint32_t present_dst_h    = 0;
	uint32_t present_src_layout = 0; // VkImageLayout as uint
};

void DebugStatsInit();
void DebugStatsShutdown();

void DebugStatsRecordDraw();
void DebugStatsRecordDispatch();
void DebugStatsRecordAlloc(uint64_t bytes);
void DebugStatsRecordFree(uint64_t bytes);
void DebugStatsRecordFlip(double fps, double frame_time_ms);
// Call from WindowDrawBuffer with the guest display image and swapchain extent.
void DebugStatsRecordPresentSource(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h, uint32_t src_layout);

// Refresh one-second rates and host CPU/RSS. Call from the window/present thread.
DebugStatsSnapshot DebugStatsTick(double now_seconds);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEBUGSTATS_H_ */
