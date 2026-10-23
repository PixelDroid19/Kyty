#include "GameTableItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal HEADER_HEIGHT = 20;
constexpr qreal HEADER_GAP    = 6;
constexpr qreal ROW_HEIGHT    = 56;
constexpr qreal ROW_SPACING   = 2;
constexpr qreal THUMB_WIDTH   = 66;
constexpr qreal THUMB_SIZE    = 44;
constexpr qreal THUMB_X       = 6;
constexpr qreal SERIAL_WIDTH  = 120;
constexpr qreal REGION_WIDTH  = 80;
constexpr qreal VERSION_WIDTH = 110;
constexpr qreal NAME_GAP      = 12;
constexpr qreal RING_WIDTH    = 2;
constexpr int   COLUMN_COUNT  = 4;

namespace {

// Left edges and widths of the name, serial, region and version columns.
struct Columns
{
	qreal x[COLUMN_COUNT] {};
	qreal width[COLUMN_COUNT] {};
};

Columns ColumnsFor(qreal table_width, qreal u)
{
	const qreal widths[COLUMN_COUNT] = {table_width - (THUMB_WIDTH + SERIAL_WIDTH + REGION_WIDTH + VERSION_WIDTH) * u, SERIAL_WIDTH * u,
	                                    REGION_WIDTH * u, VERSION_WIDTH * u};
	Columns     columns;
	qreal       x = THUMB_WIDTH * u;
	for (int i = 0; i < COLUMN_COUNT; i++)
	{
		columns.x[i]     = x;
		columns.width[i] = widths[i];
		x += widths[i];
	}
	return columns;
}

// What a row's texts depend on; they are laid out again only when it changes.
struct RowContent
{
	GameCard card;
	qreal    width = 0;
	qreal    u     = 0;

	bool operator==(const RowContent& other) const { return card == other.card && width == other.width && u == other.u; }
};

class RowNode: public QSGTransformNode
{
public:
	explicit RowNode(QQuickWindow* window)
	{
		backing = new RoundedBoxNode;
		appendChildNode(backing);
		for (auto*& text: texts)
		{
			text = window->createTextNode();
			appendChildNode(text);
		}
	}

	void SetContent(const RowContent& content)
	{
		if (has_content && content == m_content)
		{
			return;
		}
		m_content   = content;
		has_content = true;

		const auto*   theme                = Theme::Instance();
		const qreal   u                    = content.u;
		const auto    columns              = ColumnsFor(content.width, u);
		const qreal   height               = ROW_HEIGHT * u;
		const QString values[COLUMN_COUNT] = {content.card.title, content.card.serial, content.card.region, content.card.version};

		SetTextLine(texts[0], values[0], SceneFont(theme->FontBody()), theme->TextPrimary(),
		            QRectF(columns.x[0], 0, columns.width[0] - NAME_GAP * u, height));
		for (int i = 1; i < COLUMN_COUNT; i++)
		{
			SetTextLine(texts[i], values[i].isEmpty() ? QStringLiteral("—") : values[i], SceneMonospaceFont(theme->FontBody()),
			            theme->TextSecondary(), QRectF(columns.x[i], 0, columns.width[i], height));
		}
	}

	// Shows the icon, or nothing while it loads; the texture is borrowed from the table's ArtworkTextures.
	void SetThumb(QQuickWindow* window, QSGTexture* texture, const QRectF& rect)
	{
		if (texture == nullptr)
		{
			delete thumb;
			thumb = nullptr;
			return;
		}
		if (thumb == nullptr)
		{
			thumb = window->createImageNode();
			thumb->setOwnsTexture(false);
			thumb->setFiltering(QSGTexture::Linear);
			// The software renderer reads the texture as soon as the node joins a shown tree.
			thumb->setTexture(texture);
			insertChildNodeAfter(thumb, backing);
		}
		if (thumb->texture() != texture)
		{
			thumb->setTexture(texture);
		}
		thumb->setRect(rect);
	}

