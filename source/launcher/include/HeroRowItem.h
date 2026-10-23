#ifndef LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_
#define LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"
#include "CoverNode.h"
#include "GameCard.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>

#include <optional>

// The recently played games as a row of wide cards showing each game's picture and name. Left and Right move
// the selection; Left on the first card is left to KeyNavigation. A click runs the game, as Enter does.
class HeroRowItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(HeroRowItem);

signals:
	void CurrentIndexChanged();
	void Activated(int index);
	void MenuRequested(int index);

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
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;
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
	// Grows the card under the pointer a little, less than the focused one.
	FocusZoom m_hover_zoom;
	// Where the pointer rests over the item, so a scroll under a still pointer moves the hover with it.
	std::optional<QPointF> m_pointer;
};

#endif /* LAUNCHER_INCLUDE_HERO_ROW_ITEM_H_ */
