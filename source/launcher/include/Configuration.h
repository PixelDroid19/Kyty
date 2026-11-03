#ifndef LAUNCHER_INCLUDE_CONFIGURATION_H_
#define LAUNCHER_INCLUDE_CONFIGURATION_H_

#include "Common.h"

#include <algorithm>

#include <QByteArray>
#include <QChar>
#include <QMetaEnum>
#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

#define KYTY_CFG_SETL(n) s->setValue(#n, n);
#define KYTY_CFG_SET(n)  s->setValue(#n, QVariant::fromValue(n).toString());
#define KYTY_CFG_GET(n)  n = s->value(#n).value<decltype(n)>();
#define KYTY_CFG_GETL(n) n = s->value(#n).toStringList();


template <class T>
inline QStringList EnumToList()
{
	QStringList ret;
	auto        me    = QMetaEnum::fromType<T>();
	int         count = me.keyCount();
	for (int i = 0; i < count; i++)
	{
		auto key = QString(me.key(i));
		ret << (key.startsWith('R') && key.size() > 2 && key.at(1).isDigit() ? key.remove('R').toLower() : key);
	}
	return ret;
}

template <class T>
T TextToEnum(const QString& text)
{
	auto me = QMetaEnum::fromType<T>();
	return static_cast<T>(me.keyToValue(((text.size() > 1 && text.at(0).isDigit()) ? 'R' + text.toUpper() : text).toUtf8().data()));
}

template <class T>
QString EnumToText(T value)
{
	auto me  = QMetaEnum::fromType<T>();
	auto key = QString(me.valueToKey(static_cast<int>(value)));
	return (key.startsWith('R') && key.size() > 2 && key.at(1).isDigit() ? key.remove('R').toLower() : key);
}

namespace Kyty {

class Configuration: public QObject
{
	Q_OBJECT

public:
	enum class Resolution
	{
		R1280X720,
		R1920X1080,
	};
	Q_ENUM(Resolution)

	enum class ShaderOptimizationType
	{
		None,
		Size,
		Performance
	};
	Q_ENUM(ShaderOptimizationType)

	enum class ShaderLogDirection
	{
		Silent,
		Console,
		File
	};
	Q_ENUM(ShaderLogDirection)

	enum class ProfilerDirection
	{
		None,
		File,
		Network,
		FileAndNetwork
	};
	Q_ENUM(ProfilerDirection)

	enum class LogDirection
	{
		Silent,
		Console,
		File,
		Directory
	};
	Q_ENUM(LogDirection)

	enum class LogLevel
	{
		Debug,
		Info,
		Warn,
		Error
	};
	Q_ENUM(LogLevel)

	enum class RenderResolutionMode
	{
		Native,
		Fixed
	};
	Q_ENUM(RenderResolutionMode)

	enum class RenderResolutionScope
	{
		Display,
		RenderTargets
	};
	Q_ENUM(RenderResolutionScope)

	enum class PresentationFilter
	{
		Linear,
		Nearest,
		Fsr,
		Fsr3
	};
	Q_ENUM(PresentationFilter)

	enum class PresentationScaling
	{
		Stretch,
		Fit,
		Integer
	};
	Q_ENUM(PresentationScaling)

	enum class PresentMode
	{
		Mailbox,
		Immediate,
		Fifo
	};
	Q_ENUM(PresentMode)

	// Guest keeps each sampler's own ratio; a level raises linear-filtered samplers to it.
	enum class TextureAnisotropy
	{
		Guest,
		X2,
		X4,
		X8,
		X16
	};
	Q_ENUM(TextureAnisotropy)

	// Dialog shortcuts that fill the graphics fields; not stored.
	enum class GraphicsPreset
	{
		Custom,
		Native,
		Quality,
		Balanced,
		Performance
	};
	Q_ENUM(GraphicsPreset)

	Configuration() = default;

