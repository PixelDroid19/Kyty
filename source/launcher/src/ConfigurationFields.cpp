#include "ConfigurationFields.h"

#include "Configuration.h"

#include <QCoreApplication>
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
const QStringList GAME_IDENTITY_KEYS = {"name", "folder", "basedir", "param_file", "elfs", "custom_settings"};

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
	f["folder"]                       = info.folder;
	f["basedir"]                      = info.basedir;
	f["param_file"]                   = info.param_file;
	f["custom_settings"]              = info.custom_settings;
	f["screen_width"]                 = info.screen_width;
	f["screen_height"]                = info.screen_height;
	f["render_resolution_mode"]       = EnumToText(info.render_resolution_mode);
	f["render_resolution_width"]      = info.render_resolution_width;
	f["render_resolution_height"]     = info.render_resolution_height;
	f["render_resolution_scope"]      = EnumToText(info.render_resolution_scope);
	f["presentation_filter"]          = EnumToText(info.presentation_filter);
	f["presentation_fsr_sharpness"]   = info.presentation_fsr_sharpness;
	f["texture_anisotropy"]           = EnumToText(info.texture_anisotropy);
	// Presets are not stored, so a loaded configuration always starts as custom.
	f["graphics_preset"] = EnumToText(Config::GraphicsPreset::Custom);
	f["neo"]                          = info.neo;
	f["vulkan_validation_enabled"]    = info.vulkan_validation_enabled;
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

	const auto text = [&fields](const char* key) { return fields.value(key).toString(); };

	info->screen_width                = fields.value("screen_width").toInt();
	info->screen_height               = fields.value("screen_height").toInt();
	info->render_resolution_mode      = TextToEnum<Config::RenderResolutionMode>(text("render_resolution_mode"));
	info->render_resolution_width     = fields.value("render_resolution_width").toInt();
	info->render_resolution_height    = fields.value("render_resolution_height").toInt();
	info->render_resolution_scope     = TextToEnum<Config::RenderResolutionScope>(text("render_resolution_scope"));
	info->presentation_filter         = TextToEnum<Config::PresentationFilter>(text("presentation_filter"));
	info->presentation_fsr_sharpness  = fields.value("presentation_fsr_sharpness").toInt();
	info->texture_anisotropy          = TextToEnum<Config::TextureAnisotropy>(text("texture_anisotropy"));
	info->neo                         = fields.value("neo").toBool();
	info->vulkan_validation_enabled   = fields.value("vulkan_validation_enabled").toBool();
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
	if (const auto error = ApplyEmulatorFields(fields, info); !error.isEmpty())
	{
		return error;
	}

	info->name            = fields.value("name").toString().trimmed();
	info->folder          = fields.value("folder").toString().trimmed();
	info->basedir         = fields.value("basedir").toString().trimmed();
	info->param_file      = fields.value("param_file").toString();
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

QVariantMap ConfigurationChoices()
{
	QVariantMap choices;
	choices["render_resolution_mode"]    = EnumToList<Config::RenderResolutionMode>();
	choices["render_resolution_scope"]   = EnumToList<Config::RenderResolutionScope>();
	choices["presentation_filter"]       = EnumToList<Config::PresentationFilter>();
	choices["texture_anisotropy"]        = EnumToList<Config::TextureAnisotropy>();
	choices["graphics_preset"]           = EnumToList<Config::GraphicsPreset>();
	choices["shader_optimization_type"]  = EnumToList<Config::ShaderOptimizationType>();
	choices["shader_log_direction"]      = EnumToList<Config::ShaderLogDirection>();
	choices["printf_direction"]          = EnumToList<Config::LogDirection>();
	choices["printf_level"]              = EnumToList<Config::LogLevel>();
	choices["profiler_direction"]        = EnumToList<Config::ProfilerDirection>();
	return choices;
}
