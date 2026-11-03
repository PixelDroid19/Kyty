#include "ConfigurationFields.h"

#include "Configuration.h"
#include "GpuDevices.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <algorithm>

namespace {

using Config = Kyty::Configuration;

QVariantList ElfsToList(const Config& info)
{
	QVariantList list;
	for (const auto& elf: info.elfs)
	{
		QVariantMap item;
		item["path"]     = elf;
		item["selected"] = info.elfs_selected.contains(elf);
		item["present"]  = true;
		list << item;
	}
	return list;
}

// Range of the screen and render sizes the editor offers.
constexpr int MIN_EXTENT = 320;
constexpr int MAX_EXTENT = 8192;

// Fields that say which game runs, as opposed to how the emulator runs it.
const QStringList GAME_IDENTITY_KEYS = {"name",           "folder",      "basedir", "param_file", "game_patch_file",
                                       "addcont_folder", "environment", "elfs",    "custom_settings"};

struct LightBarColor
{
	const char* name;
	const char* hex;
};

const LightBarColor LIGHT_BAR_COLORS[] = {
    {"Guest", ""},          {"Blue", "#0000ff"},   {"Red", "#ff0000"},    {"Green", "#00ff00"},
    {"Pink", "#ff3399"},    {"Violet", "#8000ff"}, {"Orange", "#ff6600"}, {"White", "#ffffff"},
};

// Output paths that must be filled when their option is enabled.
bool RequiredPathsFilled(const QVariantMap& f)
{
	const auto text = [&f](const char* key) { return f.value(key).toString().trimmed(); };

	const auto printf_direction = TextToEnum<Config::LogDirection>(text("printf_direction"));
	if (printf_direction == Config::LogDirection::File && text("printf_output_file").isEmpty())
	{
		return false;
	}
	if (printf_direction == Config::LogDirection::Directory && text("printf_output_folder").isEmpty())
	{
		return false;
	}
	if (TextToEnum<Config::ShaderLogDirection>(text("shader_log_direction")) == Config::ShaderLogDirection::File &&
	    text("shader_log_folder").isEmpty())
	{
		return false;
	}
	if (f.value("command_buffer_dump_enabled").toBool() && text("command_buffer_dump_folder").isEmpty())
	{
		return false;
	}
	const auto profiler = TextToEnum<Config::ProfilerDirection>(text("profiler_direction"));
	if ((profiler == Config::ProfilerDirection::File || profiler == Config::ProfilerDirection::FileAndNetwork) &&
	    text("profiler_output_file").isEmpty())
	{
		return false;
	}
	if (f.value("pipeline_dump_enabled").toBool() && text("pipeline_dump_folder").isEmpty())
	{
		return false;
	}
	return true;
}

bool SharpnessValid(const QVariantMap& f)
{
	bool       ok        = false;
	const auto sharpness = f.value("presentation_fsr_sharpness").toInt(&ok);
	return ok && sharpness >= 0 && sharpness <= 100;
}

// The guest's name buffers hold 16 bytes and a terminator.
bool UserNameValid(const QVariantMap& f)
{
	const auto size = f.value("user_name").toString().trimmed().toUtf8().size();
	return size >= 1 && size <= 16;
}

bool NumberInRange(const QVariantMap& f, const char* key, int low, int high)
{
	bool       ok    = false;
	const auto value = f.value(key).toInt(&ok);
	return ok && value >= low && value <= high;
}

bool ResolutionsValid(const QVariantMap& f)
{
	return f.value("screen_width").toInt() > 0 && f.value("screen_height").toInt() > 0 && f.value("render_resolution_width").toInt() > 0 &&
	       f.value("render_resolution_height").toInt() > 0;
}

} // namespace

