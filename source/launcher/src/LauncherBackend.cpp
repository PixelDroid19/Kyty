#include "LauncherBackend.h"

#include "ArtworkProvider.h"

#include "Configuration.h"
#include "ConfigurationFields.h"
#include "GamepadInput.h"
#include "Psf.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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

} // namespace

LauncherBackend::LauncherBackend(QObject* parent)
    : QObject(parent), m_gamepad(new GamepadInput(this)), m_run_modes(EmulatorRunner::RunModes())
{
	m_import_pool.setMaxThreadCount(1);
	m_import_pool.setThreadPriority(QThread::IdlePriority);

	Load();

	connect(&m_runner, &EmulatorRunner::Probed, this, &LauncherBackend::OnProbed);
	connect(&m_runner, &EmulatorRunner::Finished, this, &LauncherBackend::OnFinished);
	connect(m_gamepad, &GamepadInput::ControllerChanged, this, &LauncherBackend::StateChanged);

	m_runner.Probe(m_emulator_override);
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
	return item;
}

QVariantList LauncherBackend::Folders() const
{
	QVariantList list;
	list << FolderToMap(tr("All games"), QString(), true);
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
	return ConfigurationChoices();
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

int LauncherBackend::RunModeIndex() const
{
	return std::max(0, static_cast<int>(m_run_modes.indexOf(m_run_mode)));
}

void LauncherBackend::SetRunModeIndex(int index)
{
	if (index < 0 || index >= m_run_modes.size())
	{
		return;
	}
	m_run_mode = m_run_modes.at(index);
	Save();
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
