#include "SceneNodes.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGTexture>
#include <QTextLayout>

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

	// The slices' materials still point at the old texture; it goes only after they have the new one.
	auto* old_texture = m_texture;
	m_texture         = window->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);

	if (m_slices[0] == nullptr)
	{
		for (auto*& slice: m_slices)
		{
			slice = window->createImageNode();
			slice->setOwnsTexture(false);
			// The software renderer reads the texture as soon as the node joins a shown tree.
			slice->setTexture(m_texture);
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
	delete old_texture;
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

RectClipNode::RectClipNode(): m_geometry(QSGGeometry::defaultAttributes_Point2D(), 4)
{
	setGeometry(&m_geometry);
	setIsRectangular(true);
}

void RectClipNode::SetRect(const QRectF& rect)
{
	setClipRect(rect);
	QSGGeometry::updateRectGeometry(&m_geometry, rect);
	markDirty(DirtyGeometry);
}

QFont SceneFont(qreal pixel_size, QFont::Weight weight)
{
	QFont font = QGuiApplication::font();
	font.setPixelSize(std::max(1, qRound(pixel_size)));
	font.setWeight(weight);
	return font;
}

QFont SceneMonospaceFont(qreal pixel_size)
{
	QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	font.setPixelSize(std::max(1, qRound(pixel_size)));
	return font;
}

qreal TextWidth(const QString& text, const QFont& font)
{
	return QFontMetricsF(font).horizontalAdvance(text);
}

namespace {

// Lays out text on one line elided to width and adds it to node with its baseline at baseline_y; left_edge maps
// the laid-out line's width to its left edge.
template <typename LeftEdge>
void AddLine(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, qreal width, qreal baseline_y,
             LeftEdge left_edge)
{
	node->clear();
	if (text.isEmpty() || width <= 0)
	{
		return;
	}
	QTextLayout layout(QFontMetricsF(font).elidedText(text, Qt::ElideRight, width), font);
	layout.beginLayout();
	QTextLine line = layout.createLine();
	line.setLineWidth(width);
	line.setPosition(QPointF(0, 0));
	layout.endLayout();

	node->setColor(color);
	node->addTextLayout(QPointF(std::round(left_edge(line.naturalTextWidth())), std::round(baseline_y - line.ascent())), &layout);
}

// Breaks the layout's text into lines no wider than width, each placed by alignment; returns the block's height.
qreal BreakLines(QTextLayout* layout, qreal width, Qt::Alignment alignment, QTextOption::WrapMode wrap)
{
	QTextOption option;
	option.setWrapMode(wrap);
	layout->setTextOption(option);
	layout->beginLayout();
	qreal y = 0;
	for (auto line = layout->createLine(); line.isValid(); line = layout->createLine())
	{
		line.setLineWidth(width);
		qreal x = 0;
		if ((alignment & Qt::AlignHCenter) != 0)
		{
			x = (width - line.naturalTextWidth()) / 2;
		} else if ((alignment & Qt::AlignRight) != 0)
		{
			x = width - line.naturalTextWidth();
		}
		line.setPosition(QPointF(std::round(x), std::round(y)));
		y += line.height();
	}
	layout->endLayout();
	return y;
}

} // namespace

qreal TextBlockHeight(const QString& text, const QFont& font, qreal width, QTextOption::WrapMode wrap)
{
	if (text.isEmpty() || width <= 0)
	{
		return 0;
	}
	QTextLayout layout(text, font);
	return std::ceil(BreakLines(&layout, width, Qt::AlignLeft, wrap));
}

void SetTextBlock(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, const QRectF& rect,
                  Qt::Alignment alignment, QTextOption::WrapMode wrap)
{
	node->clear();
	if (text.isEmpty() || rect.width() <= 0)
	{
		return;
	}
	QTextLayout layout(text, font);
	BreakLines(&layout, rect.width(), alignment, wrap);
	node->setColor(color);
	node->addTextLayout(QPointF(std::round(rect.left()), std::round(rect.top())), &layout);
}

qreal CentredBaseline(const QFont& font, const QRectF& rect)
{
	const QFontMetricsF metrics(font);
	return rect.top() + (rect.height() - metrics.height()) / 2 + metrics.ascent();
}

void SetTextLine(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, const QRectF& rect,
                 Qt::Alignment alignment)
{
	AddLine(node, text, font, color, rect.width(), CentredBaseline(font, rect),
	        [&](qreal line_width)
	        {
		        if ((alignment & Qt::AlignHCenter) != 0)
		        {
			        return rect.left() + (rect.width() - line_width) / 2;
		        }
		        if ((alignment & Qt::AlignRight) != 0)
		        {
			        return rect.right() - line_width;
		        }
		        return rect.left();
	        });
}

void SetTextAtBaseline(QSGTextNode* node, const QString& text, const QFont& font, const QColor& color, QPointF origin, qreal max_width)
{
	AddLine(node, text, font, color, max_width, origin.y(), [&](qreal /*line_width*/) { return origin.x(); });
}
