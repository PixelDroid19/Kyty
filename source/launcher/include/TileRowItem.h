#ifndef LAUNCHER_INCLUDE_TILE_ROW_ITEM_H_
#define LAUNCHER_INCLUDE_TILE_ROW_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"
#include "GameCard.h"
#include "ViewScroll.h"

#include <QQuickItem>
#include <QVariantList>

// The games as one row of square icons, as on the console's home screen: the selected game stays large at the
// row's left edge, the games after it follow smaller, and those before it leave the row. An add tile ends the
// row. Left and Right move the selection; Enter and a click on the selected tile activate it.
class TileRowItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(TileRowItem);

signals:
	void CurrentIndexChanged();
	// index is a game's, or Games().size() for the add tile.
	void Activated(int index);
	void MenuRequested(int index);

public:
	explicit TileRowItem(QQuickItem* parent = nullptr);
	~TileRowItem() override = default;

	[[nodiscard]] QVariantList Games() const { return m_games; }
	void                       SetGames(const QVariantList& games);
	// The selected tile; Games().size() is the add tile.
	[[nodiscard]] int CurrentIndex() const { return m_current; }
	void              SetCurrentIndex(int index);
	// The height the item needs: the large tile and the caption under it.
	[[nodiscard]] static qreal RowHeight();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     wheelEvent(QWheelEvent* event) override;

private:
	// Where tile index is drawn at the current slide.
	[[nodiscard]] QRectF TileRect(int index) const;
	[[nodiscard]] int    IndexAt(QPointF point) const;
	[[nodiscard]] int    TileCount() const { return static_cast<int>(m_cards.size()) + 1; }
	void                 Changed();

	QVariantList      m_games;
	QVector<GameCard> m_cards;
	int               m_current = 0;
	// The selection as it slides, in tiles.
	ViewScroll m_slide;
	ArtworkSet m_art;
};

#endif /* LAUNCHER_INCLUDE_TILE_ROW_ITEM_H_ */
