#include "Emulator/Config.h"
#include "Emulator/ConfigSource.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Config {

struct Config
{
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	RenderResolutionMode   render_resolution_mode      = RenderResolutionMode::Fixed;
	RenderResolutionScope  render_resolution_scope     = RenderResolutionScope::Display;
	uint32_t               render_resolution_width     = 1280;
	uint32_t               render_resolution_height    = 720;
	PresentationFilter     presentation_filter         = PresentationFilter::Nearest;
	uint32_t               presentation_fsr_sharpness  = 90;
	uint32_t               present_fps_limit           = 0;
	TextureAnisotropy      texture_anisotropy          = TextureAnisotropy::Guest;
	String                 gpu_device;
	PresentMode            present_mode                = PresentMode::Mailbox;
	bool                   fullscreen_enabled          = false;
	bool                   hide_cursor_enabled         = false;
	bool                   perf_overlay_enabled        = false;
	bool                   shader_cache_enabled        = true;
	int32_t                console_language            = 1;
	String                 user_name                   = U"Kyty";
	bool                   notice_screen_skip_enabled  = false;
	bool                   network_connected           = true;
	uint32_t               controller_vibration        = 100;
	bool                   neo                         = true;
	GuestPlatform          guest_platform              = GuestPlatform::Unknown;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	// Host resource defaults: stay quiet and dump-free. Verbose console/file
	// logging and buffer/pipeline dumps are opt-in — full boots can otherwise
	// fill host disk and burn CPU/RAM on log formatting alone.
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	String                 shader_log_folder           = U"_Shaders";
	bool                   command_buffer_dump_enabled = false;
	String                 command_buffer_dump_folder  = U"_Buffers";
	Log::Direction         printf_direction            = Log::Direction::Silent;
	Log::Level             printf_level                = Log::Level::Info;
	String                 printf_output_file          = U"_kyty.txt";
	String                 printf_output_folder        = U"_Logs";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	String                 profiler_output_file        = U"_profile.prof";
	// Empty selects the built-in input profile.
	String                 input_profile_file;
	String                 game_patch_file;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   pipeline_dump_enabled       = false;
	String                 pipeline_dump_folder        = U"_Pipelines";
};

static Config* g_config = nullptr;

void ConfigSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	EXIT_IF(g_config != nullptr);

	g_config = new Config;
}

void ConfigSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void ConfigSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

template <class T>
void LoadInt(T& dst, const ConfigSource& cfg, const String& key)
{
	if (cfg.Has(key))
	{
		dst = static_cast<T>(cfg.GetInteger(key));
	}
}

void LoadBool(bool& dst, const ConfigSource& cfg, const String& key)
{
	if (cfg.Has(key))
	{
		dst = cfg.GetBool(key);
	}
}

template <class T>
void LoadEnum(T& dst, const ConfigSource& cfg, const String& key)
{
	if (cfg.Has(key))
	{
		dst = Core::EnumValue(cfg.GetString(key), dst);
	}
}

template <class T>
void LoadStrictEnum(T& dst, const ConfigSource& cfg, const String& key)
{
	if (cfg.Has(key))
	{
		const auto text  = cfg.GetString(key);
		const auto value = Core::EnumValue(text, dst);
		if (Core::EnumName(value) != text)
		{
			EXIT("Invalid config value for %s\n", key.C_Str());
		}
		dst = value;
	}
}

void LoadStr(String& dst, const ConfigSource& cfg, const String& key)
{
	if (cfg.Has(key))
	{
		dst = cfg.GetString(key);
	}
}