	RoundedBoxNode* backing = nullptr;
	QSGImageNode*   thumb   = nullptr;
	QSGTextNode*    texts[COLUMN_COUNT] {};
	bool            has_content = false;

private:
	RowContent m_content;
};

class TableNode: public GroupNode
{
public:
	explicit TableNode(QQuickWindow* window)
	{
		for (auto*& name: names)
		{
			name = window->createTextNode();
			appendChildNode(name);
		}
		clip = new RectClipNode;
		appendChildNode(clip);
	}

	ArtworkTextures   textures;
	QSGTextNode*      names[COLUMN_COUNT] {};
	qreal             names_width = -1;
	qreal             names_u     = 0;
	RectClipNode*     clip        = nullptr;
	QVector<RowNode*> rows;
};

} // namespace

GameTableItem::GameTableItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
	setAcceptHoverEvents(true);
	connect(&m_scroll, &ViewScroll::Moved, this, &GameTableItem::Changed);
	connect(&m_art, &ArtworkSet::Loaded, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        ShowCurrent(false);
		        Changed();
	        });
}

void GameTableItem::SetGames(const QVariantList& games)
{
	m_games    = games;
	auto cards = GameCardsFromList(games);
	if (cards != m_cards)
	{
		m_cards = std::move(cards);
		if (m_current >= m_cards.size())
		{
			SetCurrentIndex(std::max(0, static_cast<int>(m_cards.size()) - 1));
		}
		ShowCurrent(false);
		Changed();
	}
	emit GamesChanged();
}

void GameTableItem::SetCurrentIndex(int index)
{
	index = std::clamp(index, 0, std::max(0, static_cast<int>(m_cards.size()) - 1));
	if (index == m_current)
	{
		return;
	}
	m_current = index;
	ShowCurrent(true);
	Changed();
	emit CurrentIndexChanged();
}

QRectF GameTableItem::ListRect() const
{
	const qreal top = (HEADER_HEIGHT + HEADER_GAP) * Theme::Instance()->U();
	return {0, top, width(), std::max<qreal>(0, height() - top)};
}

qreal GameTableItem::Pitch() const
{
	return (ROW_HEIGHT + ROW_SPACING) * Theme::Instance()->U();
}

qreal GameTableItem::MaxContentY() const
{
	return m_cards.size() * Pitch() - ROW_SPACING * Theme::Instance()->U() - ListRect().height();
}

int GameTableItem::IndexAt(QPointF point) const
{
	const auto list = ListRect();
	if (!list.contains(point))
	{
		return -1;
	}
	const qreal y     = point.y() - list.top() + m_scroll.Position();
	const int   index = static_cast<int>(std::floor(y / Pitch()));
	if (index < 0 || index >= m_cards.size() || y - index * Pitch() > ROW_HEIGHT * Theme::Instance()->U())
	{
		return -1;
	}
	return index;
}

void GameTableItem::ShowCurrent(bool animated)
{
	const qreal top = m_current * Pitch();
	m_scroll.Show(top, top + ROW_HEIGHT * Theme::Instance()->U(), ListRect().height(), MaxContentY(), animated);
}

void GameTableItem::SetHovered(int index)
{
	if (index != m_hovered)
	{
		m_hovered = index;
		update();
	}
}

void GameTableItem::Changed()
{
	polish();
	update();
}

void GameTableItem::updatePolish()
{
	if (!isVisible() || m_cards.isEmpty())
	{
		m_art.Keep({}, {});
		return;
	}
	// The rows in view and a screen's worth more, so a short scroll finds its icons decoded.
	const auto  list    = ListRect();
	const int   visible = static_cast<int>(std::ceil(list.height() / Pitch()));
	const int   first   = std::max(0, static_cast<int>(m_scroll.Position() / Pitch()) - visible / 2);
	const int   last    = std::min(static_cast<int>(m_cards.size()), first + 2 * visible + 1);
	QStringList ids;
	for (int i = first; i < last; i++)
	{
		ids << m_cards.at(i).icon_id;
	}
	const qreal dpr  = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const int   side = static_cast<int>(std::ceil(THUMB_SIZE * Theme::Instance()->U() * dpr));
	m_art.Keep(ids, QSize(side, side));
}

void GameTableItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		ShowCurrent(false);
		Changed();
	}
}

void GameTableItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemActiveFocusHasChanged)
	{
		update();
	} else if (change == ItemVisibleHasChanged)
	{
		Changed();
	}
}

