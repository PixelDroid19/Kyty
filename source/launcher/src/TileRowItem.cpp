#include "TileRowItem.h"

#include "Accessibility.h"
#include "CoverNode.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal LARGE_TILE    = 150;
constexpr qreal SMALL_TILE    = 96;
constexpr qreal TILE_SPACING  = 14;
constexpr qreal CAPTION_GAP   = 10;
constexpr qreal CAPTION_LINES = 1.5;
// Room kept around the tiles for the focus ring.
constexpr qreal RING_ROOM = 16;
// Tiles kept decoded past the last one on screen, so a quick move finds them ready.
constexpr int KEEP_AHEAD = 4;

namespace {

class TileNode: public GroupNode
{
public:
	explicit TileNode(QQuickWindow* window)
	{
		cover = new CoverNode;
		appendChildNode(cover);
		label = window->createTextNode();
		cover->Overlay()->appendChildNode(label);
	}

	CoverNode*   cover = nullptr;
	QSGTextNode* label = nullptr;
};

class RowNode: public RectClipNode
{
public:
	explicit RowNode(QQuickWindow* window)
	{
		caption = window->createTextNode();
		appendChildNode(caption);
	}

	ArtworkTextures    textures;
	QVector<TileNode*> tiles;
	QSGTextNode*       caption = nullptr;
};

qreal CaptionHeight()
{
	return std::ceil(Theme::Instance()->FontBody() * CAPTION_LINES);
}

} // namespace

TileRowItem::TileRowItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope, false);
	setActiveFocusOnTab(true);
	setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
	connect(&m_slide, &ViewScroll::Moved, this, &TileRowItem::Changed);
	connect(&m_art, &ArtworkSet::Loaded, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::UChanged, this, &TileRowItem::Changed);
}

qreal TileRowItem::RowHeight()
{
	const qreal u = Theme::Instance()->U();
	return LARGE_TILE * u + CAPTION_GAP * u + CaptionHeight();
}

void TileRowItem::SetGames(const QVariantList& games)
{
	m_games    = games;
	auto cards = GameCardsFromList(games);
	if (cards == m_cards)
	{
		return;
	}
	m_cards = std::move(cards);
	if (m_current >= TileCount())
	{
		SetCurrentIndex(TileCount() - 1);
	}
	m_slide.ScrollTo(m_current, TileCount(), false);
	Changed();
}

void TileRowItem::SetCurrentIndex(int index)
{
	index = std::clamp(index, 0, TileCount() - 1);
	if (index == m_current)
	{
		return;
	}
	m_current = index;
	m_slide.ScrollTo(m_current, TileCount(), true);
	Changed();
	emit CurrentIndexChanged();
}

QRectF TileRowItem::TileRect(int index) const
{
	const qreal u     = Theme::Instance()->U();
	const qreal large = LARGE_TILE * u;
	const qreal small = SMALL_TILE * u;
	const qreal pitch = small + TILE_SPACING * u;
	// Distance from the sliding selection: the selected tile is large and each tile grows as it nears it.
	const qreal from  = index - m_slide.Position();
	const qreal size  = small + (large - small) * std::max<qreal>(0, 1 - std::abs(from));
	const qreal left  = from * pitch + (large - small) * std::clamp<qreal>(from, 0, 1);
	// Small tiles line up with the large one's bottom edge.
	return {left, large - size, size, size};
}

int TileRowItem::IndexAt(QPointF point) const
{
	for (int i = 0; i < TileCount(); i++)
	{
		if (TileRect(i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

void TileRowItem::Changed()
{
	SetAccessible(this, QAccessible::List, m_current < m_cards.size() ? m_cards.at(m_current).title : tr("Add game"));
	polish();
	update();
}

void TileRowItem::updatePolish()
{
	if (!isVisible() || m_cards.isEmpty())
	{
		m_art.Keep({}, {});
		return;
	}
	QStringList ids;
	const int   first = std::max(0, m_current - 1);
	for (int i = first; i < m_cards.size() && TileRect(i).left() < width() + KEEP_AHEAD * SMALL_TILE * Theme::Instance()->U(); i++)
	{
		ids << m_cards.at(i).icon_id;
	}
	const qreal dpr  = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const int   side = static_cast<int>(std::ceil(LARGE_TILE * Theme::Instance()->U() * dpr));
	m_art.Keep(ids, QSize(side, side));
}

void TileRowItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		Changed();
	}
}

void TileRowItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged || change == ItemVisibleHasChanged)
	{
		Changed();
	}
}

void TileRowItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Left:
			if (m_current > 0)
			{
				SetCurrentIndex(m_current - 1);
				return;
			}
			break;
		case Qt::Key_Right:
			if (m_current + 1 < TileCount())
			{
				SetCurrentIndex(m_current + 1);
				return;
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter: emit Activated(m_current); return;
		default: break;
	}
	event->ignore();
}

void TileRowItem::mousePressEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	forceActiveFocus(Qt::MouseFocusReason);
	if (event->button() == Qt::RightButton)
	{
		if (index < m_cards.size())
		{
			emit MenuRequested(index);
		}
		return;
	}
	// The first click selects, a click on the selected tile opens it.
	if (index == m_current)
	{
		emit Activated(index);
		return;
	}
	SetCurrentIndex(index);
}

void TileRowItem::wheelEvent(QWheelEvent* event)
{
	const auto angle = event->angleDelta();
	const int  delta = angle.x() != 0 ? angle.x() : angle.y();
	if (delta != 0)
	{
		SetCurrentIndex(m_current + (delta < 0 ? 1 : -1));
	}
}

QSGNode* TileRowItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RowNode*>(old_node);
	if (node == nullptr)
	{
		node = new RowNode(window());
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	const qreal room  = RING_ROOM * u;
	// Tiles before the selection leave on the left edge; the ring of the large one still fits.
	node->SetRect(QRectF(-room, -room, width() + 2 * room, height() + 2 * room));

	QVector<int> shown;
	for (int i = 0; i < TileCount(); i++)
	{
		// Tiles before the selection are gone once they leave the row's left edge.
		const auto rect = TileRect(i);
		if (rect.right() > 0 && rect.left() < width() + room)
		{
			shown << i;
		}
	}
	while (node->tiles.size() > shown.size())
	{
		delete node->tiles.takeLast();
	}
	while (node->tiles.size() < shown.size())
	{
		node->tiles << new TileNode(window());
		node->appendChildNode(node->tiles.last());
	}

	for (int i = 0; i < shown.size(); i++)
	{
		const int   index = shown.at(i);
		const auto  rect  = TileRect(index);
		auto*       tile  = node->tiles.at(i);
		const bool  add   = index == m_cards.size();
		const auto& id    = add ? QString() : m_cards.at(index).icon_id;

		CoverNode::Look look;
		look.rect = rect;
		look.fill = add ? theme->Glass() : theme->Surface();
		look.ring = hasActiveFocus() && index == m_current;
		look.dpr  = dpr;
		tile->cover->Update(window(), look, node->textures.Get(window(), id, m_art.Find(id)));

		tile->label->clear();
		if (add || id.isEmpty())
		{
			const auto text = add ? QStringLiteral("+") : Initials(m_cards.at(index).title);
			SetTextLine(tile->label, text, SceneFont(rect.height() * 0.3, QFont::DemiBold), theme->TextPrimary(), rect, Qt::AlignHCenter);
		}
	}

	const auto caption = m_current < m_cards.size() ? m_cards.at(m_current).title : tr("Add game");
	const auto large   = LARGE_TILE * u;
	SetTextLine(node->caption, caption, SceneFont(theme->FontBody(), QFont::DemiBold), theme->TextPrimary(),
	            QRectF(0, large + CAPTION_GAP * u, std::max(large, width() / 2), CaptionHeight()));
	node->textures.Prune();
	return node;
}
