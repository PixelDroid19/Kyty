#include "SettingsRailItem.h"

#include "Accessibility.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGTexture>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal ENTRY_HEIGHT  = 54;
constexpr qreal ENTRY_SPACING = 6;
constexpr qreal ENTRY_PADDING = 12;
constexpr qreal ICON          = 32;
constexpr qreal ICON_GAP      = 12;

namespace {

class EntryNode: public GroupNode
{
public:
	explicit EntryNode(QQuickWindow* window)
	{
		backing = new RoundedBoxNode;
		appendChildNode(backing);
		icon = window->createImageNode();
		icon->setOwnsTexture(false);
		icon->setFiltering(QSGTexture::Linear);
		title = window->createTextNode();
		appendChildNode(title);
	}
	~EntryNode() override { delete texture; }

	KYTY_QT_CLASS_NO_COPY(EntryNode);

	// The software renderer reads the texture as soon as the node joins a shown tree, so the icon joins once it has one.
	void SetIcon(QQuickWindow* window, const QImage& image)
	{
		auto* next = window->createTextureFromImage(image);
		icon->setTexture(next);
		delete texture;
		texture = next;
		if (icon->parent() == nullptr)
		{
			appendChildNode(icon);
		}
	}

	RoundedBoxNode* backing = nullptr;
	QSGImageNode*   icon    = nullptr;
	QSGTextNode*    title   = nullptr;
	QSGTexture*     texture = nullptr;
};

class RailNode: public GroupNode
{
public:
	int generation = -1;
};

} // namespace

SettingsRailItem::SettingsRailItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);

	m_clock.setStartValue(0.0);
	m_clock.setEndValue(1.0);
	m_clock.setDuration(1000);
	m_clock.setLoopCount(-1);
	connect(&m_clock, &QVariantAnimation::valueChanged, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::LiveChanged, this, &SettingsRailItem::UpdateClock);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        m_generation++;
		        update();
	        });
	m_since.start();
}

void SettingsRailItem::SetEntries(const QVector<Entry>& entries)
{
	m_entries = entries;
	m_current = std::clamp<int>(m_current, 0, std::max<int>(0, static_cast<int>(entries.size()) - 1));
	m_generation++;
	SetAccessible(this, QAccessible::PageTabList, m_entries.value(m_current).title);
	UpdateClock();
	update();
}

void SettingsRailItem::SetCurrent(int index)
{
	if (index < 0 || index >= m_entries.size() || index == m_current)
	{
		return;
	}
	m_current = index;
	SetAccessible(this, QAccessible::PageTabList, m_entries.value(m_current).title);
	// The newly selected icon starts its animation from its rest pose.
	m_since.restart();
	m_generation++;
	update();
	emit CurrentChanged();
}

qreal SettingsRailItem::ContentHeight() const
{
	const qreal u = Theme::Instance()->U();
	return m_entries.isEmpty() ? 0 : m_entries.size() * (ENTRY_HEIGHT + ENTRY_SPACING) * u - ENTRY_SPACING * u;
}

QRectF SettingsRailItem::EntryRect(int index) const
{
	const qreal u = Theme::Instance()->U();
	return {0, index * (ENTRY_HEIGHT + ENTRY_SPACING) * u, width(), ENTRY_HEIGHT * u};
}

int SettingsRailItem::EntryAt(QPointF point) const
{
	for (int i = 0; i < m_entries.size(); i++)
	{
		if (EntryRect(i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

void SettingsRailItem::UpdateClock()
{
	const bool moving = Theme::Instance()->Live() && isVisible() && !m_entries.isEmpty();
	if (moving == (m_clock.state() == QAbstractAnimation::Running))
	{
		return;
	}
	if (moving)
	{
		m_since.restart();
		m_clock.start();
	} else
	{
		m_clock.stop();
	}
	update();
}

void SettingsRailItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemVisibleHasChanged)
	{
		UpdateClock();
	}
	if (change == ItemActiveFocusHasChanged)
	{
		m_generation++;
		update();
	}
}

void SettingsRailItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up: SetCurrent(m_current - 1); return;
		case Qt::Key_Down: SetCurrent(m_current + 1); return;
		case Qt::Key_Right:
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: emit Entered(); return;
		default: break;
	}
	event->ignore();
}

