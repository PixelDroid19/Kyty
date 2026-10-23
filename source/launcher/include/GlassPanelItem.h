#ifndef LAUNCHER_INCLUDE_GLASS_PANEL_ITEM_H_
#define LAUNCHER_INCLUDE_GLASS_PANEL_ITEM_H_

#include "Common.h"

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// Translucent rounded panel used for every card-like surface.
class GlassPanelItem: public QQuickItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(GlassPanel)
	KYTY_QT_CLASS_NO_COPY(GlassPanelItem);

	Q_PROPERTY(QColor color READ Color WRITE SetColor NOTIFY ColorChanged)

signals:
	void ColorChanged();

public:
	explicit GlassPanelItem(QQuickItem* parent = nullptr);
	~GlassPanelItem() override = default;

	[[nodiscard]] QColor Color() const { return m_color; }
	void                 SetColor(const QColor& color);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
	QColor m_color;
};

#endif /* LAUNCHER_INCLUDE_GLASS_PANEL_ITEM_H_ */
