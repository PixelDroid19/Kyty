#ifndef LAUNCHER_INCLUDE_GAME_ART_ITEM_H_
#define LAUNCHER_INCLUDE_GAME_ART_ITEM_H_

#include "ArtworkSet.h"
#include "Common.h"

#include <QQuickItem>
#include <QString>

// One game's artwork as a card filling the item, or its initials on a plain card while it has none.
class GameArtItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(GameArtItem);

public:
	explicit GameArtItem(QQuickItem* parent = nullptr);
	~GameArtItem() override = default;

	// image:// address of the artwork, and the title its initials come from.
	void SetGame(const QString& url, const QString& title);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
	QString    m_id;
	QString    m_title;
	ArtworkSet m_art;
};

#endif /* LAUNCHER_INCLUDE_GAME_ART_ITEM_H_ */
