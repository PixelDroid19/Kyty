#include "CoverNode.h"

#include "Theme.h"

#include <QMatrix4x4>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTransformNode>

#include <algorithm>

// Width of the focus ring in units.
constexpr qreal RING_WIDTH = 3;

CoverNode::CoverNode()
{
	m_zoom    = new QSGTransformNode;
	m_fill    = new RoundedBoxNode;
	m_overlay = new GroupNode;
	m_edge    = new RoundedBoxNode;
	m_zoom->appendChildNode(m_fill);
	m_zoom->appendChildNode(m_overlay);
	m_zoom->appendChildNode(m_edge);
	appendChildNode(m_zoom);

	m_ring_opacity = new QSGOpacityNode;
	m_ring         = new RoundedBoxNode;
	m_ring_opacity->appendChildNode(m_ring);
	appendChildNode(m_ring_opacity);
}

void CoverNode::Update(QQuickWindow* window, const Look& look, QSGTexture* art)
{
	const auto* theme  = Theme::Instance();
	const qreal radius = theme->ArtCorner() * std::min(look.rect.width(), look.rect.height());

	QMatrix4x4 matrix;
	const auto centre = look.rect.center();
	const auto zoom   = static_cast<float>(look.zoom);
	matrix.translate(static_cast<float>(centre.x()), static_cast<float>(centre.y()));
	matrix.scale(zoom, zoom);
	matrix.translate(static_cast<float>(-centre.x()), static_cast<float>(-centre.y()));
	m_zoom->setMatrix(matrix);

	RoundedBoxNode::Style fill;
	fill.fill   = look.fill;
	fill.radius = radius;
	fill.dpr    = look.dpr;
	m_fill->Update(window, look.rect, fill);

	if (art == nullptr)
	{
		delete m_art;
		m_art = nullptr;
	} else
	{
		if (m_art == nullptr)
		{
			m_art = window->createImageNode();
			m_art->setOwnsTexture(false);
			m_art->setFiltering(QSGTexture::Linear);
			// The software renderer reads the texture as soon as the node joins a shown tree.
			m_art->setTexture(art);
			m_zoom->insertChildNodeAfter(m_art, m_fill);
		}
		// The artwork comes already cut to the card's shape, so it fills the card as it is.
		if (m_art->texture() != art)
		{
			m_art->setTexture(art);
		}
		m_art->setRect(look.rect);
	}

	// A hairline edge, so dark artwork does not melt into a dark backdrop.
	RoundedBoxNode::Style edge;
	edge.fill         = Qt::transparent;
	edge.border       = theme->ArtEdge();
	edge.border_width = 1;
	edge.radius       = radius;
	edge.dpr          = look.dpr;
	m_edge->Update(window, look.rect, edge);

	m_ring_opacity->setOpacity(look.ring ? 1 : 0);
	if (look.ring)
	{
		// Outside the art and following its corners, so the picture cannot cover it.
		const qreal           gap    = theme->FocusGap();
		const qreal           half_w = look.rect.width() * look.zoom / 2 + gap;
		const qreal           half_h = look.rect.height() * look.zoom / 2 + gap;
		RoundedBoxNode::Style ring;
		ring.fill         = Qt::transparent;
		ring.border       = theme->Accent();
		ring.border_width = RING_WIDTH * theme->U();
		ring.radius       = radius * look.zoom + gap;
		ring.dpr          = look.dpr;
		m_ring->Update(window, QRectF(centre.x() - half_w, centre.y() - half_h, 2 * half_w, 2 * half_h), ring);
	}
}

FocusZoom::FocusZoom(qreal grown, QQuickItem* item): m_grown(grown)
{
	m_animation.setStartValue(0.0);
	m_animation.setEndValue(1.0);
	m_animation.setDuration(Theme::Instance()->Animation());
	connect(&m_animation, &QVariantAnimation::valueChanged, this,
	        [this, item](const QVariant& value)
	        {
		        m_progress = value.toReal();
		        item->update();
	        });
}

void FocusZoom::SetFocused(int index)
{
	if (index == m_focused)
	{
		return;
	}
	m_previous = m_focused;
	m_focused  = index;
	m_progress = 0;
	m_animation.stop();
	m_animation.start();
}

qreal FocusZoom::Zoom(int index) const
{
	if (index == m_focused)
	{
		return 1 + (m_grown - 1) * m_progress;
	}
	if (index == m_previous)
	{
		return m_grown - (m_grown - 1) * m_progress;
	}
	return 1;
}
