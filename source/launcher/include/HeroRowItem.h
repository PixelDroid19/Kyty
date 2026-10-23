#ifndef LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_
#define LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"
#include "CoverNode.h"
#include "GameCard.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// The recently played games as a row of wide cards showing each game's picture and name. Left and Right move
// the selection; Left on the first card is left to KeyNavigation. A click runs the game, as Enter does.
class HeroRowItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(HeroRow)
	KYTY_QT_CLASS_NO_COPY(HeroRowItem);

	Q_PROPERTY(QVariantList games READ Games WRITE SetGames NOTIFY GamesChanged)
	Q_PROPERTY(int currentIndex READ CurrentIndex WRITE SetCurrentIndex NOTIFY CurrentIndexChanged)

signals:
	void GamesChanged();
	void CurrentIndexChanged();
	// QML maps only signals that start in lower case to on... handlers.
	void activated(int index);     // NOLINT(readability-identifier-naming)
	void menuRequested(int index); // NOLINT(readability-identifier-naming)

public:
	explicit HeroRowItem(QQuickItem* parent = nullptr);
	~HeroRowItem() override = default;

	[[nodiscard]] QVariantList Games() const { return m_games; }
	void                       SetGames(const QVariantList& games);
	[[nodiscard]] int          CurrentIndex() const { return m_current; }
	void                       SetCurrentIndex(int index);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	[[nodiscard]] QSizeF CardSize() const;
	[[nodiscard]] qreal  Pitch() const;
	[[nodiscard]] qreal  MaxContentX() const;
	[[nodiscard]] int    IndexAt(QPointF point) const;
	void                 ShowCurrent(bool animated);
	void                 Changed();

	QVariantList      m_games;
	QVector<GameCard> m_cards;
	int               m_current = 0;
	ViewScroll        m_scroll;
	ArtworkSet        m_art;
	FocusZoom         m_zoom;
};

#endif /* LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_ */