	QString name;
	QStringList sections;   /* Library sections the game is shown in */
	QString basedir;    /* Game base directory */
	QString param_file; /* Path to param.sfo / param.json */
	QString game_patch_file; /* Path to a mods file applied at boot, empty for none */
	QString addcont_folder; /* Folder holding <TITLE_ID>/ add-on packages, empty for none */
	QString environment; /* NAME=value pairs separated by ';' added to the emulator's environment */
	qint64  last_run = 0; /* Milliseconds since epoch of the last run, 0 when never run */
	qint64  play_time       = 0; /* Seconds the game has run when started from the library */
	bool    favorite        = false;
	bool    custom_settings = false; /* False runs the game with the global emulator settings */

	// Screen / window
	int screen_width  = 1280;
	int screen_height = 720;

	// Backward compat: kept for migration from old configs
	Resolution screen_resolution = Resolution::R1280X720;

	// Render resolution (docs/render-resolution.md)
	RenderResolutionMode  render_resolution_mode     = RenderResolutionMode::Fixed;
	RenderResolutionScope render_resolution_scope    = RenderResolutionScope::Display;
	int                   render_resolution_width    = 1280;
	int                   render_resolution_height   = 720;
	PresentationFilter    presentation_filter        = PresentationFilter::Linear;
	PresentationScaling   presentation_scaling       = PresentationScaling::Stretch;
	int                   presentation_fsr_sharpness = 90;
	TextureAnisotropy     texture_anisotropy         = TextureAnisotropy::Guest;

	// Host display
	QString     gpu_device; /* Vulkan device name, empty for the automatic choice */
	PresentMode present_mode         = PresentMode::Mailbox;
	int         present_fps_limit    = 0; /* 0 leaves presents unpaced */
	bool        fullscreen_enabled   = false;
	bool        hide_cursor_enabled  = false;
	bool        perf_overlay_enabled = false;

	// What games see of the console
	int     console_language           = 1; /* SYSTEM_PARAM_ID_LANG value, 1 is English (United States) */
	QString user_name                  = "Kyty";
	bool    notice_screen_skip_enabled = false;
	bool    network_connected          = true;
	bool    psn_signed_in              = false;
	int     controller_vibration       = 100;
	QString controller_light_bar       = "Guest"; /* A colour name from ControllerLightBarColors(), or Guest */
	int     controller_speaker_volume  = 0;
	bool    microphone_enabled         = false;

	// Platform / validation
	bool                   neo                         = true;
	bool                   shader_cache_enabled        = true;
	bool                   vulkan_validation_enabled   = false;
	bool                   renderdoc_capture_enabled   = false;
	bool                   shader_validation_enabled   = false;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	QString                shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	QString                command_buffer_dump_folder  = "_Buffers";
	LogDirection           printf_direction            = LogDirection::Silent;
	LogLevel               printf_level                = LogLevel::Info;
	QString                printf_output_file          = "_kyty.txt";
	QString                printf_output_folder        = "_Logs";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	QString                profiler_output_file        = "_profile.prof";
	bool                   spirv_debug_printf_enabled  = false;
	bool                   pipeline_dump_enabled       = false;
	QString                pipeline_dump_folder        = "_Pipelines";

	QStringList elfs;
	QStringList elfs_selected;

