#include "Emulator/Graphics/Window.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/SafeDelete.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Subsystems.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Timer.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Agent/AgentLifecycle.h"
#include "Emulator/Agent/EventRing.h"
#include "Emulator/Audio.h"
#include "Emulator/Config.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/DebugOverlay.h"
#include "Emulator/Graphics/DebugStats.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Image.h"
#include "Emulator/Graphics/KeyboardInput.h"
#include "Emulator/Graphics/NativeCapture.h"
#include "Emulator/Graphics/Objects/VulkanImageBuilder.h"
#include "Emulator/Graphics/PresentationScaler.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/VulkanQueueIdentity.h"
#include "Emulator/Graphics/WindowControls.h"
#include "Emulator/Host/CaptureImageCodec.h"
#include "Emulator/Host/HostInput.h"
#include "Emulator/Host/HostWindow.h"
#include "Emulator/Loader/SystemContent.h"
#include "Emulator/Log.h"
#include "Emulator/Profiler.h"

#include "KytyBuildInfo.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#include <vulkan/vk_enum_string_helper.h>
#include <vulkan/vk_platform.h>

// IWYU pragma: no_include <intrin.h>

#ifdef KYTY_EMU_ENABLED

// #define KYTY_ENABLE_BEST_PRACTICES
#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Kyty::Libs::Graphics {

constexpr float FPS_AVERAGE_FRAMES = 5.0f;
constexpr float FPS_UPDATE_TIME    = 0.25f;

// Lock-free mirror of the presented-frame counter. Submission-thread tooling
// needs the frame index without touching window state owned by the UI thread.
static std::atomic<int> g_presented_frame_num {0};

void WindowPublishPresentedFrameNum(int frame_num)
{
	g_presented_frame_num.store(frame_num, std::memory_order_relaxed);
}

int WindowGetPresentedFrameNum()
{
	return g_presented_frame_num.load(std::memory_order_relaxed);
}

// KYTY_HLE_TRACE_FRAMES=first-last turns the HLE call log on for a bounded
// presented-frame window. Tracing a full boot costs about 40x of the frame
// budget, so a late phase is unreachable without a window.
static void MaybeToggleFrameWindowHleTrace(int frame_num)
{
	static const char* spec = std::getenv("KYTY_HLE_TRACE_FRAMES");
	if (spec == nullptr || spec[0] == '\0')
	{
		return;
	}
	static int  first  = 0;
	static int  last   = 0;
	static bool parsed = false;
	if (!parsed)
	{
		parsed = true;
		if (std::sscanf(spec, "%d-%d", &first, &last) != 2)
		{
			first = last = 0;
		}
	}
	if (first <= 0 || last < first)
	{
		return;
	}
	if (frame_num == first)
	{
		const char* path = std::getenv("KYTY_HLE_TRACE_FILE");
		Log::SetDirection(Log::Direction::File);
		Log::SetOutputFile(String::FromUtf8(path != nullptr && path[0] != '\0' ? path : "/tmp/kyty-hle-trace.log"));
	} else if (frame_num == last + 1)
	{
		Log::SetDirection(Log::Direction::Silent);
	}
}

using EventKeyboard   = ::Kyty::Emulator::Host::KeyboardEvent;
using EventMouse      = ::Kyty::Emulator::Host::MouseEvent;
using EventFinger     = ::Kyty::Emulator::Host::FingerEvent;
using EventController = ::Kyty::Emulator::Host::ControllerEvent;
using EventDisplay    = ::Kyty::Emulator::Host::DisplayEventData;

// struct GraphicContext;

struct GameApi
{
	bool (*init)(GameApi* game, const Core::Timer& timer, void* data)            = nullptr;
	bool (*render_and_update)(GameApi* game, const Core::Timer& timer)           = nullptr;
	bool (*close)(GameApi* game)                                                 = nullptr;
	void (*event_quit)(GameApi* game)                                            = nullptr;
	void (*event_terminate)(GameApi* game)                                       = nullptr;
	void (*event_keyboard)(GameApi* game, const EventKeyboard* key)              = nullptr;
	void (*event_mouse)(GameApi* game, const EventMouse* mb)                     = nullptr;
	void (*event_finger)(GameApi* game, const EventFinger* f)                    = nullptr;
	void (*event_controller)(GameApi* game, const EventController* f)            = nullptr;
	void (*event_display)(GameApi* game, const EventDisplay* d)                  = nullptr;
	void (*event_low_memory)(GameApi* game)                                      = nullptr;
	void (*event_will_enter_background)(GameApi* game)                           = nullptr;
	void (*event_did_enter_background)(GameApi* game)                            = nullptr;
	void (*event_will_enter_foreground)(GameApi* game)                           = nullptr;
	void (*event_did_enter_foreground)(GameApi* game)                            = nullptr;
	void (*event_resize)(GameApi* game, uint32_t new_width, uint32_t new_height) = nullptr;
	void (*show_window)(GameApi* game, const Core::Timer& timer)                 = nullptr;
	bool (*need_exit)(GameApi* game)                                             = nullptr;
	bool (*is_paused)(GameApi* game)                                             = nullptr;

	int (*poll_event)(GameApi* game)                    = nullptr;
	int (*wait_event)(GameApi* game)                    = nullptr;
	void (*process_event)(GameApi* game, double time_s) = nullptr;

	void* data1 = nullptr;
	void* data2 = nullptr;

	bool     m_game_need_exit        = {false};
	bool     m_game_is_paused        = {false};
	uint32_t m_screen_width          = {0};
	uint32_t m_screen_height         = {0};
	double   m_current_time_seconds  = {0.0};
	double   m_previous_time_seconds = {0.0};
	int      m_update_num            = {0};
	int      m_frame_num             = {0};
	double   m_update_time_seconds   = {0.0};
	double   m_current_fps           = {0.0};
	int      m_max_updates_per_frame = {4};
	double   m_update_fixed_time     = 1.0 / 60.0;
	int      m_fps_frames_num        = {0};
	double   m_fps_start_time        = {0};
};

struct GameApiPrivateStruct
{
	GameApiPrivateStruct() = default;

	Core::Mutex     mutex;
	int             skip_frames = 0;
	GraphicContext* ctx         = nullptr;
};

// void game_main_loop(GameApi* game);

GameApi* game_create_api();
void     game_delete_api(GameApi* api);
int      game_poll_event(GameApi* game);
int      game_wait_event(GameApi* game);
void     game_process_event(GameApi* game, double time_s);

struct VulkanExtensions
{
	bool enable_validation_layers = false;

	Vector<const char*>           required_extensions;
	Vector<VkExtensionProperties> available_extensions;
	Vector<const char*>           required_layers;
	Vector<VkLayerProperties>     available_layers;
};

struct SurfaceCapabilities
{
	VkSurfaceCapabilitiesKHR   capabilities {};
	Vector<VkSurfaceFormatKHR> formats;
	Vector<VkPresentModeKHR>   present_modes;
	bool                       format_srgb_bgra32  = false;
	bool                       format_unorm_bgra32 = false;
};

struct WindowContext
{
	GraphicContext                    graphic_ctx;
	VulkanSwapchain*                  swapchain               = nullptr;
	::Kyty::Emulator::Host::HostWindow* host_window            = nullptr;
	VkSurfaceKHR                      surface                 = nullptr;
	SurfaceCapabilities*              surface_capabilities    = nullptr;
	GameApi*                          game                    = nullptr;
	HostWindowControls                controls;

	char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {0};
	char processor_name[64]                            = {0};

	Core::Mutex   mutex;
	bool          graphic_initialized = false;
	Core::CondVar graphic_initialized_condvar;
	NativeCaptureState native_capture;
};

static WindowContext* g_window_ctx = nullptr;

static uint64_t WindowSteadyMs()
{
	using clock = std::chrono::steady_clock;
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

static bool DumpVideoOutFrameSelected(int frame)
{
	const char* spec = std::getenv("KYTY_DUMP_VIDEOOUT_FRAMES");
	if (spec == nullptr || spec[0] == '\0')
	{
		return frame > 0 && (frame % 200) == 0;
	}

	const char* cursor = spec;
	while (*cursor != '\0')
	{
		char*      end   = nullptr;
		const long start = std::strtol(cursor, &end, 10);
		if (end == cursor)
		{
			return false;
		}

		long finish = start;
		long step   = 1;
		if (*end == '-')
		{
			cursor            = end + 1;
			const long parsed = std::strtol(cursor, &end, 10);
			if (end == cursor)
			{
				return false;
			}
			finish = parsed;
			if (*end == ':')
			{
				cursor      = end + 1;
				const long interval = std::strtol(cursor, &end, 10);
				if (end == cursor || interval <= 0)
				{
					return false;
				}
				step = interval;
			}
		}

		if (start <= finish && frame >= start && frame <= finish && ((frame - start) % step) == 0)
		{
			return true;
		}

		if (*end != ',')
		{
			return false;
		}
		cursor = end + 1;
	}

	return false;
}

static void SetHostCursorVisible(bool visible)
{
	if (g_window_ctx == nullptr || g_window_ctx->host_window == nullptr)
	{
		return;
	}
	g_window_ctx->host_window->SetCursorVisible(visible);
}

static void UpdateHostCursorPolicy(bool temporary_visibility = false)
{
	if (g_window_ctx == nullptr || g_window_ctx->host_window == nullptr)
	{
		return;
	}
	g_window_ctx->host_window->UpdateCursorPolicy(temporary_visibility);
}

static bool ToggleHostFullscreen()
{
	if (g_window_ctx == nullptr || g_window_ctx->host_window == nullptr)
	{
		return false;
	}
	return g_window_ctx->host_window->ToggleFullscreen();
}

static NativeCaptureMilestone NativeCaptureNext(WindowContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	if (ctx->native_capture.directory.empty())
	{
		return NativeCaptureMilestone::None;
	}

	if (ctx->native_capture.first_pending && WindowSteadyMs() >= ctx->native_capture.first_probe_after_ms)
	{
		return NativeCaptureMilestone::FirstPresent;
	}

	const auto next_present = ctx->native_capture.present_count + 1;
	if (ctx->native_capture.every_present != 0 && next_present % ctx->native_capture.every_present == 0)
	{
		return NativeCaptureMilestone::Interval;
	}

	if (!ctx->native_capture.trigger_file.empty())
	{
		std::error_code error;
		if (std::filesystem::exists(ctx->native_capture.trigger_file, error) && !error)
		{
			return NativeCaptureMilestone::Manual;
		}
	}

	if (ctx->native_capture.manual_pending)
	{
		return NativeCaptureMilestone::Manual;
	}

	return NativeCaptureMilestone::None;
}

static const char* NativeCaptureMilestoneName(NativeCaptureMilestone milestone)
{
	switch (milestone)
	{
		case NativeCaptureMilestone::FirstPresent: return "first_present";
		case NativeCaptureMilestone::Interval: return "interval";
		case NativeCaptureMilestone::Manual: return "manual";
		case NativeCaptureMilestone::None: break;
	}
	return "none";
}

static void NativeCapturePublishResult(WindowContext* ctx, bool ok, const char* path, const char* milestone, const char* format,
                                       uint32_t width, uint32_t height, int frame, const char* error_code, const char* error_message)
{
	EXIT_IF(ctx == nullptr);

	Core::LockGuard lock(ctx->native_capture.result_mutex);
	ctx->native_capture.manual_pending     = false;
	ctx->native_capture.completed_id       = ctx->native_capture.request_id;
	ctx->native_capture.last_ok            = ok;
	ctx->native_capture.last_path          = path != nullptr ? path : "";
	ctx->native_capture.last_milestone     = milestone != nullptr ? milestone : "none";
	ctx->native_capture.last_format        = format != nullptr ? format : "";
	ctx->native_capture.last_width         = width;
	ctx->native_capture.last_height        = height;
	ctx->native_capture.last_frame         = frame;
	ctx->native_capture.last_present       = ctx->native_capture.present_count;
	ctx->native_capture.last_error_code    = error_code != nullptr ? error_code : "";
	ctx->native_capture.last_error_message = error_message != nullptr ? error_message : "";
	ctx->native_capture.result_cv.Signal();

	if (ok)
	{
		Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Capture, "capture_ok", path != nullptr ? path : "");
	} else
	{
		Emulator::Agent::EventRing::Instance().Push(Emulator::Agent::EventKind::Error,
		                                            error_code != nullptr ? error_code : "capture_failed",
		                                            error_message != nullptr ? error_message : "");
	}
}

