#include "PageItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal RING_WIDTH  = 3;
constexpr qreal SHOW_MARGIN = 16;
constexpr qreal WHEEL_STEP  = 48;

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

class PageNode: public GroupNode
{
public:
	PageNode()
	{
		clip = new RectClipNode;
		appendChildNode(clip);
		scroll = new QSGTransformNode;
		clip->appendChildNode(scroll);
		statics = new GroupNode;
		scroll->appendChildNode(statics);
		buttons = new GroupNode;
		scroll->appendChildNode(buttons);
	}

	RectClipNode*        clip    = nullptr;
	QSGTransformNode*    scroll  = nullptr;
	GroupNode*           statics = nullptr;
	GroupNode*           buttons = nullptr;
	QVector<ButtonNode*> button_nodes;
	int                  generation = -1;
};

QColor Faded(QColor color, bool allowed)
{
	if (!allowed)
	{
		color.setAlphaF(color.alphaF() * DISALLOWED_OPACITY);
	}
	return color;
}

} // namespace

PageItem::PageItem(qreal max_width, qreal top, qreal spacing, QQuickItem* parent)
    : QQuickItem(parent), m_max_width(max_width), m_top(top), m_spacing(spacing)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	connect(Theme::Instance(), &Theme::UChanged, this, &PageItem::Recompose);
	connect(&m_scroll, &ViewScroll::Moved, this,
	        [this]()
	        {
		        update();
		        Placed();
	        });
}

void PageItem::focusDefault()
{
	FocusButton(0);
	forceActiveFocus(Qt::OtherFocusReason);
}

void PageItem::Recompose()
{
	m_dirty = true;
	polish();
}

QPointF PageItem::ContentOrigin() const
{
	const qreal u = Theme::Instance()->U();
	return {Theme::Instance()->Margin(), m_top * u - m_scroll.Position()};
}

qreal PageItem::Viewport() const
{
	return std::max<qreal>(0, height() - m_top * Theme::Instance()->U() - Theme::Instance()->Margin());
}

qreal PageItem::ScrollLimit() const
{
	return std::max<qreal>(0, m_layout.Height() - Viewport());
}

void PageItem::updatePolish()
{
	if (!m_dirty)
	{
		return;
	}
	m_dirty                   = false;
	const auto* theme         = Theme::Instance();
	const qreal u             = theme->U();
	const qreal content_width = std::max<qreal>(0, std::min(width() - 2 * theme->Margin(), m_max_width * u));
	PageLayout  layout(content_width, u, m_spacing * u);
	Compose(&layout);
	m_layout = layout;
	m_generation++;
	m_focused = std::clamp(m_focused, 0, std::max(0, static_cast<int>(m_layout.Buttons().size()) - 1));
	m_hovered = -1;
	m_pressed = -1;
	m_scroll.ScrollTo(m_scroll.Position(), ScrollLimit(), false);
	update();
	Placed();
}

void PageItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		Recompose();
	}
}

void PageItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	} else if (change == ItemSceneChange)
	{
		Recompose();
	}
}

int PageItem::ButtonAt(QPointF point) const
{
	if (point.y() < m_top * Theme::Instance()->U() || point.y() > height() - Theme::Instance()->Margin())
	{
		return -1;
	}
	const auto  content = point - ContentOrigin();
	const auto& buttons = m_layout.Buttons();
	for (int i = 0; i < buttons.size(); i++)
	{
		if (buttons.at(i).rect.contains(content))
		{
			return i;
		}
	}
	return -1;
}

int PageItem::ButtonInRow(int direction) const
{
	const auto& buttons = m_layout.Buttons();
	if (m_focused >= buttons.size())
	{
		return -1;
	}
	const int   row    = buttons.at(m_focused).row + direction;
	const qreal centre = buttons.at(m_focused).rect.center().x();
	int         best   = -1;
	for (int i = 0; i < buttons.size(); i++)
	{
		if (buttons.at(i).row != row)
		{
			continue;
		}
		if (best < 0 || std::abs(buttons.at(i).rect.center().x() - centre) < std::abs(buttons.at(best).rect.center().x() - centre))
		{
			best = i;
		}
	}
	return best;
}

void PageItem::FocusButton(int index)
{
	const auto& buttons = m_layout.Buttons();
	if (index < 0 || index >= buttons.size())
	{
		return;
	}
	m_focused        = index;
	const auto& rect = buttons.at(index).rect;
	const qreal u    = Theme::Instance()->U();
	// The first and last rows bring the page's ends into view, so the title shows when the focus is back on top.
	const int   last_row = buttons.last().row;
	const qreal start    = buttons.at(index).row == 0 ? 0 : rect.top() - SHOW_MARGIN * u;
	const qreal end      = buttons.at(index).row == last_row ? m_layout.Height() : rect.bottom() + SHOW_MARGIN * u;
	m_scroll.Show(start, end, Viewport(), ScrollLimit(), isVisible());
	update();
}

void PageItem::PressButton(int index)
{
	const auto& buttons = m_layout.Buttons();
	if (index < 0 || index >= buttons.size() || !buttons.at(index).spec.allowed)
	{
		return;
	}
	Press(buttons.at(index).spec.id);
}

