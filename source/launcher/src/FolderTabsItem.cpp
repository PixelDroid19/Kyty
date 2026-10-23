#include "FolderTabsItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGOpacityNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QVariantMap>

#include <algorithm>

// Sizes in units.
constexpr qreal TAB_PADDING = 12;
constexpr qreal TAB_SPACING = 6;
constexpr qreal COUNT_GAP   = 8;
constexpr qreal RING_INSET  = 2;
constexpr qreal RING_WIDTH  = 2;
constexpr qreal UNDERLINE   = 2;

namespace {

class TabNode: public GroupNode
{
public:
	explicit TabNode(QQuickWindow* window)
	{
		ring_opacity = new QSGOpacityNode;
		ring         = new RoundedBoxNode;
		ring_opacity->appendChildNode(ring);
		appendChildNode(ring_opacity);
		name  = window->createTextNode();
		count = window->createTextNode();
		appendChildNode(name);
		appendChildNode(count);
		underline_opacity = new QSGOpacityNode;
		underline         = new RoundedBoxNode;
		underline_opacity->appendChildNode(underline);
		appendChildNode(underline_opacity);
	}

	QSGOpacityNode* ring_opacity      = nullptr;
	RoundedBoxNode* ring              = nullptr;
	QSGTextNode*    name              = nullptr;
	QSGTextNode*    count             = nullptr;
	QSGOpacityNode* underline_opacity = nullptr;
	RoundedBoxNode* underline         = nullptr;
	// The layout and selection the texts were set for.
	int  layout   = -1;
	bool selected = false;
};

class TabsNode: public RectClipNode
{
public:
	QSGTransformNode* strip = nullptr;
	QVector<TabNode*> tabs;
};

} // namespace

FolderTabsItem::FolderTabsItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	connect(&m_scroll, &ViewScroll::Moved, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        Layout();
		        emit TabPaddingChanged();
	        });
}

qreal FolderTabsItem::TabPadding() const
{
	return TAB_PADDING * Theme::Instance()->U();
}

void FolderTabsItem::SetFolders(const QVariantList& folders)
{
	if (folders == m_folders)
	{
		return;
	}
	// The same tab stays current when tabs come or go before it; names and counts change, the rest names a tab.
	auto identity = [](const QVariant& folder)
	{
		auto map = folder.toMap();
		map.remove("title");
		map.remove("count");
		return map;
	};
	const auto current = identity(m_folders.value(m_current));
	m_folders          = folders;
	int index          = 0;
	for (int i = 0; i < m_folders.size(); i++)
	{
		if (identity(m_folders.at(i)) == current)
		{
			index = i;
		}
	}
	const bool moved = index != m_current;
	m_current        = index;
	Layout();
	if (moved)
	{
		emit CurrentIndexChanged();
	}
}

void FolderTabsItem::SetCurrentIndex(int index)
{
	index = std::clamp(index, 0, std::max(0, static_cast<int>(m_folders.size()) - 1));
	if (index == m_current)
	{
		return;
	}
	m_current = index;
	ShowCurrent(true);
	update();
	emit CurrentIndexChanged();
}

void FolderTabsItem::Layout()
{
	const auto* theme      = Theme::Instance();
	const qreal u          = theme->U();
	const auto  name_font  = SceneFont(theme->FontBody(), QFont::DemiBold);
	const auto  count_font = SceneFont(theme->FontSmall());
	m_tabs.clear();
	qreal x = 0;
	for (const auto& value: m_folders)
	{
		const auto map = value.toMap();
		Tab        tab;
		tab.title = map.value("title").toString();
		tab.count = map.value("count").toString();
		// Measured in the selected weight, so a tab does not change width when it is selected.
		tab.name_width = TextWidth(tab.title, name_font);
		tab.width      = tab.name_width + COUNT_GAP * u + TextWidth(tab.count, count_font) + 2 * TAB_PADDING * u;
		tab.x          = x;
		x += tab.width + TAB_SPACING * u;
		m_tabs << tab;
	}
	m_layout++;
	ShowCurrent(false);
	update();
}

