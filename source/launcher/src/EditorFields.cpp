#include "EditorFields.h"

namespace {

bool Is(const QVariantMap& values, const char* key, const char* value)
{
	return values.value(key).toString() == QLatin1String(value);
}

// Default is the emulator's own layout, and a damaged profile has nothing to edit: both show only the profile list.
bool Editable(const QVariantMap& values)
{
	return !Is(values, "input_profile", "Default") && values.value("profile_error").toString().isEmpty();
}

const QVector<EditorField> GAME_FIELDS = {
    {"name", QT_TRANSLATE_NOOP("EditorPageItem", "Name"), FieldType::Text, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "General")},
    {"basedir", QT_TRANSLATE_NOOP("EditorPageItem", "Game folder"), FieldType::Path, nullptr, true},
    {"param_file", QT_TRANSLATE_NOOP("EditorPageItem", "Parameters file"), FieldType::Path},
    {"custom_settings", QT_TRANSLATE_NOOP("EditorPageItem", "Custom emulator settings"), FieldType::Toggle},
};

const QVector<EditorField> DISPLAY_FIELDS = {
    {"screen_width", QT_TRANSLATE_NOOP("EditorPageItem", "Screen width"), FieldType::Number, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Display")},
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
    {"presentation_fsr_sharpness", QT_TRANSLATE_NOOP("EditorPageItem", "FSR sharpness (0–100)"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "presentation_filter", "Fsr"); }},
    {"texture_anisotropy", QT_TRANSLATE_NOOP("EditorPageItem", "Texture anisotropy"), FieldType::Choice, "texture_anisotropy"},
    {"neo", QT_TRANSLATE_NOOP("EditorPageItem", "Neo mode"), FieldType::Toggle},
};

const QVector<EditorField> SHADER_FIELDS = {
    {"shader_optimization_type", QT_TRANSLATE_NOOP("EditorPageItem", "Optimization"), FieldType::Choice, "shader_optimization_type", false,
	 nullptr, QT_TRANSLATE_NOOP("EditorPageItem", "Shaders and validation")},
    {"shader_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Shader validation"), FieldType::Toggle},
    {"vulkan_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Vulkan validation"), FieldType::Toggle},
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

