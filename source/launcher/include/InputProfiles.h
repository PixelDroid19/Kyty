#ifndef LAUNCHER_INCLUDE_INPUT_PROFILES_H_
#define LAUNCHER_INCLUDE_INPUT_PROFILES_H_

#include "Common.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// The controls profiles: kyty_input_profile_v1 files (docs/input-profiles.md) in a folder of their own, one file
// per profile named after its id. Default always exists and cannot be removed. A file that does not follow the
// format is listed with the reason and cannot be used until it is restored or removed.
class InputProfiles
{
public:
	struct Profile
	{
		QString     id;
		QJsonObject json;
		// Why the file cannot be used; empty when it follows the format.
		QString error;
	};

	static constexpr char DEFAULT_ID[] = "Default";
	// The guest pad controls in the format's order; the first BUTTON_CONTROL_COUNT bind on a gamepad too.
	static const QStringList& ControlNames();
	static constexpr int      BUTTON_CONTROL_COUNT = 16;

	InputProfiles() = default;

	// Reads every profile in folder, writing Default first when it is missing. Returns an error, or an empty string.
	QString Load(const QString& folder);

	[[nodiscard]] const QString&          Folder() const { return m_folder; }
	[[nodiscard]] const QVector<Profile>& List() const { return m_profiles; }
	[[nodiscard]] const Profile*          Find(const QString& id) const;
	[[nodiscard]] QString                 FilePath(const QString& id) const;

	// Checks json against the format and writes it as the profile id. Returns an error, or an empty string.
	QString Save(const QString& id, const QJsonObject& json);
	// Adds a copy of the profile from_id under a new name; new_id receives its id. Returns an error, or an empty string.
	QString Duplicate(const QString& from_id, QString* new_id);
	// Removes the profile's file; Default stays. Returns an error, or an empty string.
	QString Remove(const QString& id);

	// The built-in profile, identical to the emulator's when no profile is configured.
	[[nodiscard]] static QJsonObject DefaultJson();
	// Why json does not follow the format, naming the field; an empty string when it does.
	[[nodiscard]] static QString Check(const QJsonObject& json);
	// Whether the key name is one the emulator keeps for its own commands.
	[[nodiscard]] static bool IsReservedKey(const QString& name);

private:
	QString                  Write(const QString& id, const QJsonObject& json) const;
	[[nodiscard]] QString    UniqueId(const QString& name) const;
	void                     Sort();

	QString          m_folder;
	QVector<Profile> m_profiles;
};

#endif /* LAUNCHER_INCLUDE_INPUT_PROFILES_H_ */
