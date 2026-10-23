#include "SceneNodes.h"

#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

#include <algorithm>
#include <cmath>

constexpr int GRADIENT_TEXELS = 256;

QSGTexture* CreateGradientTexture(QQuickWindow* window, const QGradientStops& stops, Qt::Orientation orientation)
{
	const bool horizontal = orientation == Qt::Horizontal;
	QImage     image(horizontal ? GRADIENT_TEXELS : 1, horizontal ? 1 : GRADIENT_TEXELS, QImage::Format_ARGB32_Premultiplied);

	QLinearGradient gradient(0, 0, horizontal ? GRADIENT_TEXELS : 0, horizontal ? 0 : GRADIENT_TEXELS);
	gradient.setStops(stops);
	QPainter painter(&image);
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	painter.fillRect(image.rect(), gradient);
	painter.end();

	return window->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
}

QSGImageNode* CreateTextureNode(QQuickWindow* window, QSGTexture* texture, const QRectF& rect)
{
	auto* node = window->createImageNode();
	node->setTexture(texture);
	node->setOwnsTexture(true);
	node->setFiltering(QSGTexture::Linear);
	node->setRect(rect);
	return node;
}

RoundedBoxNode::~RoundedBoxNode()
{
	// The slices only borrow the texture; they are deleted after this, without touching it.
	delete m_texture;
}

void RoundedBoxNode::CreateSlices(QQuickWindow* window, const Style& style)
{
	const qreal border = style.border_width * style.dpr;
	const qreal radius = style.radius * style.dpr;
	m_corner_px        = std::max(1, static_cast<int>(std::ceil(std::max(radius, border))));
	const int side     = 2 * m_corner_px + 1;

	// The border lies inside the box and the fill inside the border, as Qt Quick's Rectangle draws them.
	QPainterPath outer;
	outer.addRoundedRect(QRectF(0, 0, side, side), radius, radius);
	QPainterPath inner;
	const qreal  inner_radius = std::max<qreal>(0, radius - border);
	inner.addRoundedRect(QRectF(border, border, side - 2 * border, side - 2 * border), inner_radius, inner_radius);

	QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	if (border > 0)
	{
		painter.fillPath(outer.subtracted(inner), style.border);
	}
	painter.fillPath(inner, style.fill);
	painter.end();

	delete m_texture;
	m_texture = window->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);

	if (m_slices[0] == nullptr)
	{
		for (auto*& slice: m_slices)
		{
			slice = window->createImageNode();
			slice->setOwnsTexture(false);
			appendChildNode(slice);
		}
	}

	const qreal edges[4] = {0, static_cast<qreal>(m_corner_px), static_cast<qreal>(m_corner_px + 1), static_cast<qreal>(side)};
	for (int i = 0; i < SLICE_COUNT; i++)
	{
		const int column = i % 3;
		const int row    = i / 3;
		auto*     slice  = m_slices[i];
		slice->setTexture(m_texture);
		slice->setSourceRect(QRectF(QPointF(edges[column], edges[row]), QPointF(edges[column + 1], edges[row + 1])));
		// Corners are drawn at their own size; the one-pixel edges and middle are stretched and must not blend.
		slice->setFiltering(column == 1 || row == 1 ? QSGTexture::Nearest : QSGTexture::Linear);
	}
	m_style = style;
}

void RoundedBoxNode::Update(QQuickWindow* window, const QRectF& bounds, const Style& style)
{
	if (m_texture == nullptr || !(style == m_style))
	{
		CreateSlices(window, style);
	}

	// A box smaller than two corners shrinks its corners, as a Rectangle clamps its radius.
	const qreal corner = std::min({m_corner_px / style.dpr, bounds.width() / 2, bounds.height() / 2});
	const qreal xs[4]  = {bounds.left(), bounds.left() + corner, bounds.right() - corner, bounds.right()};
	const qreal ys[4]  = {bounds.top(), bounds.top() + corner, bounds.bottom() - corner, bounds.bottom()};
	for (int i = 0; i < SLICE_COUNT; i++)
	{
		const int column = i % 3;
		const int row    = i / 3;
		m_slices[i]->setRect(QRectF(QPointF(xs[column], ys[row]), QPointF(xs[column + 1], ys[row + 1])));
	}
}