QVariantMap ConfigurationToFields(const Kyty::Configuration& info)
{
	QVariantMap f;
	f["name"]                         = info.name;
	f["basedir"]                      = info.basedir;
	f["param_file"]                   = info.param_file;
	f["game_patch_file"]              = info.game_patch_file;
	f["addcont_folder"]               = info.addcont_folder;
	f["environment"]                  = info.environment;
	f["custom_settings"]              = info.custom_settings;
	f["screen_width"]                 = info.screen_width;
	f["screen_height"]                = info.screen_height;
	f["render_resolution_mode"]       = EnumToText(info.render_resolution_mode);
	f["render_resolution_width"]      = info.render_resolution_width;
	f["render_resolution_height"]     = info.render_resolution_height;
	f["render_resolution_scope"]      = EnumToText(info.render_resolution_scope);
	f["presentation_filter"]          = EnumToText(info.presentation_filter);
	f["presentation_scaling"]         = EnumToText(info.presentation_scaling);
	f["presentation_fsr_sharpness"]   = info.presentation_fsr_sharpness;
	f["texture_anisotropy"]           = EnumToText(info.texture_anisotropy);
	// Presets are not stored, so a loaded configuration always starts as custom.
	f["graphics_preset"] = EnumToText(Config::GraphicsPreset::Custom);
	f["gpu_device"]                   = info.gpu_device.isEmpty() ? QString(AUTOMATIC_GPU) : info.gpu_device;
	f["present_mode"]                 = EnumToText(info.present_mode);
	f["present_fps_limit"]            = info.present_fps_limit;
	f["fullscreen_enabled"]           = info.fullscreen_enabled;
	f["hide_cursor_enabled"]          = info.hide_cursor_enabled;
	f["perf_overlay_enabled"]         = info.perf_overlay_enabled;
	f["console_language"]             = QString::number(info.console_language);
	f["user_name"]                    = info.user_name;
	f["notice_screen_skip_enabled"]   = info.notice_screen_skip_enabled;
	f["network_connected"]            = info.network_connected;
	f["psn_signed_in"]                = info.psn_signed_in;
	f["controller_vibration"]         = info.controller_vibration;
	f["controller_light_bar"]         = info.controller_light_bar;
	f["controller_speaker_volume"]    = info.controller_speaker_volume;
	f["microphone_enabled"]           = info.microphone_enabled;
	f["shader_cache_enabled"]         = info.shader_cache_enabled;
	f["neo"]                          = info.neo;
	f["vulkan_validation_enabled"]    = info.vulkan_validation_enabled;
	f["renderdoc_capture_enabled"]    = info.renderdoc_capture_enabled;
	f["shader_validation_enabled"]    = info.shader_validation_enabled;
	f["shader_optimization_type"]     = EnumToText(info.shader_optimization_type);
	f["shader_log_direction"]         = EnumToText(info.shader_log_direction);
	f["shader_log_folder"]            = info.shader_log_folder;
	f["command_buffer_dump_enabled"]  = info.command_buffer_dump_enabled;
	f["command_buffer_dump_folder"]   = info.command_buffer_dump_folder;
	f["printf_direction"]             = EnumToText(info.printf_direction);
	f["printf_level"]                 = EnumToText(info.printf_level);
	f["printf_output_file"]           = info.printf_output_file;
	f["printf_output_folder"]         = info.printf_output_folder;
	f["profiler_direction"]           = EnumToText(info.profiler_direction);
	f["profiler_output_file"]         = info.profiler_output_file;
	f["spirv_debug_printf_enabled"]   = info.spirv_debug_printf_enabled;
	f["pipeline_dump_enabled"]        = info.pipeline_dump_enabled;
	f["pipeline_dump_folder"]         = info.pipeline_dump_folder;
	f["elfs"]                         = ElfsToList(info);
	return f;
}

