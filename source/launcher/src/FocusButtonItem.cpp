#include "FocusButtonItem.h"

#include "PageLayout.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>

#include <cmath>

// Sizes in units.
constexpr qreal RING_WIDTH = 3;

constexpr qreal DISALLOWED_OPACITY = 0.4;

namespace {

class ButtonNode: public GroupNode
{
public:
	explicit ButtonNode(QQuickWindow* window)
	{
		pill = new RoundedBoxNode;
		appendChildNode(pill);
		label = window->createTextNode();
		appendChildNode(label);
	}

	RoundedBoxNode* pill  = nullptr;
	QSGTextNode*    label = nullptr;
};

} // namespace

FocusButtonItem::FocusButtonItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	setCursor(Qt::PointingHandCursor);
	connect(Theme::Instance(), &Theme::UChanged, this, &FocusButtonItem::Resize);
	connect(this, &FocusButtonItem::Changed, this, &FocusButtonItem::Resize);
	Resize();
}

void FocusButtonItem::SetText(const QString& text)
{
	if (text != m_text)
	{
		m_text = text;
		emit Changed();
	}
}

void FocusButtonItem::SetPrimary(bool primary)
{
	if (primary != m_primary)
	{
		m_primary = primary;
		emit Changed();
	}
}

void FocusButtonItem::SetDanger(bool danger)
{
	if (danger != m_danger)
	{
		m_danger = danger;
		emit Changed();
	}
}

void FocusButtonItem::SetAllowed(bool allowed)
{
	if (allowed != m_allowed)
	{
		m_allowed = allowed;
		setOpacity(allowed ? 1 : DISALLOWED_OPACITY);
		emit Changed();
	}
}

void FocusButtonItem::Resize()
{
	const qreal u = Theme::Instance()->U();
	setImplicitSize(std::ceil(TextWidth(m_text, PageLayout::ButtonFont(m_primary)) + PageLayout::ButtonPadding(u)),
	                PageLayout::ButtonHeight(u));
	update();
}

void FocusButtonItem::Activate()
{
	if (m_allowed)
	{
		emit Activated();
	}
}

void FocusButtonItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	}
}

void FocusButtonItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Activate(); return;
		default: event->ignore(); return;
	}
}

void FocusButtonItem::mousePressEvent(QMouseEvent* event)
{
	if (!m_allowed)
	{
		event->ignore();
		return;
	}
	m_pressed = true;
	update();
}

void FocusButtonItem::mouseUngrabEvent()
{
	m_pressed = false;
	update();
}

void FocusButtonItem::hoverEnterEvent(QHoverEvent* /*event*/)
{
	m_hovered = true;
	update();
}

void FocusButtonItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hovered = false;
	update();
}

void FocusButtonItem::mouseReleaseEvent(QMouseEvent* event)
{
	m_pressed = false;
	update();
	if (!contains(event->position()))
	{
		return;
	}
	forceActiveFocus(Qt::MouseFocusReason);
	Activate();
}

QSGNode* FocusButtonItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<ButtonNode*>(old_node);
	if (node == nullptr)
	{
		node = new ButtonNode(window());
	}
	const auto* theme   = Theme::Instance();
	const bool  focused = hasActiveFocus();

	RoundedBoxNode::Style style;
	const bool            lit = m_allowed && (m_hovered || m_pressed);
	if (m_pressed)
	{
		style.fill = theme->GlassBorder();
	} else
	{
		style.fill = m_primary || lit ? theme->GlassStrong() : theme->Glass();
	}
	style.border       = focused ? theme->Accent() : (lit ? theme->TextMuted() : theme->GlassBorder());
	style.border_width = focused ? RING_WIDTH * theme->U() : 1;
	style.radius       = height() / 2;
	style.dpr          = window()->effectiveDevicePixelRatio();
	node->pill->Update(window(), boundingRect(), style);

	SetTextLine(node->label, m_text, PageLayout::ButtonFont(m_primary), m_danger ? theme->Danger() : theme->TextPrimary(), boundingRect(),
	            Qt::AlignHCenter);
	return node;
}