	void WriteSettings(QSettings* s) const
	{
		KYTY_CFG_SET(name);
		KYTY_CFG_SETL(sections);
		KYTY_CFG_SET(basedir);
		KYTY_CFG_SET(param_file);
		KYTY_CFG_SET(game_patch_file);
		KYTY_CFG_SET(addcont_folder);
		KYTY_CFG_SET(environment);
		s->setValue("last_run", last_run);
		s->setValue("play_time", play_time);
		s->setValue("favorite", favorite);
		s->setValue("custom_settings", custom_settings);
		s->setValue("screen_width", screen_width);
		s->setValue("screen_height", screen_height);
		KYTY_CFG_SET(render_resolution_mode);
		KYTY_CFG_SET(render_resolution_scope);
		s->setValue("render_resolution_width", render_resolution_width);
		s->setValue("render_resolution_height", render_resolution_height);
		KYTY_CFG_SET(presentation_filter);
		KYTY_CFG_SET(presentation_scaling);
		s->setValue("presentation_fsr_sharpness", presentation_fsr_sharpness);
		KYTY_CFG_SET(texture_anisotropy);
		KYTY_CFG_SET(gpu_device);
		KYTY_CFG_SET(present_mode);
		s->setValue("present_fps_limit", present_fps_limit);
		s->setValue("fullscreen_enabled", fullscreen_enabled);
		s->setValue("hide_cursor_enabled", hide_cursor_enabled);
		s->setValue("perf_overlay_enabled", perf_overlay_enabled);
		s->setValue("console_language", console_language);
		KYTY_CFG_SET(user_name);
		s->setValue("notice_screen_skip_enabled", notice_screen_skip_enabled);
		s->setValue("network_connected", network_connected);
		s->setValue("psn_signed_in", psn_signed_in);
		s->setValue("controller_vibration", controller_vibration);
		KYTY_CFG_SET(controller_light_bar);
		s->setValue("controller_speaker_volume", controller_speaker_volume);
		s->setValue("microphone_enabled", microphone_enabled);
		s->setValue("shader_cache_enabled", shader_cache_enabled);
		KYTY_CFG_SET(neo);
		KYTY_CFG_SET(vulkan_validation_enabled);
		KYTY_CFG_SET(renderdoc_capture_enabled);
		KYTY_CFG_SET(shader_validation_enabled);
		KYTY_CFG_SET(shader_optimization_type);
		KYTY_CFG_SET(shader_log_direction);
		KYTY_CFG_SET(shader_log_folder);
		KYTY_CFG_SET(command_buffer_dump_enabled);
		KYTY_CFG_SET(command_buffer_dump_folder);
		KYTY_CFG_SET(printf_direction);
		KYTY_CFG_SET(printf_level);
		KYTY_CFG_SET(printf_output_file);
		KYTY_CFG_SET(printf_output_folder);
		KYTY_CFG_SET(profiler_direction);
		KYTY_CFG_SET(profiler_output_file);
		KYTY_CFG_SET(spirv_debug_printf_enabled);
		KYTY_CFG_SET(pipeline_dump_enabled);
		KYTY_CFG_SET(pipeline_dump_folder);
		KYTY_CFG_SETL(elfs);
		KYTY_CFG_SETL(elfs_selected);
	}

	void ReadSettings(QSettings* s)
	{
		KYTY_CFG_GET(name);
		KYTY_CFG_GETL(sections);
		last_run = s->value("last_run", 0).toLongLong();
		play_time       = s->value("play_time", 0).toLongLong();
		favorite        = s->value("favorite", false).toBool();
		custom_settings = s->value("custom_settings", false).toBool();
		KYTY_CFG_GET(basedir);
		KYTY_CFG_GET(param_file);
		KYTY_CFG_GET(game_patch_file);
		KYTY_CFG_GET(addcont_folder);
		KYTY_CFG_GET(environment);
		if (s->contains("screen_width"))
		{
			screen_width = s->value("screen_width").toInt();
		}
		if (s->contains("screen_height"))
		{
			screen_height = s->value("screen_height").toInt();
		}
		// Migrate legacy screen_resolution enum if present and no explicit width/height
		if (s->contains("screen_resolution") && !s->contains("screen_width"))
		{
			Resolution res = s->value("screen_resolution").value<Resolution>();
			if (res == Resolution::R1920X1080)
			{
				screen_width  = 1920;
				screen_height = 1080;
			} else
			{
				screen_width  = 1280;
				screen_height = 720;
			}
		}
		if (s->contains("render_resolution_mode"))
		{
			KYTY_CFG_GET(render_resolution_mode);
		}
		if (s->contains("render_resolution_scope"))
		{
			KYTY_CFG_GET(render_resolution_scope);
		}
		if (s->contains("render_resolution_width"))
		{
			render_resolution_width = s->value("render_resolution_width").toInt();
		}
		if (s->contains("render_resolution_height"))
		{
			render_resolution_height = s->value("render_resolution_height").toInt();
		}
		if (s->contains("presentation_filter"))
		{
			KYTY_CFG_GET(presentation_filter);
		}
		if (s->contains("presentation_scaling"))
		{
			KYTY_CFG_GET(presentation_scaling);
		}
		if (s->contains("presentation_fsr_sharpness"))
		{
			presentation_fsr_sharpness = std::clamp(s->value("presentation_fsr_sharpness").toInt(), 0, 100);
		}
		if (s->contains("texture_anisotropy"))
		{
			KYTY_CFG_GET(texture_anisotropy);
		}
		gpu_device = s->value("gpu_device").toString();
		if (s->contains("present_mode"))
		{
			KYTY_CFG_GET(present_mode);
		}
		present_fps_limit          = std::clamp(s->value("present_fps_limit", 0).toInt(), 0, 1000);
		fullscreen_enabled         = s->value("fullscreen_enabled", false).toBool();
		hide_cursor_enabled        = s->value("hide_cursor_enabled", false).toBool();
		perf_overlay_enabled       = s->value("perf_overlay_enabled", false).toBool();
		console_language           = std::clamp(s->value("console_language", 1).toInt(), 0, 29);
		user_name                  = s->value("user_name", "Kyty").toString();
		notice_screen_skip_enabled = s->value("notice_screen_skip_enabled", false).toBool();
		network_connected          = s->value("network_connected", true).toBool();
		psn_signed_in              = s->value("psn_signed_in", false).toBool();
		controller_vibration       = std::clamp(s->value("controller_vibration", 100).toInt(), 0, 100);
		controller_light_bar       = s->value("controller_light_bar", "Guest").toString();
		controller_speaker_volume  = std::clamp(s->value("controller_speaker_volume", 0).toInt(), 0, 100);
		microphone_enabled         = s->value("microphone_enabled", false).toBool();
		shader_cache_enabled       = s->value("shader_cache_enabled", true).toBool();
		KYTY_CFG_GET(neo);
		KYTY_CFG_GET(vulkan_validation_enabled);
		KYTY_CFG_GET(renderdoc_capture_enabled);
		KYTY_CFG_GET(shader_validation_enabled);
		KYTY_CFG_GET(shader_optimization_type);
		KYTY_CFG_GET(shader_log_direction);
		KYTY_CFG_GET(shader_log_folder);
		KYTY_CFG_GET(command_buffer_dump_enabled);
		KYTY_CFG_GET(command_buffer_dump_folder);
		KYTY_CFG_GET(printf_direction);
		if (s->contains("printf_level"))
		{
			KYTY_CFG_GET(printf_level);
		}
		KYTY_CFG_GET(printf_output_file);
		if (s->contains("printf_output_folder"))
		{
			KYTY_CFG_GET(printf_output_folder);
		}
		KYTY_CFG_GET(profiler_direction);
		KYTY_CFG_GET(profiler_output_file);
		if (s->contains("spirv_debug_printf_enabled"))
		{
			KYTY_CFG_GET(spirv_debug_printf_enabled);
		}
		if (s->contains("pipeline_dump_enabled"))
		{
			KYTY_CFG_GET(pipeline_dump_enabled);
		}
		if (s->contains("pipeline_dump_folder"))
		{
			KYTY_CFG_GET(pipeline_dump_folder);
		}
		KYTY_CFG_GETL(elfs);
		KYTY_CFG_GETL(elfs_selected);
		// Clamp invalid zero extents to defaults
		if (screen_width <= 0)
		{
			screen_width = 1280;
		}
		if (screen_height <= 0)
		{
			screen_height = 720;
		}
		if (render_resolution_width <= 0)
		{
			render_resolution_width = 1280;
		}
		if (render_resolution_height <= 0)
		{
			render_resolution_height = 720;
		}
	}

	QString GetScreenResolutionText() const
	{
		return QString("%1x%2").arg(screen_width).arg(screen_height);
	}
};

} // namespace Kyty

Q_DECLARE_METATYPE(Kyty::Configuration*)

#endif /* LAUNCHER_INCLUDE_CONFIGURATION_H_ */