QString ApplyEmulatorFields(const QVariantMap& fields, Kyty::Configuration* info)
{
	if (!RequiredPathsFilled(fields))
	{
		return QCoreApplication::translate("Launcher", "Please fill all mandatory fields");
	}
	if (!ResolutionsValid(fields))
	{
		return QCoreApplication::translate("Launcher", "Resolution extents must be > 0");
	}
	if (!SharpnessValid(fields))
	{
		return QCoreApplication::translate("Launcher", "FSR sharpness must be between 0 and 100");
	}

	if (!NumberInRange(fields, "present_fps_limit", 0, 1000))
	{
		return QCoreApplication::translate("Launcher", "The frame limit must be between 0 and 1000");
	}
	if (!UserNameValid(fields))
	{
		return QCoreApplication::translate("Launcher", "The user name must have 1 to 16 bytes");
	}
	if (!NumberInRange(fields, "controller_vibration", 0, 100))
	{
		return QCoreApplication::translate("Launcher", "Vibration must be between 0 and 100");
	}
	if (!NumberInRange(fields, "controller_speaker_volume", 0, 100))
	{
		return QCoreApplication::translate("Launcher", "The controller speaker volume must be between 0 and 100");
	}
	if (!ControllerLightBarNames().contains(fields.value("controller_light_bar").toString()))
	{
		return QCoreApplication::translate("Launcher", "Choose a light bar colour from the list");
	}

	const auto text = [&fields](const char* key) { return fields.value(key).toString(); };

	info->gpu_device                  = text("gpu_device") == QLatin1String(AUTOMATIC_GPU) ? QString() : text("gpu_device");
	info->present_mode                = TextToEnum<Config::PresentMode>(text("present_mode"));
	info->present_fps_limit           = fields.value("present_fps_limit").toInt();
	info->fullscreen_enabled          = fields.value("fullscreen_enabled").toBool();
	info->hide_cursor_enabled         = fields.value("hide_cursor_enabled").toBool();
	info->perf_overlay_enabled        = fields.value("perf_overlay_enabled").toBool();
	info->console_language            = text("console_language").toInt();
	info->user_name                   = text("user_name").trimmed();
	info->notice_screen_skip_enabled  = fields.value("notice_screen_skip_enabled").toBool();
	info->network_connected           = fields.value("network_connected").toBool();
	info->psn_signed_in               = fields.value("psn_signed_in").toBool();
	info->controller_vibration        = fields.value("controller_vibration").toInt();
	info->controller_light_bar        = text("controller_light_bar");
	info->controller_speaker_volume   = fields.value("controller_speaker_volume").toInt();
	info->microphone_enabled          = fields.value("microphone_enabled").toBool();
	info->shader_cache_enabled        = fields.value("shader_cache_enabled").toBool();
	info->screen_width                = fields.value("screen_width").toInt();
	info->screen_height               = fields.value("screen_height").toInt();
	info->render_resolution_mode      = TextToEnum<Config::RenderResolutionMode>(text("render_resolution_mode"));
	info->render_resolution_width     = fields.value("render_resolution_width").toInt();
	info->render_resolution_height    = fields.value("render_resolution_height").toInt();
	info->render_resolution_scope     = TextToEnum<Config::RenderResolutionScope>(text("render_resolution_scope"));
	info->presentation_filter         = TextToEnum<Config::PresentationFilter>(text("presentation_filter"));
	info->presentation_scaling        = TextToEnum<Config::PresentationScaling>(text("presentation_scaling"));
	info->presentation_fsr_sharpness  = fields.value("presentation_fsr_sharpness").toInt();
	info->texture_anisotropy          = TextToEnum<Config::TextureAnisotropy>(text("texture_anisotropy"));
	info->neo                         = fields.value("neo").toBool();
	info->vulkan_validation_enabled   = fields.value("vulkan_validation_enabled").toBool();
	info->renderdoc_capture_enabled   = fields.value("renderdoc_capture_enabled").toBool();
	info->shader_validation_enabled   = fields.value("shader_validation_enabled").toBool();
	info->shader_optimization_type    = TextToEnum<Config::ShaderOptimizationType>(text("shader_optimization_type"));
	info->shader_log_direction        = TextToEnum<Config::ShaderLogDirection>(text("shader_log_direction"));
	info->shader_log_folder           = text("shader_log_folder");
	info->command_buffer_dump_enabled = fields.value("command_buffer_dump_enabled").toBool();
	info->command_buffer_dump_folder  = text("command_buffer_dump_folder");
	info->printf_direction            = TextToEnum<Config::LogDirection>(text("printf_direction"));
	info->printf_level                = TextToEnum<Config::LogLevel>(text("printf_level"));
	info->printf_output_file          = text("printf_output_file");
	info->printf_output_folder        = text("printf_output_folder");
	info->profiler_direction          = TextToEnum<Config::ProfilerDirection>(text("profiler_direction"));
	info->profiler_output_file        = text("profiler_output_file");
	info->spirv_debug_printf_enabled  = fields.value("spirv_debug_printf_enabled").toBool();
	info->pipeline_dump_enabled       = fields.value("pipeline_dump_enabled").toBool();
	info->pipeline_dump_folder        = text("pipeline_dump_folder");
	return {};
}

