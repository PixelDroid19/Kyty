#include "InputProfiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include <SDL3/SDL.h>
#include <algorithm>

constexpr char SCHEMA[] = "kyty_input_profile_v1";
// The same cap the emulator reads profiles with.
constexpr qint64 MAX_PROFILE_BYTES = 256 * 1024;
constexpr int    MAX_PROFILES      = 64;
constexpr int    MAX_NAME          = 64;
constexpr int    MAX_ID            = 40;

namespace {

// Keys the emulator keeps for its own commands (docs/input-profiles.md).
constexpr SDL_Scancode RESERVED_KEYS[] = {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F1, SDL_SCANCODE_F2,
                                          SDL_SCANCODE_F3,     SDL_SCANCODE_F9, SDL_SCANCODE_F11};
const QStringList      MOUSE_BUTTONS   = {"Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};

QString CheckMembers(const QJsonObject& object, const QStringList& keys, const QString& path)
{
	for (auto it = object.begin(); it != object.end(); ++it)
	{
		if (!keys.contains(it.key()))
		{
			return QCoreApplication::translate("InputProfiles", "%1.%2 is not a known field").arg(path, it.key());
		}
	}
	for (const auto& key: keys)
	{
		if (!object.contains(key))
		{
			return QCoreApplication::translate("InputProfiles", "%1.%2 is missing").arg(path, key);
		}
	}
	return {};
}

QString CheckBool(const QJsonObject& object, const QString& key, const QString& path)
{
	return object.value(key).isBool() ? QString()
	                                  : QCoreApplication::translate("InputProfiles", "%1.%2 must be true or false").arg(path, key);
}

QString CheckRange(const QJsonObject& object, const QString& key, double min, double max, const QString& path)
{
	const auto value = object.value(key);
	if (value.isDouble() && value.toDouble() >= min && value.toDouble() <= max)
	{
		return {};
	}
	return QCoreApplication::translate("InputProfiles", "%1.%2 must be a number from %3 to %4").arg(path, key).arg(min).arg(max);
}

QString CheckKey(const QString& name, const QString& path)
{
	if (MOUSE_BUTTONS.contains(name))
	{
		return {};
	}
	const auto code = name.isEmpty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(name.toUtf8().constData());
	if (code == SDL_SCANCODE_UNKNOWN)
	{
		return QCoreApplication::translate("InputProfiles", "%1: '%2' is not a key name").arg(path, name);
	}
	return InputProfiles::IsReservedKey(name)
	           ? QCoreApplication::translate("InputProfiles", "%1: '%2' is reserved for a host command").arg(path, name)
			   : QString();
}

QString CheckButton(const QString& name, const QString& path)
{
	const bool known = !name.isEmpty() && SDL_GetGamepadButtonFromString(name.toUtf8().constData()) != SDL_GAMEPAD_BUTTON_INVALID;
	return known ? QString() : QCoreApplication::translate("InputProfiles", "%1: '%2' is not a gamepad button name").arg(path, name);
}

// Each control of controls is a list of names that check accepts.
QString CheckBindings(const QJsonObject& object, const QStringList& controls, const QString& path,
                      QString (*check)(const QString&, const QString&))
{
	if (auto error = CheckMembers(object, controls, path); !error.isEmpty())
	{
		return error;
	}
	for (const auto& control: controls)
	{
		const auto value = object.value(control);
		const auto where = path + QLatin1Char('.') + control;
		if (!value.isArray())
		{
			return QCoreApplication::translate("InputProfiles", "%1 must be a list of names").arg(where);
		}
		for (const auto& item: value.toArray())
		{
			if (auto error = check(item.toString(), where); !error.isEmpty())
			{
				return error;
			}
		}
	}
	return {};
}

QString CheckStick(const QJsonObject& stick, const QString& path)
{
	if (auto error = CheckMembers(stick, {"deadzone", "invert_x", "invert_y"}, path); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckRange(stick, "deadzone", 0, 0.9, path); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckBool(stick, "invert_x", path); !error.isEmpty())
	{
		return error;
	}
	return CheckBool(stick, "invert_y", path);
}

QString CheckMouse(const QJsonObject& mouse)
{
	if (auto error = CheckMembers(mouse, {"look", "sensitivity", "invert_y"}, "mouse"); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckBool(mouse, "look", "mouse"); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckRange(mouse, "sensitivity", 0.1, 10, "mouse"); !error.isEmpty())
	{
		return error;
	}
	return CheckBool(mouse, "invert_y", "mouse");
}

QString CheckGamepad(const QJsonObject& gamepad)
{
	if (auto error = CheckMembers(gamepad, {"device", "buttons", "left_stick", "right_stick", "trigger_deadzone"}, "gamepad");
	    !error.isEmpty())
	{
		return error;
	}
	if (!gamepad.value("device").isString())
	{
		return QCoreApplication::translate("InputProfiles", "gamepad.device must be a string");
	}
	for (const char* key: {"buttons", "left_stick", "right_stick"})
	{
		if (!gamepad.value(key).isObject())
		{
			return QCoreApplication::translate("InputProfiles", "gamepad.%1 must be an object").arg(key);
		}
	}
	const auto buttons = InputProfiles::ControlNames().mid(0, InputProfiles::BUTTON_CONTROL_COUNT);
	if (auto error = CheckBindings(gamepad.value("buttons").toObject(), buttons, "gamepad.buttons", CheckButton); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckStick(gamepad.value("left_stick").toObject(), "gamepad.left_stick"); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckStick(gamepad.value("right_stick").toObject(), "gamepad.right_stick"); !error.isEmpty())
	{
		return error;
	}
	return CheckRange(gamepad, "trigger_deadzone", 0, 0.9, "gamepad");
}

QJsonArray Names(std::initializer_list<const char*> names)
{
	QJsonArray array;
	for (const char* name: names)
	{
		array.append(QString::fromLatin1(name));
	}
	return array;
}

} // namespace

const QStringList& InputProfiles::ControlNames()
{
	static const QStringList names = {"cross",
	                                  "circle",
	                                  "square",
	                                  "triangle",
	                                  "up",
	                                  "down",
	                                  "left",
	                                  "right",
	                                  "l1",
	                                  "r1",
	                                  "l2",
	                                  "r2",
	                                  "l3",
	                                  "r3",
	                                  "options",
	                                  "touchpad",
	                                  "left-stick-left",
	                                  "left-stick-right",
	                                  "left-stick-up",
	                                  "left-stick-down",
	                                  "right-stick-left",
	                                  "right-stick-right",
	                                  "right-stick-up",
	                                  "right-stick-down"};
	return names;
}

// Kept identical to the emulator's built-in profile (docs/input-profiles.md).
QJsonObject InputProfiles::DefaultJson()
{
	const QJsonObject keyboard {
	    {"cross", Names({"Z", "Space", "Return"})},
	    {"circle", Names({"X"})},
	    {"square", Names({"C"})},
	    {"triangle", Names({"V"})},
	    {"up", Names({"Up"})},
	    {"down", Names({"Down"})},
	    {"left", Names({"Left"})},
	    {"right", Names({"Right"})},
	    {"l1", Names({"Q"})},
	    {"r1", Names({"E"})},
	    {"l2", Names({"1", "Mouse Right"})},
	    {"r2", Names({"3", "Mouse Left"})},
	    {"l3", Names({"F"})},
	    {"r3", Names({"H"})},
	    {"options", Names({"Tab", "Backspace"})},
	    {"touchpad", Names({"T"})},
	    {"left-stick-left", Names({"A"})},
	    {"left-stick-right", Names({"D"})},
	    {"left-stick-up", Names({"W"})},
	    {"left-stick-down", Names({"S"})},
	    {"right-stick-left", Names({"J"})},
	    {"right-stick-right", Names({"L"})},
	    {"right-stick-up", Names({"I"})},
	    {"right-stick-down", Names({"K"})},
	};
	const QJsonObject buttons {
	    {"cross", Names({"a"})},
	    {"circle", Names({"b"})},
	    {"square", Names({"x"})},
	    {"triangle", Names({"y"})},
	    {"up", Names({"dpup"})},
	    {"down", Names({"dpdown"})},
	    {"left", Names({"dpleft"})},
	    {"right", Names({"dpright"})},
	    {"l1", Names({"leftshoulder"})},
	    {"r1", Names({"rightshoulder"})},
	    {"l2", QJsonArray()},
	    {"r2", QJsonArray()},
	    {"l3", Names({"leftstick"})},
	    {"r3", Names({"rightstick"})},
	    {"options", Names({"start"})},
	    {"touchpad", Names({"back", "touchpad"})},
	};
	const QJsonObject stick {{"deadzone", 0.10}, {"invert_x", false}, {"invert_y", false}};
	return {
	    {"schema", SCHEMA},
	    {"name", DEFAULT_ID},
	    {"keyboard", keyboard},
	    {"mouse", QJsonObject {{"look", true}, {"sensitivity", 1.0}, {"invert_y", false}}},
	    {"gamepad",
		 QJsonObject {{"device", ""}, {"buttons", buttons}, {"left_stick", stick}, {"right_stick", stick}, {"trigger_deadzone", 0.0}}},
	};
}

QString InputProfiles::Check(const QJsonObject& json)
{
	if (auto error = CheckMembers(json, {"schema", "name", "keyboard", "mouse", "gamepad"}, "profile"); !error.isEmpty())
	{
		return error;
	}
	if (json.value("schema").toString() != QLatin1String(SCHEMA))
	{
		return QCoreApplication::translate("InputProfiles", "profile.schema must be '%1'").arg(SCHEMA);
	}
	const auto name = json.value("name");
	if (!name.isString() || name.toString().isEmpty())
	{
		return QCoreApplication::translate("InputProfiles", "profile.name must be a non-empty string");
	}
	for (const char* key: {"keyboard", "mouse", "gamepad"})
	{
		if (!json.value(key).isObject())
		{
			return QCoreApplication::translate("InputProfiles", "profile.%1 must be an object").arg(key);
		}
	}
	if (auto error = CheckBindings(json.value("keyboard").toObject(), ControlNames(), "keyboard", CheckKey); !error.isEmpty())
	{
		return error;
	}
	if (auto error = CheckMouse(json.value("mouse").toObject()); !error.isEmpty())
	{
		return error;
	}
	return CheckGamepad(json.value("gamepad").toObject());
}

bool InputProfiles::IsReservedKey(const QString& name)
{
	const auto code = SDL_GetScancodeFromName(name.toUtf8().constData());
	return std::find(std::begin(RESERVED_KEYS), std::end(RESERVED_KEYS), code) != std::end(RESERVED_KEYS);
}

QString InputProfiles::Load(const QString& folder)
{
	m_folder = folder;
	m_profiles.clear();
	if (!QDir().mkpath(folder))
	{
		return QCoreApplication::translate("InputProfiles", "Cannot create the profiles folder %1.").arg(QDir::toNativeSeparators(folder));
	}
	if (!QFile::exists(FilePath(DEFAULT_ID)))
	{
		if (auto error = Write(DEFAULT_ID, DefaultJson()); !error.isEmpty())
		{
			return error;
		}
	}
	const QDir dir(folder);
	// Ids follow the same rule as new profiles; other files in the folder are not profiles.
	const QRegularExpression id_rule(QStringLiteral("^[A-Za-z0-9._-]{1,%1}$").arg(MAX_ID));
	for (const auto& info: dir.entryInfoList({"*.json"}, QDir::Files, QDir::Name))
	{
		const auto id = info.completeBaseName();
		if (!id_rule.match(id).hasMatch())
		{
			continue;
		}
		if (m_profiles.size() >= MAX_PROFILES)
		{
			break;
		}
		Profile profile;
		profile.id = id;
		QFile file(info.filePath());
		if (info.size() > MAX_PROFILE_BYTES)
		{
			profile.error = QCoreApplication::translate("InputProfiles", "The file is larger than %1 KB.").arg(MAX_PROFILE_BYTES / 1024);
		} else if (!file.open(QIODevice::ReadOnly))
		{
			profile.error = QCoreApplication::translate("InputProfiles", "The file cannot be read.");
		} else
		{
			QJsonParseError parse;
			const auto      document = QJsonDocument::fromJson(file.readAll(), &parse);
			profile.json             = document.object();
			profile.error            = parse.error != QJsonParseError::NoError
			                               ? QCoreApplication::translate("InputProfiles", "Not valid JSON: %1").arg(parse.errorString())
			                           : !document.isObject() ? QCoreApplication::translate("InputProfiles", "profile must be an object")
			                                                  : Check(profile.json);
		}
		m_profiles << profile;
	}
	Sort();
	return {};
}

const InputProfiles::Profile* InputProfiles::Find(const QString& id) const
{
	const auto found = std::find_if(m_profiles.begin(), m_profiles.end(), [&](const Profile& profile) { return profile.id == id; });
	return found != m_profiles.end() ? &*found : nullptr;
}

QString InputProfiles::FilePath(const QString& id) const
{
	return QDir(m_folder).filePath(id + QStringLiteral(".json"));
}

QString InputProfiles::Write(const QString& id, const QJsonObject& json) const
{
	QSaveFile file(FilePath(id));
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(json).toJson(QJsonDocument::Indented)) < 0 || !file.commit())
	{
		return QCoreApplication::translate("InputProfiles", "Cannot write %1.").arg(QDir::toNativeSeparators(FilePath(id)));
	}
	return {};
}

