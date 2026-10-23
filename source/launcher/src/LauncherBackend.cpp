#include "LauncherBackend.h"

#include "ArtworkProvider.h"
#include "Configuration.h"
#include "ConfigurationFields.h"
#include "GamepadInput.h"
#include "Psf.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <QUrl>
#include <QVector>
#include <QWindow>

#include <algorithm>
#include <memory>

constexpr char CONF_FILE_NAME[]      = "Kyty.ini";
constexpr char CONF_ORG_NAME[]       = "Kyty";
constexpr char CONF_APP_NAME[]       = "Kyty";
constexpr char SETTINGS_LAUNCHER[]   = "Launcher";
constexpr char SETTINGS_WINDOW[]     = "Window";
constexpr char SETTINGS_GLOBAL[]     = "GlobalConfiguration";
constexpr int  RECENT_COUNT          = 5;
// Upper bound for executables listed from a game directory, so the editor list stays usable.
constexpr int  MAX_LISTED_ELFS       = 512;
// Bounds for importing a folder of games: how deep game folders are looked for, and how much is walked.
constexpr int  IMPORT_DEPTH          = 3;
constexpr int  MAX_IMPORT_DIRS       = 4096;
constexpr int  MAX_IMPORTED_GAMES    = 512;
// The colour scheme and language that follow the desktop's.
constexpr char SYSTEM_CHOICE[] = "System";

