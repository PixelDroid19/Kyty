#include "Emulator/Agent/StallWatch.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

const char* StallCodeName(StallCode code)
{
	switch (code)
	{
		case StallCode::Healthy: return "healthy";
		case StallCode::PresentStalled: return "present_stalled";
		case StallCode::FrameStalled: return "frame_stalled";
		case StallCode::LowFps: return "low_fps";
		case StallCode::NotReady: return "not_ready";
	}
	return "healthy";
}

StallWatchResult ClassifyStall(const StallSample& start, const StallSample& end, const StallWatchArgs& args)
{
	StallWatchResult out {};
	out.start         = start;
	out.end           = end;
	out.frame_delta   = static_cast<int64_t>(end.frame) - static_cast<int64_t>(start.frame);
	out.present_delta = static_cast<int64_t>(end.present) - static_cast<int64_t>(start.present);

	if (args.require_graphic && !end.graphic_ready)
	{
		out.code      = StallCode::NotReady;
		out.healthy   = false;
		out.code_name = StallCodeName(out.code);
		out.hint      = "graphics/window not ready yet";
		return out;
	}

	// Present stall is the strongest host-visible hang signal: the swapchain
	// path stopped completing presents.
	if (out.present_delta <= 0 && end.ms_since_present >= args.present_stall_ms)
	{
		out.code      = StallCode::PresentStalled;
		out.healthy   = false;
		out.code_name = StallCodeName(out.code);
		out.hint      = "present counter did not advance; capture UI and check GPU/HLE waits";
		return out;
	}

	if (out.frame_delta <= 0 && end.ms_since_frame >= args.frame_stall_ms)
	{
		out.code      = StallCode::FrameStalled;
		out.healthy   = false;
		out.code_name = StallCodeName(out.code);
		out.hint      = "game frame counter stalled while presents may still tick";
		return out;
	}

	if (end.fps > 0.0 && end.fps < args.min_fps && out.present_delta <= 1)
	{
		out.code      = StallCode::LowFps;
		out.healthy   = false;
		out.code_name = StallCodeName(out.code);
		out.hint      = "fps below threshold with almost no present progress (loading hang / host thrash)";
		return out;
	}

	out.code      = StallCode::Healthy;
	out.healthy   = true;
	out.code_name = StallCodeName(out.code);
	out.hint      = "progress observed in the watch window";
	return out;
}

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED
