#ifndef LAUNCHER_INCLUDE_CONFIGURATION_H_
#define LAUNCHER_INCLUDE_CONFIGURATION_H_

#include "Common.h"

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

	enum class PresentationFilter
	{
		Linear,
		Nearest
	};
	Q_ENUM(PresentationFilter)

	Configuration() = default;

	QString name;
	QString basedir;    /* Game base directory */
	QString param_file; /* Path to param.sfo / param.json */

	// Screen / window
	int screen_width  = 1280;
	int screen_height = 720;

	// Backward compat: kept for migration from old configs
	Resolution screen_resolution = Resolution::R1280X720;

	// Render resolution (docs/render-resolution.md)
	RenderResolutionMode render_resolution_mode   = RenderResolutionMode::Fixed;
	int                  render_resolution_width  = 1280;
	int                  render_resolution_height = 720;
	PresentationFilter   presentation_filter      = PresentationFilter::Linear;

	// Platform / validation
	bool                   neo                         = true;
	bool                   vulkan_validation_enabled   = false;
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
		KYTY_CFG_SET(basedir);
		KYTY_CFG_SET(param_file);
		s->setValue("screen_width", screen_width);
		s->setValue("screen_height", screen_height);
		KYTY_CFG_SET(render_resolution_mode);
		s->setValue("render_resolution_width", render_resolution_width);
		s->setValue("render_resolution_height", render_resolution_height);
		KYTY_CFG_SET(presentation_filter);
		KYTY_CFG_SET(neo);
		KYTY_CFG_SET(vulkan_validation_enabled);
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
		KYTY_CFG_GET(basedir);
		KYTY_CFG_GET(param_file);
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
		KYTY_CFG_GET(neo);
		KYTY_CFG_GET(vulkan_validation_enabled);
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
