#include "SidebarItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTextNode>

#include <cmath>

// Sizes in units.
constexpr qreal BUTTON       = 60;
constexpr qreal SPACING      = 12;
constexpr qreal DISC         = 46;
constexpr qreal ICON         = 24;
constexpr qreal RING_WIDTH   = 3;
constexpr qreal BOTTOM       = 40;
constexpr qreal LABEL_GAP    = 10;
constexpr qreal LABEL_PAD    = 20;
constexpr qreal LABEL_HEIGHT = 30;

namespace {

enum class Group
{
	Top,
	Bottom
};

struct Entry
{
	const char* page; // The page whose button this is, or nullptr for an action.
	const char* icon;
	const char* label;
	Group       group;
	int         slot; // Place inside its group.
};

// In keyboard order, top to bottom.
const Entry ENTRIES[] = {
    {"library", ":/qt/qml/Kyty/Launcher/qml/icons/library.svg", QT_TRANSLATE_NOOP("SidebarItem", "Library"), Group::Top, 0},
    {nullptr, ":/qt/qml/Kyty/Launcher/qml/icons/add.svg", QT_TRANSLATE_NOOP("SidebarItem", "Add game"), Group::Top, 1},
    {"settings", ":/qt/qml/Kyty/Launcher/qml/icons/settings.svg", QT_TRANSLATE_NOOP("SidebarItem", "Settings"), Group::Top, 2},
    {"about", ":/qt/qml/Kyty/Launcher/qml/icons/info.svg", QT_TRANSLATE_NOOP("SidebarItem", "About and controls"), Group::Bottom, 0},
    {nullptr, ":/qt/qml/Kyty/Launcher/qml/icons/power.svg", QT_TRANSLATE_NOOP("SidebarItem", "Quit"), Group::Bottom, 1},
};
constexpr int ENTRY_COUNT    = static_cast<int>(std::size(ENTRIES));
constexpr int BOTTOM_ENTRIES = 2;

QImage PaintIcon(const char* path, int px)
{
	QImageReader reader(QString::fromLatin1(path));
	reader.setScaledSize(QSize(px, px));
	auto image = reader.read();
	if (image.isNull())
	{
		qWarning("Sidebar icon %s: %s", path, qPrintable(reader.errorString()));
	}
	return image;
}

class EntryNode: public GroupNode
{
public:
	explicit EntryNode(QQuickWindow* window)
	{
		disc = new RoundedBoxNode;
		appendChildNode(disc);
		label_opacity = new QSGOpacityNode;
		pill          = new RoundedBoxNode;
		label         = window->createTextNode();
		label_opacity->appendChildNode(pill);
		label_opacity->appendChildNode(label);
		appendChildNode(label_opacity);
	}

	RoundedBoxNode* disc          = nullptr;
	QSGImageNode*   icon          = nullptr;
	QSGOpacityNode* label_opacity = nullptr;
	RoundedBoxNode* pill          = nullptr;
	QSGTextNode*    label         = nullptr;
};

class RailNode: public GroupNode
{
public:
	ArtworkTextures     textures;
	QVector<EntryNode*> entries;
};

} // namespace

SidebarItem::SidebarItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);
	m_icons.resize(ENTRY_COUNT);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        polish();
		        update();
	        });
}

void SidebarItem::SetCurrent(const QString& current)
{
	if (current == m_current)
	{
		return;
	}
	m_current = current;
	update();
	emit CurrentChanged();
}

void SidebarItem::SetRightTarget(QQuickItem* target)
{
	if (target == m_right_target)
	{
		return;
	}
	m_right_target = target;
	emit RightTargetChanged();
}

void SidebarItem::SetEntriesTop(qreal top)
{
	if (top == m_entries_top)
	{
		return;
	}
	m_entries_top = top;
	update();
	emit EntriesTopChanged();
}

QRectF SidebarItem::EntryRect(int index) const
{
	const qreal u     = Theme::Instance()->U();
	const qreal size  = BUTTON * u;
	const qreal pitch = size + SPACING * u;
	const auto& entry = ENTRIES[index];
	const qreal x     = (width() - size) / 2;
	if (entry.group == Group::Top)
	{
		return {x, m_entries_top + entry.slot * pitch, size, size};
	}
	const qreal bottom_top = height() - BOTTOM * u - BOTTOM_ENTRIES * pitch + SPACING * u;
	return {x, bottom_top + entry.slot * pitch, size, size};
}

