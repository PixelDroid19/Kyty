#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>
#include <string>

#ifdef KYTY_EMU_ENABLED

struct VkSurfaceCapabilitiesKHR;

namespace Kyty::Libs::Graphics {

struct GraphicContext;
struct VideoOutVulkanImage;

struct WindowPresentStats
{
	int      frame           = 0;
	uint64_t present         = 0;
	double   fps             = 0.0;
	bool     capture_ready   = false;
	bool     capture_dir_set = false;
	bool     graphic_ready   = false;
	uint64_t ms_since_present = 0; // host steady-clock since last present++
	uint64_t ms_since_frame   = 0; // host steady-clock since last frame++
};

struct WindowNativeCaptureResult
{
	bool        ok = false;
	std::string path;
	std::string milestone;
	std::string format;
	uint32_t    width   = 0;
	uint32_t    height  = 0;
	uint64_t    present = 0;
	int         frame   = 0;
	std::string error_code;
	std::string error_message;
};

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities();

GraphicContext* WindowGetGraphicContext();

void WindowInit(uint32_t width, uint32_t height);
void WindowRun();
void WindowWaitForGraphicInitialized();
void WindowDrawBuffer(VideoOutVulkanImage* image);

// Realtime agent / tooling seam. Capture still happens on the present path;
// these helpers only request and observe host-side VideoOut readback.
bool WindowGetPresentStats(WindowPresentStats* out);
bool WindowRequestNativeCapture(uint64_t* out_request_id, WindowNativeCaptureResult* error_out);
bool WindowWaitNativeCapture(uint64_t request_id, uint32_t timeout_ms, WindowNativeCaptureResult* out);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
