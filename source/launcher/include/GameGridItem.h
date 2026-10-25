#ifndef LAUNCHER_INCLUDE_GAME_GRID_ITEM_H_
#define LAUNCHER_INCLUDE_GAME_GRID_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"
#include "CoverNode.h"
#include "GameCard.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>

#include <optional>

// The games of the library as a grid of covers with their names. Covers grow to share each row evenly; only the
// rows in view are drawn and only their artwork is kept. Arrows move the selection; a key the grid cannot use
// at its edge (Up on the first row, Left on the first column) is left to KeyNavigation.
class GameGridItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(GameGridItem);

signals:
	void CurrentIndexChanged();
	void Activated(int index);
	void MenuRequested(int index);

public:
	explicit GameGridItem(QQuickItem* parent = nullptr);
	~GameGridItem() override = default;

	[[nodiscard]] QVariantList Games() const { return m_games; }
	void                       SetGames(const QVariantList& games);
	// Covers at least min_tile units wide, with or without the game's name under each.
	void                       SetCoverLook(qreal min_tile, bool titles);
	[[nodiscard]] int          CurrentIndex() const { return m_current; }
	void                       SetCurrentIndex(int index);
	// The height of one row of covers when the grid is grid_width wide.
	[[nodiscard]] qreal RowHeight(qreal grid_width) const;

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;
	void     mouseDoubleClickEvent(QMouseEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	struct Metrics
	{
		int   columns     = 1;
		qreal cell_width  = 0;
		qreal cell_height = 0;
		qreal tile        = 0;
	};

	[[nodiscard]] Metrics LayoutMetrics() const;
	[[nodiscard]] Metrics MetricsFor(qreal grid_width) const;
	[[nodiscard]] QRectF  TileRect(int index, const Metrics& metrics) const;
	[[nodiscard]] int     IndexAt(QPointF point) const;
	[[nodiscard]] qreal   MaxContentY() const;
	void                  ShowCurrent(bool animated);
	void                  Changed();

	QVariantList      m_games;
	QVector<GameCard> m_cards;
	int               m_current = 0;
	qreal             m_min_tile = 0;
	bool              m_titles   = true;
	ViewScroll        m_scroll;
	ArtworkSet        m_art;
	FocusZoom         m_zoom;
	// Grows the card under the pointer a little, less than the focused one.
	FocusZoom m_hover_zoom;
	// Where the pointer rests over the item, so a scroll under a still pointer moves the hover with it.
	std::optional<QPointF> m_pointer;
};

#endif /* LAUNCHER_INCLUDE_GAME_GRID_ITEM_H_ */