QString ApplyFields(const QVariantMap& fields, Kyty::Configuration* info)
{
	if (fields.value("name").toString().trimmed().isEmpty() || fields.value("basedir").toString().trimmed().isEmpty())
	{
		return QCoreApplication::translate("Launcher", "Please fill all mandatory fields");
	}
	if (!ParseEnvironment(fields.value("environment").toString(), nullptr))
	{
		return QCoreApplication::translate("Launcher", "Environment variables must be NAME=value pairs separated by ';'");
	}
	if (const auto error = ApplyEmulatorFields(fields, info); !error.isEmpty())
	{
		return error;
	}

	info->name            = fields.value("name").toString().trimmed();
	info->basedir         = fields.value("basedir").toString().trimmed();
	info->param_file      = fields.value("param_file").toString();
	info->game_patch_file = fields.value("game_patch_file").toString().trimmed();
	info->addcont_folder  = fields.value("addcont_folder").toString().trimmed();
	info->environment     = fields.value("environment").toString().trimmed();
	info->custom_settings = fields.value("custom_settings").toBool();

	// Only ELFs that still exist on disk are kept, as the Qt widget editor did.
	info->elfs.clear();
	info->elfs_selected.clear();
	for (const auto& entry: fields.value("elfs").toList())
	{
		const auto item = entry.toMap();
		if (!item.value("present").toBool())
		{
			continue;
		}
		const auto path = item.value("path").toString();
		info->elfs << path;
		if (item.value("selected").toBool())
		{
			info->elfs_selected << path;
		}
	}
	return {};
}

QVariantMap ApplyGraphicsPreset(const QVariantMap& fields, const QString& preset_text)
{
	using Preset = Config::GraphicsPreset;
	using Mode   = Config::RenderResolutionMode;
	using Scope  = Config::RenderResolutionScope;
	using Filter = Config::PresentationFilter;
	using Aniso  = Config::TextureAnisotropy;

	struct Graphics
	{
		Mode   mode;
		Scope  scope;
		int    percent;
		Filter filter;
		Aniso  anisotropy;
	};
	Graphics graphics {};
	switch (TextToEnum<Preset>(preset_text))
	{
		case Preset::Custom: return fields;
		case Preset::Native: graphics = {Mode::Native, Scope::Display, 100, Filter::Linear, Aniso::Guest}; break;
		case Preset::Quality: graphics = {Mode::Fixed, Scope::RenderTargets, 100, Filter::Linear, Aniso::X16}; break;
		case Preset::Balanced: graphics = {Mode::Fixed, Scope::RenderTargets, 75, Filter::Fsr, Aniso::X8}; break;
		case Preset::Performance: graphics = {Mode::Fixed, Scope::RenderTargets, 50, Filter::Fsr, Aniso::Guest}; break;
	}

	// The render size follows the screen size, kept even and inside the editor's range.
	const auto scaled = [&](const char* screen_key)
	{ return std::clamp((fields.value(screen_key).toInt() * graphics.percent / 100) & ~1, MIN_EXTENT, MAX_EXTENT); };

	auto result                        = fields;
	result["render_resolution_mode"]   = EnumToText(graphics.mode);
	result["render_resolution_scope"]  = EnumToText(graphics.scope);
	result["render_resolution_width"]  = scaled("screen_width");
	result["render_resolution_height"] = scaled("screen_height");
	result["presentation_filter"]      = EnumToText(graphics.filter);
	result["texture_anisotropy"]       = EnumToText(graphics.anisotropy);
	return result;
}

QVariantMap FieldsToRun(const QVariantMap& game, const QVariantMap& global)
{
	if (game.value("custom_settings").toBool())
	{
		return game;
	}
	auto fields = global;
	for (const auto& key: GAME_IDENTITY_KEYS)
	{
		fields[key] = game.value(key);
	}
	return fields;
}

