#ifndef LAUNCHER_INCLUDE_GAME_TROPHIES_H_
#define LAUNCHER_INCLUDE_GAME_TROPHIES_H_

#include <QLocale>
#include <QString>
#include <QVector>

struct GameTrophy
{
	// P, G, S or B.
	QChar   grade;
	QString name;
	QString detail;
	bool    hidden = false;
	// Unix seconds; 0 while locked.
	qint64 unlocked = 0;
	// The value the unlock waits for, and the recorded one when the trophy shows its progress (-1 when none is).
	qint64 target   = 0;
	qint64 progress = -1;
};

// The trophies of the game whose files are in app_dir (sce_sys/trophy2/trophy00.ucp), named in the set's language
// closest to locale, with what the emulator recorded in progress_file. Empty when the game has no readable set.
[[nodiscard]] QVector<GameTrophy> ReadGameTrophies(const QString& app_dir, const QString& progress_file, const QLocale& locale);
// The progress file the emulator keeps for the game's first user under the save-data root (docs/HOST_RUNTIME.md).
[[nodiscard]] QString TrophyProgressFile(const QString& save_root, const QString& title_id);

#endif /* LAUNCHER_INCLUDE_GAME_TROPHIES_H_ */
