#include "LauncherBackend.h"

#include "ArtworkProvider.h"
#include "Configuration.h"
#include "ConfigurationFields.h"
#include "GamepadInput.h"
#include "Psf.h"
#include "Theme.h"
#include "UiSounds.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
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

constexpr char CONF_FILE_NAME[]    = "Kyty.ini";
constexpr char CONF_ORG_NAME[]     = "Kyty";
constexpr char CONF_APP_NAME[]     = "Kyty";
constexpr char SETTINGS_LAUNCHER[] = "Launcher";
constexpr char SETTINGS_WINDOW[]   = "Window";
constexpr char SETTINGS_GLOBAL[]   = "GlobalConfiguration";
constexpr int  RECENT_COUNT        = 5;
// Upper bound for executables listed from a game directory, so the editor list stays usable.
constexpr int MAX_LISTED_ELFS = 512;
// Bounds for importing a folder of games: how deep game folders are looked for, and how much is walked.
constexpr int IMPORT_DEPTH       = 3;
constexpr int MAX_IMPORT_DIRS    = 4096;
constexpr int MAX_IMPORTED_GAMES = 512;
// Longer section names would not fit their tab.
constexpr int SECTION_NAME_MAX = 40;
// The colour scheme and language that follow the desktop's.
constexpr char SYSTEM_CHOICE[] = "System";
// The launcher's own look, which needs no add-on theme.
constexpr char OWN_THEME[] = "Kyty";

namespace {

// Kyty.ini next to the working directory wins, otherwise the per-user settings: the machine-wide location is not
// writable by an ordinary user on Windows and macOS.
std::unique_ptr<QSettings> OpenSettings()
{
	QFile file(QDir(".").absoluteFilePath(CONF_FILE_NAME));
	if (file.exists())
	{
		return std::make_unique<QSettings>(CONF_FILE_NAME, QSettings::IniFormat);
	}
	auto settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME, CONF_APP_NAME);
#ifndef __linux__
	// The widget launcher kept its library in the machine-wide file there; a first start brings it over once.
	if (!QFile::exists(settings->fileName()))
	{
		const QSettings machine(QSettings::IniFormat, QSettings::SystemScope, CONF_ORG_NAME, CONF_APP_NAME);
		if (QFile::exists(machine.fileName()))
		{
			QDir().mkpath(QFileInfo(settings->fileName()).absolutePath());
			QFile::copy(machine.fileName(), settings->fileName());
			settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, CONF_ORG_NAME, CONF_APP_NAME);
		}
	}
#endif
	return settings;
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
	return {{"accent", "Blue"},       {"interface_size", "Normal"}, {"animations", "Full"},   {"background", "Artwork"},
	        {"cover_size", "Medium"}, {"show_titles", true},        {"show_recent", true},    {"on_launch", "Minimize"},
	        {"sort", "Manual"},       {"window_mode", "Windowed"},  {"start_on", "LastGame"}, {"theme", OWN_THEME},
	        {"layout", "Sidebar"},    {"ui_sounds", true},          {"ui_volume", "Medium"}};
}

// Scale of every size for each interface_size, in the order of its choices.
constexpr qreal INTERFACE_SCALES[] = {0.88, 1, 1.14, 1.3};

// The deadzones offered, in steps of 5 % up to the format's 90 %, and the mouse sensitivities.
QStringList DeadzoneChoices()
{
	QStringList choices;
	for (int percent = 0; percent <= 90; percent += 5)
	{
		choices << QString::number(percent / 100.0);
	}
	return choices;
}

const QStringList SENSITIVITY_CHOICES = {"0.1", "0.25", "0.5", "0.75", "1", "1.25", "1.5", "2", "3", "4", "5", "7.5", "10"};

QStringList Names(const QJsonValue& list)
{
	QStringList names;
	for (const auto& name: list.toArray())
	{
		names << name.toString();
	}
	return names;
}

