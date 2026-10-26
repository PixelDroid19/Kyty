#ifndef LAUNCHER_INCLUDE_THEME_PACK_H_
#define LAUNCHER_INCLUDE_THEME_PACK_H_

#include "Common.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// A theme installed as an add-on: a folder themes/<id>/ holding a theme.ini and the files it names. A theme is
// data only, colours and files, so it cannot run code. Its theme.ini:
//
//   [Theme]
//   api = 1
//   name = Midnight            ; required
//   author, license, version   ; shown in the settings
//   screenshot = shot.png      ; optional files, relative to the folder
//   font = face.ttf
//   background = wall.png
//   layout = Console           ; Sidebar or Console, optional
//   [Dark] and [Light]         ; colour = #rrggbb or #aarrggbb, see ThemePack::COLOR_KEYS
//   [Sounds]                   ; move, accept, back, start = file.wav
struct ThemePack
{
	// The colours a theme may set; any it leaves out keep the launcher's own.
	static const QStringList& ColorKeys();
	static const QStringList& SoundKeys();

	QString id;
	QString folder;
	QString name;
	QString author;
	QString license;
	QString version;
	// Absolute paths inside the folder; empty when the theme names none.
	QString screenshot;
	QString font;
	QString background;
	QString layout;
	QHash<QString, QColor>  dark;
	QHash<QString, QColor>  light;
	QHash<QString, QString> sounds;
	// Why the theme cannot be used; empty when it can.
	QString error;
};

// Where add-on themes are installed: the themes folder of the launcher's per-user data.
QString ThemesFolder();
// Every folder in ThemesFolder() with a theme.ini, sorted by id, the unusable ones with their error.
QVector<ThemePack> FindThemePacks();
// Reads and checks the theme in folder; the result's error says what is wrong with it.
ThemePack ReadThemePack(const QString& folder);

#endif /* LAUNCHER_INCLUDE_THEME_PACK_H_ */