static void NativeCaptureFrame(WindowContext* ctx, VideoOutVulkanImage* image, int frame, NativeCaptureMilestone milestone)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);
	EXIT_IF(milestone == NativeCaptureMilestone::None);

	const bool agent_waiting = [&]()
	{
		Core::LockGuard lock(ctx->native_capture.result_mutex);
		return ctx->native_capture.manual_pending || ctx->native_capture.request_id > ctx->native_capture.completed_id;
	}();

	if (milestone == NativeCaptureMilestone::Manual && !ctx->native_capture.trigger_file.empty())
	{
		std::error_code error;
		std::filesystem::remove(ctx->native_capture.trigger_file, error);
	}

	const bool hdr_capture = (image->format == VK_FORMAT_R16G16B16A16_SFLOAT);
	if (image->format != VK_FORMAT_B8G8R8A8_SRGB && image->format != VK_FORMAT_R8G8B8A8_SRGB && !hdr_capture)
	{
		if (milestone == NativeCaptureMilestone::FirstPresent)
		{
			ctx->native_capture.first_pending = false;
		}
		std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=readback frame=%d format=%s recoverable=0\n", frame,
		             NativeCaptureFormatName(image->format));
		if (agent_waiting)
		{
			NativeCapturePublishResult(ctx, false, nullptr, NativeCaptureMilestoneName(milestone), NativeCaptureFormatName(image->format),
			                           0, 0, frame, "unsupported_format", "native capture requires B8G8R8A8_SRGB or R8G8B8A8_SRGB");
		}
		return;
	}

	const uint64_t width  = image->extent.width;
	const uint64_t height = image->extent.height;
	if (width == 0 || height == 0 || width > UINT64_MAX / height || width * height > UINT64_MAX / 8)
	{
		if (milestone == NativeCaptureMilestone::FirstPresent)
		{
			ctx->native_capture.first_pending = false;
		}
		std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=validate_extent frame=%d recoverable=0\n", frame);
		if (agent_waiting)
		{
			NativeCapturePublishResult(ctx, false, nullptr, NativeCaptureMilestoneName(milestone), NativeCaptureFormatName(image->format),
			                           0, 0, frame, "invalid_extent", "native capture extent is invalid");
		}
		return;
	}

	// HDR (16:16:16:16 float) sources are read back as floats and converted to
	// RGBA8 with a saturating clamp for the PNG; no tone mapping.
	const uint64_t bpp  = hdr_capture ? 8u : 4u;
	const uint64_t size = width * height * bpp;
	std::vector<uint8_t> pixels(size);
	UtilFillBuffer(&ctx->graphic_ctx, pixels.data(), size, static_cast<uint32_t>(width), image,
	               static_cast<uint64_t>(image->layout));
	const auto capture_pixel_format = hdr_capture ? Emulator::Host::HostCaptureImagePixelFormat::Rgba16G16B16A16Sfloat
	                                              : (image->format == VK_FORMAT_B8G8R8A8_SRGB
	                                                     ? Emulator::Host::HostCaptureImagePixelFormat::Bgra8
	                                                     : Emulator::Host::HostCaptureImagePixelFormat::Rgba8);
	if (hdr_capture)
	{
		std::vector<uint8_t> converted;
		if (!Emulator::Host::HostCaptureImageCodecNormalizeRgba8(
		        {pixels.data(), {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, width * bpp, capture_pixel_format}, &converted))
		{
			std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=convert_image frame=%d recoverable=0\n", frame);
			if (agent_waiting)
			{
				NativeCapturePublishResult(ctx, false, nullptr, NativeCaptureMilestoneName(milestone), NativeCaptureFormatName(image->format),
				                           static_cast<uint32_t>(width), static_cast<uint32_t>(height), frame, "convert_image",
				                           "failed to normalize native capture image");
			}
			return;
		}
		pixels = std::move(converted);
	}
	if (milestone == NativeCaptureMilestone::FirstPresent)
	{
		const uint64_t now_ms = WindowSteadyMs();
		if (!NativeCaptureFrameHasVisibleColor(pixels.data(), pixels.size()) && now_ms < ctx->native_capture.first_probe_deadline_ms)
		{
			ctx->native_capture.first_probe_after_ms = now_ms + 1000;
			return;
		}
		ctx->native_capture.first_pending = false;
	}

	Core::String title_id;
	Core::String app_ver;
	Loader::SystemContentGetMetadata(&title_id, &app_ver);
	const auto title_name   = NativeCaptureSanitizeName(title_id.C_Str(), "unknown-title");
	const auto version_name = NativeCaptureSanitizeName(app_ver.C_Str(), "unknown-version");
	const auto revision     = NativeCaptureSanitizeName(BuildInfo::Revision, "unknown-revision");
	const auto stamp        = NativeCaptureUtcStamp();
	const auto sequence     = ctx->native_capture.sequence++;
	const auto filename     = title_name + "-" + version_name + "-" + revision + "-frame-" + std::to_string(frame) + "-" + stamp + "-" +
	                          std::to_string(sequence) + ".png";

	std::error_code error;
	std::filesystem::create_directories(ctx->native_capture.directory, error);
	if (error)
	{
		std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=create_directory frame=%d recoverable=0\n", frame);
		if (agent_waiting)
		{
			NativeCapturePublishResult(ctx, false, nullptr, NativeCaptureMilestoneName(milestone), NativeCaptureFormatName(image->format),
			                           static_cast<uint32_t>(width), static_cast<uint32_t>(height), frame, "create_directory",
			                           "failed to create capture directory");
		}
		return;
	}

	const auto image_path = ctx->native_capture.directory / filename;
	const auto codec_result = Emulator::Host::HostCaptureImageCodecWritePng(
	    {pixels.data(), {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, width * 4u,
	     hdr_capture ? Emulator::Host::HostCaptureImagePixelFormat::Rgba8 : capture_pixel_format},
	    ctx->native_capture.max_edge, image_path);
	if (codec_result.downscale_fallback)
	{
		std::fprintf(stderr, "KYTY_CAPTURE_WARN subsystem=frame_capture operation=downscale frame=%d kept_full=1\n", frame);
	}
	if (!codec_result.success)
	{
		const bool create_surface = codec_result.error == Emulator::Host::HostCaptureImageCodecError::CreateSurface;
		std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=%s frame=%d recoverable=0\n",
		             create_surface ? "create_surface" : "save_image", frame);
		if (agent_waiting)
		{
			NativeCapturePublishResult(ctx, false, nullptr, NativeCaptureMilestoneName(milestone), NativeCaptureFormatName(image->format),
			                           static_cast<uint32_t>(width), static_cast<uint32_t>(height), frame,
			                           create_surface ? "create_surface" : "save_image",
			                           create_surface ? "SDL_CreateRGBSurfaceFrom failed" : "failed to write native capture PNG");
		}
		return;
	}
	const uint32_t out_width  = codec_result.output_extent.width;
	const uint32_t out_height = codec_result.output_extent.height;

	NativeCapturePruneDirectory(ctx->native_capture.directory, ctx->native_capture.keep_files);

	const auto            image_filename = image_path.filename().string();
	NativeCaptureMetadata metadata {};
	metadata.milestone           = NativeCaptureMilestoneName(milestone);
	metadata.frame               = frame;
	metadata.present             = ctx->native_capture.present_count;
	metadata.title_id            = title_name.c_str();
	metadata.app_version         = version_name.c_str();
	metadata.build_revision      = revision.c_str();
	metadata.build_dirty         = BuildInfo::Dirty;
	metadata.format              = NativeCaptureFormatName(image->format);
	metadata.width               = out_width;
	metadata.height              = out_height;
	metadata.source_width        = static_cast<uint32_t>(width);
	metadata.source_height       = static_cast<uint32_t>(height);
	metadata.image_filename      = image_filename.c_str();
	metadata.host_peak_rss_bytes = NativeCaptureHostPeakRssBytes();
	if (!NativeCaptureWriteMetadata(image_path, metadata))
	{
		std::fprintf(stderr, "KYTY_CAPTURE_ERROR subsystem=frame_capture operation=save_metadata frame=%d recoverable=0\n", frame);
		if (agent_waiting)
		{
			NativeCapturePublishResult(ctx, false, image_path.string().c_str(), NativeCaptureMilestoneName(milestone),
			                           NativeCaptureFormatName(image->format), out_width, out_height, frame, "save_metadata",
			                           "failed to write capture metadata");
		}
		return;
	}

	std::fprintf(stderr, "KYTY_NATIVE_CAPTURE milestone=%s frame=%d present=%llu file=%s format=%s size=%ux%u source=%llux%llu\n",
	             NativeCaptureMilestoneName(milestone), frame, static_cast<unsigned long long>(ctx->native_capture.present_count),
	             image_path.string().c_str(), NativeCaptureFormatName(image->format), out_width, out_height,
	             static_cast<unsigned long long>(width), static_cast<unsigned long long>(height));

	if (agent_waiting)
	{
		NativeCapturePublishResult(ctx, true, image_path.string().c_str(), NativeCaptureMilestoneName(milestone),
		                           NativeCaptureFormatName(image->format), out_width, out_height, frame, nullptr, nullptr);
	}
}

static void CalcFrameTime(GameApi* game, double game_time_s)
{
	game->m_previous_time_seconds = game->m_current_time_seconds;
	game->m_current_time_seconds  = game_time_s;

	game->m_frame_num++;
	WindowPublishPresentedFrameNum(game->m_frame_num);
	MaybeToggleFrameWindowHleTrace(game->m_frame_num);
	// Agent observation at the real frame producer (not status poll).
	if (game->m_frame_num == 1)
	{
		Emulator::Agent::Lifecycle::EmitFirstFrame(1);
	}

	int fps_model = 1;

	if (fps_model == 1)
	{
		game->m_fps_frames_num++;
		if (game->m_current_time_seconds - game->m_fps_start_time > FPS_UPDATE_TIME)
		{
			game->m_current_fps    = static_cast<double>(game->m_fps_frames_num) / (game->m_current_time_seconds - game->m_fps_start_time);
			game->m_fps_frames_num = 0;
			game->m_fps_start_time = game->m_current_time_seconds;
		}
	} else
	{
		game->m_current_fps = (1.0f / (game->m_current_time_seconds - game->m_previous_time_seconds)) * (1.0f / FPS_AVERAGE_FRAMES) +
		                      game->m_current_fps * (1.0f - (1.0f / FPS_AVERAGE_FRAMES));
	}
}

static bool Init(GameApi* /*game*/)
{
	return true;
}
static bool Update(GameApi* /*game*/)
{
	return true;
}
static bool Render(GameApi* /*game*/)
{
	return true;
}
static bool Close(GameApi* /*game*/)
{
	return true;
}
static void SetPause(GameApi* game, bool flag)
{
	printf("Pause: %s\n", flag ? "true" : "false");

	Audio::AudioOut::AudioOutSetHostPaused(flag);
	game->m_game_is_paused = flag;
}

static bool RenderAndUpdate(GameApi* game)
{
	static double lag = 0.0;

	lag += game->m_current_time_seconds - game->m_previous_time_seconds;

	int num = 0;

	bool ok = true;

	while (lag >= game->m_update_fixed_time)
	{
		if (num < game->m_max_updates_per_frame)
		{
			ok = ok && Update(game);

			game->m_update_num++;
			num++;
			game->m_update_time_seconds = game->m_update_num * game->m_update_fixed_time;
		}

		lag -= game->m_update_fixed_time;
	}

	ok = ok && Render(game);

	return ok;
}

bool game_init(GameApi* game, const Core::Timer& timer, void* data)
{
	EXIT_IF(game == nullptr);
	EXIT_IF(data == nullptr);
	EXIT_IF(game->data1 || game->data2);

	auto* ctx = static_cast<GraphicContext*>(data);

	EXIT_IF(ctx->screen_width == 0 || ctx->screen_height == 0);

	auto* pdata = new GameApiPrivateStruct;
	pdata->ctx  = ctx;

	game->data1 = pdata;
	game->data2 = ::Kyty::Emulator::Host::HostInput::Create();

	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;

	// The first presentation shows the host window.

	CalcFrameTime(game, timer.GetTimeS());

	return Init(game);
}

bool game_render_and_update(GameApi* game, const Core::Timer& /*timer*/)
{
	return RenderAndUpdate(game);
}

bool game_close(GameApi* game)
{
	EXIT_IF(!game);

	EXIT_IF(!game->data1 || !game->data2);

	delete (static_cast<GameApiPrivateStruct*>(game->data1));
	delete (static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2));

	return Close(game);
}

void game_show_window(GameApi* game, const Core::Timer& timer)
{
	EXIT_IF(!game);

	auto* p = static_cast<GameApiPrivateStruct*>(game->data1);

	EXIT_IF(!p);
	UpdateHostCursorPolicy();

	p->mutex.Lock();
	{
		if (p->skip_frames > 0)
		{
			p->skip_frames--;
			printf("skip frame %d\n", p->skip_frames);
		} else
		{
			VideoOut::VideoOutBeginVblank();
			if (VideoOut::VideoOutFlipWindow(100000))
			{
				CalcFrameTime(game, timer.GetTimeS());
			}
			VideoOut::VideoOutEndVblank();
		}
	}
	p->mutex.Unlock();
}

void game_event_quit(GameApi* game)
{
	printf("Event: quit\n");

	game->m_game_need_exit = true;
}

void game_event_terminate(GameApi* game)
{
	printf("Event: terminate\n");

	game->m_game_need_exit = true;
}

static KeyboardLeftStickState g_keyboard_left_stick;

static void ApplyKeyboardLeftStickControllerAxes(const KeyboardLeftStickUpdate& update)
{
	if (!update.changed)
	{
		return;
	}
	Controller::ControllerAxis(Controller::CONTROLLER_KEYBOARD_ID, Controller::Axis::LeftX, update.axes.x);
	Controller::ControllerAxis(Controller::CONTROLLER_KEYBOARD_ID, Controller::Axis::LeftY, update.axes.y);
}

