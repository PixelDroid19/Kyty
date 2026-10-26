#include "ThemePack.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

constexpr char THEME_FILE[]  = "theme.ini";
constexpr char THEME_API[]   = "1";
constexpr int  MAX_TEXT      = 64;
constexpr int  MAX_THEMES    = 64;
constexpr int  MAX_INI_BYTES = 64 * 1024;
// A font or a picture larger than this would slow every start for no visible gain.
constexpr qint64 MAX_FILE_BYTES  = 16 * 1024 * 1024;
constexpr qint64 MAX_SOUND_BYTES = 4 * 1024 * 1024;
constexpr int    MAX_IMAGE_SIDE  = 8192;

namespace {

// The sections of an ini file as key = value maps. Lines starting with ; or # are comments.
using IniSections = QHash<QString, QHash<QString, QString>>;

QString ParseIni(const QByteArray& data, IniSections* sections)
{
	QString section;
	int     number = 0;
	for (const auto& raw: QString::fromUtf8(data).split(QLatin1Char('\n')))
	{
		number++;
		const auto line = raw.trimmed();
		if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1Char('#')))
		{
			continue;
		}
		if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
		{
			section = line.mid(1, line.size() - 2).trimmed();
			(*sections)[section];
			continue;
		}
		const auto equals = line.indexOf(QLatin1Char('='));
		if (equals <= 0 || section.isEmpty())
		{
			return QCoreApplication::translate("ThemePack", "Line %1 of theme.ini is not a key = value inside a [section].").arg(number);
		}
		(*sections)[section][line.left(equals).trimmed().toLower()] = line.mid(equals + 1).trimmed();
	}
	return {};
}

// The absolute path of a file the theme names, which must stay inside the theme's folder; error says why not.
QString ThemeFile(const QDir& dir, const QString& name, const QStringList& suffixes, qint64 max_bytes, QString* error)
{
	const QFileInfo info(dir.filePath(name));
	const auto      path = info.canonicalFilePath();
	if (QDir::isAbsolutePath(name) || path.isEmpty() || !info.isFile())
	{
		*error = QCoreApplication::translate("ThemePack", "The file %1 is missing.").arg(name);
		return {};
	}
	if (!path.startsWith(dir.canonicalPath() + QLatin1Char('/')))
	{
		*error = QCoreApplication::translate("ThemePack", "The file %1 is outside the theme's folder.").arg(name);
		return {};
	}
	if (!suffixes.contains(info.suffix().toLower()))
	{
		*error = QCoreApplication::translate("ThemePack", "The file %1 must be one of: %2.").arg(name, suffixes.join(QStringLiteral(", ")));
		return {};
	}
	if (info.size() > max_bytes)
	{
		*error = QCoreApplication::translate("ThemePack", "The file %1 is larger than %2 MB.").arg(name).arg(max_bytes / (1024 * 1024));
		return {};
	}
	return path;
}

QString ReadColors(const QHash<QString, QString>& values, const QString& section, QHash<QString, QColor>* colors)
{
	for (auto it = values.cbegin(); it != values.cend(); ++it)
	{
		if (!ThemePack::ColorKeys().contains(it.key()))
		{
			return QCoreApplication::translate("ThemePack", "[%1] has an unknown colour %2.").arg(section, it.key());
		}
		const auto& text  = it.value();
		const auto  color = QColor::fromString(text);
		if (!text.startsWith(QLatin1Char('#')) || (text.size() != 7 && text.size() != 9) || !color.isValid())
		{
			return QCoreApplication::translate("ThemePack", "%1 in [%2] is not a colour like #rrggbb or #aarrggbb.").arg(it.key(), section);
		}
		colors->insert(it.key(), color);
	}
	return {};
}

