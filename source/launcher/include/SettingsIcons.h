#ifndef LAUNCHER_INCLUDE_SETTINGS_ICONS_H_
#define LAUNCHER_INCLUDE_SETTINGS_ICONS_H_

#include "Common.h"

#include <QColor>
#include <QImage>

enum class SettingsIcon
{
	Graphics,
	Shaders,
	Logs,
	Emulator,
	Controls,
	Interface
};

// Paints one of the settings categories' icons, px pixels square, as it looks seconds into its looping animation;
// 0 is its rest pose. Lines are drawn in ink and the moving parts in accent.
QImage PaintSettingsIcon(SettingsIcon icon, int px, qreal seconds, const QColor& ink, const QColor& accent);

#endif /* LAUNCHER_INCLUDE_SETTINGS_ICONS_H_ */