QString InputProfiles::Save(const QString& id, const QJsonObject& json)
{
	if (json.value("name").toString().size() > MAX_NAME)
	{
		return QCoreApplication::translate("InputProfiles", "A profile name has at most %1 characters.").arg(MAX_NAME);
	}
	if (auto error = Check(json); !error.isEmpty())
	{
		return error;
	}
	if (auto error = Write(id, json); !error.isEmpty())
	{
		return error;
	}
	const auto found = std::find_if(m_profiles.begin(), m_profiles.end(), [&](const Profile& profile) { return profile.id == id; });
	if (found != m_profiles.end())
	{
		found->json = json;
		found->error.clear();
	} else
	{
		m_profiles << Profile {id, json, {}};
	}
	Sort();
	return {};
}

QString InputProfiles::UniqueId(const QString& name) const
{
	auto base = name;
	base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
	base = base.left(MAX_ID - 4);
	if (base.isEmpty() || base.startsWith(QLatin1Char('.')))
	{
		base = QStringLiteral("Profile");
	}
	auto id = base;
	for (int n = 2; Find(id) != nullptr || QFile::exists(FilePath(id)); n++)
	{
		id = QStringLiteral("%1-%2").arg(base).arg(n);
	}
	return id;
}

QString InputProfiles::Duplicate(const QString& from_id, QString* new_id)
{
	const auto* from = Find(from_id);
	auto        json = from != nullptr && from->error.isEmpty() ? from->json : DefaultJson();
	if (m_profiles.size() >= MAX_PROFILES)
	{
		return QCoreApplication::translate("InputProfiles", "There are already %1 profiles.").arg(MAX_PROFILES);
	}
	const auto name = QCoreApplication::translate("InputProfiles", "%1 copy").arg(json.value("name").toString()).left(MAX_NAME);
	json["name"]    = name;
	const auto id   = UniqueId(name);
	if (auto error = Save(id, json); !error.isEmpty())
	{
		return error;
	}
	*new_id = id;
	return {};
}

QString InputProfiles::Remove(const QString& id)
{
	if (id == QLatin1String(DEFAULT_ID))
	{
		return QCoreApplication::translate("InputProfiles", "The Default profile cannot be deleted.");
	}
	if (QFile::exists(FilePath(id)) && !QFile::remove(FilePath(id)))
	{
		return QCoreApplication::translate("InputProfiles", "Cannot delete %1.").arg(QDir::toNativeSeparators(FilePath(id)));
	}
	m_profiles.erase(std::remove_if(m_profiles.begin(), m_profiles.end(), [&](const Profile& profile) { return profile.id == id; }),
	                 m_profiles.end());
	return {};
}

// Default first, then by the name shown.
void InputProfiles::Sort()
{
	std::stable_sort(m_profiles.begin(), m_profiles.end(),
	                 [](const Profile& a, const Profile& b)
	                 {
		                 const bool a_default = a.id == QLatin1String(DEFAULT_ID);
		                 const bool b_default = b.id == QLatin1String(DEFAULT_ID);
		                 if (a_default != b_default)
		                 {
			                 return a_default;
		                 }
		                 return QString::localeAwareCompare(a.json.value("name").toString(a.id), b.json.value("name").toString(b.id)) < 0;
	                 });
}