void SettingsRailItem::mousePressEvent(QMouseEvent* event)
{
	m_pressed = EntryAt(event->position());
	if (m_pressed < 0)
	{
		event->ignore();
		return;
	}
	forceActiveFocus(Qt::MouseFocusReason);
	SetCurrent(m_pressed);
}

void SettingsRailItem::mouseReleaseEvent(QMouseEvent* event)
{
	if (m_pressed >= 0 && m_pressed == EntryAt(event->position()))
	{
		emit Entered();
	}
	m_pressed = -1;
}

void SettingsRailItem::hoverMoveEvent(QHoverEvent* event)
{
	const int hovered = EntryAt(event->position());
	if (hovered != m_hovered)
	{
		m_hovered = hovered;
		m_generation++;
		update();
	}
}

void SettingsRailItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	if (m_hovered >= 0)
	{
		m_hovered = -1;
		m_generation++;
		update();
	}
}

QSGNode* SettingsRailItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RailNode*>(old_node);
	if (node == nullptr)
	{
		node = new RailNode;
	}
	const auto* theme   = Theme::Instance();
	const qreal u       = theme->U();
	const qreal dpr     = window()->effectiveDevicePixelRatio();
	const int   icon_px = static_cast<int>(std::ceil(ICON * u * dpr));
	const qreal now     = m_clock.state() == QAbstractAnimation::Running ? static_cast<qreal>(m_since.elapsed()) / 1000 : 0;

	const bool rebuild = node->generation != m_generation;
	if (rebuild)
	{
		node->generation = m_generation;
		while (auto* child = node->firstChild())
		{
			node->removeChildNode(child);
			delete child;
		}
	}
	for (int i = 0; i < m_entries.size(); i++)
	{
		const bool   current = i == m_current;
		const QColor ink     = current ? theme->TextPrimary() : theme->TextSecondary();
		const QRectF rect    = EntryRect(i);
		const QRectF icon_rect(rect.left() + ENTRY_PADDING * u, std::round(rect.center().y() - ICON * u / 2), ICON * u, ICON * u);
		if (!rebuild)
		{
			// Between rebuilds only the selected icon moves.
			if (current && now > 0)
			{
				auto* entry = static_cast<EntryNode*>(node->childAtIndex(i));
				entry->SetIcon(window(), PaintSettingsIcon(m_entries.at(i).icon, icon_px, now, ink, theme->Accent()));
			}
			continue;
		}
		auto* entry = new EntryNode(window());
		node->appendChildNode(entry);

		RoundedBoxNode::Style style;
		style.radius = theme->RadiusSmall();
		style.dpr    = dpr;
		if (current)
		{
			style.fill = theme->GlassStrong();
		} else if (i == m_hovered)
		{
			style.fill = theme->Glass();
		} else
		{
			style.fill = Qt::transparent;
		}
		if (current && hasActiveFocus())
		{
			style.border       = theme->Accent();
			style.border_width = 2;
		}
		entry->backing->Update(window(), rect, style);

		const QRectF title_rect(icon_rect.right() + ICON_GAP * u, rect.top(),
		                        std::max<qreal>(0, rect.right() - ENTRY_PADDING * u - icon_rect.right() - ICON_GAP * u), rect.height());
		SetTextLine(entry->title, m_entries.at(i).title, SceneFont(theme->FontBody(), current ? QFont::DemiBold : QFont::Normal), ink,
		            title_rect);
		entry->SetIcon(window(), PaintSettingsIcon(m_entries.at(i).icon, icon_px, current ? now : 0, ink, theme->Accent()));
		entry->icon->setRect(icon_rect);
	}
	return node;
}
