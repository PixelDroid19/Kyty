#include "EditorFields.h"

namespace {

bool Is(const QVariantMap& values, const char* key, const char* value)
{
	return values.value(key).toString() == QLatin1String(value);
}

const QVector<EditorField> GAME_FIELDS = {
    {"name", QT_TRANSLATE_NOOP("EditorPageItem", "Name"), FieldType::Text, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "General")},
    {"folder", QT_TRANSLATE_NOOP("EditorPageItem", "Folder (group)"), FieldType::Text},
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
};

} // namespace

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
	}
	return GAME_FIELDS;
}
