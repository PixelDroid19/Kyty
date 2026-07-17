#ifndef EMULATOR_INCLUDE_EMULATOR_AGENT_STALLWATCH_H_
#define EMULATOR_INCLUDE_EMULATOR_AGENT_STALLWATCH_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Emulator::Agent {

enum class StallCode: uint8_t
{
	Healthy        = 0,
	PresentStalled = 1,
	FrameStalled   = 2,
	LowFps         = 3,
	NotReady       = 4,
};

// Coarse host-visible phase for agent loops (status / wait_phase).
// Loading: presents still complete but FPS is low (loading card / heavy upload).
// Stalled: presents stopped. Interactive: FPS recovered. Not a guest title ID.
enum class PhaseHint: uint8_t
{
	NotReady     = 0,
	Booting      = 1,
	Loading      = 2,
	Interactive  = 3,
	Stalled      = 4,
};

struct StallSample
{
	bool     graphic_ready    = false;
	int      frame            = 0;
	uint64_t present          = 0;
	double   fps              = 0.0;
	uint64_t ms_since_present = 0;
	uint64_t ms_since_frame   = 0;
};

struct StallWatchArgs
{
	uint32_t window_ms        = 10000;
	uint32_t present_stall_ms = 5000;
	uint32_t frame_stall_ms   = 5000;
	double   min_fps          = 2.0;
	bool     require_graphic  = true;
};

struct StallWatchResult
{
	StallCode   code          = StallCode::Healthy;
	bool        healthy       = true;
	const char* code_name     = "healthy";
	const char* hint          = "";
	StallSample start         {};
	StallSample end           {};
	int64_t     frame_delta   = 0;
	int64_t     present_delta = 0;
};

struct PhaseHintArgs
{
	double   interactive_min_fps = 5.0;
	uint64_t stall_ms            = 2000;
	uint64_t boot_present_max    = 10;
};

const char* StallCodeName(StallCode code);
const char* PhaseHintName(PhaseHint phase);
PhaseHint   ClassifyPhaseHint(bool graphic_ready, uint64_t present, double fps, uint64_t ms_since_present,
                              const PhaseHintArgs& args = {});
StallWatchResult ClassifyStall(const StallSample& start, const StallSample& end, const StallWatchArgs& args);

} // namespace Kyty::Emulator::Agent

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AGENT_STALLWATCH_H_ */
