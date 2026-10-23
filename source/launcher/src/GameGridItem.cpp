#include "GameGridItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// Sizes in units. Covers are at least MIN_TILE wide unless the owner sets another width; each cell pads its cover
// by TILE_PADDING.
constexpr qreal MIN_TILE     = 128;
constexpr qreal TILE_PADDING = 24;
// Room below a cover for its name, and above it for the focus ring around the grown cover.
constexpr qreal LABEL_ROOM = 60;
// Room below a cover without its name.
constexpr qreal BARE_ROOM  = 28;
constexpr qreal COVER_TOP  = 12;
constexpr qreal LABEL_GAP  = 14;
constexpr qreal HOVER_ZOOM = 1.03;
constexpr qreal FOCUS_ZOOM = 1.06;
// Wheel notches scroll this share of a row each.
constexpr qreal WHEEL_ROW_SHARE = 0.5;

namespace {

// The card of a game without artwork, picked by its place in the library so it keeps its colour.
QColor CoverColor(int row)
{
	static const QColor colors[] = {QColor(0x6f, 0x8f, 0xd6), QColor(0xc9, 0x6f, 0xa0), QColor(0x6f, 0xb8, 0x9a),
	                                QColor(0xd9, 0xa3, 0x5a), QColor(0x8f, 0x78, 0xd6), QColor(0x5f, 0xa6, 0xc9)};
	return colors[std::max(0, row) % std::size(colors)];
}

// What a tile's texts depend on; they are laid out again only when it changes.
struct TileContent
{
	GameCard card;
	bool     current = false;
	bool     title   = true;
	qreal    tile    = 0;
	qreal    u       = 0;
	qreal    dpr     = 1;

	bool operator==(const TileContent& other) const
	{
		return card == other.card && current == other.current && title == other.title && tile == other.tile && u == other.u &&
		       dpr == other.dpr;
	}
};

// One visible cell: the cover and its name, placed by a translation so that scrolling only moves it.
class TileNode: public QSGTransformNode
{
public:
	explicit TileNode(QQuickWindow* window): m_window(window)
	{
		cover = new CoverNode;
		label = window->createTextNode();
		appendChildNode(cover);
		appendChildNode(label);
	}

	void SetContent(const TileContent& content)
	{
		if (has_content && content == m_content)
		{
			return;
		}
		m_content   = content;
		has_content = true;

		const auto*  theme = Theme::Instance();
		const auto   font  = SceneFont(theme->FontSmall());
		const QRectF label_rect(0, content.tile + LABEL_GAP * content.u, content.tile, QFontMetricsF(font).height());
		SetTextLine(label, content.title ? content.card.title : QString(), font,
		            content.current ? theme->TextPrimary() : theme->TextSecondary(), label_rect, Qt::AlignHCenter);

		auto* overlay = cover->Overlay();
		while (auto* child = overlay->firstChild())
		{
			delete child;
		}
		const QRectF card(0, 0, content.tile, content.tile);
		if (content.card.icon_id.isEmpty())
		{
			auto* initials = m_window->createTextNode();
			SetTextLine(initials, Initials(content.card.title), SceneFont(content.tile * 0.3, QFont::DemiBold), theme->TextOnArt(), card,
			            Qt::AlignHCenter);
			overlay->appendChildNode(initials);
		}
		if (content.card.running)
		{
			AddRunningBadge(overlay, card, content.u, content.dpr);
		}
	}

	CoverNode*   cover       = nullptr;
	QSGTextNode* label       = nullptr;
	bool         has_content = false;

private:
	void AddRunningBadge(QSGNode* overlay, const QRectF& card, qreal u, qreal dpr)
	{
		const auto*  theme  = Theme::Instance();
		const auto   font   = SceneFont(theme->FontSmall());
		const auto   text   = GameGridItem::tr("Running");
		const qreal  width  = TextWidth(text, font) + 20 * u;
		const qreal  height = 24 * u;
		const QRectF pill(card.center().x() - width / 2, card.bottom() - 8 * u - height, width, height);

		auto*                 badge = new RoundedBoxNode;
		RoundedBoxNode::Style style;
		style.fill   = QColor::fromRgbF(0, 0, 0, 0.7F);
		style.radius = height / 2;
		style.dpr    = dpr;
		badge->Update(m_window, pill, style);
		overlay->appendChildNode(badge);

		auto* label_node = m_window->createTextNode();
		SetTextLine(label_node, text, font, theme->TextOnArt(), pill, Qt::AlignHCenter);
		overlay->appendChildNode(label_node);
	}

	QQuickWindow* m_window;
	TileContent   m_content;
};

class GridNode: public GroupNode
{
public:
	ArtworkTextures    textures;
	QVector<TileNode*> tiles;
};

} // namespace

