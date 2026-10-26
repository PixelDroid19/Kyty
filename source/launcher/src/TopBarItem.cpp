#include "TopBarItem.h"

#include "Accessibility.h"
#include "ArtworkSet.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QTime>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal TAB_PADDING  = 14;
constexpr qreal TAB_SPACING  = 6;
constexpr qreal ITEM_SPACING = 22;
constexpr qreal BUTTON       = 38;
constexpr qreal ICON         = 18;
constexpr qreal DOT          = 8;
constexpr qreal DOT_GAP      = 8;
constexpr qreal UNDERLINE    = 2;
constexpr qreal RING_WIDTH   = 2;
constexpr qreal ENTRY_HEIGHT = 36;

// The clock moves on the minute; this is how late it may be.
constexpr int TICK_MS = 15000;

constexpr const char* SEARCH_ICON = ":/assets/icons/search.svg";
constexpr const char* POWER_ICON  = ":/assets/icons/power.svg";

namespace {

// Holds the icon textures across frames; the entries under it are built again on every frame, as there are few.
class BarNode: public GroupNode
{
public:
	ArtworkTextures textures;
	QSGNode*        entries = nullptr;
};

} // namespace

TopBarItem::TopBarItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setActiveFocusOnTab(true);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	m_tick.setInterval(TICK_MS);
	connect(&m_tick, &QTimer::timeout, this, &TopBarItem::Refresh);
	m_tick.start();
	connect(Theme::Instance(), &Theme::UChanged, this, &TopBarItem::Layout);
	Layout();
}

void TopBarItem::SetTabs(const QStringList& tabs)
{
	m_tabs = tabs;
	Layout();
}

void TopBarItem::SetCurrentTab(int index)
{
	if (index == m_current_tab)
	{
		return;
	}
	m_current_tab = index;
	update();
}

void TopBarItem::Refresh()
{
	Layout();
}

void TopBarItem::Layout()
{
	const auto* theme   = Theme::Instance();
	const auto* backend = LauncherBackend::Instance();
	const qreal u       = theme->U();
	const auto  small   = SceneFont(theme->FontSmall());
	const qreal top     = std::round((height() - ENTRY_HEIGHT * u) / 2);
	const qreal entry_h = ENTRY_HEIGHT * u;

	QVector<Entry> entries;
	qreal          x = 0;
	for (int i = 0; i < m_tabs.size(); i++)
	{
		const qreal w = TextWidth(m_tabs.at(i), SceneFont(theme->FontBody(), QFont::DemiBold)) + 2 * TAB_PADDING * u;
		entries << Entry {Kind::Tab, m_tabs.at(i), QRectF(x, top, w, entry_h), i};
		x += w + TAB_SPACING * u;
	}

	// The right side, laid out from the right edge.
	QVector<Entry> right;
	const bool     ready = backend->IsEmulatorReady();
	right << Entry {Kind::Status, ready ? tr("Emulator ready") : tr("Emulator not found"), {}, ready ? 1 : 0};
	if (!backend->ControllerName().isEmpty())
	{
		const int battery = backend->ControllerBattery();
		QString   text    = battery >= 0 ? tr("Controller %1%").arg(battery) : backend->ControllerName();
		if (backend->ControllerCharging())
		{
			text += QStringLiteral(" ⚡");
		}
		right << Entry {Kind::Battery, text, {}, battery};
	}
	right << Entry {Kind::Search, tr("Search"), {}};
	right << Entry {Kind::Clock, QLocale().toString(QTime::currentTime(), QLocale::ShortFormat), {}};
	right << Entry {Kind::Power, tr("Power"), {}};
	qreal edge = width();
	for (auto it = right.rbegin(); it != right.rend(); ++it)
	{
		qreal w = BUTTON * u;
		if (it->kind == Kind::Status || it->kind == Kind::Battery)
		{
			w = (DOT + DOT_GAP) * u + TextWidth(it->text, small);
		} else if (it->kind == Kind::Clock)
		{
			w = TextWidth(it->text, SceneFont(theme->FontBody(), QFont::DemiBold));
		}
		it->rect = QRectF(edge - w, top, w, entry_h);
		edge -= w + ITEM_SPACING * u;
	}
	entries << right;

	const bool same_focus = m_focused >= 0 && m_focused < m_entries.size() && m_focused < entries.size() &&
	                        m_entries.at(m_focused).kind == entries.at(m_focused).kind;
	m_entries = entries;
	if (!same_focus)
	{
		m_focused = -1;
	}

	const qreal dpr   = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const qreal scale = u * dpr;
	if (scale != m_icon_scale)
	{
		m_icon_scale = scale;
		const int px = static_cast<int>(std::ceil(ICON * scale));
		m_icons[0]   = PaintIcon(SEARCH_ICON, px, theme->TextPrimary());
		m_icons[1]   = PaintIcon(POWER_ICON, px, theme->TextPrimary());
	}
	update();
}

int TopBarItem::EntryAt(QPointF point) const
{
	for (int i = 0; i < m_entries.size(); i++)
	{
		if (Focusable(m_entries.at(i)) && m_entries.at(i).rect.contains(point))
		{
			return i;
		}
	}
	return -1;
}

void TopBarItem::SetFocused(int index)
{
	m_focused = index;
	if (index >= 0)
	{
		SetAccessible(this, QAccessible::PageTabList, m_entries.at(index).text);
	}
	update();
}

void TopBarItem::Activate(int index)
{
	const auto& entry = m_entries.at(index);
	switch (entry.kind)
	{
		case Kind::Tab: emit TabRequested(entry.value); break;
		case Kind::Search: emit SearchRequested(); break;
		case Kind::Power: emit PowerRequested(); break;
		default: break;
	}
}

void TopBarItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		Layout();
	}
}

void TopBarItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged || change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
	{
		Layout();
	}
}

// The focus lands on the current page's tab, or on search when there are no tabs.
void TopBarItem::focusInEvent(QFocusEvent* event)
{
	QQuickItem::focusInEvent(event);
	if (m_focused >= 0)
	{
		return;
	}
	for (int i = 0; i < m_entries.size(); i++)
	{
		const auto& entry = m_entries.at(i);
		if ((entry.kind == Kind::Tab && entry.value == m_current_tab) || (m_tabs.isEmpty() && entry.kind == Kind::Search))
		{
			SetFocused(i);
			return;
		}
	}
}

void TopBarItem::keyPressEvent(QKeyEvent* event)
{
	const int key = event->key();
	if (key == Qt::Key_Down || key == Qt::Key_Escape)
	{
		emit DownRequested();
		return;
	}
	if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space)
	{
		if (m_focused >= 0)
		{
			Activate(m_focused);
		}
		return;
	}
	if (key != Qt::Key_Left && key != Qt::Key_Right)
	{
		event->ignore();
		return;
	}
	const int step = key == Qt::Key_Left ? -1 : 1;
	for (int i = m_focused + step; i >= 0 && i < m_entries.size(); i += step)
	{
		if (Focusable(m_entries.at(i)))
		{
			SetFocused(i);
			// Moving along the tabs shows their pages, as on the console.
			if (m_entries.at(i).kind == Kind::Tab)
			{
				Activate(i);
			}
			return;
		}
	}
}

void TopBarItem::mousePressEvent(QMouseEvent* event)
{
	const int index = EntryAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	Activate(index);
}

void TopBarItem::hoverMoveEvent(QHoverEvent* event)
{
	const int index = EntryAt(event->position());
	// Hover between the entries belongs to the page below.
	event->setAccepted(index >= 0);
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
	update();
}

void TopBarItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hovered = -1;
	unsetCursor();
	update();
}

QSGNode* TopBarItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<BarNode*>(old_node);
	if (node == nullptr)
	{
		node = new BarNode;
	}
	delete node->entries;
	node->entries = new QSGNode;
	node->appendChildNode(node->entries);

	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	const auto  small = SceneFont(theme->FontSmall());
	const auto  bold  = SceneFont(theme->FontBody(), QFont::DemiBold);

	for (int i = 0; i < m_entries.size(); i++)
	{
		const auto& entry   = m_entries.at(i);
		const auto& rect    = entry.rect;
		const bool  focused = hasActiveFocus() && i == m_focused;
		auto*       text    = window()->createTextNode();

		if (Focusable(entry))
		{
			RoundedBoxNode::Style style;
			style.fill         = i == m_hovered ? theme->Glass() : QColor(Qt::transparent);
			style.border       = theme->Accent();
			style.border_width = focused ? RING_WIDTH * u : 0;
			style.radius       = rect.height() / 2;
			style.dpr          = dpr;
			auto* box          = new RoundedBoxNode;
			box->Update(window(), rect, style);
			node->entries->appendChildNode(box);
		}
		switch (entry.kind)
		{
			case Kind::Tab:
			{
				const bool current = entry.value == m_current_tab;
				SetTextLine(text, entry.text, bold, current ? theme->TextPrimary() : theme->TextSecondary(), rect, Qt::AlignHCenter);
				if (current)
				{
					auto* line = window()->createRectangleNode();
					line->setColor(theme->Accent());
					line->setRect(QRectF(rect.left() + TAB_PADDING * u, rect.bottom() - UNDERLINE * u, rect.width() - 2 * TAB_PADDING * u,
					                     UNDERLINE * u));
					node->entries->appendChildNode(line);
				}
				break;
			}
			case Kind::Status:
			case Kind::Battery:
			{
				const bool  good = entry.kind == Kind::Status ? entry.value == 1 : entry.value < 0 || entry.value > 20;
				const qreal dot  = DOT * u;
				RoundedBoxNode::Style style;
				style.fill   = good ? theme->Ok() : theme->Danger();
				style.radius = dot / 2;
				style.dpr    = dpr;
				auto* mark   = new RoundedBoxNode;
				mark->Update(window(), QRectF(rect.left(), rect.center().y() - dot / 2, dot, dot), style);
				node->entries->appendChildNode(mark);
				SetTextLine(text, entry.text, small, theme->TextSecondary(), rect.adjusted((DOT + DOT_GAP) * u, 0, 0, 0));
				break;
			}
			case Kind::Clock: SetTextLine(text, entry.text, bold, theme->TextPrimary(), rect); break;
			case Kind::Search:
			case Kind::Power:
			{
				const char* path    = entry.kind == Kind::Search ? SEARCH_ICON : POWER_ICON;
				auto*       texture = node->textures.Get(window(), QString::fromLatin1(path), m_icons[entry.kind == Kind::Search ? 0 : 1]);
				if (texture != nullptr)
				{
					const qreal icon  = ICON * u;
					auto*       image = window()->createImageNode();
					image->setOwnsTexture(false);
					image->setFiltering(QSGTexture::Linear);
					image->setTexture(texture);
					image->setRect(QRectF(rect.center().x() - icon / 2, rect.center().y() - icon / 2, icon, icon));
					node->entries->appendChildNode(image);
				}
				break;
			}
		}
		node->entries->appendChildNode(text);
	}
	node->textures.Prune();
	return node;
}