void PageItem::keyPressEvent(QKeyEvent* event)
{
	const auto& buttons = m_layout.Buttons();
	int         target  = -1;
	switch (event->key())
	{
		case Qt::Key_Up: target = ButtonInRow(-1); break;
		case Qt::Key_Down: target = ButtonInRow(1); break;
		case Qt::Key_Left:
			if (m_focused > 0 && m_focused < buttons.size() && buttons.at(m_focused - 1).row == buttons.at(m_focused).row)
			{
				target = m_focused - 1;
			}
			break;
		case Qt::Key_Right:
			if (m_focused + 1 < buttons.size() && buttons.at(m_focused + 1).row == buttons.at(m_focused).row)
			{
				target = m_focused + 1;
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: PressButton(m_focused); return;
		case Qt::Key_Escape: emit closed(); return;
		default: break;
	}
	if (target < 0)
	{
		// Past the page's edge, so KeyNavigation can move the focus to the sidebar.
		event->ignore();
		return;
	}
	FocusButton(target);
}

void PageItem::mousePressEvent(QMouseEvent* event)
{
	m_pressed = ButtonAt(event->position());
	if (m_pressed < 0)
	{
		event->ignore();
	}
}

void PageItem::mouseReleaseEvent(QMouseEvent* event)
{
	const int index   = ButtonAt(event->position());
	const int pressed = m_pressed;
	m_pressed         = -1;
	if (index < 0 || index != pressed)
	{
		return;
	}
	m_focused = index;
	forceActiveFocus(Qt::MouseFocusReason);
	update();
	PressButton(index);
}

void PageItem::hoverMoveEvent(QHoverEvent* event)
{
	const int index = ButtonAt(event->position());
	if (index == m_hovered)
	{
		return;
	}
	m_hovered = index;
	if (index >= 0)
	{
		setCursor(Qt::PointingHandCursor);
	} else
	{
		unsetCursor();
	}
}

void PageItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hovered = -1;
	unsetCursor();
}

void PageItem::wheelEvent(QWheelEvent* event)
{
	const qreal step  = WHEEL_STEP * Theme::Instance()->U();
	const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().y() : event->angleDelta().y() / 120.0 * step;
	m_scroll.ScrollTo(m_scroll.Position() - delta, ScrollLimit(), false);
}

QSGNode* PageItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<PageNode*>(old_node);
	if (node == nullptr)
	{
		node = new PageNode;
	}
	const auto* theme  = Theme::Instance();
	const qreal u      = theme->U();
	const qreal dpr    = window()->effectiveDevicePixelRatio();
	const auto  origin = ContentOrigin();

	node->clip->SetRect(QRectF(0, m_top * u, width(), Viewport()));
	QMatrix4x4 matrix;
	matrix.translate(static_cast<float>(origin.x()), static_cast<float>(origin.y()));
	node->scroll->setMatrix(matrix);

	const auto& buttons = m_layout.Buttons();
	if (node->generation != m_generation)
	{
		node->generation = m_generation;
		while (auto* child = node->statics->firstChild())
		{
			node->statics->removeChildNode(child);
			delete child;
		}
		for (const auto& box: m_layout.Boxes())
		{
			auto* box_node = new RoundedBoxNode;
			node->statics->appendChildNode(box_node);
			RoundedBoxNode::Style style;
			style.fill         = box.fill;
			style.border       = box.border;
			style.border_width = box.border.isValid() ? 1 : 0;
			style.radius       = box.radius;
			style.dpr          = dpr;
			box_node->Update(window(), box.rect, style);
		}
		for (const auto& text: m_layout.Texts())
		{
			auto* text_node = window()->createTextNode();
			SetTextBlock(text_node, text.text, text.font, text.color, text.rect, text.alignment, text.wrap);
			node->statics->appendChildNode(text_node);
		}
		for (auto* button: node->button_nodes)
		{
			node->buttons->removeChildNode(button);
			delete button;
		}
		node->button_nodes.clear();
		for (int i = 0; i < buttons.size(); i++)
		{
			node->button_nodes << new ButtonNode(window());
			node->buttons->appendChildNode(node->button_nodes.last());
		}
	}

	for (int i = 0; i < buttons.size(); i++)
	{
		const auto& button  = buttons.at(i);
		auto*       target  = node->button_nodes.at(i);
		const bool  focused = hasActiveFocus() && i == m_focused;

		RoundedBoxNode::Style style;
		style.fill         = Faded(button.spec.primary ? theme->GlassStrong() : theme->Glass(), button.spec.allowed);
		style.border       = Faded(focused ? theme->Accent() : theme->GlassBorder(), button.spec.allowed);
		style.border_width = focused ? RING_WIDTH * u : 1;
		style.radius       = button.rect.height() / 2;
		style.dpr          = dpr;
		target->pill->Update(window(), button.rect, style);

		const auto color = Faded(button.spec.danger ? theme->Danger() : theme->TextPrimary(), button.spec.allowed);
		SetTextLine(target->label, button.spec.text, PageLayout::ButtonFont(button.spec.primary), color, button.rect, Qt::AlignHCenter);
	}
	return node;
}
