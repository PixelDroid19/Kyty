#include "EditorFields.h"

namespace {

bool Is(const QVariantMap& values, const char* key, const char* value)
{
	return values.value(key).toString() == QLatin1String(value);
}

const QVector<EditorField> GAME_FIELDS = {
    {"name", QT_TRANSLATE_NOOP("EditorPageItem", "Name"), FieldType::Text, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "General")},
    {"basedir", QT_TRANSLATE_NOOP("EditorPageItem", "Game folder"), FieldType::Path, nullptr, true},
    {"param_file", QT_TRANSLATE_NOOP("EditorPageItem", "Parameters file"), FieldType::Path},
    {"game_patch_file", QT_TRANSLATE_NOOP("EditorPageItem", "Game patch file"), FieldType::Path},
    {"addcont_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Add-on content folder"), FieldType::Path, nullptr, true},
    {"environment", QT_TRANSLATE_NOOP("EditorPageItem", "Environment variables"), FieldType::Text},
    {"custom_settings", QT_TRANSLATE_NOOP("EditorPageItem", "Custom emulator settings"), FieldType::Toggle},
};

const QVector<EditorField> DISPLAY_FIELDS = {
    {"gpu_device", QT_TRANSLATE_NOOP("EditorPageItem", "Graphics card"), FieldType::Choice, "gpu_device", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Display")},
    {"fullscreen_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Start in full screen"), FieldType::Toggle},
    {"present_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Frame delivery"), FieldType::Choice, "present_mode"},
    {"present_fps_limit", QT_TRANSLATE_NOOP("EditorPageItem", "Frame limit (0 = none)"), FieldType::Number},
    {"perf_overlay_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Show performance overlay"), FieldType::Toggle},
    {"hide_cursor_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Hide the mouse cursor"), FieldType::Toggle},
    {"screen_width", QT_TRANSLATE_NOOP("EditorPageItem", "Screen width"), FieldType::Number},
    {"screen_height", QT_TRANSLATE_NOOP("EditorPageItem", "Screen height"), FieldType::Number},
    {"graphics_preset", QT_TRANSLATE_NOOP("EditorPageItem", "Graphics preset"), FieldType::Choice, "graphics_preset"},
    {"render_resolution_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Render resolution"), FieldType::Choice, "render_resolution_mode"},
    {"render_resolution_scope", QT_TRANSLATE_NOOP("EditorPageItem", "Scaled surfaces"), FieldType::Choice, "render_resolution_scope", false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"render_resolution_width", QT_TRANSLATE_NOOP("EditorPageItem", "Render width"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"render_resolution_height", QT_TRANSLATE_NOOP("EditorPageItem", "Render height"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"presentation_filter", QT_TRANSLATE_NOOP("EditorPageItem", "Presentation filter"), FieldType::Choice, "presentation_filter"},
    {"presentation_scaling", QT_TRANSLATE_NOOP("EditorPageItem", "Picture shape"), FieldType::Choice, "presentation_scaling"},
    {"presentation_fsr_sharpness", QT_TRANSLATE_NOOP("EditorPageItem", "FSR sharpness (0–100)"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "presentation_filter", "Fsr") || Is(f, "presentation_filter", "Fsr3"); }},
    {"texture_anisotropy", QT_TRANSLATE_NOOP("EditorPageItem", "Texture anisotropy"), FieldType::Choice, "texture_anisotropy"},
    {"neo", QT_TRANSLATE_NOOP("EditorPageItem", "Neo mode"), FieldType::Toggle},
};

const QVector<EditorField> SYSTEM_FIELDS = {
    {"console_language", QT_TRANSLATE_NOOP("EditorPageItem", "System language"), FieldType::Choice, "console_language", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Console")},
    {"user_name", QT_TRANSLATE_NOOP("EditorPageItem", "User name"), FieldType::Text},
    {"network_connected", QT_TRANSLATE_NOOP("EditorPageItem", "Network connected"), FieldType::Toggle},
    {"notice_screen_skip_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Skip notice screens"), FieldType::Toggle},
    {"controller_vibration", QT_TRANSLATE_NOOP("EditorPageItem", "Vibration strength (0–100)"), FieldType::Number},
    {"controller_light_bar", QT_TRANSLATE_NOOP("EditorPageItem", "Light bar colour"), FieldType::Choice, "controller_light_bar"},
    {"controller_speaker_volume", QT_TRANSLATE_NOOP("EditorPageItem", "Controller speaker volume (0–100)"), FieldType::Number},
    {"microphone_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Microphone"), FieldType::Toggle},
};

