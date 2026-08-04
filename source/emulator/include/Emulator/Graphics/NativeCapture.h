#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_

#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"

#include "Kyty/Core/Threads.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vulkan/vulkan_core.h>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

enum class NativeCaptureMilestone
{
	None,
	FirstPresent,
	Interval,
	Manual,
};

// Window owns presentation and Vulkan lifetime; this controller owns the
// capture policy, counters, and request/result synchronization. Keeping the
// mutable capture state here prevents WindowContext from becoming another
// global registry for tooling concerns.
struct NativeCaptureState
{
	std::filesystem::path directory;
	std::filesystem::path trigger_file;
	bool                  first_present           = false;
	uint32_t              every_present           = 0;
	bool                  telemetry               = false;
	uint32_t              max_edge                = 0;
	uint32_t              keep_files              = 8;
	bool                  first_pending           = false;
	uint64_t              first_probe_after_ms    = 0;
	uint64_t              first_probe_deadline_ms = 0;
	bool                  manual_pending          = false;
	uint64_t              sequence                = 0;
	uint64_t              present_count           = 0;
	double                last_log_time           = 0.0;
	uint64_t              last_present_steady_ms  = 0;
	uint64_t              last_frame_steady_ms    = 0;
	int                   last_seen_frame         = -1;

	Core::Mutex   result_mutex;
	Core::CondVar result_cv;
	uint64_t      request_id   = 0;
	uint64_t      completed_id = 0;
	bool          last_ok      = false;
	std::string   last_path;
	std::string   last_milestone;
	std::string   last_format;
	uint32_t      last_width   = 0;
	uint32_t      last_height  = 0;
	int           last_frame   = 0;
	uint64_t      last_present = 0;
	std::string   last_error_code;
	std::string   last_error_message;

	void Configure(uint64_t now_ms);
	void RecordPresent(uint64_t now_ms);
	void ObserveFrame(int frame, uint64_t now_ms);
	[[nodiscard]] bool TelemetryDue(double now_seconds, double interval_seconds);
};

struct NativeCaptureMetadata
{
	const char* milestone           = "none";
	int         frame               = 0;
	uint64_t    present             = 0;
	const char* title_id            = "unknown-title";
	const char* app_version         = "unknown-version";
	const char* build_revision      = "unknown-revision";
	bool        build_dirty         = false;
	const char* format              = "VK_FORMAT_UNSUPPORTED";
	uint32_t    width               = 0;
	uint32_t    height              = 0;
	uint32_t    source_width        = 0;
	uint32_t    source_height       = 0;
	const char* image_filename      = "";
	uint64_t    host_peak_rss_bytes = 0;
};

[[nodiscard]] bool        NativeCaptureEnvEnabled(const char* name);
[[nodiscard]] uint32_t    NativeCaptureEnvPositive(const char* name);
[[nodiscard]] std::string NativeCaptureSanitizeName(std::string value, const char* default_name);
[[nodiscard]] std::string NativeCaptureUtcStamp();
[[nodiscard]] const char* NativeCaptureFormatName(VkFormat format);
[[nodiscard]] uint64_t    NativeCaptureHostPeakRssBytes();

[[nodiscard]] bool NativeCaptureWriteMetadata(const std::filesystem::path& image_path, const NativeCaptureMetadata& metadata);
void               NativeCapturePruneDirectory(const std::filesystem::path& directory, uint32_t keep_files);

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NATIVECAPTURE_H_ */
