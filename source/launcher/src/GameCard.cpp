#include "GameCard.h"

#include "ArtworkProvider.h"

#include <QRegularExpression>
#include <QVariantMap>

namespace {

// The id of the artwork at url cut to the box it is drawn in, with rounded corners.
QString RoundedId(const QString& url)
{
	const auto id = ArtworkProvider::IdFromUrl(url);
	return id.isEmpty() ? QString() : QString(ArtworkProvider::ROUNDED_PREFIX) + id;
}

} // namespace

QVector<GameCard> GameCardsFromList(const QVariantList& games)
{
	QVector<GameCard> cards;
	cards.reserve(games.size());
	for (const auto& value: games)
	{
		const auto map = value.toMap();
		GameCard   card;
		card.row        = map.value("row", -1).toInt();
		card.title      = map.value("title").toString();
		card.serial     = map.value("serial").toString();
		card.region     = map.value("region").toString();
		card.version    = map.value("version").toString();
		card.icon_id    = RoundedId(map.value("icon").toString());
		card.picture_id = RoundedId(map.value("picture").toString());
		card.running    = map.value("running").toBool();
		if (card.title.isEmpty())
		{
			card.title = map.value("name").toString();
		}
		cards << card;
	}
	return cards;
}

QString Initials(const QString& title)
{
	static const QRegularExpression spaces("\\s+");
	QString                         letters;
	for (const auto& word: title.split(spaces, Qt::SkipEmptyParts))
	{
		letters += word.at(0);
		if (letters.size() == 2)
		{
			break;
		}
	}
	return letters.toUpper();
}