const QVector<EditorField> SHADER_FIELDS = {
    {"shader_cache_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Shader cache"), FieldType::Toggle, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Shaders and validation")},
    {"shader_optimization_type", QT_TRANSLATE_NOOP("EditorPageItem", "Optimization"), FieldType::Choice, "shader_optimization_type"},
    {"shader_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Shader validation"), FieldType::Toggle},
    {"vulkan_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Vulkan validation"), FieldType::Toggle},
    {"renderdoc_capture_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "RenderDoc capture"), FieldType::Toggle},
    {"spirv_debug_printf_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "SPIR-V debug printf"), FieldType::Toggle},
};

const QVector<EditorField> LOG_FIELDS = {
    {"printf_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Log output"), FieldType::Choice, "printf_direction", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Logs and dumps")},
    {"printf_level", QT_TRANSLATE_NOOP("EditorPageItem", "Log level"), FieldType::Choice, "printf_level", false,
	 [](const QVariantMap& f) { return !Is(f, "printf_direction", "Silent"); }},
    {"printf_output_file", QT_TRANSLATE_NOOP("EditorPageItem", "Log file"), FieldType::Path, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "printf_direction", "File"); }},
    {"printf_output_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Log folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return Is(f, "printf_direction", "Directory"); }},
    {"shader_log_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Shader log"), FieldType::Choice, "shader_log_direction"},
    {"shader_log_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Shader log folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return Is(f, "shader_log_direction", "File"); }},
    {"command_buffer_dump_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Command buffer dump"), FieldType::Toggle},
    {"command_buffer_dump_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Dump folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return f.value("command_buffer_dump_enabled").toBool(); }},
    {"pipeline_dump_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Pipeline dump"), FieldType::Toggle},
    {"pipeline_dump_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Pipeline dump folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return f.value("pipeline_dump_enabled").toBool(); }},
    {"profiler_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Profiler output"), FieldType::Choice, "profiler_direction"},
    {"profiler_output_file", QT_TRANSLATE_NOOP("EditorPageItem", "Profiler file"), FieldType::Path, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "profiler_direction", "File") || Is(f, "profiler_direction", "FileAndNetwork"); }},
};

