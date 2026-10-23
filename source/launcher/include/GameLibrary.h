#ifndef LAUNCHER_INCLUDE_GAME_LIBRARY_H_
#define LAUNCHER_INCLUDE_GAME_LIBRARY_H_

#include "Common.h"

#include "Psf.h"

#include <QList>
#include <QString>
#include <QStringList>

class QSettings;

namespace Kyty {
class Configuration;
} // namespace Kyty

// Owns the configurations shown by the launcher, in the order the user arranged them.
class GameLibrary
{
public:
	struct Game
	{
		Kyty::Configuration* config = nullptr;
		ParamInfo            param;   /* title, serial, region and version from the param file */
		QString              icon;    /* sce_sys/icon0.png, empty when missing */
		QString              picture; /* sce_sys/pic0.png, the key art; empty when missing */
	};

	GameLibrary() = default;
	~GameLibrary();

	KYTY_QT_CLASS_NO_COPY(GameLibrary);

	void Read(QSettings* s);
	void Write(QSettings* s) const;

	[[nodiscard]] int                Count() const { return m_games.size(); }
	[[nodiscard]] const Game&        At(int row) const { return m_games.at(row); }
	[[nodiscard]] QStringList        Folders() const;

	// Takes ownership of config. Returns the new row.
	int  Add(Kyty::Configuration* config);
	// Re-reads artwork and title after the configuration at row changed.
	void Refresh(int row);
	void Remove(int row);
	void Move(int row, int delta);

private:
	static Game MakeGame(Kyty::Configuration* config);

	QList<Game> m_games;
};

#endif /* LAUNCHER_INCLUDE_GAME_LIBRARY_H_ */
