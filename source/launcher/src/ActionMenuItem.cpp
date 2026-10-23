#include "ActionMenuItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal PANEL_WIDTH = 440;
constexpr qreal PADDING     = 20;
constexpr qreal SPACING     = 10;
constexpr qreal ROW_HEIGHT  = 44;
constexpr qreal ROW_PITCH   = 48;
constexpr qreal LABEL_INSET = 18;

constexpr qreal HEADING_SCALE      = 0.7;
constexpr qreal DISALLOWED_OPACITY = 0.4;

namespace {

class MenuNode: public GroupNode
{
public:
	explicit MenuNode(QQuickWindow* window)
	{
		scrim = window->createRectangleNode();
		appendChildNode(scrim);
		panel = new RoundedBoxNode;
		appendChildNode(panel);
		highlight = new RoundedBoxNode;
		appendChildNode(highlight);
		texts = new GroupNode;
		appendChildNode(texts);
	}

	QSGRectangleNode* scrim     = nullptr;
	RoundedBoxNode*   panel     = nullptr;
	RoundedBoxNode*   highlight = nullptr;
	GroupNode*        texts     = nullptr;
};

QFont HeadingFont()
{
	return SceneFont(Theme::Instance()->FontTitle() * HEADING_SCALE, QFont::DemiBold);
}

QFont NoteFont()
{
	return SceneFont(Theme::Instance()->FontSmall());
}

QFont LabelFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

bool Allowed(const QVariant& action)
{
	return action.toMap().value("allowed", true).toBool();
}

QColor Faded(QColor color, bool allowed)
{
	if (!allowed)
	{
		color.setAlphaF(color.alphaF() * DISALLOWED_OPACITY);
	}
	return color;
}

} // namespace

ActionMenuItem::ActionMenuItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::AllButtons);
	setAcceptHoverEvents(true);
	setVisible(false);
	setZ(10);
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::update);
}

void ActionMenuItem::SetShown(bool shown)
{
	if (shown == m_shown)
	{
		return;
	}
	m_shown = shown;
	setVisible(shown);
	emit ShownChanged();
}

void ActionMenuItem::OpenFor(const QVariantMap& game, int row, const QVariantList& items)
{
	m_row        = row;
	m_heading    = game.value("title").toString();
	m_heading    = m_heading.isEmpty() ? game.value("name").toString() : m_heading;
	m_actions    = items;
	m_confirming = false;
	m_current    = 0;
	SetShown(true);
	forceActiveFocus(Qt::OtherFocusReason);
	update();
}

void ActionMenuItem::Close()
{
	m_confirming = false;
	SetShown(false);
}

void ActionMenuItem::Activate(int index)
{
	if (index < 0 || index >= m_actions.size() || !Allowed(m_actions.at(index)))
	{
		return;
	}
	const auto action = m_actions.at(index).toMap().value("action").toString();
	if (action == QLatin1String("close"))
	{
		Close();
		return;
	}
	if (action == QLatin1String("delete"))
	{
		m_confirming = true;
		m_actions    = {QVariantMap {{"label", tr("Remove from library")}, {"action", "confirm-delete"}, {"danger", true}},
		                QVariantMap {{"label", tr("Cancel")}, {"action", "close"}}};
		m_current    = 1;
		update();
		return;
	}
	Close();
	emit Chosen(action == QLatin1String("confirm-delete") ? QStringLiteral("delete") : action, m_row);
}

ActionMenuItem::Geometry ActionMenuItem::Layout() const
{
	const qreal u       = Theme::Instance()->U();
	const qreal inner   = PANEL_WIDTH * u - 2 * PADDING * u;
	const qreal spacing = SPACING * u;
	Geometry    geometry;
	qreal       y    = PADDING * u;
	geometry.heading = QRectF(PADDING * u, y, inner, std::ceil(QFontMetricsF(HeadingFont()).height()));
	y += geometry.heading.height() + spacing;
	if (m_confirming)
	{
		geometry.note = QRectF(PADDING * u, y, inner, TextBlockHeight(tr("Game files on disk are not deleted."), NoteFont(), inner));
		y += geometry.note.height() + spacing;
	}
	geometry.rows = QRectF(PADDING * u, y, inner, ROW_HEIGHT * u);
	y += m_actions.size() * ROW_PITCH * u + PADDING * u;

	// Built at the panel's top left, then moved to the centre.
	const QPointF offset(std::round((width() - PANEL_WIDTH * u) / 2), std::round((height() - y) / 2));
	geometry.panel = QRectF(offset, QSizeF(PANEL_WIDTH * u, y));
	geometry.heading.translate(offset);
	geometry.note.translate(offset);
	geometry.rows.translate(offset);
	return geometry;
}