QString ConsoleLanguageName(int language)
{
	// In the order of the values games read; each name is written in its own language, with the English name
	// where the host may lack the script's font.
	static const char* const NAMES[CONSOLE_LANGUAGE_COUNT] = {"日本語 (Japanese)",
	                                                          "English (United States)",
	                                                          "Français (France)",
	                                                          "Español (España)",
	                                                          "Deutsch",
	                                                          "Italiano",
	                                                          "Nederlands",
	                                                          "Português (Portugal)",
	                                                          "Русский",
	                                                          "한국어 (Korean)",
	                                                          "繁體中文 (Traditional Chinese)",
	                                                          "简体中文 (Simplified Chinese)",
	                                                          "Suomi",
	                                                          "Svenska",
	                                                          "Dansk",
	                                                          "Norsk",
	                                                          "Polski",
	                                                          "Português (Brasil)",
	                                                          "English (United Kingdom)",
	                                                          "Türkçe",
	                                                          "Español (Latinoamérica)",
	                                                          "العربية (Arabic)",
	                                                          "Français (Canada)",
	                                                          "Čeština",
	                                                          "Magyar",
	                                                          "Ελληνικά",
	                                                          "Română",
	                                                          "ไทย (Thai)",
	                                                          "Tiếng Việt",
	                                                          "Bahasa Indonesia"};
	return language >= 0 && language < CONSOLE_LANGUAGE_COUNT ? QString::fromUtf8(NAMES[language]) : QString();
}

QStringList ControllerLightBarNames()
{
	QStringList names;
	for (const auto& color: LIGHT_BAR_COLORS)
	{
		names << color.name;
	}
	return names;
}

QString ControllerLightBarHex(const QString& name)
{
	for (const auto& color: LIGHT_BAR_COLORS)
	{
		if (name == QLatin1String(color.name))
		{
			return color.hex;
		}
	}
	return {};
}

bool ParseEnvironment(const QString& text, QList<QPair<QString, QString>>* pairs)
{
	static const QRegularExpression PAIR(R"(^([A-Za-z_][A-Za-z0-9_]*)=(.*)$)");
	for (const auto& entry: text.split(';', Qt::SkipEmptyParts))
	{
		const auto trimmed = entry.trimmed();
		if (trimmed.isEmpty())
		{
			continue;
		}
		const auto match = PAIR.match(trimmed);
		if (!match.hasMatch())
		{
			return false;
		}
		if (pairs != nullptr)
		{
			pairs->append({match.captured(1), match.captured(2)});
		}
	}
	return true;
}

QVariantMap ConfigurationChoices()
{
	QVariantMap choices;
	QStringList languages;
	for (int language = 0; language < CONSOLE_LANGUAGE_COUNT; language++)
	{
		languages << QString::number(language);
	}
	choices["console_language"]          = languages;
	choices["gpu_device"]                = QStringList {AUTOMATIC_GPU} + GpuDeviceNames();
	choices["present_mode"]              = EnumToList<Config::PresentMode>();
	choices["render_resolution_mode"]    = EnumToList<Config::RenderResolutionMode>();
	choices["render_resolution_scope"]   = EnumToList<Config::RenderResolutionScope>();
	choices["presentation_filter"]       = EnumToList<Config::PresentationFilter>();
	choices["presentation_scaling"]      = EnumToList<Config::PresentationScaling>();
	choices["controller_light_bar"]      = ControllerLightBarNames();
	choices["texture_anisotropy"]        = EnumToList<Config::TextureAnisotropy>();
	choices["graphics_preset"]           = EnumToList<Config::GraphicsPreset>();
	choices["shader_optimization_type"]  = EnumToList<Config::ShaderOptimizationType>();
	choices["shader_log_direction"]      = EnumToList<Config::ShaderLogDirection>();
	choices["printf_direction"]          = EnumToList<Config::LogDirection>();
	choices["printf_level"]              = EnumToList<Config::LogLevel>();
	choices["profiler_direction"]        = EnumToList<Config::ProfilerDirection>();
	return choices;
}