void game_event_keyboard(GameApi* game, const EventKeyboard* key)
{
	if (NativeCaptureEnvEnabled("KYTY_INPUT_LOG") && key != nullptr && (key->down || key->up))
	{
		std::fprintf(stderr, "KYTY_INPUT_EDGE key=%d down=%d up=%d pressed=%d released=%d\n", key->key_code, key->down ? 1 : 0,
		             key->up ? 1 : 0, key->pressed ? 1 : 0, key->released ? 1 : 0);
	}

#ifdef KYTY_DBG_INPUT
	printf("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n", key->timestamp_seconds,
	       (key->down ? "down" : ""), (key->up ? "up" : ""), (key->pressed ? "pressed" : ""), (key->released ? "released" : ""),
	       (key->repeat ? "repeat" : ""), key->scan_code, key->key_code, key->mod);
#endif

	HostWindowKey host_key {};
	host_key.pressed = key->down;
	host_key.repeat  = key->repeat;
	host_key.escape  = ::Kyty::Emulator::Host::HostInput::IsEscapeKey(key->key_code);
	host_key.pause   = ::Kyty::Emulator::Host::HostInput::IsPauseKey(key->key_code);
	host_key.f11     = ::Kyty::Emulator::Host::HostInput::IsF11Key(key->key_code);
	host_key.enter   = ::Kyty::Emulator::Host::HostInput::IsEnterKey(key->key_code);
	host_key.alt     = ::Kyty::Emulator::Host::HostInput::HasAltModifier(key->mod);

	const bool enter_allowed_before = g_window_ctx == nullptr || g_window_ctx->controls.GuestEnterAllowed();
	const auto host_command         = g_window_ctx == nullptr ? HostWindowCommand::None : g_window_ctx->controls.HandleKey(host_key);
	const bool enter_allowed_after  = g_window_ctx == nullptr || g_window_ctx->controls.GuestEnterAllowed();
	const bool suppress_guest_enter = host_key.enter && (!enter_allowed_before || !enter_allowed_after);

	switch (host_command)
	{
		case HostWindowCommand::Quit: game->m_game_need_exit = true; break;
		case HostWindowCommand::TogglePause: SetPause(game, !game->m_game_is_paused); break;
		case HostWindowCommand::ToggleFullscreen: ToggleHostFullscreen(); break;
		case HostWindowCommand::None: break;
	}

	// Virtual pad (id 0): keyboard maps to DualShock buttons so splash/menus are skippable
	// without a physical controller. Repeat is ignored to avoid stuck buttons.
	if (!key->repeat && (key->down || key->up) && !suppress_guest_enter)
	{
		const bool down = key->down;
		ApplyKeyboardLeftStickControllerAxes(ApplyKeyboardLeftStickKey(g_keyboard_left_stick, key->key_code, down));
		uint32_t button = 0;
		switch (::Kyty::Emulator::Host::HostInput::ClassifyKeyboardAction(key->key_code))
		{
			case ::Kyty::Emulator::Host::KeyboardAction::Cross: button = Controller::PAD_BUTTON_CROSS; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Circle: button = Controller::PAD_BUTTON_CIRCLE; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Square: button = Controller::PAD_BUTTON_SQUARE; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Triangle: button = Controller::PAD_BUTTON_TRIANGLE; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Up: button = Controller::PAD_BUTTON_UP; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Down: button = Controller::PAD_BUTTON_DOWN; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Left: button = Controller::PAD_BUTTON_LEFT; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Right: button = Controller::PAD_BUTTON_RIGHT; break;
			case ::Kyty::Emulator::Host::KeyboardAction::L1: button = Controller::PAD_BUTTON_L1; break;
			case ::Kyty::Emulator::Host::KeyboardAction::R1: button = Controller::PAD_BUTTON_R1; break;
			case ::Kyty::Emulator::Host::KeyboardAction::Options: button = Controller::PAD_BUTTON_OPTIONS; break;
			case ::Kyty::Emulator::Host::KeyboardAction::None: break;
		}
		if (button != 0)
		{
			Controller::ControllerButton(Controller::CONTROLLER_KEYBOARD_ID, button, down);
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	// Host debug HUD toggle (does not map to guest pad).
	if (key->down && !key->repeat && ::Kyty::Emulator::Host::HostInput::IsF1Key(key->key_code))
	{
		DebugOverlayToggle();
	}
#endif
}

void game_event_mouse([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventMouse* mb)
{
#ifdef KYTY_DBG_INPUT
	if (mb->wheel)
	{
		printf("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb->timestamp_seconds, (mb->touch ? "touch, " : ""), mb->x, mb->y);
	} else if (mb->motion)
	{
		printf("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n", mb->timestamp_seconds, (mb->left ? "left" : ""),
		       (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""), (mb->x2 ? "x2" : ""),
		       (mb->touch ? "_touch" : ""), mb->x, mb->y, mb->motion_x, mb->motion_y);
	} else
	{
		printf("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n", mb->timestamp_seconds, mb->num_of_clicks,
		       (mb->left ? "left" : ""), (mb->middle ? "middle" : ""), (mb->right ? "right" : ""), (mb->x1 ? "x1" : ""),
		       (mb->x2 ? "x2" : ""), (mb->touch ? "_touch" : ""), (mb->down ? "down" : ""), (mb->up ? "up" : ""),
		       (mb->pressed ? "pressed" : ""), (mb->released ? "released" : ""), mb->x, mb->y);
	}
#endif
	if (mb->motion || mb->down)
	{
		UpdateHostCursorPolicy(true);
	}
	if (HostWindowControls::HandlePrimaryClick(mb->left, mb->down, static_cast<uint8_t>(mb->num_of_clicks)) ==
	    HostWindowCommand::ToggleFullscreen)
	{
		ToggleHostFullscreen();
	}
}

void game_event_finger([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventFinger* f)
{
#ifdef KYTY_DBG_INPUT
	if (f->motion)
	{
		printf("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure = %f\n", f->timestamp_seconds,
		       f->touch_id, f->finger_id, f->x, f->y, f->dx, f->dy, f->pressure);
	} else
	{
		printf("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure = %f\n", f->timestamp_seconds,
		       f->touch_id, f->finger_id, (f->down ? "down" : ""), (f->up ? "up" : ""), f->x, f->y, f->dx, f->dy, f->pressure);
	}
#endif
}

// static int controller_get_axis(int min, int max, int value)
//{
//	int v = (255 * (value - min)) / (max - min);
//	return (v < 0 ? 0 : (v > 255 ? 255 : v));
//}

void game_event_controller([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventController* f)
{
#ifdef KYTY_DBG_INPUT
	if (f->added || f->removed)
	{
		printf("Controller %s: %d, time = %.04f\n", (f->added ? "added" : "removed"), f->id, f->timestamp_seconds);
	} else if (f->axis)
	{
		printf("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f->id, static_cast<int>(f->axis_id), f->axis_value,
		       f->timestamp_seconds);
	} else
	{
		printf("Controller button: "
		       "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		       f->id, (f->down ? "down" : ""), (f->up ? "up" : ""), (f->pressed ? "pressed" : ""), (f->released ? "released" : ""),
		       static_cast<int>(f->button), f->timestamp_seconds);
	}
#endif
	if (f->remapped)
	{
		// SDL has already updated the active mapping. Subsequent axis/button
		// events use it; the guest connection identity remains unchanged.
		return;
	}

	if (f->added)
	{
		auto* input = static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2);
		int   id    = 0;
		if (input == nullptr || !input->OpenController(f->id, &id))
		{
			std::fprintf(stderr, "Kyty controller open failed for device %d: %s\n", f->id,
			             input == nullptr ? "host input unavailable" : input->LastError());
			return;
		}
		Controller::ControllerConnect(id);
	}

	if (f->removed)
	{
		Controller::ControllerDisconnect(f->id);
		if (auto* input = static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2); input != nullptr)
		{
			input->CloseController(f->id);
		}
	}

	if (f->down || f->up)
	{
		uint32_t button = 0;
		switch (f->button)
		{
			case ::Kyty::Emulator::Host::ControllerButton::A: button = Controller::PAD_BUTTON_CROSS; break;
			case ::Kyty::Emulator::Host::ControllerButton::B: button = Controller::PAD_BUTTON_CIRCLE; break;
			case ::Kyty::Emulator::Host::ControllerButton::X: button = Controller::PAD_BUTTON_SQUARE; break;
			case ::Kyty::Emulator::Host::ControllerButton::Y: button = Controller::PAD_BUTTON_TRIANGLE; break;
			case ::Kyty::Emulator::Host::ControllerButton::Back: button = Controller::PAD_BUTTON_TOUCH_PAD; break;
			case ::Kyty::Emulator::Host::ControllerButton::Guide: break;
			case ::Kyty::Emulator::Host::ControllerButton::Start: button = Controller::PAD_BUTTON_OPTIONS; break;
			case ::Kyty::Emulator::Host::ControllerButton::LeftStick: button = Controller::PAD_BUTTON_L3; break;
			case ::Kyty::Emulator::Host::ControllerButton::RightStick: button = Controller::PAD_BUTTON_R3; break;
			case ::Kyty::Emulator::Host::ControllerButton::LeftShoulder: button = Controller::PAD_BUTTON_L1; break;
			case ::Kyty::Emulator::Host::ControllerButton::RightShoulder: button = Controller::PAD_BUTTON_R1; break;
			case ::Kyty::Emulator::Host::ControllerButton::DpadUp: button = Controller::PAD_BUTTON_UP; break;
			case ::Kyty::Emulator::Host::ControllerButton::DpadDown: button = Controller::PAD_BUTTON_DOWN; break;
			case ::Kyty::Emulator::Host::ControllerButton::DpadLeft: button = Controller::PAD_BUTTON_LEFT; break;
			case ::Kyty::Emulator::Host::ControllerButton::DpadRight: button = Controller::PAD_BUTTON_RIGHT; break;
			case ::Kyty::Emulator::Host::ControllerButton::Invalid: break;
			default: break;
		}
		if (button != 0)
		{
			Controller::ControllerButton(f->id, button, f->down);
		}
	}

	if (f->axis)
	{
		const int value = ::Kyty::Emulator::Host::HostInput::NormalizeAxis(f->axis_id, f->axis_value);

		Controller::Axis axis = Controller::Axis::AxisMax;
		switch (f->axis_id)
		{
			case ::Kyty::Emulator::Host::ControllerAxis::LeftX: axis = Controller::Axis::LeftX; break;
			case ::Kyty::Emulator::Host::ControllerAxis::LeftY: axis = Controller::Axis::LeftY; break;
			case ::Kyty::Emulator::Host::ControllerAxis::RightX: axis = Controller::Axis::RightX; break;
			case ::Kyty::Emulator::Host::ControllerAxis::RightY: axis = Controller::Axis::RightY; break;
			case ::Kyty::Emulator::Host::ControllerAxis::TriggerLeft: axis = Controller::Axis::TriggerLeft; break;
			case ::Kyty::Emulator::Host::ControllerAxis::TriggerRight: axis = Controller::Axis::TriggerRight; break;
			case ::Kyty::Emulator::Host::ControllerAxis::Invalid: break;
		}

		if (axis != Controller::Axis::AxisMax)
		{
			Controller::ControllerAxis(f->id, axis, value);
		}
	}
}

void game_event_display([[maybe_unused]] GameApi* game, [[maybe_unused]] const EventDisplay* d)
{
	auto* p   = static_cast<GameApiPrivateStruct*>(game->data1);
	auto* ctx = static_cast<GraphicContext*>(p->ctx);

	p->mutex.Lock();
	game->m_screen_width  = ctx->screen_width;
	game->m_screen_height = ctx->screen_height;
	p->mutex.Unlock();
}

void game_event_low_memory(GameApi* /*game*/)
{
	printf("Event: low_memory\n");
}

void game_event_will_enter_background(GameApi* game)
{
	printf("Event: will_enter_background\n");

	SetPause(game, true);
}

void game_event_did_enter_background(GameApi* /*game*/)
{
	printf("Event: did_enter_background\n");
}

void game_event_will_enter_foreground(GameApi* /*game*/)
{
	printf("Event: will_enter_foreground\n");
}

void game_event_did_enter_foreground(GameApi* game)
{
	printf("Event: did_enter_foreground\n");

	SetPause(game, false);
}

bool game_need_exit(GameApi* game)
{
	return game->m_game_need_exit;
}

bool game_is_paused(GameApi* game)
{
	return game->m_game_is_paused ||
	       (g_window_ctx != nullptr && g_window_ctx->host_window != nullptr && g_window_ctx->host_window->IsMinimized());
}

void game_event_resize(GameApi* game, uint32_t new_width, uint32_t new_height)
{
	EXIT_IF(new_width == 0 || new_height == 0);
	EXIT_IF(!game);

	auto* p = static_cast<GameApiPrivateStruct*>(game->data1);
	EXIT_IF(p == nullptr);

	auto* ctx = static_cast<GraphicContext*>(p->ctx);
	EXIT_IF(ctx == nullptr);

	p->mutex.Lock();
	{
		p->skip_frames++;
		ctx->screen_width  = new_width;
		ctx->screen_height = new_height;

		game->m_screen_width  = ctx->screen_width;
		game->m_screen_height = ctx->screen_height;
	}
	p->mutex.Unlock();
}

static void process_window_event(GameApi* game, const ::Kyty::Emulator::Host::WindowEventData& window)
{
	switch (window.event)
	{
		case ::Kyty::Emulator::Host::WindowEvent::Shown: printf("Window %" PRIu32 " shown\n", window.window_id); break;

		case ::Kyty::Emulator::Host::WindowEvent::Hidden: printf("Window %" PRIu32 " hidden\n", window.window_id); break;

		case ::Kyty::Emulator::Host::WindowEvent::Exposed: printf("Window %" PRIu32 " exposed\n", window.window_id); break;

		case ::Kyty::Emulator::Host::WindowEvent::Moved:
			printf("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window.window_id, window.data1, window.data2);
			break;

		case ::Kyty::Emulator::Host::WindowEvent::Resized:
		case ::Kyty::Emulator::Host::WindowEvent::SizeChanged:
			printf("Window %" PRIu32 " drawable size changed to %" PRId32 "x%" PRId32 "\n", window.window_id, window.data1, window.data2);
			if (game->event_resize != nullptr && window.data1 > 0 && window.data2 > 0 &&
			    (game->m_screen_width != static_cast<uint32_t>(window.data1) ||
			     game->m_screen_height != static_cast<uint32_t>(window.data2)))
			{
				game->event_resize(game, window.data1, window.data2);
			}
			break;

		case ::Kyty::Emulator::Host::WindowEvent::Minimized:
			printf("Window %" PRIu32 " minimized\n", window.window_id);
			if (g_window_ctx != nullptr && g_window_ctx->host_window != nullptr)
			{
				g_window_ctx->host_window->SetMinimized(true);
				SetHostCursorVisible(true);
			}
			break;
		case ::Kyty::Emulator::Host::WindowEvent::Maximized: printf("Window %" PRIu32 " maximized\n", window.window_id); break;
		case ::Kyty::Emulator::Host::WindowEvent::Restored:
			printf("Window %" PRIu32 " restored\n", window.window_id);
			if (g_window_ctx != nullptr && g_window_ctx->host_window != nullptr)
			{
				g_window_ctx->host_window->SetMinimized(false);
				UpdateHostCursorPolicy();
			}
			break;
		case ::Kyty::Emulator::Host::WindowEvent::Enter: printf("Mouse entered window %" PRIu32 "\n", window.window_id); break;
		case ::Kyty::Emulator::Host::WindowEvent::Leave: printf("Mouse left window %" PRIu32 "\n", window.window_id); break;
		case ::Kyty::Emulator::Host::WindowEvent::FocusGained:
			printf("Window %" PRIu32 " gained keyboard focus\n", window.window_id);
			if (g_window_ctx != nullptr && g_window_ctx->host_window != nullptr)
			{
				g_window_ctx->host_window->SetFocused(true);
				g_window_ctx->controls.SetFocused(true);
				const auto* input = game == nullptr ? nullptr : static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2);
				g_window_ctx->controls.ReconcileEnter(input != nullptr && input->EnterPressed());
				UpdateHostCursorPolicy();
			}
			break;
		case ::Kyty::Emulator::Host::WindowEvent::FocusLost:
			printf("Window %" PRIu32 " lost keyboard focus\n", window.window_id);
			if (g_window_ctx != nullptr && g_window_ctx->host_window != nullptr)
			{
				g_window_ctx->host_window->SetFocused(false);
				g_window_ctx->controls.SetFocused(false);
				SetHostCursorVisible(true);
			}
			ApplyKeyboardLeftStickControllerAxes(ResetKeyboardLeftStick(g_keyboard_left_stick));
			break;
		case ::Kyty::Emulator::Host::WindowEvent::Close:
			printf("Window %" PRIu32 " closed\n", window.window_id);
			game->m_game_need_exit = true;
			break;
		default:
			printf("Window %" PRIu32 " got unknown event %" PRIu8 "\n", window.window_id,
			       static_cast<uint8_t>(window.event));
			break;
	}
}

static void process_display_event(GameApi* game, const ::Kyty::Emulator::Host::DisplayEventData& display)
{
	if (!display.native_orientation &&
	    display.orientation_value != static_cast<int32_t>(::Kyty::Emulator::Host::DisplayOrientation::DisplayEventOrientation))
	{
		printf("Display %" PRIu32 " got unknown orientation event 0x%" PRIx32 "\n", display.display,
		       static_cast<uint32_t>(display.orientation_value));
		return;
	}

	printf("Display %" PRIu32 "[%s] changed orientation to %" PRId32 " - ", display.display,
	       display.native_orientation ? "SDL" : "Kyty", display.orientation_value);
	switch (display.orientation)
	{
		case ::Kyty::Emulator::Host::DisplayOrientation::Unknown: printf("UNKNOWN\n"); break;
		case ::Kyty::Emulator::Host::DisplayOrientation::Landscape: printf("LANDSCAPE\n"); break;
		case ::Kyty::Emulator::Host::DisplayOrientation::LandscapeFlipped: printf("LANDSCAPE_FLIPPED\n"); break;
		case ::Kyty::Emulator::Host::DisplayOrientation::Portrait: printf("PORTRAIT\n"); break;
		case ::Kyty::Emulator::Host::DisplayOrientation::PortraitFlipped: printf("PORTRAIT_FLIPPED\n"); break;
		default: printf("???\n"); break;
	}

	if (!display.native_orientation && game->event_display != nullptr)
	{
		EventDisplay d = display;
		game->event_display(game, &d);
	}
}

int game_poll_event(GameApi* game)
{
	EXIT_IF(!game);

	auto* event = static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2);

	EXIT_IF(!event);

	return event->Poll();
}

int game_wait_event(GameApi* game)
{
	EXIT_IF(!game);

	auto* event = static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2);

	EXIT_IF(!event);

	return event->Wait();
}

void game_process_event(GameApi* game, double time_s)
{
	EXIT_IF(!game);

	auto* input = static_cast<::Kyty::Emulator::Host::HostInput*>(game->data2);

	EXIT_IF(input == nullptr);

	const auto* event = input->GetEvent();
	EXIT_IF(event == nullptr);

	DebugOverlayProcessEvent(input->GetNativeEvent());

	// printf("Event: 0x%04" PRIx32 "\n", event.type);

	EXIT_IF(!input->DisplayEventsEnabled());

	switch (event->type)
	{
		case ::Kyty::Emulator::Host::InputEventType::Quit:
			if (game->event_quit != nullptr)
			{
				game->event_quit(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::Terminate:
			if (game->event_terminate != nullptr)
			{
				game->event_terminate(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::LowMemory:
			if (game->event_low_memory != nullptr)
			{
				game->event_low_memory(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::WillEnterBackground:
			if (game->event_will_enter_background != nullptr)
			{
				game->event_will_enter_background(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::DidEnterBackground:
			if (game->event_did_enter_background != nullptr)
			{
				game->event_did_enter_background(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::WillEnterForeground:
			if (game->event_will_enter_foreground != nullptr)
			{
				game->event_will_enter_foreground(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::DidEnterForeground:
			if (game->event_did_enter_foreground != nullptr)
			{
				game->event_did_enter_foreground(game);
			}
			break;

		case ::Kyty::Emulator::Host::InputEventType::Keyboard:
		{
			EventKeyboard key      = event->keyboard;
			key.timestamp_seconds = time_s;

			if (game->event_keyboard != nullptr)
			{
				game->event_keyboard(game, &key);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::Window: process_window_event(game, event->window); break;

		case ::Kyty::Emulator::Host::InputEventType::Display: process_display_event(game, event->display); break;

		case ::Kyty::Emulator::Host::InputEventType::MouseButton:
		{
			EventMouse mb = event->mouse;

			// printf("event.button.which = %" PRIu32"\n", event.button.which);

			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::MouseWheel:
		{
			EventMouse mb = event->mouse;

			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::MouseMotion:
		{
			EventMouse mb = event->mouse;

			mb.timestamp_seconds = time_s;

			if (game->event_mouse != nullptr)
			{
				game->event_mouse(game, &mb);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::Finger:
		{
			EventFinger f       = event->finger;
			f.timestamp_seconds = time_s;

			if (game->event_finger != nullptr)
			{
				game->event_finger(game, &f);
			}

			break;
		}

			//		case SDL_JOYAXISMOTION:
			//		case SDL_JOYBALLMOTION:
			//		case SDL_JOYHATMOTION:
			//		case SDL_JOYBUTTONDOWN:
			//		case SDL_JOYBUTTONUP:
			//		case SDL_JOYDEVICEADDED:
			//		case SDL_JOYDEVICEREMOVED:
			//		{
			//			EXIT("joystick event: %d\n", static_cast<int>(event->type));
			//			break;
			//		}

		case ::Kyty::Emulator::Host::InputEventType::ControllerAxis:
		{
			EventController c = event->controller;
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::ControllerButton:
		{
			EventController c = event->controller;
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::ControllerDevice:
		{
			EventController c = event->controller;
			c.timestamp_seconds = time_s;

			if (game->event_controller != nullptr)
			{
				game->event_controller(game, &c);
			}

			break;
		}

		case ::Kyty::Emulator::Host::InputEventType::None: break;
	}
}

GameApi* game_create_api()
{
	auto* api = new GameApi;

	// memset(api, 0, sizeof(GameApi));

	api->init              = game_init;
	api->render_and_update = game_render_and_update;
	api->close             = game_close;

	api->event_quit                  = game_event_quit;
	api->event_terminate             = game_event_terminate;
	api->event_keyboard              = game_event_keyboard;
	api->event_mouse                 = game_event_mouse;
	api->event_finger                = game_event_finger;
	api->event_controller            = game_event_controller;
	api->event_display               = game_event_display;
	api->event_low_memory            = game_event_low_memory;
	api->event_will_enter_background = game_event_will_enter_background;
	api->event_did_enter_background  = game_event_did_enter_background;
	api->event_will_enter_foreground = game_event_will_enter_foreground;
	api->event_did_enter_foreground  = game_event_did_enter_foreground;
	api->event_resize                = game_event_resize;

	api->show_window = game_show_window;

	api->need_exit = game_need_exit;
	api->is_paused = game_is_paused;

	api->poll_event    = game_poll_event;
	api->wait_event    = game_wait_event;
	api->process_event = game_process_event;

	return api;
}

void game_delete_api(GameApi* api)
{
	Delete(api);
}

void game_main_loop(GameApi* game, void* data)
{
	bool need_exit = false;

	Core::Timer timer;
	timer.Start();
	if (!game->init(game, timer, data))
	{
		need_exit = true;
	}

	for (;;)
	{
		if (need_exit)
		{
			break;
		}

		if (game->poll_event(game) != 0)
		{
			if (game->process_event != nullptr)
			{
				game->process_event(game, timer.GetTimeS());
			}
			continue;
		}

		if (game->is_paused(game))
		{
			if (!timer.IsPaused())
			{
				timer.Pause();
			}

			game->wait_event(game);

			if (game->process_event != nullptr)
			{
				game->process_event(game, timer.GetTimeS());
			}
			need_exit = game->need_exit(game);
			continue;
		}

		need_exit = game->need_exit(game);

		if (game->is_paused(game))
		{
			if (!timer.IsPaused())
			{
				timer.Pause();
			}
		} else
		{
			if (timer.IsPaused())
			{
				timer.Resume();
			}

			if (!need_exit)
			{
				need_exit = !game->render_and_update(game, timer);
			}

			if (!need_exit)
			{
				//				dbg_inc();
				game->show_window(game, timer);
			}
		}
	}

	game->close(game);
}

static void WindowCreate(WindowContext* ctx)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->host_window != nullptr);
	EXIT_IF(ctx->graphic_ctx.screen_width == 0);
	EXIT_IF(ctx->graphic_ctx.screen_height == 0);

	ctx->host_window = ::Kyty::Emulator::Host::HostWindow::Create(ctx->graphic_ctx.screen_width, ctx->graphic_ctx.screen_height);
	ctx->controls.SetFocused(ctx->host_window->IsFocused());
}

static void VulkanGetSurfaceCapabilities(VkPhysicalDevice physical_device, VkSurfaceKHR surface, SurfaceCapabilities* r)
{
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &r->capabilities);

	uint32_t formats_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count, nullptr);

	EXIT_NOT_IMPLEMENTED(formats_count == 0);

	r->formats = Vector<VkSurfaceFormatKHR>(formats_count); // @suppress("Ambiguous problem")
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &formats_count, r->formats.GetData());

	uint32_t present_modes_count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count, nullptr);

	EXIT_NOT_IMPLEMENTED(present_modes_count == 0);

	r->present_modes = Vector<VkPresentModeKHR>(present_modes_count); // @suppress("Ambiguous problem")
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &present_modes_count, r->present_modes.GetData());

	r->format_srgb_bgra32  = false;
	r->format_unorm_bgra32 = false;
	for (const auto& f: r->formats)
	{
		// Scan all surface formats; do not stop at the first match so both
		// UNORM and SRGB LDR candidates are visible to swapchain selection.
		if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			r->format_srgb_bgra32 = true;
		}
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			r->format_unorm_bgra32 = true;
		}
	}
}

static bool CheckFormat(VkPhysicalDevice device, VkFormat format, bool tile, VkFormatFeatureFlags features)
{
	VkFormatProperties format_props {};
	vkGetPhysicalDeviceFormatProperties(device, format, &format_props);
	if (tile)
	{
		if ((format_props.optimalTilingFeatures & features) == features)
		{
			return true;
		}
	} else
	{
		if ((format_props.linearTilingFeatures & features) == features)
		{
			return true;
		}
	}
	return false;
}

struct QueueInfo
{
	uint32_t family   = 0;
	uint32_t index    = 0;
	bool     graphics = false;
	bool     compute  = false;
	bool     transfer = false;
	bool     present  = false;
};

struct VulkanQueues
{
	uint32_t          family_count = 0;
	Vector<uint32_t>  family_used;
	Vector<QueueInfo> available;
	Vector<QueueInfo> graphics;
	Vector<QueueInfo> compute;
	Vector<QueueInfo> transfer;
	Vector<QueueInfo> present;
};

static void VulkanFindQueues(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t graphics_num, uint32_t compute_num,
                             uint32_t transfer_num, uint32_t present_num, VulkanQueues* out)
{
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(out == nullptr);

	VulkanQueues& qs = *out;

	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
	Vector<VkQueueFamilyProperties> queue_families(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.GetData());

	qs.family_count = queue_family_count;

	uint32_t family = 0;
	for (auto& f: queue_families)
	{
		VkBool32 presentation_supported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &presentation_supported);

		for (uint32_t i = 0; i < f.queueCount; i++)
		{
			QueueInfo info;
			info.family   = family;
			info.index    = i;
			info.graphics = (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
			info.compute  = (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
			info.transfer = (f.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
			info.present  = (presentation_supported == VK_TRUE);

			qs.available.Add(info);
		}

		qs.family_used.Add(0);

		family++;
	}

	// Preferred path: assign each role a dedicated (unshared) queue, consuming it
	// from the pool. This works on desktop GPUs that expose many queues/families.
	for (uint32_t i = 0; i < graphics_num; i++)
	{
		if (auto index = qs.available.Find(true, [](auto& q, auto& b) { return q.graphics == b; }); qs.available.IndexValid(index))
		{
			qs.family_used[qs.available.At(index).family]++;
			qs.graphics.Add(qs.available.At(index));
			qs.available.RemoveAt(index);
		}
	}

	// Keep exclusive resources on the graphics queue family whenever possible.
	// The renderer uses VK_SHARING_MODE_EXCLUSIVE and does not emit queue-family
	// ownership transfers, so selecting a separate compute/transfer family makes
	// uploads and presentation invalid on drivers that expose dedicated families.
	if (!qs.graphics.IsEmpty())
	{
		const QueueInfo primary = qs.graphics.At(0);
		if (primary.compute)
		{
			qs.compute.Add(primary);
		}
		if (primary.transfer)
		{
			qs.transfer.Add(primary);
		}
		if (primary.present)
		{
			qs.present.Add(primary);
		}
	}

	if (qs.compute.IsEmpty())
	{
		for (uint32_t i = 0; i < compute_num; i++)
		{
			if (auto index = qs.available.Find(true, [](auto& q, auto& b) { return q.compute == b; }); qs.available.IndexValid(index))
			{
				qs.family_used[qs.available.At(index).family]++;
				qs.compute.Add(qs.available.At(index));
				qs.available.RemoveAt(index);
			}
		}
	}

	if (qs.transfer.IsEmpty())
	{
		for (uint32_t i = 0; i < transfer_num; i++)
		{
			if (auto index = qs.available.Find(true, [](auto& q, auto& b) { return q.transfer == b; }); qs.available.IndexValid(index))
			{
				qs.family_used[qs.available.At(index).family]++;
				qs.transfer.Add(qs.available.At(index));
				qs.available.RemoveAt(index);
			}
		}
	}

	if (qs.present.IsEmpty())
	{
		for (uint32_t i = 0; i < present_num; i++)
		{
			if (auto index = qs.available.Find(true, [](auto& q, auto& b) { return q.present == b; }); qs.available.IndexValid(index))
			{
				qs.family_used[qs.available.At(index).family]++;
				qs.present.Add(qs.available.At(index));
				qs.available.RemoveAt(index);
			}
		}
	}

	// Fallback for GPUs that expose a single queue family with a single queue
	// (notably Metal via MoltenVK on Apple Silicon): the one queue supports
	// graphics, compute, transfer and present, so share it across every role
	// that could not be given a dedicated queue above.
	{
		bool      have_shared = false;
		QueueInfo shared;
		if (!qs.graphics.IsEmpty())
		{
			shared      = qs.graphics.At(0);
			have_shared = true;
		} else if (!qs.available.IsEmpty())
		{
			shared = qs.available.At(0);
			qs.family_used[shared.family]++;
			qs.graphics.Add(shared);
			qs.available.RemoveAt(0);
			have_shared = true;
		}

		if (have_shared)
		{
			if (qs.compute.IsEmpty())
			{
				qs.compute.Add(shared);
			}
			if (qs.transfer.IsEmpty())
			{
				qs.transfer.Add(shared);
			}
			if (qs.present.IsEmpty() && shared.present)
			{
				qs.present.Add(shared);
			}
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(VkInstance instance, VkSurfaceKHR surface, const Vector<const char*>& device_extensions,
                                     SurfaceCapabilities* out_capabilities, VkPhysicalDevice* out_device, VulkanQueues* out_queues)
{
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(out_capabilities == nullptr);
	EXIT_IF(out_device == nullptr);
	EXIT_IF(out_queues == nullptr);

	uint32_t devices_count = 0;
	vkEnumeratePhysicalDevices(instance, &devices_count, nullptr);

	EXIT_NOT_IMPLEMENTED(devices_count == 0);

	Vector<VkPhysicalDevice> devices(devices_count);
	vkEnumeratePhysicalDevices(instance, &devices_count, devices.GetData());

	VkPhysicalDevice best_device = nullptr;
	VulkanQueues     best_queues;

	for (const auto& device: devices)
	{
		bool skip_device = false;

		VkPhysicalDeviceProperties device_properties {};
		VkPhysicalDeviceFeatures2  device_features2 {};

		VkPhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
		color_write_ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT;
		color_write_ext.pNext = nullptr;

		device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		device_features2.pNext = &color_write_ext;

		vkGetPhysicalDeviceProperties(device, &device_properties);
		vkGetPhysicalDeviceFeatures2(device, &device_features2);

		printf("Vulkan device: %s\n", device_properties.deviceName);

		VulkanQueues qs;
		VulkanFindQueues(device, surface, GraphicContext::QUEUE_GFX_NUM, GraphicContext::QUEUE_COMPUTE_NUM, GraphicContext::QUEUE_UTIL_NUM,
		                 GraphicContext::QUEUE_PRESENT_NUM, &qs);

		if (qs.graphics.Size() != GraphicContext::QUEUE_GFX_NUM ||
		    !(qs.compute.Size() >= 1 && qs.compute.Size() <= GraphicContext::QUEUE_COMPUTE_NUM) ||
		    qs.transfer.Size() != GraphicContext::QUEUE_UTIL_NUM || qs.present.Size() != GraphicContext::QUEUE_PRESENT_NUM)
		{
			printf("Not enough queues\n");
			skip_device = true;
		}

		if (color_write_ext.colorWriteEnable != VK_TRUE)
		{
			// Not fatal: without VK_EXT_color_write_enable (e.g. MoltenVK) we bake
			// the color write mask into the pipeline instead of using dynamic state.
			printf("colorWriteEnable is not supported (using pipeline fallback)\n");
		}

		if (device_features2.features.fragmentStoresAndAtomics != VK_TRUE)
		{
			printf("fragmentStoresAndAtomics is not supported\n");
			skip_device = true;
		}
		if (device_features2.features.vertexPipelineStoresAndAtomics != VK_TRUE)
		{
			printf("vertexPipelineStoresAndAtomics is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.robustBufferAccess != VK_TRUE)
		{
			printf("robustBufferAccess is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.samplerAnisotropy != VK_TRUE)
		{
			printf("samplerAnisotropy is not supported\n");
			skip_device = true;
		}

		if (device_features2.features.shaderStorageImageReadWithoutFormat != VK_TRUE ||
		    device_features2.features.shaderStorageImageWriteWithoutFormat != VK_TRUE)
		{
			printf("formatless storage images are not supported\n");
			skip_device = true;
		}

		//		if (device_features2.features.shaderImageGatherExtended != VK_TRUE)
		//		{
		//			printf("shaderImageGatherExtended is not supported\n");
		//			skip_device = true;
		//		}

		if (!skip_device)
		{
			uint32_t extensions_count = 0;
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count, nullptr);

			EXIT_NOT_IMPLEMENTED(extensions_count == 0);

			Vector<VkExtensionProperties> available_extensions(extensions_count);
			vkEnumerateDeviceExtensionProperties(device, nullptr, &extensions_count, available_extensions.GetData());

			for (const char* ext: device_extensions)
			{
				// These extensions are optional (absent on MoltenVK); the renderer
				// uses core-Vulkan fallbacks when they are missing.
				if (strcmp(ext, VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME) == 0 ||
				    strcmp(ext, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) == 0 ||
				    strcmp(ext, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) == 0 ||
				    strcmp(ext, VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME) == 0 ||
				    strcmp(ext, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) == 0)
				{
					continue;
				}
				if (!available_extensions.Contains(ext, [](auto p, auto ext) { return strcmp(p.extensionName, ext) == 0; }))
				{
					printf("Vulkan device extension missing: %s\n", ext);
					skip_device = true;
					break;
				}
			}

			if (skip_device)
			{
				for (const auto& ext: available_extensions)
				{
					printf("Vulkan available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
				}
			}
		}

		if (!skip_device)
		{
			VulkanGetSurfaceCapabilities(device, surface, out_capabilities);

			if ((out_capabilities->capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
			{
				printf("Surface cannot be destination of blit\n");
				skip_device = true;
			}
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, VK_FORMAT_FEATURE_BLIT_SRC_BIT))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as transfer source\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D32_SFLOAT cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D32_SFLOAT_S8_UINT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D32_SFLOAT_S8_UINT cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D16_UNORM, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			printf("Format VK_FORMAT_D16_UNORM cannot be used as depth buffer\n");
			skip_device = true;
		}

		if (!skip_device && !CheckFormat(device, VK_FORMAT_D24_UNORM_S8_UINT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
			// Apple GPUs / MoltenVK do not support D24_UNORM_S8_UINT; D32_SFLOAT_S8_UINT
			// (validated above) is used instead, so this is not fatal.
			printf("Format VK_FORMAT_D24_UNORM_S8_UINT cannot be used as depth buffer (using D32_SFLOAT_S8_UINT)\n");
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_BC3_SRGB_BLOCK, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_BC3_SRGB_BLOCK cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_BC7_UNORM_BLOCK, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_BC7_UNORM_BLOCK cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_BC7_SRGB_BLOCK, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_BC7_SRGB_BLOCK cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8_UNORM, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8_UNORM cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8_UNORM, true, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8G8_UNORM cannot be used as texture\n");
			skip_device = true;
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_R8G8B8A8_SRGB, true, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_R8G8B8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_R8G8B8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			{
				printf("Format VK_FORMAT_R8G8B8A8_UNORM cannot be used as texture\n");
				skip_device = true;
			}
		}

		if (!skip_device &&
		    !CheckFormat(device, VK_FORMAT_B8G8R8A8_SRGB, true, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
		{
			printf("Format VK_FORMAT_B8G8R8A8_SRGB cannot be used as texture\n");

			if (!skip_device && !CheckFormat(device, VK_FORMAT_B8G8R8A8_UNORM, true,
			                                 VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			{
				printf("Format VK_FORMAT_B8G8R8A8_UNORM cannot be used as texture\n");
				skip_device = true;
			}
		}

		/*if (!skip_device && !CheckFormat(device, VK_FORMAT_S8_UINT, true, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
		{
		    printf("Format VK_FORMAT_S8_UINT cannot be used as depth buffer");
		    skip_device = true;
		}*/

		if (!skip_device && device_properties.limits.maxSamplerAnisotropy < 16.0f)
		{
			printf("maxSamplerAnisotropy < 16.0f");
			skip_device = true;
		}

		if (skip_device)
		{
			continue;
		}

		if (best_device == nullptr || device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			best_device = device;
			best_queues = std::move(qs);
		}
	}

	*out_device = best_device;
	*out_queues = std::move(best_queues);
}

static VkDevice VulkanCreateDevice(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VulkanExtensions* r,
                                   const VulkanQueues& queues, const Vector<const char*>& device_extensions,
                                   bool color_write_enable_supported, bool depth_clip_control_supported)
{
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(surface == nullptr);

	Vector<VkDeviceQueueCreateInfo> queue_create_info(queues.family_count);
	Vector<Vector<float>>           queue_priority(queues.family_count);
	uint32_t                        queue_create_info_num = 0;

	for (uint32_t i = 0; i < queues.family_count; i++)
	{
		if (queues.family_used[i] != 0)
		{
			for (uint32_t pi = 0; pi < queues.family_used[i]; pi++)
			{
				queue_priority[queue_create_info_num].Add(1.0f);
			}

			queue_create_info[queue_create_info_num].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queue_create_info[queue_create_info_num].pNext            = nullptr;
			queue_create_info[queue_create_info_num].flags            = 0;
			queue_create_info[queue_create_info_num].queueFamilyIndex = i;
			queue_create_info[queue_create_info_num].queueCount       = queues.family_used[i];
			queue_create_info[queue_create_info_num].pQueuePriorities = queue_priority[queue_create_info_num].GetDataConst();

			queue_create_info_num++;
		}
	}

	VkPhysicalDeviceFeatures device_features {};
	device_features.robustBufferAccess       = VK_TRUE;
	device_features.vertexPipelineStoresAndAtomics = VK_TRUE;
	device_features.fragmentStoresAndAtomics = VK_TRUE;
	device_features.samplerAnisotropy        = VK_TRUE;
	device_features.shaderStorageImageReadWithoutFormat  = VK_TRUE;
	device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	VkPhysicalDeviceFeatures supported_features {};
	vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
	device_features.depthBiasClamp    = supported_features.depthBiasClamp;
	device_features.sampleRateShading = supported_features.sampleRateShading;
	// Needed for the depthClipEnable=FALSE fallback when VK_EXT_depth_clip_enable
	// is unavailable (MoltenVK).
	device_features.depthClamp = VK_TRUE;
	// device_features.shaderImageGatherExtended = VK_TRUE;

	VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control_ext {};
	depth_clip_control_ext.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
	depth_clip_control_ext.pNext            = nullptr;
	depth_clip_control_ext.depthClipControl = VK_TRUE;

	VkPhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	color_write_ext.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT;
	color_write_ext.pNext            = (depth_clip_control_supported ? &depth_clip_control_ext : nullptr);
	color_write_ext.colorWriteEnable = VK_TRUE;

	VkDeviceCreateInfo create_info {};
	create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create_info.pNext =
	    (color_write_enable_supported ? static_cast<const void*>(&color_write_ext)
	                                  : (depth_clip_control_supported ? static_cast<const void*>(&depth_clip_control_ext) : nullptr));
	create_info.flags                   = 0;
	create_info.pQueueCreateInfos       = queue_create_info.GetDataConst();
	create_info.queueCreateInfoCount    = queue_create_info_num;
	create_info.enabledLayerCount       = (r->enable_validation_layers ? r->required_layers.Size() : 0);
	create_info.ppEnabledLayerNames     = (r->enable_validation_layers ? r->required_layers.GetDataConst() : nullptr);
	create_info.enabledExtensionCount   = device_extensions.Size();
	create_info.ppEnabledExtensionNames = device_extensions.GetDataConst();
	create_info.pEnabledFeatures        = &device_features;

	VkDevice device = nullptr;

	vkCreateDevice(physical_device, &create_info, nullptr, &device);

	return device;
}

static void VulkanGetExtensions(const ::Kyty::Emulator::Host::HostWindow* window, VulkanExtensions* r)
{
	EXIT_IF(window == nullptr);
	EXIT_IF(r == nullptr);

	uint32_t available_extensions_count = 0;
	uint32_t available_layers_count     = 0;
	std::vector<const char*> host_extensions;

	const bool host_result = window->GetVulkanInstanceExtensions(&host_extensions);

	EXIT_NOT_IMPLEMENTED(!host_result);
	EXIT_NOT_IMPLEMENTED(host_extensions.empty() || host_extensions.size() > UINT32_MAX);

	r->required_extensions = Vector<const char*>(static_cast<uint32_t>(host_extensions.size()), false); // @suppress("Ambiguous problem")
	r->required_extensions.Memset(0);
	for (uint32_t i = 0; i < r->required_extensions.Size(); i++)
	{
		r->required_extensions[i] = host_extensions[i];
	}

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count, nullptr);

	r->available_extensions = Vector<VkExtensionProperties>(available_extensions_count); // @suppress("Ambiguous problem")
	r->available_extensions.Memset(0);

	vkEnumerateInstanceExtensionProperties(nullptr, &available_extensions_count, r->available_extensions.GetData());

	EXIT_NOT_IMPLEMENTED(available_extensions_count != r->available_extensions.Size());

// Allow config-driven validation without a separate compile define so
// diagnostic runs (KYTY_VULKAN_VALIDATION=1) can surface VUID failures.
#if defined(KYTY_ENABLE_VULKAN_VALIDATION) || 1
	r->enable_validation_layers = Config::VulkanValidationEnabled();
#else
	r->enable_validation_layers = false;
#endif

	if (r->available_extensions.Contains(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; }))
	{
		r->required_extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	} else
	{
		r->enable_validation_layers = false;
	}

	for (const char* ext: r->required_extensions)
	{
		printf("Vulkan required extension: %s\n", ext);
	}

	for (const auto& ext: r->available_extensions)
	{
		printf("Vulkan available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
	}

	vkEnumerateInstanceLayerProperties(&available_layers_count, nullptr);

	r->available_layers = Vector<VkLayerProperties>(available_layers_count); // @suppress("Ambiguous problem")
	r->available_layers.Memset(0);
	vkEnumerateInstanceLayerProperties(&available_layers_count, r->available_layers.GetData());

	EXIT_NOT_IMPLEMENTED(available_layers_count != r->available_layers.Size());

	for (const auto& l: r->available_layers)
	{
		printf("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n", l.layerName, l.specVersion, l.implementationVersion,
		       l.description);
	}

	r->required_layers = {"VK_LAYER_KHRONOS_validation"};

	if (r->enable_validation_layers)
	{
		for (const char* l: r->required_layers)
		{
			if (!r->available_layers.Contains(l, [](auto s, auto l) { return strcmp(s.layerName, l) == 0; }))
			{
				printf("no validation layer: %s\n", l);
				r->enable_validation_layers = false;
				break;
			}
		}
	}

	if (r->enable_validation_layers)
	{
		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &available_extensions_count, nullptr);

		Vector<VkExtensionProperties> available_extensions(available_extensions_count);

		vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &available_extensions_count, available_extensions.GetData());

		for (const auto& ext: available_extensions)
		{
			printf("VK_LAYER_KHRONOS_validation available extension: %s, version = %u\n", ext.extensionName, ext.specVersion);
		}

		// Optional: keep validation layers even when VK_EXT_validation_features is
		// unavailable (common on some loaders). Prefer features when present.
		if (available_extensions.Contains("VK_EXT_validation_features", [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; }))
		{
			r->required_extensions.Add("VK_EXT_validation_features");
		}
	}
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
                                                                   VkDebugUtilsMessageTypeFlagsEXT             message_types,
                                                                   const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
                                                                   void* /*user_data*/)
{
	const char* severity_str   = nullptr;
	const char* severity_color = FG_DEFAULT;
	bool        skip           = false;
	bool        error          = false;
	bool        debug_printf   = false;
	switch (message_severity)
	{
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
			severity_str   = "V";
			severity_color = FG_BRIGHT_WHITE;
			skip           = true;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			if ((message_types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 && Config::SpirvDebugPrintfEnabled() &&
			    strcmp(callback_data->pMessageIdName, "UNASSIGNED-DEBUG-PRINTF") == 0)
			{
				debug_printf   = true;
				severity_color = FG_BRIGHT_YELLOW;
				skip           = true;
			} else
			{
				severity_str   = "I";
				severity_color = FG_DEFAULT;
			}
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			severity_str   = "W";
			severity_color = FG_RED;
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			severity_str   = "E";
			severity_color = FG_BRIGHT_RED;
			error          = true;
			break;
		default: severity_str = "?";
	}

	if (error)
	{
		EXIT("%s[Vulkan][%s][%u]: %s%s\n", severity_color, severity_str, static_cast<uint32_t>(message_types), callback_data->pMessage,
		     FG_DEFAULT);
	}

	if (!skip)
	{
		printf("%s[Vulkan][%s][%u]: %s%s\n", severity_color, severity_str, static_cast<uint32_t>(message_types), callback_data->pMessage,
		       FG_DEFAULT);
	}

	if (debug_printf)
	{
		auto strs = String::FromUtf8(callback_data->pMessage).Split(U'|');
		if (!strs.IsEmpty())
		{
			printf("%s%s%s\n", severity_color, strs.At(strs.Size() - 1).C_Str(), FG_DEFAULT);
		}
	}

	return VK_FALSE;
}

static VKAPI_ATTR VkResult VKAPI_CALL VulkanCreateDebugUtilsMessengerEXT(VkInstance                                instance,
                                                                         const VkDebugUtilsMessengerCreateInfoEXT* create_info,
                                                                         const VkAllocationCallbacks*              allocator,
                                                                         VkDebugUtilsMessengerEXT*                 messenger)
{
	EXIT_IF(instance == nullptr);

	if (auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
	    func != nullptr)
	{
		return func(instance, create_info, allocator, messenger);
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

[[maybe_unused]] static VkSwapchainKHR VulkanCreateSwapchainInternal(VkDevice device, VkSurfaceKHR surface, VkSwapchainKHR old_swapchain,
                                                                     uint32_t width, uint32_t height, uint32_t image_count,
                                                                     SurfaceCapabilities* r, VkFormat* swapchain_format,
                                                                     VkExtent2D* swapchain_extent, VkImage** swapchain_images,
                                                                     VkImageView** swapchain_image_views, uint32_t* swapchain_images_count)
{
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);
	EXIT_IF(r == nullptr);
	EXIT_IF(swapchain_format == nullptr);
	EXIT_IF(swapchain_extent == nullptr);
	EXIT_IF(swapchain_images == nullptr);
	EXIT_IF(swapchain_image_views == nullptr);
	EXIT_IF(swapchain_images_count == nullptr);

	EXIT_NOT_IMPLEMENTED(r->formats.IsEmpty());

	VkExtent2D extent {};
	// Surface extents: only clamp when the max bound is usable (min <= max).
	const auto& min_ext = r->capabilities.minImageExtent;
	const auto& max_ext = r->capabilities.maxImageExtent;
	if (min_ext.width <= max_ext.width)
	{
		extent.width = std::clamp(width, min_ext.width, max_ext.width);
	} else
	{
		extent.width = width;
	}
	if (min_ext.height <= max_ext.height)
	{
		extent.height = std::clamp(height, min_ext.height, max_ext.height);
	} else
	{
		extent.height = height;
	}

	// Vulkan: maxImageCount == 0 means no upper limit (only min applies).
	if (r->capabilities.maxImageCount == 0)
	{
		image_count = std::max(image_count, r->capabilities.minImageCount);
	} else if (r->capabilities.minImageCount <= r->capabilities.maxImageCount)
	{
		image_count = std::clamp(image_count, r->capabilities.minImageCount, r->capabilities.maxImageCount);
	}

	VkSwapchainCreateInfoKHR create_info {};
	create_info.sType         = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create_info.pNext         = nullptr;
	create_info.flags         = 0;
	create_info.surface       = surface;
	create_info.minImageCount = image_count;

	// Host presentation default is ordinary LDR sRGB. Never prefer HDR10/HLG/etc.
	// even when a driver lists them first (SelectDefaultSwapchainSurfaceFormat).
	if (r->format_unorm_bgra32)
	{
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_UNORM;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else if (r->format_srgb_bgra32)
	{
		create_info.imageFormat     = VK_FORMAT_B8G8R8A8_SRGB;
		create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	} else
	{
		const auto chosen           = SelectDefaultSwapchainSurfaceFormat(r->formats.GetData(), static_cast<uint32_t>(r->formats.Size()));
		create_info.imageFormat     = chosen.format;
		create_info.imageColorSpace = chosen.colorSpace;
	}

	create_info.imageExtent           = extent;
	create_info.imageArrayLayers      = 1;
	create_info.imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create_info.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
	create_info.queueFamilyIndexCount = 0;
	create_info.pQueueFamilyIndices   = nullptr;
	create_info.preTransform          = r->capabilities.currentTransform;
	create_info.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	// Capability-driven present mode: prefer MAILBOX (low-latency triple-buffer)
	// then IMMEDIATE, else FIFO (guaranteed vsync). Hardcoding FIFO alone can
	// couple host present to display refresh with only minImageCount images.
	create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	for (const auto mode: r->present_modes)
	{
		if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			create_info.presentMode = mode;
			break;
		}
		if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			create_info.presentMode = mode;
		}
	}
	// MAILBOX needs ≥3 images to avoid degenerating into FIFO-like blocking.
	if (create_info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR && image_count < 3)
	{
		image_count = 3;
		if (r->capabilities.maxImageCount == 0)
		{
			image_count = std::max(image_count, r->capabilities.minImageCount);
		} else if (r->capabilities.minImageCount <= r->capabilities.maxImageCount)
		{
			image_count = std::clamp(image_count, r->capabilities.minImageCount, r->capabilities.maxImageCount);
		}
		create_info.minImageCount = image_count;
	}
	create_info.clipped      = VK_TRUE;
	create_info.oldSwapchain = old_swapchain;

	*swapchain_format = create_info.imageFormat;
	*swapchain_extent = extent;

	VkSwapchainKHR swapchain = nullptr;

	const VkResult create_result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
	if (create_result != VK_SUCCESS || swapchain == nullptr)
	{
		printf("vkCreateSwapchainKHR failed: result = %d\n", static_cast<int>(create_result));
		return nullptr;
	}
	printf("Swapchain presentMode=%d extent=%ux%u minImageCount=%u\n", static_cast<int>(create_info.presentMode), extent.width,
	       extent.height, create_info.minImageCount);

	vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, nullptr);
	EXIT_NOT_IMPLEMENTED(*swapchain_images_count == 0);

	*swapchain_images = new VkImage[*swapchain_images_count];
	vkGetSwapchainImagesKHR(device, swapchain, swapchain_images_count, *swapchain_images);

	*swapchain_image_views = new VkImageView[*swapchain_images_count];
	for (uint32_t i = 0; i < *swapchain_images_count; i++)
	{
		VulkanImageViewDescriptor view_descriptor {};
		view_descriptor.image  = (*swapchain_images)[i];
		view_descriptor.format = *swapchain_format;
		EXIT_NOT_IMPLEMENTED(!VulkanCreateDeviceImageView(device, view_descriptor, &((*swapchain_image_views)[i])));
	}

	return swapchain;
}

static void VulkanCreateQueues(GraphicContext* ctx, const VulkanQueues& queues)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->device == nullptr);
	EXIT_IF(queues.graphics.Size() != 1);
	EXIT_IF(queues.transfer.Size() != 1);
	EXIT_IF(queues.present.Size() != 1);
	EXIT_IF(!(queues.compute.Size() >= 1 && queues.compute.Size() <= GraphicContext::QUEUE_COMPUTE_NUM));

	auto get_queue = [ctx](int id, const QueueInfo& info)
	{
		ctx->queues[id].family = info.family;
		ctx->queues[id].index  = info.index;
		EXIT_IF(ctx->queues[id].vk_queue != nullptr);
		vkGetDeviceQueue(ctx->device, ctx->queues[id].family, ctx->queues[id].index, &ctx->queues[id].vk_queue);
		EXIT_NOT_IMPLEMENTED(ctx->queues[id].vk_queue == nullptr);
	};

	get_queue(GraphicContext::QUEUE_GFX, queues.graphics.At(0));
	get_queue(GraphicContext::QUEUE_UTIL, queues.transfer.At(0));
	get_queue(GraphicContext::QUEUE_PRESENT, queues.present.At(0));

	for (int id = 0; id < GraphicContext::QUEUE_COMPUTE_NUM; id++)
	{
		get_queue(GraphicContext::QUEUE_COMPUTE_START + id, queues.compute.At(id % queues.compute.Size()));
	}

	VulkanQueueIdentity identities[GraphicContext::QUEUES_NUM] {};
	uint32_t            lock_indices[GraphicContext::QUEUES_NUM] {};
	for (int id = 0; id < GraphicContext::QUEUES_NUM; id++)
	{
		const auto& queue = ctx->queues[id];
		identities[id]    = {queue.family, queue.index, queue.vk_queue};
	}

	const auto assignment = VulkanAssignQueueLockIndices(identities, GraphicContext::QUEUES_NUM, lock_indices, &ctx->queue_mutex_count);
	EXIT_NOT_IMPLEMENTED(assignment != VulkanQueueLockAssignmentStatus::Success || ctx->queue_mutex_count == 0 ||
	                     ctx->queue_mutex_count > GraphicContext::QUEUES_NUM);
	for (int id = 0; id < GraphicContext::QUEUES_NUM; id++)
	{
		EXIT_IF(lock_indices[id] >= ctx->queue_mutex_count);
		ctx->queues[id].mutex = &ctx->queue_mutexes[lock_indices[id]];
	}
}

static VulkanSwapchain* VulkanCreateSwapchain(GraphicContext* ctx, uint32_t image_count)
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(ctx->screen_width == 0);
	EXIT_IF(ctx->screen_height == 0);

	Core::LockGuard lock(g_window_ctx->mutex);

	auto* s = new VulkanSwapchain;

	s->swapchain = VulkanCreateSwapchainInternal(
	    ctx->device, g_window_ctx->surface, nullptr, ctx->screen_width, ctx->screen_height, image_count, g_window_ctx->surface_capabilities,
	    &s->swapchain_format, &s->swapchain_extent, &s->swapchain_images, &s->swapchain_image_views, &s->swapchain_images_count);
	if (s->swapchain == nullptr)
	{
		EXIT("Could not create swapchain");
	}

	s->current_index = static_cast<uint32_t>(-1);

	VkSemaphoreCreateInfo render_finished_info {};
	render_finished_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	render_finished_info.pNext = nullptr;
	render_finished_info.flags = 0;

	s->render_finished_semaphores = new VkSemaphore[s->swapchain_images_count] {};
	for (uint32_t i = 0; i < s->swapchain_images_count; i++)
	{
		auto result = vkCreateSemaphore(ctx->device, &render_finished_info, nullptr, &s->render_finished_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	VkFenceCreateInfo fence_info;
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.pNext = nullptr;
	fence_info.flags = 0;

	auto result = vkCreateFence(ctx->device, &fence_info, nullptr, &s->present_complete_fence);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	return s;
}

// Rebuild swapchain after surface state changes (e.g. first host-window show on
// X11/Mesa). Presentation semaphores follow swapchain-image ownership.
static void VulkanRecreateSwapchain(GraphicContext* ctx, VulkanSwapchain* s, uint32_t image_count)
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(s == nullptr);
	EXIT_IF(ctx->device == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	vkDeviceWaitIdle(ctx->device);

	if (s->render_finished_semaphores != nullptr)
	{
		for (uint32_t i = 0; i < s->swapchain_images_count; i++)
		{
			vkDestroySemaphore(ctx->device, s->render_finished_semaphores[i], nullptr);
		}
		delete[] s->render_finished_semaphores;
		s->render_finished_semaphores = nullptr;
	}

	if (s->swapchain_image_views != nullptr)
	{
		for (uint32_t i = 0; i < s->swapchain_images_count; i++)
		{
			vkDestroyImageView(ctx->device, s->swapchain_image_views[i], nullptr);
		}
		delete[] s->swapchain_image_views;
		s->swapchain_image_views = nullptr;
	}
	delete[] s->swapchain_images;
	s->swapchain_images       = nullptr;
	s->swapchain_images_count = 0;

	const VkSwapchainKHR old = s->swapchain;
	s->swapchain             = nullptr;

	VulkanGetSurfaceCapabilities(ctx->physical_device, g_window_ctx->surface, g_window_ctx->surface_capabilities);

	s->swapchain = VulkanCreateSwapchainInternal(
	    ctx->device, g_window_ctx->surface, old, ctx->screen_width, ctx->screen_height, image_count, g_window_ctx->surface_capabilities,
	    &s->swapchain_format, &s->swapchain_extent, &s->swapchain_images, &s->swapchain_image_views, &s->swapchain_images_count);
	if (s->swapchain == nullptr)
	{
		EXIT("Could not recreate swapchain");
	}

	VkSemaphoreCreateInfo render_finished_info {};
	render_finished_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	render_finished_info.pNext = nullptr;
	render_finished_info.flags = 0;

	s->render_finished_semaphores = new VkSemaphore[s->swapchain_images_count] {};
	for (uint32_t i = 0; i < s->swapchain_images_count; i++)
	{
		auto result = vkCreateSemaphore(ctx->device, &render_finished_info, nullptr, &s->render_finished_semaphores[i]);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	}

	if (old != nullptr)
	{
		vkDestroySwapchainKHR(ctx->device, old, nullptr);
	}

	s->current_index = static_cast<uint32_t>(-1);
	printf("Swapchain recreated: %ux%u images=%u\n", s->swapchain_extent.width, s->swapchain_extent.height, s->swapchain_images_count);

	DebugOverlayOnSwapchainRecreated(ctx, s);
}

static void VulkanCreate(WindowContext* ctx)
{
	EXIT_IF(ctx->host_window == nullptr);
	EXIT_IF(ctx->graphic_ctx.instance != nullptr);
	EXIT_IF(ctx->graphic_ctx.physical_device != nullptr);
	EXIT_IF(ctx->graphic_ctx.device != nullptr);
	EXIT_IF(ctx->surface_capabilities != nullptr);

	VulkanExtensions r;
	VulkanGetExtensions(ctx->host_window, &r);

	VkApplicationInfo app_info {};
	app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pNext              = nullptr;
	app_info.pApplicationName   = "Kyty";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VK_API_VERSION_1_2; // NOLINT

	VkValidationFeatureDisableEXT disabled_features[] = {};
	VkValidationFeatureEnableEXT  enabled_features[]  = {
#ifdef KYTY_ENABLE_BEST_PRACTICES
	    VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
#endif
#ifdef KYTY_ENABLE_DEBUG_PRINTF
	    VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT,
#endif
	};

	uint32_t enabled_features_count = sizeof(enabled_features) / sizeof(VkValidationFeatureEnableEXT);

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (!Config::SpirvDebugPrintfEnabled())
	{
		enabled_features_count--;
	}
#endif

	VkValidationFeaturesEXT validation_features {};
	validation_features.sType                          = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
	validation_features.pNext                          = nullptr;
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;
	validation_features.disabledValidationFeatureCount = 0;
	validation_features.pDisabledValidationFeatures    = disabled_features;

	VkDebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbg_create_info.pNext           = &validation_features;
	dbg_create_info.flags           = 0;
	dbg_create_info.messageSeverity = static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
	dbg_create_info.messageType     = static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) |
	                                  static_cast<uint32_t>(VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;
	dbg_create_info.pUserData       = nullptr;

	VkInstanceCreateInfo inst_info {};
	inst_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
	inst_info.flags                   = 0;
	inst_info.pApplicationInfo        = &app_info;
	inst_info.enabledExtensionCount   = r.required_extensions.Size();
	inst_info.ppEnabledExtensionNames = r.required_extensions.GetDataConst();
	inst_info.enabledLayerCount       = (r.enable_validation_layers ? r.required_layers.Size() : 0);
	inst_info.ppEnabledLayerNames     = (r.enable_validation_layers ? r.required_layers.GetDataConst() : nullptr);

	VkResult result = vkCreateInstance(&inst_info, nullptr, &ctx->graphic_ctx.instance);
	if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
	{
		EXIT("Unable to find a compatible Vulkan Driver");
	} else if (result != VK_SUCCESS)
	{
		EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}

	if (r.enable_validation_layers)
	{
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(ctx->graphic_ctx.instance, &dbg_create_info, nullptr, &ctx->graphic_ctx.debug_messenger) !=
		    VK_SUCCESS)
		{
			EXIT("Could not create debug messenger");
		}
	}

	if (!ctx->host_window->CreateVulkanSurface(ctx->graphic_ctx.instance, &ctx->surface))
	{
		EXIT("Could not create a Vulkan surface");
	}

	Vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	                                         VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
	                                         VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME,
	                                         VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	                                         VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
	                                         VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
	                                         "VK_KHR_maintenance1"};

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled())
	{
		device_extensions.Add(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	ctx->surface_capabilities = new SurfaceCapabilities {};

	VulkanQueues queues;

	VulkanFindPhysicalDevice(ctx->graphic_ctx.instance, ctx->surface, device_extensions, ctx->surface_capabilities,
	                         &ctx->graphic_ctx.physical_device, &queues);

	if (ctx->graphic_ctx.physical_device == nullptr)
	{
		std::fflush(stdout);
		EXIT("Could not find suitable device");
	}

	// Detect VK_EXT_color_write_enable support; drop it from the enabled extension
	// list when absent (MoltenVK) so device creation still succeeds.
	{
		uint32_t dev_ext_count = 0;
		vkEnumerateDeviceExtensionProperties(ctx->graphic_ctx.physical_device, nullptr, &dev_ext_count, nullptr);
		Vector<VkExtensionProperties> dev_exts(dev_ext_count);
		dev_exts.Memset(0);
		vkEnumerateDeviceExtensionProperties(ctx->graphic_ctx.physical_device, nullptr, &dev_ext_count, dev_exts.GetData());

		auto has_ext = [&](const char* name)
		{ return dev_exts.Contains(name, [](auto s, auto l) { return strcmp(s.extensionName, l) == 0; }); };
		auto drop_ext = [&](const char* name)
		{
			if (auto idx = device_extensions.Find(name, [](auto s, auto l) { return strcmp(s, l) == 0; });
			    device_extensions.IndexValid(idx))
			{
				device_extensions.RemoveAt(idx);
			}
		};

		ctx->graphic_ctx.color_write_enable_supported = has_ext(VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME);
		if (!ctx->graphic_ctx.color_write_enable_supported)
		{
			drop_ext(VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME);
			printf("VK_EXT_color_write_enable absent: baking color write mask into pipelines\n");
		}

		ctx->graphic_ctx.depth_clip_enable_supported = has_ext(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
		if (!ctx->graphic_ctx.depth_clip_enable_supported)
		{
			drop_ext(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
			printf("VK_EXT_depth_clip_enable absent: using depthClampEnable fallback\n");
		}

		ctx->graphic_ctx.depth_clip_control_supported = has_ext(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
		if (!ctx->graphic_ctx.depth_clip_control_supported)
		{
			drop_ext(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
			printf("VK_EXT_depth_clip_control absent: OpenGL clip space needs host remapping\n");
		} else if (!device_extensions.Contains(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, [](auto s, auto l) { return strcmp(s, l) == 0; }))
		{
			device_extensions.Add(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
		}

		ctx->graphic_ctx.depth_range_unrestricted_supported = has_ext(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
		if (!ctx->graphic_ctx.depth_range_unrestricted_supported)
		{
			drop_ext(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
		} else if (!device_extensions.Contains(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
		                                       [](auto s, auto l) { return strcmp(s, l) == 0; }))
		{
			device_extensions.Add(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
		}

		ctx->graphic_ctx.subgroup_size_control_supported = has_ext(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);

		ctx->graphic_ctx.sample_location_capabilities.extension_enabled = has_ext(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME) ? 1u : 0u;
		if (ctx->graphic_ctx.sample_location_capabilities.extension_enabled == 0)
		{
			drop_ext(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
			printf("VK_EXT_sample_locations absent: custom sample locations are unavailable\n");
		} else
		{
			VkPhysicalDeviceSampleLocationsPropertiesEXT sample_location_properties {};
			sample_location_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT;

			VkPhysicalDeviceProperties2 physical_device_properties {};
			physical_device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			physical_device_properties.pNext = &sample_location_properties;
			vkGetPhysicalDeviceProperties2(ctx->graphic_ctx.physical_device, &physical_device_properties);

			auto& sample_locations               = ctx->graphic_ctx.sample_location_capabilities;
			sample_locations.sample_counts       = sample_location_properties.sampleLocationSampleCounts;
			sample_locations.max_grid_size       = sample_location_properties.maxSampleLocationGridSize;
			sample_locations.coordinate_range[0] = sample_location_properties.sampleLocationCoordinateRange[0];
			sample_locations.coordinate_range[1] = sample_location_properties.sampleLocationCoordinateRange[1];
			sample_locations.subpixel_bits       = sample_location_properties.sampleLocationSubPixelBits;
			sample_locations.variable_locations  = sample_location_properties.variableSampleLocations == VK_TRUE ? 1u : 0u;
		}
	}

	VkPhysicalDeviceProperties device_properties {};
	vkGetPhysicalDeviceProperties(ctx->graphic_ctx.physical_device, &device_properties);
	VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroup_size_control {};
	VkPhysicalDeviceSubgroupProperties subgroup_properties {};
	VkPhysicalDeviceProperties2 physical_device_properties {};
	subgroup_size_control.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT;
	subgroup_properties.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
	subgroup_properties.pNext   = ctx->graphic_ctx.subgroup_size_control_supported ? &subgroup_size_control : nullptr;
	physical_device_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	physical_device_properties.pNext = &subgroup_properties;
	vkGetPhysicalDeviceProperties2(ctx->graphic_ctx.physical_device, &physical_device_properties);
	ctx->graphic_ctx.subgroup_size       = subgroup_properties.subgroupSize;
	ctx->graphic_ctx.subgroup_stages     = subgroup_properties.supportedStages;
	ctx->graphic_ctx.subgroup_operations = subgroup_properties.supportedOperations;
	if (ctx->graphic_ctx.subgroup_size_control_supported)
	{
		ctx->graphic_ctx.subgroup_min_size = subgroup_size_control.minSubgroupSize;
		ctx->graphic_ctx.subgroup_max_size = subgroup_size_control.maxSubgroupSize;
	} else
	{
		ctx->graphic_ctx.subgroup_min_size = subgroup_properties.subgroupSize;
		ctx->graphic_ctx.subgroup_max_size = subgroup_properties.subgroupSize;
	}
	VkPhysicalDeviceFeatures device_features {};
	vkGetPhysicalDeviceFeatures(ctx->graphic_ctx.physical_device, &device_features);
	ctx->graphic_ctx.depth_bias_clamp_supported    = device_features.depthBiasClamp == VK_TRUE;
	ctx->graphic_ctx.sample_rate_shading_supported = device_features.sampleRateShading == VK_TRUE;

	printf("Select device: %s\n", device_properties.deviceName);

	memcpy(ctx->device_name, device_properties.deviceName, sizeof(ctx->device_name));
	memcpy(ctx->processor_name, Core::GetSystemInfo().ProcessorName.C_Str(), sizeof(ctx->processor_name));

	ctx->graphic_ctx.device =
	    VulkanCreateDevice(ctx->graphic_ctx.physical_device, ctx->surface, &r, queues, device_extensions,
	                       ctx->graphic_ctx.color_write_enable_supported, ctx->graphic_ctx.depth_clip_control_supported);
	if (ctx->graphic_ctx.device == nullptr)
	{
		EXIT("Could not create device");
	}

	VulkanCreateQueues(&ctx->graphic_ctx, queues);

	ctx->swapchain = VulkanCreateSwapchain(&ctx->graphic_ctx, 2);
	DebugOverlayInit(ctx->host_window, &ctx->graphic_ctx, ctx->swapchain);
}

void WindowInit(uint32_t width, uint32_t height)
{
	EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	EXIT_IF(g_window_ctx != nullptr);

	g_window_ctx = new WindowContext;

	g_window_ctx->graphic_ctx.screen_width  = width;
	g_window_ctx->graphic_ctx.screen_height = height;
	g_window_ctx->native_capture.Configure(WindowSteadyMs());
}

void WindowWaitForGraphicInitialized()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	while (!g_window_ctx->graphic_initialized)
	{
		g_window_ctx->graphic_initialized_condvar.Wait(&g_window_ctx->mutex);
	}
}

void WindowRun()
{
	EXIT_IF(g_window_ctx == nullptr);

	// KYTY_PROFILER_THREAD("Thread_Window");
	EASY_MAIN_THREAD

	GameApi* api = nullptr;

	g_window_ctx->mutex.Lock();
	{
		EXIT_IF(g_window_ctx->graphic_initialized);

		WindowCreate(g_window_ctx);
		VulkanCreate(g_window_ctx);
		api = game_create_api();

		g_window_ctx->game = api;

		g_window_ctx->graphic_initialized = true;
		g_window_ctx->graphic_initialized_condvar.Signal();
		// Agent observation at graphics producer (not status poll).
		Emulator::Agent::Lifecycle::EmitGraphicsInit();
		// Pad overlay is available once the window thread is running.
		Emulator::Agent::Lifecycle::EmitInputReady();
	}
	g_window_ctx->mutex.Unlock();

	game_main_loop(api, &g_window_ctx->graphic_ctx);
	// game_delete_api(api);

	DebugOverlayShutdown(&g_window_ctx->graphic_ctx);

	Core::SubsystemsListSingleton::Instance()->ShutdownAll();
	std::_Exit(0);
}

VkSurfaceCapabilitiesKHR* VulkanGetSurfaceCapabilities()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	// auto* ctx = &g_window_ctx->graphic_ctx;

	return &g_window_ctx->surface_capabilities->capabilities;
}

GraphicContext* WindowGetGraphicContext()
{
	EXIT_IF(g_window_ctx == nullptr);

	Core::LockGuard lock(g_window_ctx->mutex);

	return &g_window_ctx->graphic_ctx;
}

void WindowUpdateIcon()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->host_window == nullptr);

	static Image* icon = nullptr;
	if (icon == nullptr)
	{
		String icon_path;
		if (!Loader::SystemContentGetIconPath(&icon_path))
		{
			return;
		}
		icon = new Image(icon_path);
		icon->Load();
	}

	if (icon != nullptr)
	{
		g_window_ctx->host_window->SetIcon(icon->GetSdlSurface());
	}
}

void WindowUpdateTitle()
{
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->game == nullptr);
	EXIT_IF(g_window_ctx->host_window == nullptr);

	static char title[128];
	static char title_id[12];
	static char app_ver[8];
	static bool has_title    = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	static bool has_title_id = Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	static bool has_app_ver  = Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));

	const int    frame_num = g_window_ctx->game->m_frame_num;
	const double fps_now   = g_window_ctx->game->m_current_fps;
	const double t         = g_window_ctx->game->m_current_time_seconds;

	// Optional host-side FPS probe for Silent runs (window title is not logged).
	// Enable with KYTY_FPS_LOG=1; writes once per second to stderr only.
	static const bool k_fps_log = (std::getenv("KYTY_FPS_LOG") != nullptr && std::getenv("KYTY_FPS_LOG")[0] == '1');
	if (k_fps_log)
	{
		static double s_last_log_time = 0.0;
		if (t - s_last_log_time >= 1.0)
		{
			std::fprintf(stderr, "KYTY_FPS_LOG frame=%d fps=%.3f t=%.3f\n", frame_num, fps_now, t);
			s_last_log_time = t;
		}
	}

	// Throttle title updates: X11/Wayland host title updates are round trips and
	// was invoked every present. Match the FPS EMA window (~4 Hz).
	static double s_last_title_time = -1.0;
	if (s_last_title_time >= 0.0 && (t - s_last_title_time) < FPS_UPDATE_TIME)
	{
		return;
	}
	s_last_title_time = t;

	auto fps = String::FromPrintf("%s%s%s%s%s%s[%s] [%s], frame: %d, fps: %f", (has_title ? title : ""), (has_title ? ", " : ""),
	                              (has_title_id ? title_id : ""), (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""),
	                              (has_app_ver ? ", " : ""), g_window_ctx->device_name, g_window_ctx->processor_name, frame_num, fps_now);

	g_window_ctx->host_window->SetTitle(fps.C_Str());
}

void WindowDrawBuffer(VideoOutVulkanImage* image)
{
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(image == nullptr);
	EXIT_IF(g_window_ctx == nullptr);
	EXIT_IF(g_window_ctx->swapchain == nullptr);
	EXIT_IF(g_window_ctx->host_window == nullptr);

	bool just_shown = false;
	if (g_window_ctx->host_window->IsHidden())
	{
		WindowUpdateIcon();

		// Drain host events so the surface size/visibility settle before acquire.
		just_shown = g_window_ctx->host_window->ShowAndPumpEvents();
	}

	// First present after ShowWindow often returns OUT_OF_DATE/SUBOPTIMAL on
	// Linux (X11/Wayland/Mesa) when the swapchain was built for a hidden window.
	if (just_shown)
	{
		VulkanRecreateSwapchain(&g_window_ctx->graphic_ctx, g_window_ctx->swapchain, 2);
	}

	g_window_ctx->swapchain->current_index = static_cast<uint32_t>(-1);

	const auto acquire_start = std::chrono::steady_clock::now();
	auto       result = vkAcquireNextImageKHR(g_window_ctx->graphic_ctx.device, g_window_ctx->swapchain->swapchain, UINT64_MAX,
	                                          nullptr, g_window_ctx->swapchain->present_complete_fence,
	                                          &g_window_ctx->swapchain->current_index);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		printf("vkAcquireNextImageKHR: result = %d, recreating swapchain\n", static_cast<int>(result));
		VulkanRecreateSwapchain(&g_window_ctx->graphic_ctx, g_window_ctx->swapchain, 2);
		g_window_ctx->swapchain->current_index = static_cast<uint32_t>(-1);
		result = vkAcquireNextImageKHR(g_window_ctx->graphic_ctx.device, g_window_ctx->swapchain->swapchain, UINT64_MAX, nullptr,
		                               g_window_ctx->swapchain->present_complete_fence, &g_window_ctx->swapchain->current_index);
	}

	// SUBOPTIMAL is usable; only hard-fail other errors.
	if (result == VK_SUBOPTIMAL_KHR)
	{
		result = VK_SUCCESS;
	}

	if (result != VK_SUCCESS)
	{
		printf("vkAcquireNextImageKHR failed: result = %d\n", static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);
	EXIT_NOT_IMPLEMENTED(g_window_ctx->swapchain->current_index == static_cast<uint32_t>(-1));
	EXIT_NOT_IMPLEMENTED(g_window_ctx->swapchain->current_index >= g_window_ctx->swapchain->swapchain_images_count);
	EXIT_IF(g_window_ctx->swapchain->render_finished_semaphores == nullptr);

	do
	{
		result = vkWaitForFences(g_window_ctx->graphic_ctx.device, 1, &g_window_ctx->swapchain->present_complete_fence, VK_TRUE, 100000000);
	} while (result == VK_TIMEOUT);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS);

	vkResetFences(g_window_ctx->graphic_ctx.device, 1, &g_window_ctx->swapchain->present_complete_fence);
	const auto acquire_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - acquire_start).count();
	DebugStatsRecordAcquire(static_cast<uint64_t>(acquire_ns));

	auto*      blt_src_image     = image;
	auto*      blt_dst_image     = g_window_ctx->swapchain;
	const auto capture_milestone = NativeCaptureNext(g_window_ctx);

	EXIT_IF(blt_src_image == nullptr);
	EXIT_IF(blt_dst_image == nullptr);

	// Opt-in VideoOut dump: KYTY_DUMP_VIDEOOUT=1 writes present sources at a
	// bounded cadence. KYTY_DUMP_VIDEOOUT_FRAMES accepts comma-separated frames
	// and inclusive ranges with an optional step, e.g. 200,240-360:20.
	// Dedup by frame (same Vulkan image is reused across presents).
	if (NativeCaptureEnvEnabled("KYTY_DUMP_VIDEOOUT") && g_window_ctx->game != nullptr)
	{
		const int frame = g_window_ctx->game->m_frame_num;
		if (DumpVideoOutFrameSelected(frame))
		{
			char prefix[128];
			std::snprintf(prefix, sizeof(prefix), "/tmp/kyty-dump-videoout-f%d", frame);
			// Bypass unique-id dedup by using frame-specific prefix paths via direct readback.
			const uint32_t w = blt_src_image->extent.width;
			const uint32_t h = blt_src_image->extent.height;
			const bool hdr_present = (blt_src_image->format == VK_FORMAT_R16G16B16A16_SFLOAT);
			if (w > 0 && h > 0 && w <= 8192 && h <= 8192 &&
			    (blt_src_image->format == VK_FORMAT_R8G8B8A8_SRGB || blt_src_image->format == VK_FORMAT_R8G8B8A8_UNORM ||
			     blt_src_image->format == VK_FORMAT_B8G8R8A8_SRGB || blt_src_image->format == VK_FORMAT_B8G8R8A8_UNORM || hdr_present))
			{
				static std::set<int> dumped_frames;
				if (dumped_frames.insert(frame).second)
				{
					const uint64_t       bpp   = hdr_present ? 8u : 4u;
					const uint64_t       bytes = static_cast<uint64_t>(w) * h * bpp;
					std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
					UtilFillBuffer(&g_window_ctx->graphic_ctx, pixels.data(), bytes, w, blt_src_image,
					               static_cast<uint64_t>(blt_src_image->layout));
					char path[192];
					std::snprintf(path, sizeof(path), "%s-present-%ux%u.png", prefix, w, h);
					if (hdr_present)
					{
						const auto half_to_float = [](uint16_t hv) -> float
						{
							const uint32_t sign = (hv >> 15u) & 1u;
							const uint32_t exp  = (hv >> 10u) & 0x1fu;
							const uint32_t mant = hv & 0x3ffu;
							uint32_t       bits = 0;
							if (exp == 0u)
							{
								if (mant == 0u)
								{
									bits = sign << 31u;
								} else
								{
									uint32_t m = mant;
									int      e = -14;
									while ((m & 0x400u) == 0u)
									{
										m <<= 1u;
										e--;
									}
									bits = (sign << 31u) | (static_cast<uint32_t>(e + 127) << 23u) | ((m & 0x3ffu) << 13u);
								}
							} else if (exp == 0x1fu)
							{
								bits = (sign << 31u) | (0xffu << 23u) | (mant << 13u);
							} else
							{
								bits = (sign << 31u) | ((exp - 15u + 127u) << 23u) | (mant << 13u);
							}
							float value = 0.0f;
							std::memcpy(&value, &bits, sizeof(value));
							return value;
						};
						const uint16_t* src_h = reinterpret_cast<const uint16_t*>(pixels.data());
						std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
						for (uint64_t pixel = 0; pixel < static_cast<uint64_t>(w) * h; pixel++)
						{
							for (int c = 0; c < 4; c++)
							{
								const float v = std::clamp(half_to_float(src_h[pixel * 4u + c]), 0.0f, 1.0f);
								rgba[pixel * 4u + c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
							}
						}
						if (UtilWriteRgba8Png(path, rgba.data(), w, h, w))
						{
							std::fprintf(stderr, "KYTY_DUMP_VIDEOOUT wrote %s\n", path);
						}
					} else
					{
						std::vector<uint8_t> rgba(static_cast<size_t>(bytes));
						const bool bgra = blt_src_image->format == VK_FORMAT_B8G8R8A8_SRGB || blt_src_image->format == VK_FORMAT_B8G8R8A8_UNORM;
						for (uint64_t pixel = 0; pixel < static_cast<uint64_t>(w) * h; pixel++)
						{
							const auto src = pixel * 4u;
							rgba[src + 0u] = bgra ? pixels[src + 2u] : pixels[src + 0u];
							rgba[src + 1u] = pixels[src + 1u];
							rgba[src + 2u] = bgra ? pixels[src + 0u] : pixels[src + 2u];
							rgba[src + 3u] = pixels[src + 3u];
						}
						if (UtilWriteRgba8Png(path, rgba.data(), w, h, w))
						{
							std::fprintf(stderr, "KYTY_DUMP_VIDEOOUT wrote %s\n", path);
							char rt_prefix[128];
							std::snprintf(rt_prefix, sizeof(rt_prefix), "/tmp/kyty-dump-rt-at-f%d", frame);
							GraphicsDumpRememberedRts(&g_window_ctx->graphic_ctx, rt_prefix);
						}
					}
				}
			}
		}
	}

	DebugStatsRecordPresentSource(blt_src_image->extent.width, blt_src_image->extent.height, blt_dst_image->swapchain_extent.width,
	                              blt_dst_image->swapchain_extent.height, static_cast<uint32_t>(blt_src_image->layout));

	CommandBuffer buffer(GraphicContext::QUEUE_PRESENT);
	// buffer.SetQueue(GraphicContext::QUEUE_PRESENT);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	auto* vk_buffer = buffer.GetPool()->buffers[buffer.GetIndex()];

	buffer.Begin();

	const auto presentation_status = PresentationScalerBlitFinalImage(&buffer, &g_window_ctx->graphic_ctx, blt_src_image, blt_dst_image);
	if (presentation_status != PresentationScaleStatus::Success)
	{
		EXIT("Presentation scaling failed: status=%s(%u) source=%ux%u swapchain=%ux%u\n", PresentationScaleStatusName(presentation_status),
		     static_cast<unsigned>(presentation_status), blt_src_image->extent.width, blt_src_image->extent.height,
		     blt_dst_image->swapchain_extent.width, blt_dst_image->swapchain_extent.height);
	}

	const double now_seconds   = (g_window_ctx->game != nullptr) ? g_window_ctx->game->m_current_time_seconds : 0.0;
	const double fps_now       = (g_window_ctx->game != nullptr) ? g_window_ctx->game->m_current_fps : 0.0;
	const double frame_time_ms = (g_window_ctx->game != nullptr) ? DebugStatsFrameIntervalMs(g_window_ctx->game->m_current_time_seconds,
	                                                                                         g_window_ctx->game->m_previous_time_seconds)
	                                                             : 0.0;
	const bool   hud_drew =
	    DebugOverlayRecord(&g_window_ctx->graphic_ctx, g_window_ctx->swapchain, vk_buffer, now_seconds, fps_now, frame_time_ms);

	VkImageMemoryBarrier pre_present_barrier {};
	pre_present_barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	pre_present_barrier.pNext               = nullptr;
	pre_present_barrier.srcAccessMask       = hud_drew ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
	pre_present_barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
	pre_present_barrier.oldLayout           = hud_drew ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	pre_present_barrier.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	pre_present_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	pre_present_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	pre_present_barrier.subresourceRange.baseMipLevel   = 0;
	pre_present_barrier.subresourceRange.levelCount     = 1;
	pre_present_barrier.subresourceRange.baseArrayLayer = 0;
	pre_present_barrier.subresourceRange.layerCount     = 1;
	pre_present_barrier.image                           = g_window_ctx->swapchain->swapchain_images[g_window_ctx->swapchain->current_index];
	const VkPipelineStageFlags src_stage = hud_drew ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	vkCmdPipelineBarrier(vk_buffer, src_stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre_present_barrier);

	buffer.End();
	auto* render_finished = &g_window_ctx->swapchain->render_finished_semaphores[g_window_ctx->swapchain->current_index];
	buffer.ExecuteWithSemaphore(*render_finished);

	VkPresentInfoKHR present;
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext              = nullptr;
	present.swapchainCount     = 1;
	present.pSwapchains        = &g_window_ctx->swapchain->swapchain;
	present.pImageIndices      = &g_window_ctx->swapchain->current_index;
	present.pWaitSemaphores    = render_finished;
	present.waitSemaphoreCount = 1;
	present.pResults           = nullptr;

	const auto& queue = g_window_ctx->graphic_ctx.queues[GraphicContext::QUEUE_PRESENT];

	const auto present_start = std::chrono::steady_clock::now();
	{
		EXIT_IF(queue.mutex == nullptr);
		Core::LockGuard queue_lock(*queue.mutex);
		result = vkQueuePresentKHR(queue.vk_queue, &present);
	}
	// OUT_OF_DATE / SUBOPTIMAL are normal after resize or first present; recreate
	// the swapchain and continue. Only hard-fail other present errors.
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
	{
		VulkanRecreateSwapchain(&g_window_ctx->graphic_ctx, g_window_ctx->swapchain, 2);
	} else if (result != VK_SUCCESS)
	{
		EXIT("vkQueuePresentKHR failed: result=%d\n", static_cast<int>(result));
	}
	const auto present_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - present_start).count();
	DebugStatsRecordPresent(static_cast<uint64_t>(present_ns));

	g_window_ctx->native_capture.RecordPresent(WindowSteadyMs());
	// Agent observation only — does not wake guest waits or change present path.
	if (g_window_ctx->native_capture.present_count == 1)
	{
		Emulator::Agent::Lifecycle::EmitFirstPresent(g_window_ctx->native_capture.present_count);
	}
	if (capture_milestone != NativeCaptureMilestone::None)
	{
		// The capture reads the emulated source only after the present submit has
		// completed, so the readback cannot race the source blit.
		buffer.WaitForFence();
		NativeCaptureFrame(g_window_ctx, image, g_window_ctx->game->m_frame_num, capture_milestone);
	}

	if (g_window_ctx->native_capture.telemetry)
	{
		const double t = g_window_ctx->game->m_current_time_seconds;
		if (g_window_ctx->native_capture.TelemetryDue(t, 1.0))
		{
			std::fprintf(stderr, "KYTY_PRESENT_TELEMETRY frame=%d present=%llu fps=%.3f peak_rss_bytes=%llu size=%ux%u format=%s\n",
			             g_window_ctx->game->m_frame_num, static_cast<unsigned long long>(g_window_ctx->native_capture.present_count),
			             g_window_ctx->game->m_current_fps, static_cast<unsigned long long>(NativeCaptureHostPeakRssBytes()),
			             image->extent.width, image->extent.height, NativeCaptureFormatName(image->format));
		}
	}

	WindowUpdateTitle();

	// VideoOut retires the present-source submission immediately after this
	// function returns. Make that lifetime boundary explicit: vkQueuePresentKHR
	// only consumes the swapchain image, while the source image is protected
	// through completion of this blit submission.
	buffer.WaitForFence();
}

bool WindowGetPresentStats(WindowPresentStats* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = WindowPresentStats {};
	if (g_window_ctx == nullptr)
	{
		return true;
	}

	const uint64_t now_ms = WindowSteadyMs();
	out->graphic_ready    = g_window_ctx->graphic_initialized;
	out->capture_dir_set  = !g_window_ctx->native_capture.directory.empty();
	out->capture_ready    = out->capture_dir_set && out->graphic_ready;
	out->present          = g_window_ctx->native_capture.present_count;
	if (g_window_ctx->game != nullptr)
	{
		out->frame = g_window_ctx->game->m_frame_num;
		out->fps   = g_window_ctx->game->m_current_fps;
		g_window_ctx->native_capture.ObserveFrame(out->frame, now_ms);
	}
	if (g_window_ctx->native_capture.last_present_steady_ms != 0)
	{
		out->ms_since_present = now_ms - g_window_ctx->native_capture.last_present_steady_ms;
	}
	if (g_window_ctx->native_capture.last_frame_steady_ms != 0)
	{
		out->ms_since_frame = now_ms - g_window_ctx->native_capture.last_frame_steady_ms;
	}
	return true;
}

bool WindowRequestNativeCapture(uint64_t* out_request_id, WindowNativeCaptureResult* error_out)
{
	if (error_out != nullptr)
	{
		*error_out = WindowNativeCaptureResult {};
	}
	if (g_window_ctx == nullptr)
	{
		if (error_out != nullptr)
		{
			error_out->error_code    = "not_ready";
			error_out->error_message = "window context is not initialized";
		}
		return false;
	}
	if (g_window_ctx->native_capture.directory.empty())
	{
		if (error_out != nullptr)
		{
			error_out->error_code    = "capture_dir_unset";
			error_out->error_message = "set KYTY_NATIVE_CAPTURE_DIR before requesting capture";
		}
		return false;
	}

	Core::LockGuard lock(g_window_ctx->native_capture.result_mutex);
	if (g_window_ctx->native_capture.manual_pending)
	{
		if (error_out != nullptr)
		{
			error_out->error_code    = "busy";
			error_out->error_message = "a native capture request is already pending";
		}
		return false;
	}

	++g_window_ctx->native_capture.request_id;
	g_window_ctx->native_capture.manual_pending = true;
	if (out_request_id != nullptr)
	{
		*out_request_id = g_window_ctx->native_capture.request_id;
	}
	return true;
}

bool WindowWaitNativeCapture(uint64_t request_id, uint32_t timeout_ms, WindowNativeCaptureResult* out)
{
	if (out == nullptr)
	{
		return false;
	}
	*out = WindowNativeCaptureResult {};
	if (g_window_ctx == nullptr)
	{
		out->error_code    = "not_ready";
		out->error_message = "window context is not initialized";
		return false;
	}

	auto&          capture = g_window_ctx->native_capture;
	const uint64_t deadline_us =
	    static_cast<uint64_t>(timeout_ms) * 1000ull +
	    static_cast<uint64_t>(
	        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

	Core::LockGuard lock(capture.result_mutex);
	while (capture.completed_id < request_id)
	{
		const uint64_t now_us = static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		if (now_us >= deadline_us)
		{
			// Cancel the pending request so the present path does not keep
			// retrying a Manual capture after the agent has given up (that
			// stalls VideoOut when readback is slow or already wedged).
			if (capture.manual_pending && capture.request_id == request_id)
			{
				capture.manual_pending     = false;
				capture.completed_id       = request_id;
				capture.last_ok            = false;
				capture.last_error_code    = "timeout";
				capture.last_error_message = "timed out waiting for native capture";
				capture.result_cv.Signal();
			}
			out->error_code    = "timeout";
			out->error_message = "timed out waiting for native capture";
			return false;
		}
		const uint64_t remaining_us = deadline_us - now_us;
		const uint32_t slice_us     = remaining_us > 200000ull ? 200000u : static_cast<uint32_t>(remaining_us == 0 ? 1 : remaining_us);
		capture.result_cv.WaitFor(&capture.result_mutex, slice_us);
	}

	out->ok            = capture.last_ok;
	out->path          = capture.last_path;
	out->milestone     = capture.last_milestone;
	out->format        = capture.last_format;
	out->width         = capture.last_width;
	out->height        = capture.last_height;
	out->present       = capture.last_present;
	out->frame         = capture.last_frame;
	out->error_code    = capture.last_error_code;
	out->error_message = capture.last_error_message;
	return capture.last_ok;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