void Load(const ConfigSource& cfg)
{
	LoadInt(g_config->screen_width, cfg, U"ScreenWidth");
	LoadInt(g_config->screen_height, cfg, U"ScreenHeight");
	LoadStrictEnum(g_config->render_resolution_mode, cfg, U"RenderResolutionMode");
	LoadStrictEnum(g_config->render_resolution_scope, cfg, U"RenderResolutionScope");
	LoadInt(g_config->render_resolution_width, cfg, U"RenderResolutionWidth");
	LoadInt(g_config->render_resolution_height, cfg, U"RenderResolutionHeight");
	LoadStrictEnum(g_config->presentation_filter, cfg, U"PresentationFilter");
	LoadInt(g_config->presentation_fsr_sharpness, cfg, U"PresentationFsrSharpness");
	LoadInt(g_config->present_fps_limit, cfg, U"PresentFpsLimit");
	LoadStrictEnum(g_config->texture_anisotropy, cfg, U"TextureAnisotropy");
	if (g_config->presentation_fsr_sharpness > 100)
	{
		EXIT("Invalid config value for PresentationFsrSharpness: %u is above 100\n", g_config->presentation_fsr_sharpness);
	}
	if (g_config->screen_width == 0 || g_config->screen_height == 0 || g_config->render_resolution_width == 0 ||
	    g_config->render_resolution_height == 0)
	{
		EXIT("Invalid zero graphics extent in configuration\n");
	}
	LoadBool(g_config->neo, cfg, U"Neo");
	LoadBool(g_config->vulkan_validation_enabled, cfg, U"VulkanValidationEnabled");
	LoadBool(g_config->shader_validation_enabled, cfg, U"ShaderValidationEnabled");
	LoadEnum(g_config->shader_optimization_type, cfg, U"ShaderOptimizationType");
	LoadEnum(g_config->shader_log_direction, cfg, U"ShaderLogDirection");
	LoadStr(g_config->shader_log_folder, cfg, U"ShaderLogFolder");
	LoadBool(g_config->command_buffer_dump_enabled, cfg, U"CommandBufferDumpEnabled");
	LoadStr(g_config->command_buffer_dump_folder, cfg, U"CommandBufferDumpFolder");
	LoadEnum(g_config->printf_direction, cfg, U"PrintfDirection");
	LoadEnum(g_config->printf_level, cfg, U"PrintfLevel");
	LoadStr(g_config->printf_output_file, cfg, U"PrintfOutputFile");
	LoadStr(g_config->printf_output_folder, cfg, U"PrintfOutputFolder");
	LoadStr(g_config->input_profile_file, cfg, U"InputProfileFile");
	LoadStr(g_config->game_patch_file, cfg, U"GamePatchFile");
	LoadEnum(g_config->profiler_direction, cfg, U"ProfilerDirection");
	LoadStr(g_config->profiler_output_file, cfg, U"ProfilerOutputFile");
	LoadBool(g_config->spirv_debug_printf_enabled, cfg, U"SpirvDebugPrintfEnabled");
	LoadBool(g_config->pipeline_dump_enabled, cfg, U"PipelineDumpEnabled");
	LoadStr(g_config->pipeline_dump_folder, cfg, U"PipelineDumpFolder");
	LoadStr(g_config->gpu_device, cfg, U"GpuDevice");
	LoadStrictEnum(g_config->present_mode, cfg, U"PresentMode");
	LoadBool(g_config->fullscreen_enabled, cfg, U"FullscreenEnabled");
	LoadBool(g_config->hide_cursor_enabled, cfg, U"HideCursorEnabled");
	LoadBool(g_config->perf_overlay_enabled, cfg, U"PerfOverlayEnabled");
	LoadBool(g_config->shader_cache_enabled, cfg, U"ShaderCacheEnabled");
	LoadInt(g_config->console_language, cfg, U"ConsoleLanguage");
	LoadStr(g_config->user_name, cfg, U"UserName");
	LoadBool(g_config->notice_screen_skip_enabled, cfg, U"NoticeScreenSkipEnabled");
	LoadBool(g_config->network_connected, cfg, U"NetworkConnected");
	LoadInt(g_config->controller_vibration, cfg, U"ControllerVibration");
	if (g_config->console_language < 0 || g_config->console_language > 29)
	{
		EXIT("Invalid config value for ConsoleLanguage: %d is outside 0..29\n", g_config->console_language);
	}
	// The guest's name buffers hold 16 bytes and a terminator; utf8_str() counts its own terminator.
	const auto user_name_size = g_config->user_name.utf8_str().Size() - 1;
	if (user_name_size == 0 || user_name_size > 16)
	{
		EXIT("Invalid config value for UserName: it must have 1 to 16 bytes\n");
	}
	if (g_config->controller_vibration > 100)
	{
		EXIT("Invalid config value for ControllerVibration: %u is above 100\n", g_config->controller_vibration);
	}
}

uint32_t GetScreenWidth()
{
	return g_config->screen_width;
}

uint32_t GetScreenHeight()
{
	return g_config->screen_height;
}

RenderResolutionMode GetRenderResolutionMode()
{
	return g_config->render_resolution_mode;
}

RenderResolutionScope GetRenderResolutionScope()
{
	return g_config->render_resolution_scope;
}

uint32_t GetRenderResolutionWidth()
{
	return g_config->render_resolution_width;
}

uint32_t GetRenderResolutionHeight()
{
	return g_config->render_resolution_height;
}

PresentationFilter GetPresentationFilter()
{
	return g_config->presentation_filter;
}

