#include "GlassPanelItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QQuickWindow>

GlassPanelItem::GlassPanelItem(QQuickItem* parent): QQuickItem(parent), m_color(Theme::Instance()->Glass())
{
	setFlag(ItemHasContents);
	// The corner radius follows the window scale.
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::update);
}

void GlassPanelItem::SetColor(const QColor& color)
{
	if (color == m_color)
	{
		return;
	}
	m_color = color;
	update();
	emit ColorChanged();
}

void GlassPanelItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

QSGNode* GlassPanelItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RoundedBoxNode*>(old_node);
	if (node == nullptr)
	{
		node = new RoundedBoxNode;
	}
	RoundedBoxNode::Style style;
	style.fill         = m_color;
	style.border       = Theme::Instance()->GlassBorder();
	style.border_width = 1;
	style.radius       = Theme::Instance()->Radius();
	style.dpr          = window()->effectiveDevicePixelRatio();
	node->Update(window(), boundingRect(), style);
	return node;
}