const QVector<EditorField> MODULE_FIELDS = {
    {"elfs", QT_TRANSLATE_NOOP("EditorPageItem", "ELF and PRX files"), FieldType::Modules, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Modules")},
    {"param_file", QT_TRANSLATE_NOOP("EditorPageItem", "param.sfo"), FieldType::Params, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Parameters")},
};

const QVector<EditorField> PROGRAM_FIELDS = {
    {"run_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Run mode"), FieldType::Choice, "run_mode"},
};

const QVector<EditorField> INTERFACE_FIELDS = {
    {"language", QT_TRANSLATE_NOOP("EditorPageItem", "Language"), FieldType::Choice, "language"},
    {"color_scheme", QT_TRANSLATE_NOOP("EditorPageItem", "Theme"), FieldType::Choice, "color_scheme", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Appearance")},
    {"theme", QT_TRANSLATE_NOOP("EditorPageItem", "Style"), FieldType::Choice, "theme"},
    {"layout", QT_TRANSLATE_NOOP("EditorPageItem", "Layout"), FieldType::Choice, "layout"},
    {"accent", QT_TRANSLATE_NOOP("EditorPageItem", "Accent colour"), FieldType::Choice, "accent"},
    {"interface_size", QT_TRANSLATE_NOOP("EditorPageItem", "Interface size"), FieldType::Choice, "interface_size"},
    {"animations", QT_TRANSLATE_NOOP("EditorPageItem", "Animations"), FieldType::Choice, "animations"},
    {"background", QT_TRANSLATE_NOOP("EditorPageItem", "Background"), FieldType::Choice, "background"},
    {"cover_size", QT_TRANSLATE_NOOP("EditorPageItem", "Cover size"), FieldType::Choice, "cover_size", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Library")},
    {"show_titles", QT_TRANSLATE_NOOP("EditorPageItem", "Names under covers"), FieldType::Toggle},
    {"show_recent", QT_TRANSLATE_NOOP("EditorPageItem", "Recently played row"), FieldType::Toggle},
    {"sort", QT_TRANSLATE_NOOP("EditorPageItem", "Sort games by"), FieldType::Choice, "sort"},
    {"on_launch", QT_TRANSLATE_NOOP("EditorPageItem", "When a game starts"), FieldType::Choice, "on_launch"},
    {"window_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Launcher window"), FieldType::Choice, "window_mode"},
    {"start_on", QT_TRANSLATE_NOOP("EditorPageItem", "Opens on"), FieldType::Choice, "start_on"},
    {"ui_sounds", QT_TRANSLATE_NOOP("EditorPageItem", "Interface sounds"), FieldType::Toggle, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Sound")},
    {"ui_volume", QT_TRANSLATE_NOOP("EditorPageItem", "Sound volume"), FieldType::Choice, "ui_volume", false,
	 [](const QVariantMap& f) { return f.value("ui_sounds").toBool(); }},
};

struct Help
{
	const char* key;
	const char* text;
};

const Help HELP[] = {
    {"gpu_device", QT_TRANSLATE_NOOP("EditorPageItem", "Automatic prefers a dedicated card. Only cards with Vulkan 1.4 are listed.")},
    {"present_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Vertical sync never tears but adds delay. If the screen lacks a mode, the next one that works is used.")},
    {"present_fps_limit", QT_TRANSLATE_NOOP("EditorPageItem", "The most frames per second shown on screen. 0 leaves them unlimited.")},
    {"perf_overlay_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Shows frame rate and timings over the game.")},
    {"hide_cursor_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "In full screen the cursor already hides when the mouse stops. This also hides it in a window.")},
    {"console_language", QT_TRANSLATE_NOOP("EditorPageItem", "The language games read from the console. A game without it uses its own default.")},
    {"user_name", QT_TRANSLATE_NOOP("EditorPageItem", "The name games show for the player. It does not change where saves are kept.")},
    {"network_connected", QT_TRANSLATE_NOOP("EditorPageItem", "Off tells games the console has no network. On reports a local connection; there is no online play.")},
    {"notice_screen_skip_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Tells games that show a notice at the start that it may be skipped. Not every game asks.")},
    {"controller_vibration", QT_TRANSLATE_NOOP("EditorPageItem", "How strongly the controller follows the vibration games request. 0 turns it off.")},
    {"controller_light_bar", QT_TRANSLATE_NOOP("EditorPageItem", "A colour here replaces the one games choose.")},
    {"controller_speaker_volume", QT_TRANSLATE_NOOP("EditorPageItem", "Sounds games send to the controller's speaker play on the computer at this volume. 0 keeps them silent.")},
    {"microphone_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Games that listen to a microphone hear the computer's default recording device. Off, they hear silence.")},
    {"presentation_scaling", QT_TRANSLATE_NOOP("EditorPageItem", "Stretch fills the window. Fit keeps the game's shape with black bars. Whole-pixel steps also keeps every pixel the same size while the picture fits the window.")},
    {"addcont_folder", QT_TRANSLATE_NOOP("EditorPageItem", "A folder with one folder per game ID, each holding that game's installed add-ons. Add-ons listed in the game's own dlc_emu.ini are found without it.")},
    {"environment", QT_TRANSLATE_NOOP("EditorPageItem", "Extra settings for the emulator process, as NAME=value pairs separated by ';'.")},
    {"shader_cache_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Keeps compiled shaders on disk so games load and run smoothly the next time. Turn it off only to rule out a bad cache.")},
    {"basedir", QT_TRANSLATE_NOOP("EditorPageItem", "The folder that holds the game's files.")},
    {"custom_settings", QT_TRANSLATE_NOOP("EditorPageItem", "Off runs the game with the emulator settings from Settings.")},
    {"screen_width", QT_TRANSLATE_NOOP("EditorPageItem", "The game window's size. It does not change the resolution the game renders at.")},
    {"graphics_preset", QT_TRANSLATE_NOOP("EditorPageItem", "Fills the graphics fields below. The preset itself is not saved.")},
    {"render_resolution_mode",
	 QT_TRANSLATE_NOOP("EditorPageItem",
	                   "Native renders at the game's own size. Fixed renders at most at the size below, keeping the shape.")},
    {"render_resolution_scope",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Render targets also scales the game's full-screen render textures; not every game supports it.")},
    {"presentation_filter",
	 QT_TRANSLATE_NOOP("EditorPageItem", "How the finished picture is scaled to the window. Fsr3 only works in an emulator built with its SDK.")},
    {"texture_anisotropy", QT_TRANSLATE_NOOP("EditorPageItem", "Sharper textures seen at a slant. It does not change the resolution.")},
    {"vulkan_validation_enabled",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Checks every graphics call for mistakes. For debugging; games run much slower.")},
    {"renderdoc_capture_enabled",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Loads an installed RenderDoc so it can capture frames. The window opens through X11, which RenderDoc needs.")},
    {"printf_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Where the emulator writes what it reports while the game runs.")},
    {"command_buffer_dump_enabled",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Writes the game's GPU commands to disk, for debugging. Uses a lot of space.")},
    {"pipeline_dump_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Writes the game's graphics pipelines to disk, for debugging.")},
    {"theme", QT_TRANSLATE_NOOP("EditorPageItem", "Add-on themes are folders with a theme.ini inside the themes folder above, found when the launcher starts. Their colours win over the accent colour.")},
    {"interface_size", QT_TRANSLATE_NOOP("EditorPageItem", "Scales every text and control of the launcher.")},
    {"animations", QT_TRANSLATE_NOOP("EditorPageItem", "Reduced keeps page changes and stops what moves on its own.")},
    {"show_recent", QT_TRANSLATE_NOOP("EditorPageItem", "Hidden when the window is too small to show it with a row of covers.")},
    {"sort", QT_TRANSLATE_NOOP("EditorPageItem", "In your own order, games move with Move up and Move down in their options.")},
    {"on_launch", QT_TRANSLATE_NOOP("EditorPageItem", "Minimizing leaves the screen and the graphics card to the game.")},
    {"window_mode", QT_TRANSLATE_NOOP("EditorPageItem", "How the launcher opens. Full screen suits a TV.")},
    {"start_on", QT_TRANSLATE_NOOP("EditorPageItem", "The last game played opens on its page, ready to continue.")},
    {"layout", QT_TRANSLATE_NOOP("EditorPageItem", "Console puts the pages in tabs on top and the games in one row, the selected game's page below it. An add-on theme may choose the layout.")},
    {"ui_sounds", QT_TRANSLATE_NOOP("EditorPageItem", "Short sounds when you move, select, go back and start a game.")},
};

} // namespace

const char* FieldHelp(const char* key)
{
	for (const auto& help: HELP)
	{
		if (qstrcmp(help.key, key) == 0)
		{
			return help.text;
		}
	}
	return nullptr;
}

const QVector<EditorField>& EditorFields(FieldGroup group)
{
	switch (group)
	{
		case FieldGroup::Game: return GAME_FIELDS;
		case FieldGroup::Display: return DISPLAY_FIELDS;
		case FieldGroup::System: return SYSTEM_FIELDS;
		case FieldGroup::Shaders: return SHADER_FIELDS;
		case FieldGroup::Logs: return LOG_FIELDS;
		case FieldGroup::Modules: return MODULE_FIELDS;
		case FieldGroup::Program: return PROGRAM_FIELDS;
		case FieldGroup::Interface: return INTERFACE_FIELDS;
	}
	return GAME_FIELDS;
}