uint32_t GetPresentationFsrSharpness()
{
	return g_config->presentation_fsr_sharpness;
}

uint32_t GetPresentFpsLimit()
{
	return g_config->present_fps_limit;
}

TextureAnisotropy GetTextureAnisotropy()
{
	return g_config->texture_anisotropy;
}

String GetGpuDevice()
{
	return g_config->gpu_device;
}

PresentMode GetPresentMode()
{
	return g_config->present_mode;
}

bool FullscreenEnabled()
{
	return g_config->fullscreen_enabled;
}

bool HideCursorEnabled()
{
	return g_config->hide_cursor_enabled;
}

bool PerfOverlayEnabled()
{
	return g_config->perf_overlay_enabled;
}

bool ShaderCacheEnabled()
{
	return g_config->shader_cache_enabled;
}

int32_t GetConsoleLanguage()
{
	return g_config->console_language;
}

String GetUserName()
{
	return g_config->user_name;
}

bool NoticeScreenSkipEnabled()
{
	return g_config->notice_screen_skip_enabled;
}

bool NetworkConnected()
{
	return g_config->network_connected;
}

uint32_t GetControllerVibration()
{
	return g_config->controller_vibration;
}

bool IsNeo()
{
	return g_config->neo || g_config->guest_platform == GuestPlatform::Ps5;
}

bool VulkanValidationEnabled()
{
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled()
{
	return g_config->shader_validation_enabled;
}

ShaderOptimizationType GetShaderOptimizationType()
{
	return g_config->shader_optimization_type;
}

ShaderLogDirection GetShaderLogDirection()
{
	return g_config->shader_log_direction;
}

String GetShaderLogFolder()
{
	return g_config->shader_log_folder;
}

String GetInputProfileFile()
{
	return g_config->input_profile_file;
}

String GetGamePatchFile()
{
	return g_config->game_patch_file;
}

bool CommandBufferDumpEnabled()
{
	return g_config->command_buffer_dump_enabled;
}

String GetCommandBufferDumpFolder()
{
	return g_config->command_buffer_dump_folder;
}

Log::Direction GetPrintfDirection()
{
	return g_config->printf_direction;
}

Log::Level GetPrintfLevel()
{
	return g_config->printf_level;
}

String GetPrintfOutputFile()
{
	return g_config->printf_output_file;
}

String GetPrintfOutputFolder()
{
	return g_config->printf_output_folder;
}

ProfilerDirection GetProfilerDirection()
{
	return g_config->profiler_direction;
}

String GetProfilerOutputFile()
{
	return g_config->profiler_output_file;
}

bool SpirvDebugPrintfEnabled()
{
	return g_config->spirv_debug_printf_enabled;
}

bool PipelineDumpEnabled()
{
	return g_config->pipeline_dump_enabled;
}

String GetPipelineDumpFolder()
{
	return g_config->pipeline_dump_folder;
}

bool SetGuestPlatform(GuestPlatform platform)
{
	EXIT_IF(g_config == nullptr);
	if (platform == GuestPlatform::Unknown)
	{
		KYTY_LOG_WARN("guest platform cannot be set to unknown\n");
		return false;
	}
	if (g_config->guest_platform != GuestPlatform::Unknown && g_config->guest_platform != platform)
	{
		KYTY_LOG_WARN("guest platform conflict: established=%s requested=%s\n", GuestPlatformName(g_config->guest_platform),
		       GuestPlatformName(platform));
		return false;
	}
	g_config->guest_platform = platform;
	return true;
}

GuestPlatform GetGuestPlatform()
{
	EXIT_IF(g_config == nullptr);
	return g_config->guest_platform;
}

void ResetGuestPlatform()
{
	EXIT_IF(g_config == nullptr);
	g_config->guest_platform = GuestPlatform::Unknown;
}

void SetNextGen(bool mode)
{
	// Compatibility seam for existing unit tests. RuntimeLinker owns the
	// production lifecycle and uses SetGuestPlatform instead.
	ResetGuestPlatform();
	const bool set = SetGuestPlatform(mode ? GuestPlatform::Ps5 : GuestPlatform::Ps4);
	EXIT_IF(!set);
}

bool IsInitialized()
{
	return g_config != nullptr;
}

bool IsNextGen()
{
	EXIT_IF(g_config == nullptr || g_config->guest_platform == GuestPlatform::Unknown);
	return g_config->guest_platform == GuestPlatform::Ps5;
}

} // namespace Kyty::Config

#endif // KYTY_EMU_ENABLED
