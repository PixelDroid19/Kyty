#ifndef LAUNCHER_INCLUDE_GAME_CARD_H_
#define LAUNCHER_INCLUDE_GAME_CARD_H_

#include "Common.h"

#include <QString>
#include <QVariantList>
#include <QVector>

// What the library views show of one game, read from the backend's game maps.
struct GameCard
{
	int     row = -1;
	QString title;
	QString serial;
	QString region;
	QString version;
	// image:// ids (the address without "image://art/") of the rounded artwork; empty when the game has none.
	QString icon_id;
	QString picture_id;
	bool    running = false;

	bool operator==(const GameCard& other) const
	{
		return row == other.row && title == other.title && serial == other.serial && region == other.region && version == other.version &&
		       icon_id == other.icon_id && picture_id == other.picture_id && running == other.running;
	}
	bool operator!=(const GameCard& other) const { return !(*this == other); }
};

QVector<GameCard> GameCardsFromList(const QVariantList& games);

// Up to two capital letters from the first words of title, shown on a game without artwork.
QString Initials(const QString& title);

#endif /* LAUNCHER_INCLUDE_GAME_CARD_H_ */