void FolderTabsItem::ShowCurrent(bool animated)
{
	if (m_current < 0 || m_current >= m_tabs.size())
	{
		return;
	}
	const auto& tab  = m_tabs.at(m_current);
	const auto& last = m_tabs.last();
	m_scroll.Show(tab.x, tab.x + tab.width, width(), last.x + last.width - width(), animated);
}

int FolderTabsItem::IndexAt(QPointF point) const
{
	const qreal x = point.x() + m_scroll.Position();
	for (int i = 0; i < m_tabs.size(); i++)
	{
		if (x >= m_tabs.at(i).x && x < m_tabs.at(i).x + m_tabs.at(i).width)
		{
			return i;
		}
	}
	return -1;
}

void FolderTabsItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		ShowCurrent(false);
		update();
	}
}

void FolderTabsItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	}
}

void FolderTabsItem::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Left && m_current > 0)
	{
		SetCurrentIndex(m_current - 1);
		return;
	}
	if (event->key() == Qt::Key_Right && m_current + 1 < m_tabs.size())
	{
		SetCurrentIndex(m_current + 1);
		return;
	}
	event->ignore();
}

void FolderTabsItem::mousePressEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	SetCurrentIndex(index);
	forceActiveFocus(Qt::MouseFocusReason);
}

QSGNode* FolderTabsItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<TabsNode*>(old_node);
	if (node == nullptr)
	{
		node        = new TabsNode;
		node->strip = new QSGTransformNode;
		node->appendChildNode(node->strip);
	}
	node->SetRect(boundingRect());
	QMatrix4x4 matrix;
	matrix.translate(static_cast<float>(-m_scroll.Position()), 0);
	node->strip->setMatrix(matrix);

	while (node->tabs.size() > m_tabs.size())
	{
		delete node->tabs.takeLast();
	}
	while (node->tabs.size() < m_tabs.size())
	{
		node->tabs << new TabNode(window());
		node->strip->appendChildNode(node->tabs.last());
	}

	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	const qreal h     = height();
	for (int i = 0; i < m_tabs.size(); i++)
	{
		const auto& tab      = m_tabs.at(i);
		auto*       tab_node = node->tabs.at(i);
		const bool  selected = i == m_current;
		const qreal name_x   = tab.x + TAB_PADDING * u;

		if (tab_node->layout != m_layout || tab_node->selected != selected)
		{
			const auto name_font = SceneFont(theme->FontBody(), selected ? QFont::DemiBold : QFont::Normal);
			const auto baseline  = CentredBaseline(name_font, QRectF(0, 0, 0, h));
			// The count follows the name as it is drawn, in the weight it has now.
			const qreal count_x = name_x + TextWidth(tab.title, name_font) + COUNT_GAP * u;
			SetTextAtBaseline(tab_node->name, tab.title, name_font, selected ? theme->TextPrimary() : theme->TextSecondary(),
			                  QPointF(name_x, baseline), tab.name_width + 1);
			SetTextAtBaseline(tab_node->count, tab.count, SceneFont(theme->FontSmall()), theme->TextMuted(), QPointF(count_x, baseline),
			                  tab.width);
			tab_node->layout   = m_layout;
			tab_node->selected = selected;
		}

		const bool ring = selected && hasActiveFocus();
		tab_node->ring_opacity->setOpacity(ring ? 1 : 0);
		if (ring)
		{
			RoundedBoxNode::Style style;
			style.fill         = Qt::transparent;
			style.border       = theme->Accent();
			style.border_width = RING_WIDTH * u;
			style.radius       = theme->RadiusSmall();
			style.dpr          = dpr;
			tab_node->ring->Update(window(), QRectF(tab.x, RING_INSET * u, tab.width, h - 2 * RING_INSET * u), style);
		}

		// On the bottom edge, in line with the search and view switch underlines.
		tab_node->underline_opacity->setOpacity(selected ? 1 : 0);
		if (selected)
		{
			RoundedBoxNode::Style style;
			style.fill        = theme->Accent();
			style.radius      = UNDERLINE * u / 2;
			style.dpr         = dpr;
			const qreal right = tab.x + tab.width - TAB_PADDING * u;
			tab_node->underline->Update(window(), QRectF(name_x, h - UNDERLINE * u, right - name_x, UNDERLINE * u), style);
		}
	}
	return node;
}