// Sets the profile member that one of the controls page's fields shows; returns an error, or an empty string.
QString ApplyControlsField(QJsonObject* json, const QString& key, const QVariant& value)
{
	auto gamepad = json->value("gamepad").toObject();
	auto mouse   = json->value("mouse").toObject();
	if (key == QLatin1String("profile_name"))
	{
		if (value.toString().trimmed().isEmpty())
		{
			return LauncherBackend::tr("A profile needs a name.");
		}
		(*json)["name"] = value.toString().trimmed();
		return {};
	}
	if (key == QLatin1String("input_device"))
	{
		gamepad["device"] = value.toString();
	} else if (key == QLatin1String("trigger_deadzone"))
	{
		gamepad["trigger_deadzone"] = value.toDouble();
	} else if (key.startsWith(QLatin1String("left_")) || key.startsWith(QLatin1String("right_")))
	{
		// left_deadzone, right_invert_x and the like: the member of that stick.
		const auto stick_key = key.section(QLatin1Char('_'), 0, 0) + QStringLiteral("_stick");
		const auto member    = key.section(QLatin1Char('_'), 1);
		auto       stick     = gamepad.value(stick_key).toObject();
		stick[member]        = member == QLatin1String("deadzone") ? QJsonValue(value.toDouble()) : QJsonValue(value.toBool());
		gamepad[stick_key]   = stick;
	} else if (key.startsWith(QLatin1String("mouse_")))
	{
		const auto member = key.mid(6);
		mouse[member]     = member == QLatin1String("sensitivity") ? QJsonValue(value.toDouble()) : QJsonValue(value.toBool());
	} else if (key.startsWith(QLatin1String("bind.")))
	{
		const auto control = key.mid(5);
		const auto index   = InputProfiles::ControlNames().indexOf(control);
		if (index < 0)
		{
			return LauncherBackend::tr("Unknown control %1.").arg(control);
		}
		const auto binding  = value.toMap();
		auto       keys     = json->value("keyboard").toObject();
		keys[control]       = QJsonArray::fromStringList(binding.value("keys").toStringList());
		(*json)["keyboard"] = keys;
		if (index < InputProfiles::BUTTON_CONTROL_COUNT)
		{
			auto buttons       = gamepad.value("buttons").toObject();
			buttons[control]   = QJsonArray::fromStringList(binding.value("buttons").toStringList());
			gamepad["buttons"] = buttons;
		}
	} else
	{
		return LauncherBackend::tr("Unknown controls setting %1.").arg(key);
	}
	(*json)["gamepad"] = gamepad;
	(*json)["mouse"]   = mouse;
	return {};
}

} // namespace

