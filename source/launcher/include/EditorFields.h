#ifndef LAUNCHER_INCLUDE_EDITOR_FIELDS_H_
#define LAUNCHER_INCLUDE_EDITOR_FIELDS_H_

#include "Common.h"
#include "FieldRowItem.h"

#include <QVector>

// The fields of the game editor and the settings page, in the order they are shown. The first field of each
// editor section carries the section's heading.
enum class FieldGroup
{
	Game,
	Display,
	Shaders,
	Logs,
	Modules,
	// The launcher's own settings, kept apart from the emulator's.
	Program,
	Interface
};

[[nodiscard]] const QVector<EditorField>& EditorFields(FieldGroup group);

#endif /* LAUNCHER_INCLUDE_EDITOR_FIELDS_H_ */