const QVector<EditorField> CONTROL_FIELDS = {
    {"input_profile", QT_TRANSLATE_NOOP("EditorPageItem", "Profile"), FieldType::Choice, "input_profile", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Profile")},
    {"profile_name", QT_TRANSLATE_NOOP("EditorPageItem", "Profile name"), FieldType::Text, nullptr, false, Editable},
    {"input_device", QT_TRANSLATE_NOOP("EditorPageItem", "Controller"), FieldType::Choice, "input_device", false, Editable,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Controller")},
    {"left_stick", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick"), FieldType::Stick},
    {"left_deadzone", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick dead zone"), FieldType::Choice, "deadzone", false, Editable},
    {"left_invert_x", QT_TRANSLATE_NOOP("EditorPageItem", "Invert left stick across"), FieldType::Toggle, nullptr, false, Editable},
    {"left_invert_y", QT_TRANSLATE_NOOP("EditorPageItem", "Invert left stick up and down"), FieldType::Toggle, nullptr, false, Editable},
    {"right_stick", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick"), FieldType::Stick},
    {"right_deadzone", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick dead zone"), FieldType::Choice, "deadzone", false, Editable},
    {"right_invert_x", QT_TRANSLATE_NOOP("EditorPageItem", "Invert right stick across"), FieldType::Toggle, nullptr, false, Editable},
    {"right_invert_y", QT_TRANSLATE_NOOP("EditorPageItem", "Invert right stick up and down"), FieldType::Toggle, nullptr, false, Editable},
    {"trigger_deadzone", QT_TRANSLATE_NOOP("EditorPageItem", "Trigger press point"), FieldType::Choice, "deadzone", false, Editable},
    {"mouse_look", QT_TRANSLATE_NOOP("EditorPageItem", "Look with the mouse"), FieldType::Toggle, nullptr, false, Editable,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Mouse")},
    {"mouse_sensitivity", QT_TRANSLATE_NOOP("EditorPageItem", "Mouse sensitivity"), FieldType::Choice, "sensitivity", false,
	 [](const QVariantMap& f) { return Editable(f) && f.value("mouse_look").toBool(); }},
    {"mouse_invert_y", QT_TRANSLATE_NOOP("EditorPageItem", "Invert mouse up and down"), FieldType::Toggle, nullptr, false,
	 [](const QVariantMap& f) { return Editable(f) && f.value("mouse_look").toBool(); }},
    {"bind.cross", QT_TRANSLATE_NOOP("EditorPageItem", "✕ Cross"), FieldType::Binding, nullptr, false, Editable,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Buttons")},
    {"bind.circle", QT_TRANSLATE_NOOP("EditorPageItem", "○ Circle"), FieldType::Binding, nullptr, false, Editable},
    {"bind.square", QT_TRANSLATE_NOOP("EditorPageItem", "□ Square"), FieldType::Binding, nullptr, false, Editable},
    {"bind.triangle", QT_TRANSLATE_NOOP("EditorPageItem", "△ Triangle"), FieldType::Binding, nullptr, false, Editable},
    {"bind.up", QT_TRANSLATE_NOOP("EditorPageItem", "D-pad up"), FieldType::Binding, nullptr, false, Editable},
    {"bind.down", QT_TRANSLATE_NOOP("EditorPageItem", "D-pad down"), FieldType::Binding, nullptr, false, Editable},
    {"bind.left", QT_TRANSLATE_NOOP("EditorPageItem", "D-pad left"), FieldType::Binding, nullptr, false, Editable},
    {"bind.right", QT_TRANSLATE_NOOP("EditorPageItem", "D-pad right"), FieldType::Binding, nullptr, false, Editable},
    {"bind.l1", QT_TRANSLATE_NOOP("EditorPageItem", "L1"), FieldType::Binding, nullptr, false, Editable},
    {"bind.r1", QT_TRANSLATE_NOOP("EditorPageItem", "R1"), FieldType::Binding, nullptr, false, Editable},
    {"bind.l2", QT_TRANSLATE_NOOP("EditorPageItem", "L2"), FieldType::Binding, nullptr, false, Editable},
    {"bind.r2", QT_TRANSLATE_NOOP("EditorPageItem", "R2"), FieldType::Binding, nullptr, false, Editable},
    {"bind.l3", QT_TRANSLATE_NOOP("EditorPageItem", "L3"), FieldType::Binding, nullptr, false, Editable},
    {"bind.r3", QT_TRANSLATE_NOOP("EditorPageItem", "R3"), FieldType::Binding, nullptr, false, Editable},
    {"bind.options", QT_TRANSLATE_NOOP("EditorPageItem", "Options"), FieldType::Binding, nullptr, false, Editable},
    {"bind.touchpad", QT_TRANSLATE_NOOP("EditorPageItem", "Touchpad"), FieldType::Binding, nullptr, false, Editable},
    {"bind.left-stick-left", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick left"), FieldType::Binding, nullptr, false, Editable},
    {"bind.left-stick-right", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick right"), FieldType::Binding, nullptr, false, Editable},
    {"bind.left-stick-up", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick up"), FieldType::Binding, nullptr, false, Editable},
    {"bind.left-stick-down", QT_TRANSLATE_NOOP("EditorPageItem", "Left stick down"), FieldType::Binding, nullptr, false, Editable},
    {"bind.right-stick-left", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick left"), FieldType::Binding, nullptr, false, Editable},
    {"bind.right-stick-right", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick right"), FieldType::Binding, nullptr, false, Editable},
    {"bind.right-stick-up", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick up"), FieldType::Binding, nullptr, false, Editable},
    {"bind.right-stick-down", QT_TRANSLATE_NOOP("EditorPageItem", "Right stick down"), FieldType::Binding, nullptr, false, Editable},
};

struct Help
{
	const char* key;
	const char* text;
};

const Help HELP[] = {
    {"basedir", QT_TRANSLATE_NOOP("EditorPageItem", "The folder that holds the game's files.")},
    {"custom_settings", QT_TRANSLATE_NOOP("EditorPageItem", "Off runs the game with the emulator settings from Settings.")},
    {"screen_width", QT_TRANSLATE_NOOP("EditorPageItem", "The game window's size. It does not change the resolution the game renders at.")},
    {"graphics_preset", QT_TRANSLATE_NOOP("EditorPageItem", "Fills the graphics fields below. The preset itself is not saved.")},
    {"render_resolution_mode",
	 QT_TRANSLATE_NOOP("EditorPageItem",
	                   "Native renders at the game's own size. Fixed renders at most at the size below, keeping the shape.")},
    {"render_resolution_scope",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Render targets also scales the game's full-screen render textures; not every game supports it.")},
    {"presentation_filter", QT_TRANSLATE_NOOP("EditorPageItem", "How the finished picture is scaled to the window.")},
    {"texture_anisotropy", QT_TRANSLATE_NOOP("EditorPageItem", "Sharper textures seen at a slant. It does not change the resolution.")},
    {"vulkan_validation_enabled",
	 QT_TRANSLATE_NOOP("EditorPageItem", "Checks every graphics call for mistakes. For debugging; games run much slower.")},
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
    {"input_profile", QT_TRANSLATE_NOOP("EditorPageItem", "Every game uses the profile chosen here. Default is the emulator's own layout and can't be changed; duplicate it to make a profile of your own.")},
    {"input_device", QT_TRANSLATE_NOOP("EditorPageItem", "Any controller uses the first one connected. Choosing one keeps the game on it while it is connected.")},
    {"left_stick", QT_TRANSLATE_NOOP("EditorPageItem", "Move the stick: the outer dot is where it is, the filled dot what the game sees after the dead zone. On this page the left stick does not move between settings; the D-pad does.")},
    {"left_deadzone", QT_TRANSLATE_NOOP("EditorPageItem", "Movements smaller than this are ignored, so a worn stick does not drift.")},
    {"trigger_deadzone", QT_TRANSLATE_NOOP("EditorPageItem", "How far L2 and R2 must be pulled to count as pressed.")},
    {"mouse_look", QT_TRANSLATE_NOOP("EditorPageItem", "In the game, F3 captures the mouse; while it is captured, moving it moves the right stick.")},
    {"bind.cross", QT_TRANSLATE_NOOP("EditorPageItem", "Choose a box, then press the key or button to use. Delete or □ clears it.")},
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
		case FieldGroup::Shaders: return SHADER_FIELDS;
		case FieldGroup::Logs: return LOG_FIELDS;
		case FieldGroup::Modules: return MODULE_FIELDS;
		case FieldGroup::Program: return PROGRAM_FIELDS;
		case FieldGroup::Interface: return INTERFACE_FIELDS;
		case FieldGroup::Controls: return CONTROL_FIELDS;
	}
	return GAME_FIELDS;
}