namespace {

// Same lookup as the Qt widget launcher: Kyty.ini next to the working directory wins, otherwise the per-user settings.
std::unique_ptr<QSettings> OpenSettings()
{
	QFile file(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	if (file.exists())
	{
		return std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	}
#ifdef __linux__
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME, CONF_APP_NAME);
#else
	return std::make_unique<QSettings>(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME, CONF_APP_NAME);
#endif
}

QString FindParamFile(const QDir& dir)
{
	const QStringList candidates = {"sce_sys/param.sfo", "param.sfo", "sce_sys/param.json", "param.json"};
	for (const auto& name: candidates)
	{
		const auto path = dir.filePath(name);
		if (QFile::exists(path))
		{
			return path;
		}
	}
	return {};
}

bool HasGame(const QDir& dir)
{
	return !FindParamFile(dir).isEmpty() || QFile::exists(dir.filePath("eboot.bin"));
}

// Breadth-first search for game folders below root. A game folder is never entered, so its asset tree is not walked.
QStringList FindGameFolders(const QString& root)
{
	QStringList games;
	QStringList level   = {QDir(root).absolutePath()};
	int         visited = 0;
	for (int depth = 0; depth <= IMPORT_DEPTH && !level.isEmpty(); depth++)
	{
		QStringList next;
		for (const auto& path: level)
		{
			const QDir dir(path);
			if (HasGame(dir))
			{
				games << path;
				if (games.size() >= MAX_IMPORTED_GAMES)
				{
					return games;
				}
				continue;
			}
			if (++visited > MAX_IMPORT_DIRS)
			{
				return games;
			}
			for (const auto& sub: dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
			{
				next << dir.filePath(sub);
			}
		}
		level = next;
	}
	return games;
}

QString FormatPsfValue(const QVariant& value)
{
	if (value.typeId() == QMetaType::UInt)
	{
		return QString("0x%1").arg(value.toUInt(), 8, 16, QLatin1Char('0'));
	}
	return value.toString();
}

LauncherBackend* g_backend = nullptr;

// The look and library settings with their defaults; each is stored under its key in the launcher group.
QVariantMap LookDefaults()
{
	return {{"accent", "Blue"},       {"interface_size", "Normal"}, {"animations", "Full"}, {"background", "Artwork"},
	        {"cover_size", "Medium"}, {"show_titles", true},        {"show_recent", true},  {"on_launch", "Minimize"},
	        {"sort", "Manual"},       {"window_mode", "Windowed"}};
}

// Scale of every size for each interface_size, in the order of its choices.
constexpr qreal INTERFACE_SCALES[] = {0.88, 1, 1.14, 1.3};

} // namespace

LauncherBackend::LauncherBackend(QObject* parent)
    : QObject(parent), m_gamepad(new GamepadInput(this)), m_run_modes(EmulatorRunner::RunModes())
{
	Q_ASSERT(g_backend == nullptr);
	g_backend = this;
	m_import_pool.setMaxThreadCount(1);
	m_import_pool.setThreadPriority(QThread::IdlePriority);

	Load();
	ApplyColorScheme();
	ApplyLanguage();
	ApplyLook();

	connect(&m_runner, &EmulatorRunner::Probed, this, &LauncherBackend::OnProbed);
	connect(&m_runner, &EmulatorRunner::Finished, this, &LauncherBackend::OnFinished);
	connect(m_gamepad, &GamepadInput::ControllerChanged, this, &LauncherBackend::StateChanged);

	m_runner.Probe(m_emulator_override);
}

LauncherBackend::~LauncherBackend()
{
	g_backend = nullptr;
}

LauncherBackend* LauncherBackend::Instance()
{
	Q_ASSERT(g_backend != nullptr);
	return g_backend;
}

void LauncherBackend::AttachWindow(QWindow* window)
{
	m_gamepad->SetWindow(window);
}

QString LauncherBackend::ControllerName() const
{
	return m_gamepad->ControllerName();
}

void LauncherBackend::Load()
{
	const auto settings = OpenSettings();
	m_settings_file     = settings->fileName();

	m_library.Read(settings.get());

	// Missing until the global settings are first saved; the defaults stand until then.
	if (settings->childGroups().contains(SETTINGS_GLOBAL))
	{
		settings->beginGroup(SETTINGS_GLOBAL);
		m_global.ReadSettings(settings.get());
		settings->endGroup();
	}

	settings->beginGroup(SETTINGS_LAUNCHER);
	m_emulator_override  = settings->value("emulator_path").toString();
	m_run_mode           = settings->value("run_mode", EmulatorRunner::DIRECT_MODE).toString();
	m_table_view         = settings->value("table_view", false).toBool();
	m_color_scheme       = settings->value("color_scheme", SYSTEM_CHOICE).toString();
	m_language           = settings->value("language", SYSTEM_CHOICE).toString();
	m_look               = LookDefaults();
	for (auto it = m_look.begin(); it != m_look.end(); ++it)
	{
		// A value edited by hand that the launcher does not know keeps the default.
		const auto value = settings->value(it.key(), it.value());
		if (IsValidLook(it.key(), value))
		{
			*it = it.value().typeId() == QMetaType::Bool ? QVariant(value.toBool()) : QVariant(value.toString());
		}
	}
	settings->endGroup();

	settings->beginGroup(SETTINGS_WINDOW);
	if (settings->contains("width") && settings->contains("height"))
	{
		m_window_geometry = QRect(settings->value("x", 0).toInt(), settings->value("y", 0).toInt(), settings->value("width").toInt(),
		                          settings->value("height").toInt());
	}
	settings->endGroup();
}

void LauncherBackend::Save() const
{
	const auto settings = OpenSettings();

	m_library.Write(settings.get());

	settings->beginGroup(SETTINGS_GLOBAL);
	m_global.WriteSettings(settings.get());
	settings->endGroup();

	settings->beginGroup(SETTINGS_LAUNCHER);
	settings->setValue("emulator_path", m_emulator_override);
	settings->setValue("run_mode", m_run_mode);
	settings->setValue("table_view", m_table_view);
	settings->setValue("color_scheme", m_color_scheme);
	settings->setValue("language", m_language);
	for (auto it = m_look.cbegin(); it != m_look.cend(); ++it)
	{
		settings->setValue(it.key(), it.value());
	}
	settings->endGroup();

	if (m_window_geometry.isValid())
	{
		settings->beginGroup(SETTINGS_WINDOW);
		settings->setValue("x", m_window_geometry.x());
		settings->setValue("y", m_window_geometry.y());
		settings->setValue("width", m_window_geometry.width());
		settings->setValue("height", m_window_geometry.height());
		settings->endGroup();
	}
}

QVariantList LauncherBackend::Games() const
{
	QVariantList list;
	for (int row = 0; row < m_library.Count(); row++)
	{
		list << GameToMap(row);
	}
	return list;
}

QVariantMap LauncherBackend::GameToMap(int row) const
{
	const auto& game = m_library.At(row);
	const auto& info = *game.config;

	QVariantMap item;
	item["row"]      = row;
	item["name"]     = info.name;
	item["folder"]   = info.folder;
	item["title"]    = game.param.title;
	item["serial"]   = game.param.serial;
	item["region"]   = game.param.region;
	item["version"]  = game.param.version;
	item["basedir"]  = info.basedir;
	item["icon"]     = ArtworkProvider::Url(game.icon);
	item["picture"]  = ArtworkProvider::Url(game.picture);
	item["running"]  = row == m_running_row;
	item["lastRun"]  = info.last_run;
	item["playTime"] = info.play_time;
	item["favorite"] = info.favorite;
	return item;
}

QVariantList LauncherBackend::Folders() const
{
	QVariantList list;
	list << FolderToMap(tr("All games"), QString(), true);
	int favorites = 0;
	for (int row = 0; row < m_library.Count(); row++)
	{
		favorites += m_library.At(row).config->favorite ? 1 : 0;
	}
	if (favorites > 0)
	{
		auto item         = FolderToMap(tr("Favorites"), QString(), false);
		item["favorites"] = true;
		item["count"]     = favorites;
		list << item;
	}
	for (const auto& name: m_library.Folders())
	{
		list << FolderToMap(name, name, false);
	}
	bool has_unfiled = false;
	for (int row = 0; row < m_library.Count(); row++)
	{
		has_unfiled = has_unfiled || m_library.At(row).config->folder.isEmpty();
	}
	if (has_unfiled)
	{
		list << FolderToMap(tr("Unfiled"), QString(), false);
	}
	return list;
}

QVariantMap LauncherBackend::FolderToMap(const QString& title, const QString& folder, bool all) const
{
	int count = 0;
	for (int row = 0; row < m_library.Count(); row++)
	{
		if (all || m_library.At(row).config->folder == folder)
		{
			count++;
		}
	}

	QVariantMap item;
	item["title"]  = title;
	item["folder"] = folder;
	item["all"]    = all;
	item["count"]  = count;
	return item;
}

QVariantList LauncherBackend::Recent() const
{
	QVector<int> rows;
	for (int row = 0; row < m_library.Count(); row++)
	{
		if (m_library.At(row).config->last_run > 0)
		{
			rows << row;
		}
	}
	std::sort(rows.begin(), rows.end(),
	          [this](int a, int b) { return m_library.At(a).config->last_run > m_library.At(b).config->last_run; });

	QVariantList list;
	for (int i = 0; i < rows.size() && i < RECENT_COUNT; i++)
	{
		list << GameToMap(rows.at(i));
	}
	return list;
}

QVariantMap LauncherBackend::FieldsFor(int row) const
{
	if (row < 0 || row >= m_library.Count())
	{
		const Kyty::Configuration defaults{};
		return ConfigurationToFields(defaults);
	}
	return ConfigurationToFields(*m_library.At(row).config);
}

QVariantMap LauncherBackend::GlobalFields() const
{
	return ConfigurationToFields(m_global);
}

QString LauncherBackend::SaveGlobal(const QVariantMap& fields)
{
	const auto error = ApplyEmulatorFields(fields, &m_global);
	if (error.isEmpty())
	{
		Save();
		emit StateChanged();
	}
	return error;
}

QVariantMap LauncherBackend::GraphicsPreset(const QVariantMap& fields, const QString& preset) const
{
	return ApplyGraphicsPreset(fields, preset);
}

QVariantMap LauncherBackend::Choices() const
{
	auto choices            = ConfigurationChoices();
	choices["run_mode"]     = m_run_modes;
	choices["color_scheme"] = QStringList {SYSTEM_CHOICE, "Dark", "Light"};
	// Languages with a translation built in, and English, which the texts are written in.
	choices["language"] = QStringList {SYSTEM_CHOICE, "en", "es", "ru"};
	// Accent and animations are listed in the order of Theme's enums.
	choices["accent"]         = QStringList {"Blue", "Violet", "Green", "Orange", "Pink"};
	choices["interface_size"] = QStringList {"Compact", "Normal", "Large", "ExtraLarge"};
	choices["animations"]     = QStringList {"Full", "Reduced", "Off"};
	choices["background"]     = QStringList {"Artwork", "Dimmed", "Plain"};
	choices["cover_size"]     = QStringList {"Small", "Medium", "Large"};
	choices["on_launch"]      = QStringList {"Minimize", "KeepOpen"};
	choices["sort"]           = QStringList {"Manual", "ByName", "LastPlayed", "PlayTime"};
	choices["window_mode"]    = QStringList {"Windowed", "Maximized", "FullScreen"};
	return choices;
}

QVariantMap LauncherBackend::LauncherFields() const
{
	auto fields            = m_look;
	fields["run_mode"]     = m_run_modes.contains(m_run_mode) ? m_run_mode : QString(EmulatorRunner::DIRECT_MODE);
	fields["color_scheme"] = m_color_scheme;
	fields["language"]     = m_language;
	return fields;
}

bool LauncherBackend::IsValidLook(const QString& key, const QVariant& value) const
{
	const auto fallback = LookDefaults().value(key);
	if (!fallback.isValid())
	{
		return false;
	}
	if (fallback.typeId() == QMetaType::Bool)
	{
		return value.canConvert<bool>();
	}
	return Choices().value(key).toStringList().contains(value.toString());
}

qreal LauncherBackend::InterfaceScale() const
{
	const auto index = Choices().value("interface_size").toStringList().indexOf(m_look.value("interface_size").toString());
	return INTERFACE_SCALES[std::max<qsizetype>(0, index)];
}

void LauncherBackend::ApplyLook()
{
	const auto choices = Choices();
	auto*      theme   = Theme::Instance();
	theme->SetAccent(static_cast<Theme::AccentColor>(choices.value("accent").toStringList().indexOf(m_look.value("accent").toString())));
	theme->SetMotion(static_cast<Theme::Motion>(choices.value("animations").toStringList().indexOf(m_look.value("animations").toString())));
}

void LauncherBackend::SetLauncherField(const QString& key, const QVariant& value)
{
	const auto text = value.toString();
	if (key == QLatin1String("run_mode") && m_run_modes.contains(text))
	{
		m_run_mode = text;
	} else if (key == QLatin1String("color_scheme"))
	{
		m_color_scheme = text;
		ApplyColorScheme();
	} else if (key == QLatin1String("language"))
	{
		m_language = text;
		ApplyLanguage();
		emit LanguageChanged();
	} else if (IsValidLook(key, value))
	{
		m_look[key] = m_look.value(key).typeId() == QMetaType::Bool ? QVariant(value.toBool()) : QVariant(text);
		ApplyLook();
		emit AppearanceChanged();
	} else
	{
		qWarning("Unknown launcher setting or value %s", qPrintable(key));
		return;
	}
	Save();
	emit StateChanged();
}

void LauncherBackend::ApplyColorScheme()
{
	auto scheme = Theme::Scheme::System;
	if (m_color_scheme == QLatin1String("Dark"))
	{
		scheme = Theme::Scheme::Dark;
	} else if (m_color_scheme == QLatin1String("Light"))
	{
		scheme = Theme::Scheme::Light;
	}
	Theme::Instance()->SetColorScheme(scheme);
}

void LauncherBackend::ApplyLanguage()
{
	QCoreApplication::removeTranslator(&m_translator);
	const QLocale locale = m_language == QLatin1String(SYSTEM_CHOICE) ? QLocale::system() : QLocale(m_language);
	// English needs no translation; any other language without one stays in English too.
	if (m_translator.load(locale, "launcher", "_", ":/i18n"))
	{
		QCoreApplication::installTranslator(&m_translator);
	}
}

QVariantMap LauncherBackend::SaveGame(int row, const QVariantMap& fields)
{
	QVariantMap result;
	if (IsRunning())
	{
		result["error"] = tr("A game is running. Changes can be saved after it closes.");
		return result;
	}

	if (row < 0)
	{
		auto* info = new Kyty::Configuration;
		const auto error = ApplyFields(fields, info);
		if (!error.isEmpty())
		{
			delete info;
			result["error"] = error;
			return result;
		}
		row = m_library.Add(info);
	} else
	{
		const auto error = ApplyFields(fields, m_library.At(row).config);
		if (!error.isEmpty())
		{
			result["error"] = error;
			return result;
		}
		m_library.Refresh(row);
	}

	Save();
	emit StateChanged();

	result["row"] = row;
	return result;
}

void LauncherBackend::DeleteGame(int row)
{
	if (IsRunning() || row < 0 || row >= m_library.Count())
	{
		return;
	}
	m_library.Remove(row);
	Save();
	emit StateChanged();
}

void LauncherBackend::MoveGame(int row, int delta)
{
	if (IsRunning() || row < 0 || row >= m_library.Count())
	{
		return;
	}
	m_library.Move(row, delta);
	Save();
	emit StateChanged();
}

void LauncherBackend::SetFavorite(int row, bool favorite)
{
	if (row < 0 || row >= m_library.Count())
	{
		return;
	}
	m_library.At(row).config->favorite = favorite;
	Save();
	emit StateChanged();
}

QString LauncherBackend::RunGame(int row)
{
	if (row < 0 || row >= m_library.Count())
	{
		return tr("Select a game first.");
	}
	const auto error = StartGame(ConfigurationToFields(*m_library.At(row).config), row);
	if (error.isEmpty())
	{
		m_library.At(row).config->last_run = QDateTime::currentMSecsSinceEpoch();
		Save();
	}
	return error;
}

QString LauncherBackend::TestGame(const QVariantMap& fields)
{
	return StartGame(fields, -1);
}

QString LauncherBackend::StartGame(const QVariantMap& fields, int row)
{
	if (IsRunning())
	{
		return tr("A game is already running.");
	}
	Kyty::Configuration info;
	if (const auto error = ApplyFields(FieldsToRun(fields, GlobalFields()), &info); !error.isEmpty())
	{
		return error;
	}
	// A mode saved on another host may be missing here; Direct is always available.
	const auto mode  = m_run_modes.contains(m_run_mode) ? m_run_mode : QString(EmulatorRunner::DIRECT_MODE);
	const auto error = m_runner.Start(info, mode);
	if (!error.isEmpty())
	{
		return error;
	}
	m_running_row = row;
	m_run_started = QDateTime::currentMSecsSinceEpoch();
	// The controller is released so the game is its only reader.
	m_gamepad->SetPaused(true);
	emit StateChanged();
	return {};
}

void LauncherBackend::StopGame()
{
	m_runner.Stop();
}

void LauncherBackend::OnFinished(int exit_code, bool crashed)
{
	// A game cannot be deleted or moved while it runs, so its row still names it.
	if (m_running_row >= 0)
	{
		m_library.At(m_running_row).config->play_time += (QDateTime::currentMSecsSinceEpoch() - m_run_started) / 1000;
		Save();
	}
	m_running_row = -1;
	m_gamepad->SetPaused(false);
	emit StateChanged();

	if (crashed)
	{
		emit GameFinished(tr("The game stopped unexpectedly."));
	} else if (exit_code != 0)
	{
		emit GameFinished(tr("The game exited with code %1.").arg(exit_code));
	} else
	{
		emit GameFinished({});
	}
}

void LauncherBackend::OnProbed(bool found, const QString& version, const QString& message)
{
	m_emulator_ready   = found;
	m_emulator_version = version;
	m_emulator_message = message;
	emit StateChanged();
}

QVariantMap LauncherBackend::DetectGame(const QString& folder) const
{
	const Kyty::Configuration defaults{};
	QVariantMap               fields = ConfigurationToFields(defaults);

	const QDir dir(folder);
	const auto param_file = FindParamFile(dir);

	const auto title = ReadParamInfo(param_file).title;

	fields["name"]       = title.isEmpty() ? dir.dirName() : title;
	fields["basedir"]    = dir.absolutePath();
	fields["param_file"] = param_file;

	// The main executable of a dump is eboot.bin; select it when it is present.
	const auto elfs = ScanElfs(folder, {});
	QVariantList selected;
	for (const auto& entry: elfs)
	{
		auto item = entry.toMap();
		item["selected"] = item.value("path").toString() == "eboot.bin";
		selected << item;
	}
	fields["elfs"] = selected;
	return fields;
}

bool LauncherBackend::IsGameFolder(const QString& folder) const
{
	return HasGame(QDir(folder));
}

QString LauncherBackend::ImportFolder(const QString& folder)
{
	if (IsRunning())
	{
		return tr("A game is running. Games can be added after it closes.");
	}
	if (m_importing)
	{
		return tr("Games are still being added from another folder.");
	}
	m_importing = true;

	QSet<QString> listed;
	for (int row = 0; row < m_library.Count(); row++)
	{
		listed.insert(QDir(m_library.At(row).config->basedir).absolutePath());
	}

	// Walking a library on a slow disk takes seconds, so it runs on one idle-priority thread.
	m_import_pool.start(
	    [this, folder, listed]
	    {
		    const auto       found = FindGameFolders(folder);
		    QList<QVariantMap> games;
		    for (const auto& path: found)
		    {
			    if (!listed.contains(path))
			    {
				    games << DetectGame(path);
			    }
		    }
		    QMetaObject::invokeMethod(
		        this, [this, games, count = found.size()] { AddImported(games, count); }, Qt::QueuedConnection);
	    });
	return {};
}

void LauncherBackend::AddImported(const QList<QVariantMap>& games, int found)
{
	m_importing = false;
	if (IsRunning())
	{
		emit FolderImported(0, found, tr("A game started while the folder was read; nothing was added."));
		return;
	}

	int     added = 0;
	QString error;
	for (const auto& fields: games)
	{
		auto*      info  = new Kyty::Configuration;
		const auto issue = ApplyFields(fields, info);
		if (!issue.isEmpty())
		{
			delete info;
			error = QString("%1: %2").arg(fields.value("basedir").toString(), issue);
			continue;
		}
		m_library.Add(info);
		added++;
	}

	if (added > 0)
	{
		Save();
		emit StateChanged();
	}
	emit FolderImported(added, found, error);
}

QVariantList LauncherBackend::ScanElfs(const QString& basedir, const QVariantList& current) const
{
	const QDir dir(basedir);

	QSet<QString> found;
	if (!basedir.isEmpty() && dir.exists())
	{
		// A package keeps its executables in the base directory and one level below it (sce_module), so the
		// asset trees, which can hold hundreds of thousands of files, are never walked.
		QStringList dirs = {dir.absolutePath()};
		for (const auto& sub: dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
		{
			dirs << dir.filePath(sub);
		}
		for (const auto& path: dirs)
		{
			QDirIterator it(path, {"*.elf", "*.prx", "*.sprx", "eboot.bin"}, QDir::Files);
			while (it.hasNext() && found.size() < MAX_LISTED_ELFS)
			{
				found.insert(dir.relativeFilePath(it.next()));
			}
		}
	}

	// Keeps the user's previous selection; entries whose file is gone stay listed as missing, as before.
	QMap<QString, QVariantMap> entries;
	for (const auto& entry: current)
	{
		auto item = entry.toMap();
		const auto path = item.value("path").toString();
		item["present"] = found.contains(path);
		entries.insert(path, item);
	}
	for (const auto& path: found)
	{
		if (!entries.contains(path))
		{
			QVariantMap item;
			item["path"]     = path;
			item["selected"] = false;
			item["present"]  = true;
			entries.insert(path, item);
		}
	}

	QVariantList list;
	for (const auto& item: entries)
	{
		list << item;
	}
	return list;
}

QVariantList LauncherBackend::ParamEntries(const QString& param_file) const
{
	Psf psf;
	psf.Load(param_file);

	QVariantList list;
	const auto&  map = psf.GetMap();
	for (auto it = map.constBegin(); it != map.constEnd(); ++it)
	{
		QVariantMap item;
		item["key"]   = it.key();
		item["value"] = FormatPsfValue(it.value());
		list << item;
	}
	return list;
}

void LauncherBackend::SetEmulatorPath(const QString& path)
{
	m_emulator_override = path;
	Save();
	m_runner.Probe(path);
	emit StateChanged();
}

void LauncherBackend::SetTableView(bool table)
{
	if (table == m_table_view)
	{
		return;
	}
	m_table_view = table;
	Save();
	emit StateChanged();
}

void LauncherBackend::OpenLog() const
{
	if (!LogPath().isEmpty())
	{
		QDesktopServices::openUrl(QUrl::fromLocalFile(LogPath()));
	}
}

void LauncherBackend::OpenGameFolder(int row) const
{
	if (row < 0 || row >= m_library.Count())
	{
		return;
	}
	QDesktopServices::openUrl(QUrl::fromLocalFile(m_library.At(row).config->basedir));
}

QVariantMap LauncherBackend::WindowGeometry() const
{
	QVariantMap geometry;
	if (m_window_geometry.isValid())
	{
		geometry["x"]      = m_window_geometry.x();
		geometry["y"]      = m_window_geometry.y();
		geometry["width"]  = m_window_geometry.width();
		geometry["height"] = m_window_geometry.height();
	}
	return geometry;
}

void LauncherBackend::SaveWindowGeometry(int x, int y, int width, int height)
{
	if (width <= 0 || height <= 0)
	{
		return;
	}
	m_window_geometry = QRect(x, y, width, height);
	Save();
}
