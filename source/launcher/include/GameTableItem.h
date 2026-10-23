#ifndef LAUNCHER_INCLUDE_GAME_TABLE_ITEM_H_
#define LAUNCHER_INCLUDE_GAME_TABLE_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"
#include "GameCard.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// The games of the library as a table: icon, name, serial, region and version under fixed column names. Only
// the rows in view are drawn. Up and Down move the selection; Up on the first row and Left are left to
// KeyNavigation.
class GameTableItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(GameTable)
	KYTY_QT_CLASS_NO_COPY(GameTableItem);

	Q_PROPERTY(QVariantList games READ Games WRITE SetGames NOTIFY GamesChanged)
	Q_PROPERTY(int currentIndex READ CurrentIndex WRITE SetCurrentIndex NOTIFY CurrentIndexChanged)

signals:
	void GamesChanged();
	void CurrentIndexChanged();
	// QML maps only signals that start in lower case to on... handlers.
	void activated(int index);     // NOLINT(readability-identifier-naming)
	void menuRequested(int index); // NOLINT(readability-identifier-naming)

public:
	explicit GameTableItem(QQuickItem* parent = nullptr);
	~GameTableItem() override = default;

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
	void     mouseDoubleClickEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	// The rows sit below the column names, from this height down.
	[[nodiscard]] QRectF ListRect() const;
	[[nodiscard]] qreal  Pitch() const;
	[[nodiscard]] qreal  MaxContentY() const;
	[[nodiscard]] int    IndexAt(QPointF point) const;
	void                 ShowCurrent(bool animated);
	void                 SetHovered(int index);
	void                 Changed();

	QVariantList      m_games;
	QVector<GameCard> m_cards;
	int               m_current = 0;
	int               m_hovered = -1;
	ViewScroll        m_scroll;
	ArtworkSet        m_art;
};

#endif /* LAUNCHER_INCLUDE_GAME_TABLE_ITEM_H_ */
