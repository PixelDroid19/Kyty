#ifndef LAUNCHER_INCLUDE_BACKGROUND_ITEM_H_
#define LAUNCHER_INCLUDE_BACKGROUND_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// Static backdrop made of linear gradients only: no blur, no animation, drawn once per size.
class BackgroundItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(Background)
	KYTY_QT_CLASS_NO_COPY(BackgroundItem);

public:
	explicit BackgroundItem(QQuickItem* parent = nullptr);
	~BackgroundItem() override = default;

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
};

#endif /* LAUNCHER_INCLUDE_BACKGROUND_ITEM_H_ */