GameGridItem::GameGridItem(QQuickItem* parent): QQuickItem(parent), m_zoom(FOCUS_ZOOM, this), m_hover_zoom(HOVER_ZOOM, this)
{
	m_min_tile = MIN_TILE;
	setFlag(ItemHasContents);
	setClip(true);
	setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
	setAcceptHoverEvents(true);

	connect(&m_scroll, &ViewScroll::Moved, this, &GameGridItem::Changed);
	connect(&m_art, &ArtworkSet::Loaded, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        ShowCurrent(false);
		        Changed();
	        });
}

void GameGridItem::SetCoverLook(qreal min_tile, bool titles)
{
	m_min_tile = min_tile;
	m_titles   = titles;
	polish();
	update();
}

void GameGridItem::SetGames(const QVariantList& games)
{
	m_games    = games;
	auto cards = GameCardsFromList(games);
	if (cards != m_cards)
	{
		m_cards = std::move(cards);
		// Keeps the selection inside the list when games are filtered out or removed.
		if (m_current >= m_cards.size())
		{
			SetCurrentIndex(std::max(0, static_cast<int>(m_cards.size()) - 1));
		}
		ShowCurrent(false);
		Changed();
	}
}

void GameGridItem::SetCurrentIndex(int index)
{
	index = std::clamp(index, 0, std::max(0, static_cast<int>(m_cards.size()) - 1));
	if (index == m_current)
	{
		return;
	}
	m_current = index;
	m_zoom.SetFocused(hasActiveFocus() ? m_current : -1);
	ShowCurrent(true);
	Changed();
	emit CurrentIndexChanged();
}

GameGridItem::Metrics GameGridItem::LayoutMetrics() const
{
	const qreal u       = Theme::Instance()->U();
	const qreal padding = TILE_PADDING * u;
	Metrics     metrics;
	metrics.columns     = std::max(1, static_cast<int>(std::floor(width() / (m_min_tile * u + padding))));
	metrics.cell_width  = std::floor(width() / metrics.columns);
	metrics.tile        = std::max<qreal>(0, metrics.cell_width - padding);
	metrics.cell_height = metrics.tile + (m_titles ? LABEL_ROOM : BARE_ROOM) * u;
	return metrics;
}

// The cover of index in content coordinates, before scrolling.
QRectF GameGridItem::TileRect(int index, const Metrics& metrics) const
{
	const int column = index % metrics.columns;
	const int row    = index / metrics.columns;
	return {column * metrics.cell_width + (metrics.cell_width - metrics.tile) / 2,
	        row * metrics.cell_height + COVER_TOP * Theme::Instance()->U(), metrics.tile, metrics.tile};
}

int GameGridItem::IndexAt(QPointF point) const
{
	const auto metrics = LayoutMetrics();
	const int  column  = static_cast<int>(point.x() / metrics.cell_width);
	const int  row     = static_cast<int>((point.y() + m_scroll.Position()) / metrics.cell_height);
	const int  index   = row * metrics.columns + column;
	if (column < 0 || column >= metrics.columns || index < 0 || index >= m_cards.size())
	{
		return -1;
	}
	return TileRect(index, metrics).contains(point + QPointF(0, m_scroll.Position())) ? index : -1;
}

qreal GameGridItem::MaxContentY() const
{
	const auto metrics = LayoutMetrics();
	const auto rows    = (m_cards.size() + metrics.columns - 1) / metrics.columns;
	return std::max<qreal>(0, rows * metrics.cell_height - height());
}

// Scrolls just enough to show the whole cell of the current game.
void GameGridItem::ShowCurrent(bool animated)
{
	const auto  metrics = LayoutMetrics();
	const qreal top     = (m_current / metrics.columns) * metrics.cell_height;
	m_scroll.Show(top, top + metrics.cell_height, height(), MaxContentY(), animated);
}

void GameGridItem::Changed()
{
	polish();
	update();
}

void GameGridItem::updatePolish()
{
	// A hidden view keeps what it decoded, so its covers are there at once when its page comes back.
	if (!isVisible())
	{
		return;
	}
	if (m_cards.isEmpty() || width() <= 0)
	{
		m_art.Keep({}, {});
		return;
	}
	// The rows in view and one more on each side, so a short scroll finds its artwork decoded.
	const auto  metrics = LayoutMetrics();
	const int   first   = std::max(0, static_cast<int>(m_scroll.Position() / metrics.cell_height) - 1) * metrics.columns;
	const int   last    = std::min(static_cast<int>(m_cards.size()),
	                               (static_cast<int>((m_scroll.Position() + height()) / metrics.cell_height) + 2) * metrics.columns);
	QStringList ids;
	for (int i = first; i < last; i++)
	{
		ids << m_cards.at(i).icon_id;
	}
	const qreal dpr  = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const int   side = static_cast<int>(std::ceil(metrics.tile * dpr));
	m_art.Keep(ids, QSize(side, side));
}

void GameGridItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		ShowCurrent(false);
		Changed();
	}
}

void GameGridItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		m_zoom.SetFocused(hasActiveFocus() ? m_current : -1);
		update();
	} else if (change == ItemVisibleHasChanged)
	{
		Changed();
	}
}

void GameGridItem::keyPressEvent(QKeyEvent* event)
{
	const int columns = LayoutMetrics().columns;
	const int count   = static_cast<int>(m_cards.size());
	int       target  = -1;
	switch (event->key())
	{
		case Qt::Key_Left: target = m_current % columns != 0 ? m_current - 1 : -1; break;
		case Qt::Key_Right: target = m_current + 1 < count ? m_current + 1 : -1; break;
		case Qt::Key_Up: target = m_current - columns; break;
		case Qt::Key_Down:
			// From a full row onto a shorter last row, the last game.
			if (m_current + columns < count)
			{
				target = m_current + columns;
			} else if (m_current / columns < (count - 1) / columns)
			{
				target = count - 1;
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			if (count > 0)
			{
				emit Activated(m_current);
			}
			return;
		default: event->ignore(); return;
	}
	if (target < 0 || target >= count)
	{
		event->ignore();
		return;
	}
	SetCurrentIndex(target);
}

void GameGridItem::hoverMoveEvent(QHoverEvent* event)
{
	m_hover_zoom.SetFocused(IndexAt(event->position()));
}

void GameGridItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hover_zoom.SetFocused(-1);
}

void GameGridItem::mousePressEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	SetCurrentIndex(index);
	forceActiveFocus(Qt::MouseFocusReason);
	if (event->button() == Qt::RightButton)
	{
		emit MenuRequested(index);
	}
}

void GameGridItem::mouseDoubleClickEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index >= 0 && event->button() == Qt::LeftButton)
	{
		emit Activated(index);
	}
}

void GameGridItem::wheelEvent(QWheelEvent* event)
{
	const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
	                                                  : event->angleDelta().y() / 120.0 * LayoutMetrics().cell_height * WHEEL_ROW_SHARE;
	m_scroll.ScrollTo(m_scroll.Position() - delta, MaxContentY(), false);
}

QSGNode* GameGridItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<GridNode*>(old_node);
	if (node == nullptr)
	{
		node = new GridNode;
	}
	const auto  metrics = LayoutMetrics();
	const qreal u       = Theme::Instance()->U();
	const qreal dpr     = window()->effectiveDevicePixelRatio();
	const int   first   = static_cast<int>(m_scroll.Position() / metrics.cell_height) * metrics.columns;
	const int   last    = metrics.tile <= 0
	                          ? first
	                          : std::min(static_cast<int>(m_cards.size()),
	                                     (static_cast<int>((m_scroll.Position() + height()) / metrics.cell_height) + 1) * metrics.columns);
	const int   shown   = std::max(0, last - first);

	while (node->tiles.size() > shown)
	{
		delete node->tiles.takeLast();
	}
	while (node->tiles.size() < shown)
	{
		node->tiles << new TileNode(window());
		node->appendChildNode(node->tiles.last());
	}

	for (int i = 0; i < shown; i++)
	{
		const int   index = first + i;
		const auto& card  = m_cards.at(index);
		auto*       tile  = node->tiles.at(i);
		const auto  rect  = TileRect(index, metrics);

		QMatrix4x4 matrix;
		matrix.translate(static_cast<float>(rect.x()), static_cast<float>(rect.y() - m_scroll.Position()));
		tile->setMatrix(matrix);

		TileContent content;
		content.card    = card;
		content.current = index == m_current;
		content.tile    = metrics.tile;
		content.title   = m_titles;
		content.u       = u;
		content.dpr     = dpr;
		tile->SetContent(content);

		CoverNode::Look look;
		look.rect = QRectF(0, 0, metrics.tile, metrics.tile);
		look.fill = card.icon_id.isEmpty() ? CoverColor(card.row) : Theme::Instance()->Glass();
		look.zoom = std::max(m_zoom.Zoom(index), m_hover_zoom.Zoom(index));
		look.ring = hasActiveFocus() && index == m_current;
		look.dpr  = dpr;
		tile->cover->Update(window(), look, node->textures.Get(window(), card.icon_id, m_art.Find(card.icon_id)));
	}
	node->textures.Prune();
	return node;
}
