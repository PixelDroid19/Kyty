#include "BackgroundItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTransformNode>

// The accent band leans like the light behind the console's home screen tiles.
constexpr float BAND_ANGLE = 18;

BackgroundItem::BackgroundItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
}

void BackgroundItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

QSGNode* BackgroundItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	// The base colour is the root; the band and the bottom glow are its children, drawn over it.
	auto* base = static_cast<QSGRectangleNode*>(old_node);
	if (base == nullptr)
	{
		base = window()->createRectangleNode();
		base->setColor(Theme::Instance()->Background());

		const QColor accent = Theme::Instance()->Accent();
		QColor       faint  = accent;
		faint.setAlpha(0x26);
		QColor clear = accent;
		clear.setAlpha(0);
		auto* band = new QSGTransformNode;
		band->appendChildNode(
		    CreateTextureNode(window(), CreateGradientTexture(window(), {{0.0, clear}, {0.5, faint}, {1.0, clear}}, Qt::Horizontal), {}));
		base->appendChildNode(band);

		const QGradientStops glow = {{0.0, QColor(0x09, 0x09, 0x0b, 0)}, {1.0, QColor(0x10, 0x1e, 0x3c, 0x14)}};
		base->appendChildNode(CreateTextureNode(window(), CreateGradientTexture(window(), glow, Qt::Vertical), {}));
	}

	const qreal w = width();
	const qreal h = height();
	base->setRect(boundingRect());

	// Tall enough that the rotated top and bottom edges stay off screen; only the faded sides show.
	const QRectF band_rect(-w * 0.15, -h, w * 0.8, h * 3);
	auto*        band = static_cast<QSGTransformNode*>(base->firstChild());
	QMatrix4x4   matrix;
	matrix.translate(static_cast<float>(band_rect.center().x()), static_cast<float>(band_rect.center().y()));
	matrix.rotate(BAND_ANGLE, 0, 0, 1);
	matrix.translate(static_cast<float>(-band_rect.center().x()), static_cast<float>(-band_rect.center().y()));
	band->setMatrix(matrix);
	static_cast<QSGImageNode*>(band->firstChild())->setRect(band_rect);

	static_cast<QSGImageNode*>(base->lastChild())->setRect(QRectF(0, h * 0.55, w, h * 0.45));
	return base;
}