int SidebarItem::EntryAt(QPointF point) const
{
	for (int i = 0; i < ENTRY_COUNT; i++)
	{
		if (EntryRect(i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

int SidebarItem::CurrentEntry() const
{
	for (int i = 0; i < ENTRY_COUNT; i++)
	{
		if (ENTRIES[i].page != nullptr && m_current == QLatin1String(ENTRIES[i].page))
		{
			return i;
		}
	}
	return 0;
}

void SidebarItem::Activate(int index)
{
	switch (index)
	{
		case 0: emit libraryRequested(); break;
		case 1: emit addGameRequested(); break;
		case 2: emit settingsRequested(); break;
		case 3: emit aboutRequested(); break;
		case 4: emit quitRequested(); break;
		default: break;
	}
}

void SidebarItem::SetHovered(int index)
{
	if (index != m_hovered)
	{
		m_hovered = index;
		update();
	}
}

void SidebarItem::updatePolish()
{
	const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const int   px  = static_cast<int>(std::ceil(ICON * Theme::Instance()->U() * dpr));
	if (px == m_icon_px)
	{
		return;
	}
	m_icon_px = px;
	for (int i = 0; i < ENTRY_COUNT; i++)
	{
		m_icons[i] = PaintIcon(ENTRIES[i].icon, px);
	}
	update();
}

void SidebarItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

void SidebarItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	} else if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
	{
		polish();
	}
}

void SidebarItem::focusInEvent(QFocusEvent* event)
{
	QQuickItem::focusInEvent(event);
	// Coming from a page, the focus lands on that page's button; a click keeps the button it hit.
	if (event->reason() != Qt::MouseFocusReason)
	{
		m_focused = CurrentEntry();
	}
}

void SidebarItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up:
			if (m_focused > 0)
			{
				m_focused--;
				update();
				return;
			}
			break;
		case Qt::Key_Down:
			if (m_focused + 1 < ENTRY_COUNT)
			{
				m_focused++;
				update();
				return;
			}
			break;
		case Qt::Key_Right:
			if (m_right_target != nullptr)
			{
				m_right_target->forceActiveFocus(Qt::TabFocusReason);
				return;
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Activate(m_focused); return;
		default: break;
	}
	event->ignore();
}

void SidebarItem::mousePressEvent(QMouseEvent* event)
{
	m_pressed = EntryAt(event->position());
	if (m_pressed < 0)
	{
		event->ignore();
	}
}

void SidebarItem::mouseReleaseEvent(QMouseEvent* event)
{
	const int index   = EntryAt(event->position());
	const int pressed = m_pressed;
	m_pressed         = -1;
	if (index < 0 || index != pressed)
	{
		return;
	}
	m_focused = index;
	forceActiveFocus(Qt::MouseFocusReason);
	update();
	Activate(index);
}

void SidebarItem::hoverMoveEvent(QHoverEvent* event)
{
	SetHovered(EntryAt(event->position()));
}

void SidebarItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	SetHovered(-1);
}

QSGNode* SidebarItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RailNode*>(old_node);
	if (node == nullptr)
	{
		node = new RailNode;
		for (int i = 0; i < ENTRY_COUNT; i++)
		{
			node->entries << new EntryNode(window());
			node->appendChildNode(node->entries.last());
		}
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	const auto  font  = SceneFont(theme->FontSmall());

	for (int i = 0; i < ENTRY_COUNT; i++)
	{
		auto*       entry   = node->entries.at(i);
		const auto  cell    = EntryRect(i);
		const auto  centre  = cell.center();
		const bool  focused = hasActiveFocus() && i == m_focused;
		const bool  hovered = i == m_hovered;
		const bool  marked  = ENTRIES[i].page != nullptr && m_current == QLatin1String(ENTRIES[i].page);
		const qreal disc    = DISC * u;

		// Only the current page and the pointer get a disc; the focus ring stays for keyboard and controller.
		RoundedBoxNode::Style style;
		style.fill         = marked ? theme->GlassStrong() : (hovered ? theme->Glass() : QColor(Qt::transparent));
		style.border       = theme->Accent();
		style.border_width = focused ? RING_WIDTH * u : 0;
		style.radius       = disc / 2;
		style.dpr          = dpr;
		entry->disc->Update(window(), QRectF(centre.x() - disc / 2, centre.y() - disc / 2, disc, disc), style);

		auto* texture = node->textures.Get(window(), QString::fromLatin1(ENTRIES[i].icon), m_icons.at(i));
		if (texture == nullptr)
		{
			delete entry->icon;
			entry->icon = nullptr;
		} else
		{
			if (entry->icon == nullptr)
			{
				entry->icon = window()->createImageNode();
				entry->icon->setOwnsTexture(false);
				entry->icon->setFiltering(QSGTexture::Linear);
				// The software renderer reads the texture as soon as the node joins a shown tree.
				entry->icon->setTexture(texture);
				entry->insertChildNodeAfter(entry->icon, entry->disc);
			}
			if (entry->icon->texture() != texture)
			{
				entry->icon->setTexture(texture);
			}
			const qreal icon = ICON * u;
			entry->icon->setRect(QRectF(centre.x() - icon / 2, centre.y() - icon / 2, icon, icon));
		}

		const bool labelled = hovered || focused;
		entry->label_opacity->setOpacity(labelled ? 1 : 0);
		if (labelled)
		{
			const auto   text = tr(ENTRIES[i].label);
			const QRectF pill(cell.right() + LABEL_GAP * u, centre.y() - LABEL_HEIGHT * u / 2, TextWidth(text, font) + LABEL_PAD * u,
			                  LABEL_HEIGHT * u);
			RoundedBoxNode::Style pill_style;
			// Opaque, because the label opens over the page's artwork.
			pill_style.fill         = theme->Surface();
			pill_style.border       = theme->GlassBorder();
			pill_style.border_width = 1;
			pill_style.radius       = pill.height() / 2;
			pill_style.dpr          = dpr;
			entry->pill->Update(window(), pill, pill_style);
			SetTextLine(entry->label, text, font, theme->TextPrimary(), pill, Qt::AlignHCenter);
		}
	}
	node->textures.Prune();
	return node;
}