LauncherBackend::LauncherBackend(QObject* parent)
    : QObject(parent), m_gamepad(new GamepadInput(this)), m_sounds(new UiSounds(this)), m_run_modes(EmulatorRunner::RunModes())
{
	Q_ASSERT(g_backend == nullptr);
	g_backend = this;
	m_import_pool.setMaxThreadCount(1);
	m_import_pool.setThreadPriority(QThread::IdlePriority);
	m_default_font = QGuiApplication::font();

	Load();
	ApplyColorScheme();
	ApplyLanguage();
	ReadAddOns();
	ApplyLook();

	connect(&m_runner, &EmulatorRunner::Probed, this, &LauncherBackend::OnProbed);
	connect(&m_runner, &EmulatorRunner::Finished, this, &LauncherBackend::OnFinished);
	connect(m_gamepad, &GamepadInput::ControllerChanged, this, &LauncherBackend::StateChanged);
	connect(m_gamepad, &GamepadInput::DevicesChanged, this, &LauncherBackend::DevicesChanged);

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

QString LauncherBackend::PadText(const QString& text) const
{
	// The hints are written with a PlayStation controller's symbols, in the order ButtonLabels() gives them.
	static const QStringList written = {QStringLiteral("✕"), QStringLiteral("○"),  QStringLiteral("□"),
	                                    QStringLiteral("△"), QStringLiteral("L1"), QStringLiteral("R1")};
	const auto               labels  = m_gamepad->ButtonLabels();
	QString                  result;
	for (qsizetype i = 0; i < text.size(); i++)
	{
		const auto found = std::find_if(written.cbegin(), written.cend(),
		                                [&](const QString& symbol) { return QStringView(text).mid(i).startsWith(symbol); });
		if (found == written.cend())
		{
			result += text.at(i);
			continue;
		}
		result += labels.at(found - written.cbegin());
		i += found->size() - 1;
	}
	return result;
}

int LauncherBackend::ControllerBattery() const
{
	return m_gamepad->BatteryPercent();
}

bool LauncherBackend::ControllerCharging() const
{
	return m_gamepad->Charging();
}

QString LauncherBackend::PadButtonText(const QString& button) const
{
	// The face and shoulder buttons in the order ButtonLabels() gives them.
	static const QStringList labelled = {"a", "b", "x", "y", "leftshoulder", "rightshoulder"};
	if (const auto index = labelled.indexOf(button); index >= 0)
	{
		return m_gamepad->ButtonLabels().at(index);
	}
	const QMap<QString, QString> names = {{"back", tr("Back")},
	                                      {"guide", tr("Guide")},
	                                      {"start", tr("Start")},
	                                      {"leftstick", tr("Left stick press")},
	                                      {"rightstick", tr("Right stick press")},
	                                      {"dpup", tr("D-pad up")},
	                                      {"dpdown", tr("D-pad down")},
	                                      {"dpleft", tr("D-pad left")},
	                                      {"dpright", tr("D-pad right")},
	                                      {"misc1", tr("Extra button")},
	                                      {"touchpad", tr("Touchpad")},
	                                      {"paddle1", tr("Paddle 1")},
	                                      {"paddle2", tr("Paddle 2")},
	                                      {"paddle3", tr("Paddle 3")},
	                                      {"paddle4", tr("Paddle 4")}};
	return names.value(button, button);
}

QString LauncherBackend::LayoutName() const
{
	const auto* theme = CurrentTheme();
	return theme != nullptr && !theme->layout.isEmpty() ? theme->layout : m_look.value("layout").toString();
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
	m_emulator_override = settings->value("emulator_path").toString();
	m_run_mode          = settings->value("run_mode", EmulatorRunner::DIRECT_MODE).toString();
	m_table_view        = settings->value("table_view", false).toBool();
	m_color_scheme      = settings->value("color_scheme", SYSTEM_CHOICE).toString();
	m_language          = settings->value("language", SYSTEM_CHOICE).toString();
	m_input_profile     = settings->value("input_profile", InputProfiles::DEFAULT_ID).toString();
	m_look              = LookDefaults();
	for (auto it = m_look.begin(); it != m_look.end(); ++it)
	{
		// A value edited by hand that the launcher does not know keeps the default. A theme is checked when it is
		// applied instead, so one that is no longer installed brings a notice saying why.
		const auto value = settings->value(it.key(), it.value());
		const bool theme = it.key() == QLatin1String("theme") && !value.toString().isEmpty();
		if (theme || IsValidLook(it.key(), value))
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

void LauncherBackend::Save()
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
	settings->setValue("input_profile", m_input_profile);
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

	settings->sync();
	if (settings->status() != QSettings::NoError)
	{
		emit SaveFailed(settings->fileName());
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
	item["row"]            = row;
	item["name"]           = info.name;
	item["sections"]       = info.sections;
	item["title"]          = game.param.title;
	item["serial"]         = game.param.serial;
	item["region"]         = game.param.region;
	item["version"]        = game.param.version;
	item["basedir"]        = info.basedir;
	item["icon"]           = ArtworkProvider::Url(game.icon);
	item["picture"]        = ArtworkProvider::Url(game.picture);
	item["running"]        = row == m_running_row;
	item["lastRun"]        = info.last_run;
	item["playTime"]       = info.play_time;
	item["favorite"]       = info.favorite;
	item["customSettings"] = info.custom_settings;
	// The size the game runs at, which is the global one unless it has its own settings.
	const auto& shown = info.custom_settings ? info : m_global;
	item["screen"]    = QString("%1 × %2").arg(shown.screen_width).arg(shown.screen_height);
	return item;
}

QVariantList LauncherBackend::Folders() const
{
	const auto   sections = m_library.Sections();
	QVariantList list;
	list << FolderToMap(tr("All games"), "all", QString(), [](const Kyty::Configuration& /*info*/) { return true; });
	const auto favorites =
	    FolderToMap(tr("Favorites"), "favorites", QString(), [](const Kyty::Configuration& info) { return info.favorite; });
	if (favorites.value("count").toInt() > 0)
	{
		list << favorites;
	}
	for (const auto& name: sections)
	{
		list << FolderToMap(name, "section", name, [&name](const Kyty::Configuration& info) { return info.sections.contains(name); });
	}
	// Without sections every game would be unfiled, and the tab would repeat All games.
	const auto unfiled =
	    FolderToMap(tr("No section"), "unfiled", QString(), [](const Kyty::Configuration& info) { return info.sections.isEmpty(); });
	if (!sections.isEmpty() && unfiled.value("count").toInt() > 0)
	{
		list << unfiled;
	}
	return list;
}

QVariantMap LauncherBackend::FolderToMap(const QString& title, const QString& kind, const QString& section,
                                         const std::function<bool(const Kyty::Configuration&)>& shows) const
{
	int count = 0;
	for (int row = 0; row < m_library.Count(); row++)
	{
		count += shows(*m_library.At(row).config) ? 1 : 0;
	}

	QVariantMap item;
	item["title"]   = title;
	item["kind"]    = kind;
	item["section"] = section;
	item["count"]   = count;
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
		const Kyty::Configuration defaults {};
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
	choices["start_on"]       = QStringList {"LastGame", "Library"};
	choices["layout"]         = QStringList {"Sidebar", "Console"};
	choices["ui_volume"]      = QStringList {"Quiet", "Medium", "Loud"};
	QStringList themes {OWN_THEME};
	for (const auto& theme: m_themes)
	{
		themes << theme.id;
	}
	choices["theme"] = themes;

	choices["deadzone"]    = DeadzoneChoices();
	choices["sensitivity"] = SENSITIVITY_CHOICES;
	QStringList profiles;
	for (const auto& profile: m_profiles.List())
	{
		profiles << profile.id;
	}
	choices["input_profile"] = profiles;
	// Any controller, the ones connected, and the one the profile names when it is not connected.
	QStringList devices {QString()};
	for (const auto& device: m_gamepad->Devices())
	{
		devices << device.guid;
	}
	const auto* active = ActiveProfile();
	const auto  saved  = active != nullptr ? active->json.value("gamepad").toObject().value("device").toString() : QString();
	if (!saved.isEmpty() &&
	    std::none_of(devices.cbegin(), devices.cend(), [&](const QString& guid) { return GamepadInput::SameDevice(guid, saved); }))
	{
		devices << saved;
	}
	choices["input_device"] = devices;
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
	// Quiet, Medium and Loud, in the order of their choices.
	constexpr float GAINS[] = {0.35F, 0.65F, 1.0F};
	const auto      volume  = choices.value("ui_volume").toStringList().indexOf(m_look.value("ui_volume").toString());
	m_sounds->SetGain(m_look.value("ui_sounds").toBool() ? GAINS[std::max<qsizetype>(0, volume)] : 0);
	ApplyTheme();
}

QString LauncherBackend::ChoiceLabel(const QString& list, const QString& value) const
{
	if (list == QLatin1String("deadzone"))
	{
		return tr("%1 %").arg(qRound(value.toDouble() * 100));
	}
	if (list == QLatin1String("sensitivity"))
	{
		return QStringLiteral("%1×").arg(value);
	}
	if (list == QLatin1String("input_profile"))
	{
		const auto* profile = m_profiles.Find(value);
		if (profile == nullptr || !profile->error.isEmpty())
		{
			return tr("%1 · Can't be used").arg(value);
		}
		return profile->json.value("name").toString();
	}
	if (list == QLatin1String("input_device"))
	{
		if (value.isEmpty())
		{
			return tr("Any controller");
		}
		for (const auto& device: m_gamepad->Devices())
		{
			if (GamepadInput::SameDevice(device.guid, value))
			{
				return device.name;
			}
		}
		return tr("A controller that is not connected");
	}
	if (list != QLatin1String("theme"))
	{
		return {};
	}
	if (value == QLatin1String(OWN_THEME))
	{
		return tr("Kyty");
	}
	for (const auto& theme: m_themes)
	{
		if (theme.id == value)
		{
			return theme.error.isEmpty() ? tr("%1 · Add-on").arg(theme.name) : tr("%1 · Can't be used").arg(theme.name);
		}
	}
	return {};
}

const ThemePack* LauncherBackend::CurrentTheme() const
{
	const auto id = m_look.value("theme").toString();
	for (const auto& theme: m_themes)
	{
		if (theme.id == id && theme.error.isEmpty())
		{
			return &theme;
		}
	}
	return nullptr;
}

// Puts the chosen add-on theme's colours and font in place, or the launcher's own when it has none or fails.
void LauncherBackend::ApplyTheme()
{
	auto* theme = Theme::Instance();
	if (m_theme_font >= 0)
	{
		QFontDatabase::removeApplicationFont(m_theme_font);
		m_theme_font = -1;
		QGuiApplication::setFont(m_default_font);
	}
	theme->SetCustomColors({}, {});
	m_sounds->SetFiles({});
	const auto id = m_look.value("theme").toString();
	if (id == QLatin1String(OWN_THEME))
	{
		return;
	}
	const auto found = std::find_if(m_themes.cbegin(), m_themes.cend(), [&id](const ThemePack& pack) { return pack.id == id; });
	if (found == m_themes.cend())
	{
		ThemeUnusable(tr("The theme %1 is no longer installed.").arg(id));
		return;
	}
	if (!found->error.isEmpty())
	{
		ThemeUnusable(tr("The theme %1 can't be used: %2").arg(found->name, found->error));
		return;
	}
	if (!found->font.isEmpty())
	{
		m_theme_font        = QFontDatabase::addApplicationFont(found->font);
		const auto families = QFontDatabase::applicationFontFamilies(m_theme_font);
		if (families.isEmpty())
		{
			ThemeUnusable(tr("The theme %1 can't be used: its font cannot be read.").arg(found->name));
			return;
		}
		auto font = m_default_font;
		font.setFamilies(families);
		QGuiApplication::setFont(font);
	}
	if (const auto error = m_sounds->SetFiles(found->sounds); !error.isEmpty())
	{
		ThemeUnusable(tr("The theme %1 can't be used: %2").arg(found->name, error));
		return;
	}
	theme->SetCustomColors(found->dark, found->light);
}

// The launcher's own look comes back and is remembered; the window hears why once its event loop runs.
void LauncherBackend::ThemeUnusable(const QString& message)
{
	m_look["theme"] = QString(OWN_THEME);
	ApplyTheme();
	Save();
	QMetaObject::invokeMethod(this, [this, message]() { emit ThemeFailed(message); }, Qt::QueuedConnection);
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
		ReadAddOns();
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

const InputProfiles::Profile* LauncherBackend::ActiveProfile() const
{
	return m_profiles.Find(m_input_profile);
}

// The launcher moves with the controller the profile names, so the one being set up is the one tested.
void LauncherBackend::SetActiveProfile(const QString& id)
{
	m_input_profile     = id;
	const auto* profile = ActiveProfile();
	m_gamepad->SetPreferredDevice(
	    profile != nullptr && profile->error.isEmpty() ? profile->json.value("gamepad").toObject().value("device").toString() : QString());
}

QVariantMap LauncherBackend::ControlsFields() const
{
	const auto* profile = ActiveProfile();
	QVariantMap fields;
	fields["input_profile"] = m_input_profile;
	fields["profile_error"] = profile == nullptr ? tr("The profiles folder cannot be written.") : profile->error;
	// A profile that cannot be used shows Default's controls, which Restore defaults gives it.
	const auto json        = profile != nullptr && profile->error.isEmpty() ? profile->json : InputProfiles::DefaultJson();
	const auto gamepad     = json.value("gamepad").toObject();
	const auto mouse       = json.value("mouse").toObject();
	const auto keyboard    = json.value("keyboard").toObject();
	const auto buttons     = gamepad.value("buttons").toObject();
	fields["profile_name"] = json.value("name").toString();
	fields["input_device"] = gamepad.value("device").toString();
	for (const char* side: {"left", "right"})
	{
		const QString prefix                         = QString::fromLatin1(side);
		const auto    stick                          = gamepad.value(prefix + QStringLiteral("_stick")).toObject();
		fields[prefix + QStringLiteral("_deadzone")] = QString::number(stick.value("deadzone").toDouble());
		fields[prefix + QStringLiteral("_invert_x")] = stick.value("invert_x").toBool();
		fields[prefix + QStringLiteral("_invert_y")] = stick.value("invert_y").toBool();
		fields[prefix + QStringLiteral("_stick")]    = QVariantMap {{"deadzone", stick.value("deadzone").toDouble()},
		                                                            {"invert_x", stick.value("invert_x").toBool()},
		                                                            {"invert_y", stick.value("invert_y").toBool()},
		                                                            {"right", prefix == QLatin1String("right")}};
	}
	fields["trigger_deadzone"]  = QString::number(gamepad.value("trigger_deadzone").toDouble());
	fields["mouse_look"]        = mouse.value("look").toBool();
	fields["mouse_sensitivity"] = QString::number(mouse.value("sensitivity").toDouble());
	fields["mouse_invert_y"]    = mouse.value("invert_y").toBool();
	const auto& controls        = InputProfiles::ControlNames();
	for (int i = 0; i < controls.size(); i++)
	{
		fields[QStringLiteral("bind.") + controls.at(i)] = QVariantMap {{"keys", Names(keyboard.value(controls.at(i)))},
		                                                                {"buttons", Names(buttons.value(controls.at(i)))},
		                                                                {"pad", i < InputProfiles::BUTTON_CONTROL_COUNT}};
	}
	return fields;
}

QString LauncherBackend::SetControlsField(const QString& key, const QVariant& value)
{
	if (key == QLatin1String("input_profile"))
	{
		if (m_profiles.Find(value.toString()) == nullptr)
		{
			return tr("The profile %1 is no longer there.").arg(value.toString());
		}
		SetActiveProfile(value.toString());
		Save();
		return {};
	}
	const auto* profile = ActiveProfile();
	if (profile == nullptr || !profile->error.isEmpty())
	{
		return tr("This profile can't be used. Restore defaults to repair it.");
	}
	if (profile->id == QLatin1String(InputProfiles::DEFAULT_ID))
	{
		return tr("Default can't be changed. Duplicate it to make a profile of your own.");
	}
	auto json = profile->json;
	if (const auto error = ApplyControlsField(&json, key, value); !error.isEmpty())
	{
		return error;
	}
	const auto id = profile->id;
	if (const auto error = m_profiles.Save(id, json); !error.isEmpty())
	{
		return error;
	}
	if (key == QLatin1String("input_device"))
	{
		SetActiveProfile(id);
	}
	return {};
}

QString LauncherBackend::DuplicateProfile()
{
	QString id;
	if (const auto error = m_profiles.Duplicate(m_input_profile, &id); !error.isEmpty())
	{
		return error;
	}
	SetActiveProfile(id);
	Save();
	return {};
}

QString LauncherBackend::DeleteProfile()
{
	if (const auto error = m_profiles.Remove(m_input_profile); !error.isEmpty())
	{
		return error;
	}
	SetActiveProfile(InputProfiles::DEFAULT_ID);
	Save();
	return {};
}

QString LauncherBackend::ResetProfile()
{
	const auto* profile = ActiveProfile();
	if (profile == nullptr)
	{
		return tr("The profiles folder cannot be written.");
	}
	auto       json = InputProfiles::DefaultJson();
	const auto name = profile->json.value("name").toString();
	json["name"]    = profile->error.isEmpty() && !name.isEmpty() ? name : profile->id;
	const auto id   = profile->id;
	if (const auto error = m_profiles.Save(id, json); !error.isEmpty())
	{
		return error;
	}
	SetActiveProfile(id);
	return {};
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

// Themes and profiles explain their faults to the user, so they are read again in each interface language.
void LauncherBackend::ReadAddOns()
{
	m_themes = FindThemePacks();
	// The profiles sit next to the settings file, so a portable Kyty.ini keeps its profiles beside it.
	if (const auto error = m_profiles.Load(QFileInfo(m_settings_file).absolutePath() + QStringLiteral("/input-profiles")); !error.isEmpty())
	{
		qWarning("%s", qPrintable(error));
	}
	SetActiveProfile(m_profiles.Find(m_input_profile) != nullptr ? m_input_profile : QString(InputProfiles::DEFAULT_ID));
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
		auto*      info  = new Kyty::Configuration;
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

void LauncherBackend::SetInSection(int row, const QString& section, bool in)
{
	const auto name = section.trimmed().left(SECTION_NAME_MAX);
	if (row < 0 || row >= m_library.Count() || name.isEmpty())
	{
		return;
	}
	auto& sections = m_library.At(row).config->sections;
	if (sections.contains(name) == in)
	{
		return;
	}
	if (in)
	{
		sections << name;
	} else
	{
		sections.removeAll(name);
	}
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
	const auto* profile = ActiveProfile();
	if (profile == nullptr || !profile->error.isEmpty())
	{
		return tr("The controls profile %1 can't be used: %2 Choose another one or repair it in Settings › Controls.")
		    .arg(m_input_profile, profile == nullptr ? tr("its file is missing.") : profile->error);
	}
	// A mode saved on another host may be missing here; Direct is always available.
	const auto mode  = m_run_modes.contains(m_run_mode) ? m_run_mode : QString(EmulatorRunner::DIRECT_MODE);
	const auto error = m_runner.Start(info, mode, m_profiles.FilePath(profile->id));
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
	const Kyty::Configuration defaults {};
	QVariantMap               fields = ConfigurationToFields(defaults);

	const QDir dir(folder);
	const auto param_file = FindParamFile(dir);

	const auto title = ReadParamInfo(param_file).title;

	fields["name"]       = title.isEmpty() ? dir.dirName() : title;
	fields["basedir"]    = dir.absolutePath();
	fields["param_file"] = param_file;

	// The main executable of a dump is eboot.bin; select it when it is present.
	const auto   elfs = ScanElfs(folder, {});
	QVariantList selected;
	for (const auto& entry: elfs)
	{
		auto item        = entry.toMap();
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
		    const auto         found = FindGameFolders(folder);
		    QList<QVariantMap> games;
		    for (const auto& path: found)
		    {
			    if (!listed.contains(path))
			    {
				    games << DetectGame(path);
			    }
		    }
		    QMetaObject::invokeMethod(this, [this, games, count = found.size()] { AddImported(games, count); }, Qt::QueuedConnection);
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
		auto       item = entry.toMap();
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