QRectF ActionMenuItem::RowRect(const Geometry& geometry, int index) const
{
	return geometry.rows.translated(0, index * ROW_PITCH * Theme::Instance()->U());
}

int ActionMenuItem::RowAt(QPointF point) const
{
	const auto geometry = Layout();
	for (int i = 0; i < m_actions.size(); i++)
	{
		if (RowRect(geometry, i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

void ActionMenuItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

void ActionMenuItem::keyPressEvent(QKeyEvent* event)
{
	// The menu is modal: no key reaches the page below while it is open.
	switch (event->key())
	{
		case Qt::Key_Up: m_current = std::max(m_current - 1, 0); break;
		case Qt::Key_Down: m_current = std::min(m_current + 1, static_cast<int>(m_actions.size()) - 1); break;
		case Qt::Key_Return:
		case Qt::Key_Enter: Activate(m_current); return;
		case Qt::Key_Escape:
		case Qt::Key_Menu: Close(); return;
		default: return;
	}
	update();
}

void ActionMenuItem::mousePressEvent(QMouseEvent* /*event*/) {}

void ActionMenuItem::mouseReleaseEvent(QMouseEvent* event)
{
	const int index = event->button() == Qt::LeftButton ? RowAt(event->position()) : -1;
	if (index >= 0)
	{
		m_current = index;
		update();
		Activate(index);
		return;
	}
	if (!Layout().panel.contains(event->position()))
	{
		Close();
	}
}

// Accepting hover keeps the page below from reacting to the pointer.
void ActionMenuItem::hoverMoveEvent(QHoverEvent* /*event*/) {}

void ActionMenuItem::wheelEvent(QWheelEvent* /*event*/) {}

QSGNode* ActionMenuItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<MenuNode*>(old_node);
	if (node == nullptr)
	{
		node = new MenuNode(window());
	}
	const auto* theme    = Theme::Instance();
	const qreal u        = theme->U();
	const qreal dpr      = window()->effectiveDevicePixelRatio();
	const auto  geometry = Layout();

	node->scrim->setRect(boundingRect());
	node->scrim->setColor(theme->Scrim());

	RoundedBoxNode::Style panel;
	panel.fill         = theme->Panel();
	panel.border       = theme->GlassBorder();
	panel.border_width = 1;
	panel.radius       = theme->Radius();
	panel.dpr          = dpr;
	node->panel->Update(window(), geometry.panel, panel);

	const bool            current_allowed = m_current < m_actions.size() && Allowed(m_actions.at(m_current));
	RoundedBoxNode::Style highlight;
	highlight.fill   = Faded(theme->GlassStrong(), current_allowed);
	highlight.radius = ROW_HEIGHT * u / 2;
	highlight.dpr    = dpr;
	node->highlight->Update(window(), RowRect(geometry, m_current), highlight);

	while (auto* child = node->texts->firstChild())
	{
		node->texts->removeChildNode(child);
		delete child;
	}
	auto add_text = [&](auto set)
	{
		auto* text = window()->createTextNode();
		set(text);
		node->texts->appendChildNode(text);
	};
	add_text([&](QSGTextNode* text) { SetTextLine(text, m_heading, HeadingFont(), theme->TextPrimary(), geometry.heading); });
	if (m_confirming)
	{
		add_text([&](QSGTextNode* text)
		         { SetTextBlock(text, tr("Game files on disk are not deleted."), NoteFont(), theme->TextSecondary(), geometry.note); });
	}
	for (int i = 0; i < m_actions.size(); i++)
	{
		const auto   action = m_actions.at(i).toMap();
		const auto   color  = Faded(action.value("danger").toBool() ? theme->Danger() : theme->TextPrimary(), Allowed(action));
		const QRectF rect   = RowRect(geometry, i).adjusted(LABEL_INSET * u, 0, 0, 0);
		add_text([&](QSGTextNode* text) { SetTextLine(text, action.value("label").toString(), LabelFont(), color, rect); });
	}
	return node;
}