QString ReadTheme(const QDir& dir, const IniSections& sections, ThemePack* theme)
{
	static const QStringList known_sections = {"Theme", "Dark", "Light", "Sounds"};
	for (auto it = sections.cbegin(); it != sections.cend(); ++it)
	{
		if (!known_sections.contains(it.key()))
		{
			return QCoreApplication::translate("ThemePack", "theme.ini has an unknown section [%1].").arg(it.key());
		}
	}
	const auto info = sections.value("Theme");
	// Names the theme in the settings even when a later check fails.
	if (const auto name = info.value("name"); !name.isEmpty())
	{
		theme->name = name.left(MAX_TEXT);
	}
	if (info.value("api") != QLatin1String(THEME_API))
	{
		return QCoreApplication::translate("ThemePack", "The theme is made for another version of the launcher (api %1).").arg(info.value("api"));
	}
	static const QStringList known_keys = {"api", "name", "author", "license", "version", "screenshot", "font", "background", "layout"};
	for (auto it = info.cbegin(); it != info.cend(); ++it)
	{
		if (!known_keys.contains(it.key()))
		{
			return QCoreApplication::translate("ThemePack", "[Theme] has an unknown key %1.").arg(it.key());
		}
		if (it.value().size() > MAX_TEXT && it.key() != QLatin1String("screenshot") && it.key() != QLatin1String("font") &&
		    it.key() != QLatin1String("background"))
		{
			return QCoreApplication::translate("ThemePack", "%1 is longer than %2 characters.").arg(it.key()).arg(MAX_TEXT);
		}
	}
	theme->name    = info.value("name");
	theme->author  = info.value("author");
	theme->license = info.value("license");
	theme->version = info.value("version");
	theme->layout  = info.value("layout");
	if (theme->name.isEmpty())
	{
		return QCoreApplication::translate("ThemePack", "[Theme] has no name.");
	}
	if (!theme->layout.isEmpty() && theme->layout != QLatin1String("Sidebar") && theme->layout != QLatin1String("Console"))
	{
		return QCoreApplication::translate("ThemePack", "layout must be Sidebar or Console.");
	}

	QString error;
	const QStringList images = {"png", "jpg", "jpeg", "webp"};
	const struct
	{
		const char* key;
		QStringList suffixes;
		QString*    path;
	} files[] = {{"screenshot", images, &theme->screenshot}, {"font", {"ttf", "otf"}, &theme->font}, {"background", images, &theme->background}};
	for (const auto& file: files)
	{
		const auto name = info.value(file.key);
		if (!name.isEmpty() && (*file.path = ThemeFile(dir, name, file.suffixes, MAX_FILE_BYTES, &error)).isEmpty())
		{
			return error;
		}
	}
	for (const auto* path: {&theme->screenshot, &theme->background})
	{
		QImageReader reader(*path);
		const auto   size = reader.size();
		if (!path->isEmpty() && (!reader.canRead() || !size.isValid() || size.width() > MAX_IMAGE_SIDE || size.height() > MAX_IMAGE_SIDE))
		{
			return QCoreApplication::translate("ThemePack", "The picture %1 cannot be read or is larger than %2 × %2.").arg(QFileInfo(*path).fileName()).arg(MAX_IMAGE_SIDE);
		}
	}

	if (error = ReadColors(sections.value("Dark"), "Dark", &theme->dark); !error.isEmpty())
	{
		return error;
	}
	if (error = ReadColors(sections.value("Light"), "Light", &theme->light); !error.isEmpty())
	{
		return error;
	}

	const auto sounds = sections.value("Sounds");
	for (auto it = sounds.cbegin(); it != sounds.cend(); ++it)
	{
		if (!ThemePack::SoundKeys().contains(it.key()))
		{
			return QCoreApplication::translate("ThemePack", "[Sounds] has an unknown sound %1.").arg(it.key());
		}
		const auto path = ThemeFile(dir, it.value(), {"wav"}, MAX_SOUND_BYTES, &error);
		if (path.isEmpty())
		{
			return error;
		}
		theme->sounds.insert(it.key(), path);
	}
	return {};
}

} // namespace

const QStringList& ThemePack::ColorKeys()
{
	static const QStringList keys = {"background", "surface",   "panel", "accent",     "text_primary", "text_secondary",
	                                 "text_muted", "text_on_art", "glass", "glass_strong", "glass_border", "scrim",
	                                 "art_edge",   "glow",        "danger", "ok",          "glitch_red",   "glitch_cyan"};
	return keys;
}

const QStringList& ThemePack::SoundKeys()
{
	static const QStringList keys = {"move", "accept", "back", "start"};
	return keys;
}

QString ThemesFolder()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Kyty/themes");
}

ThemePack ReadThemePack(const QString& folder)
{
	ThemePack theme;
	const QDir dir(folder);
	theme.id     = dir.dirName();
	theme.folder = dir.absolutePath();
	theme.name   = theme.id;

	static const QRegularExpression id_pattern(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,63}$"));
	if (!id_pattern.match(theme.id).hasMatch())
	{
		theme.error = QCoreApplication::translate("ThemePack", "The folder name may only hold letters, digits, dots, dashes and underscores.");
		return theme;
	}
	QFile file(dir.filePath(THEME_FILE));
	if (file.size() > MAX_INI_BYTES)
	{
		theme.error = QCoreApplication::translate("ThemePack", "theme.ini is larger than %1 KB.").arg(MAX_INI_BYTES / 1024);
		return theme;
	}
	if (!file.open(QIODevice::ReadOnly))
	{
		theme.error = QCoreApplication::translate("ThemePack", "theme.ini cannot be read.");
		return theme;
	}
	IniSections sections;
	theme.error = ParseIni(file.readAll(), &sections);
	if (theme.error.isEmpty())
	{
		theme.error = ReadTheme(dir, sections, &theme);
	}
	return theme;
}

QVector<ThemePack> FindThemePacks()
{
	QVector<ThemePack> themes;
	const QDir         root(ThemesFolder());
	for (const auto& name: root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
	{
		if (themes.size() >= MAX_THEMES)
		{
			break;
		}
		if (QFile::exists(root.filePath(name + QLatin1Char('/') + THEME_FILE)))
		{
			themes << ReadThemePack(root.filePath(name));
		}
	}
	return themes;
}
