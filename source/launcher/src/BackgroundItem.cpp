#include "BackgroundItem.h"

#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTransformNode>

#include <algorithm>

// The accent band leans like the light behind the console's home screen tiles.
constexpr float BAND_ANGLE = 18;

BackgroundItem::BackgroundItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	// The theme was checked when it was read, so a picture that still fails to load only leaves the gradients.
	if (const auto* theme = LauncherBackend::Instance()->CurrentTheme(); theme != nullptr && !theme->background.isEmpty())
	{
		m_picture = QImage(theme->background);
	}
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
		if (!m_picture.isNull())
		{
			auto* picture = CreateTextureNode(window(), window()->createTextureFromImage(m_picture), {});
			picture->setFiltering(QSGTexture::Linear);
			base->appendChildNode(picture);
		}

		const QColor accent = Theme::Instance()->Accent();
		QColor       faint  = accent;
		faint.setAlpha(0x26);
		QColor clear = accent;
		clear.setAlpha(0);
		auto* band = new QSGTransformNode;
		band->appendChildNode(
		    CreateTextureNode(window(), CreateGradientTexture(window(), {{0.0, clear}, {0.5, faint}, {1.0, clear}}, Qt::Horizontal), {}));
		base->appendChildNode(band);

		QColor glow_clear = Theme::Instance()->Glow();
		glow_clear.setAlpha(0);
		const QGradientStops glow = {{0.0, glow_clear}, {1.0, Theme::Instance()->Glow()}};
		base->appendChildNode(CreateTextureNode(window(), CreateGradientTexture(window(), glow, Qt::Vertical), {}));
	}

	const qreal w = width();
	const qreal h = height();
	base->setRect(boundingRect());

	// Tall enough that the rotated top and bottom edges stay off screen; only the faded sides show.
	const QRectF band_rect(-w * 0.15, -h, w * 0.8, h * 3);
	auto*        band = static_cast<QSGTransformNode*>(base->lastChild()->previousSibling());
	QMatrix4x4   matrix;
	matrix.translate(static_cast<float>(band_rect.center().x()), static_cast<float>(band_rect.center().y()));
	matrix.rotate(BAND_ANGLE, 0, 0, 1);
	matrix.translate(static_cast<float>(-band_rect.center().x()), static_cast<float>(-band_rect.center().y()));
	band->setMatrix(matrix);
	static_cast<QSGImageNode*>(band->firstChild())->setRect(band_rect);

	static_cast<QSGImageNode*>(base->lastChild())->setRect(QRectF(0, h * 0.55, w, h * 0.45));

	if (band != base->firstChild() && !m_picture.isNull() && w > 0 && h > 0)
	{
		// Covers the window and crops the picture's overflow, centred.
		auto*       picture = static_cast<QSGImageNode*>(base->firstChild());
		const QSizeF source = m_picture.size();
		const qreal  scale  = std::max(w / source.width(), h / source.height());
		const QSizeF shown(w / scale, h / scale);
		picture->setRect(boundingRect());
		picture->setSourceRect(QRectF(QPointF((source.width() - shown.width()) / 2, (source.height() - shown.height()) / 2), shown));
	}
	return base;
}