void GameTableItem::keyPressEvent(QKeyEvent* event)
{
	const int count = static_cast<int>(m_cards.size());
	switch (event->key())
	{
		case Qt::Key_Up:
			if (m_current > 0)
			{
				SetCurrentIndex(m_current - 1);
				return;
			}
			break;
		case Qt::Key_Down:
			if (m_current + 1 < count)
			{
				SetCurrentIndex(m_current + 1);
				return;
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			if (count > 0)
			{
				emit activated(m_current);
				return;
			}
			break;
		default: break;
	}
	event->ignore();
}

void GameTableItem::mousePressEvent(QMouseEvent* event)
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
		emit menuRequested(index);
	}
}

void GameTableItem::mouseDoubleClickEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index >= 0 && event->button() == Qt::LeftButton)
	{
		emit activated(index);
	}
}

void GameTableItem::hoverMoveEvent(QHoverEvent* event)
{
	SetHovered(IndexAt(event->position()));
}

void GameTableItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	SetHovered(-1);
}

void GameTableItem::wheelEvent(QWheelEvent* event)
{
	const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().y() : event->angleDelta().y() / 120.0 * Pitch();
	m_scroll.ScrollTo(m_scroll.Position() - delta, MaxContentY(), false);
}

QSGNode* GameTableItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<TableNode*>(old_node);
	if (node == nullptr)
	{
		node = new TableNode(window());
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();

	// Column names sit outside the list, so rows scroll beneath nothing and need no backing.
	if (node->names_width != width() || node->names_u != u)
	{
		const auto    columns              = ColumnsFor(width(), u);
		const QString labels[COLUMN_COUNT] = {tr("Name"), tr("Serial"), tr("Region"), tr("Version")};
		const auto    font                 = SceneFont(theme->FontSmall(), QFont::DemiBold);
		for (int i = 0; i < COLUMN_COUNT; i++)
		{
			SetTextLine(node->names[i], labels[i], font, theme->TextMuted(), QRectF(columns.x[i], 0, columns.width[i], HEADER_HEIGHT * u));
		}
		node->names_width = width();
		node->names_u     = u;
	}

	const auto list = ListRect();
	node->clip->SetRect(list);
	const qreal scroll = m_scroll.Position();
	const int   first  = static_cast<int>(scroll / Pitch());
	const int   last   = std::min(static_cast<int>(m_cards.size()), static_cast<int>(std::ceil((scroll + list.height()) / Pitch())));
	const int   shown  = std::max(0, last - first);
	while (node->rows.size() > shown)
	{
		delete node->rows.takeLast();
	}
	while (node->rows.size() < shown)
	{
		node->rows << new RowNode(window());
		node->clip->appendChildNode(node->rows.last());
	}

	const qreal  row_height = ROW_HEIGHT * u;
	const qreal  thumb      = THUMB_SIZE * u;
	const QRectF thumb_rect(THUMB_X * u, (row_height - thumb) / 2, thumb, thumb);
	for (int i = 0; i < shown; i++)
	{
		const int   index = first + i;
		const auto& card  = m_cards.at(index);
		auto*       row   = node->rows.at(i);

		QMatrix4x4 matrix;
		matrix.translate(0, static_cast<float>(list.top() + index * Pitch() - scroll));
		row->setMatrix(matrix);

		RowContent content;
		content.card  = card;
		content.width = width();
		content.u     = u;
		row->SetContent(content);

		const bool            current = index == m_current;
		RoundedBoxNode::Style backing;
		backing.fill         = current ? theme->GlassStrong() : (index == m_hovered ? theme->Glass() : QColor(Qt::transparent));
		backing.border       = theme->Accent();
		backing.border_width = current && hasActiveFocus() ? RING_WIDTH * u : 0;
		backing.radius       = theme->RadiusSmall();
		backing.dpr          = dpr;
		row->backing->Update(window(), QRectF(0, 0, width(), row_height), backing);

		row->SetThumb(window(), node->textures.Get(window(), card.icon_id, m_art.Find(card.icon_id)), thumb_rect);
	}
	node->textures.Prune();
	return node;
}
