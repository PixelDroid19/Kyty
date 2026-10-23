#ifndef LAUNCHER_INCLUDE_CONFIGURATION_FIELDS_H_
#define LAUNCHER_INCLUDE_CONFIGURATION_FIELDS_H_

#include "Common.h"

#include <QString>
#include <QVariantMap>

namespace Kyty {
class Configuration;
} // namespace Kyty

// Plain-map view of a configuration, the form the QML editor works on.
// Enum fields are stored as their text, elfs as a list of {path, selected, present}.
QVariantMap ConfigurationToFields(const Kyty::Configuration& info);

// Validates the fields and copies them into info. Returns an error text, or an empty string on success.
QString ApplyFields(const QVariantMap& fields, Kyty::Configuration* info);

// Validates only the emulator settings (display, shaders, logs and dumps) and copies them into info.
// Returns an error text, or an empty string on success. Used for the global settings, which belong to no game.
QString ApplyEmulatorFields(const QVariantMap& fields, Kyty::Configuration* info);

// The fields a game runs with: its own when it has custom settings, otherwise the global emulator settings
// together with the game's name, folders, parameters file and modules.
QVariantMap FieldsToRun(const QVariantMap& game, const QVariantMap& global);

// Allowed texts for each enum field, keyed by field name.
QVariantMap ConfigurationChoices();

#endif /* LAUNCHER_INCLUDE_CONFIGURATION_FIELDS_H_ */
