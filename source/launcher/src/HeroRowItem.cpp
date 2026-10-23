#include "HeroRowItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTextNode>
#include <QSGTransformNode>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal CARD_WIDTH   = 240;
constexpr qreal CARD_HEIGHT  = 136;
constexpr qreal CARD_SPACING = 16;
constexpr qreal TITLE_MARGIN = 12;
constexpr qreal HOVER_ZOOM   = 1.02;
constexpr qreal FOCUS_ZOOM   = 1.04;
// The shade under the name covers this share of the card, from the bottom.
constexpr qreal SHADE_SHARE = 0.55;
// Room kept around the cards for the grown card and its focus ring.
constexpr qreal RING_ROOM = 16;

namespace {

// Fades the art into the name instead of laying a bar over it; the same rounded corners as the card.
QImage PaintShade(const QSizeF& size, qreal radius, qreal dpr)
{
	QImage image(static_cast<int>(std::ceil(size.width() * dpr)), static_cast<int>(std::ceil(size.height() * dpr)),
	             QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QLinearGradient gradient(0, 0, 0, image.height());
	gradient.setColorAt(0, Qt::transparent);
	gradient.setColorAt(1, QColor::fromRgbF(0, 0, 0, 0.8F));
	QPainterPath path;
	path.addRoundedRect(QRectF(image.rect()), radius * dpr, radius * dpr);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.fillPath(path, gradient);
	painter.end();
	return image;
}

// What a card's overlay depends on; it is built again only when this changes.
struct CardContent
{
	QString     title;
	QSizeF      size;
	qreal       u     = 0;
	QSGTexture* shade = nullptr;

	bool operator==(const CardContent& other) const
	{
		return title == other.title && size == other.size && u == other.u && shade == other.shade;
	}
};

class CardNode: public QSGTransformNode
{
public:
	// The software renderer reads the shade texture as soon as the card joins a shown tree.
	CardNode(QQuickWindow* window, QSGTexture* shade_texture)
	{
		cover = new CoverNode;
		shade = window->createImageNode();
		shade->setOwnsTexture(false);
		shade->setFiltering(QSGTexture::Linear);
		shade->setTexture(shade_texture);
		title = window->createTextNode();
		cover->Overlay()->appendChildNode(shade);
		cover->Overlay()->appendChildNode(title);
		appendChildNode(cover);
	}

	void SetContent(const CardContent& content)
	{
		if (has_content && content == m_content)
		{
			return;
		}
		m_content   = content;
		has_content = true;

		const auto& size = content.size;
		shade->setTexture(content.shade);
		shade->setRect(QRectF(0, size.height() * (1 - SHADE_SHARE), size.width(), size.height() * SHADE_SHARE));

		const auto* theme  = Theme::Instance();
		const auto  font   = SceneFont(theme->FontBody(), QFont::DemiBold);
		const qreal margin = TITLE_MARGIN * content.u;
		const qreal line   = QFontMetricsF(font).height();
		SetTextLine(title, content.title, font, theme->TextOnArt(),
		            QRectF(margin, size.height() - margin - line, size.width() - 2 * margin, line));
	}

	CoverNode*    cover       = nullptr;
	QSGImageNode* shade       = nullptr;
	QSGTextNode*  title       = nullptr;
	bool          has_content = false;

private:
	CardContent m_content;
};

class RowNode: public RectClipNode
{
public:
	~RowNode() override { delete shade; }

	KYTY_QT_CLASS_NO_COPY(RowNode);

	RowNode() = default;

	// Painted again when the card size or the pixel ratio changes; the cards borrow it.
	void UpdateShade(QQuickWindow* window, const QSizeF& card, qreal dpr)
	{
		if (shade != nullptr && card == shade_card && dpr == shade_dpr)
		{
			return;
		}
		delete shade;
		const QSizeF size(card.width(), card.height() * SHADE_SHARE);
		shade      = window->createTextureFromImage(PaintShade(size, Theme::Instance()->ArtCorner() * card.height(), dpr),
		                                            QQuickWindow::TextureHasAlphaChannel);
		shade_card = card;
		shade_dpr  = dpr;
	}

	ArtworkTextures    textures;
	QVector<CardNode*> cards;
	QSGTexture*        shade = nullptr;
	QSizeF             shade_card;
	qreal              shade_dpr = 0;
};

} // namespace

HeroRowItem::HeroRowItem(QQuickItem* parent): QQuickItem(parent), m_zoom(FOCUS_ZOOM, this), m_hover_zoom(HOVER_ZOOM, this)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
	setAcceptHoverEvents(true);
	connect(&m_scroll, &ViewScroll::Moved, this, &HeroRowItem::Changed);
	connect(&m_art, &ArtworkSet::Loaded, this, &QQuickItem::update);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        ShowCurrent(false);
		        Changed();
	        });
}

void HeroRowItem::SetGames(const QVariantList& games)
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
}

void HeroRowItem::SetCurrentIndex(int index)
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

QSizeF HeroRowItem::CardSize() const
{
	const qreal u = Theme::Instance()->U();
	return {CARD_WIDTH * u, CARD_HEIGHT * u};
}

qreal HeroRowItem::Pitch() const
{
	return CardSize().width() + CARD_SPACING * Theme::Instance()->U();
}

qreal HeroRowItem::MaxContentX() const
{
	return m_cards.size() * Pitch() - CARD_SPACING * Theme::Instance()->U() - width();
}

int HeroRowItem::IndexAt(QPointF point) const
{
	const qreal x     = point.x() + m_scroll.Position();
	const int   index = static_cast<int>(std::floor(x / Pitch()));
	if (index < 0 || index >= m_cards.size() || x - index * Pitch() > CardSize().width() || point.y() < 0 ||
	    point.y() > CardSize().height())
	{
		return -1;
	}
	return index;
}

void HeroRowItem::ShowCurrent(bool animated)
{
	const qreal left = m_current * Pitch();
	m_scroll.Show(left, left + CardSize().width(), width(), MaxContentX(), animated);
}

void HeroRowItem::Changed()
{
	polish();
	update();
}

void HeroRowItem::updatePolish()
{
	// A hidden view keeps what it decoded, so its covers are there at once when its page comes back.
	if (!isVisible())
	{
		return;
	}
	if (m_cards.isEmpty())
	{
		m_art.Keep({}, {});
		return;
	}
	// The recently played row is short, so all of its pictures are kept.
	QStringList ids;
	for (const auto& card: m_cards)
	{
		ids << (card.picture_id.isEmpty() ? card.icon_id : card.picture_id);
	}
	const qreal dpr  = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const auto  size = CardSize();
	m_art.Keep(ids, QSize(static_cast<int>(std::ceil(size.width() * dpr)), static_cast<int>(std::ceil(size.height() * dpr))));
}

void HeroRowItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		ShowCurrent(false);
		Changed();
	}
}

void HeroRowItem::itemChange(ItemChange change, const ItemChangeData& value)
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

void HeroRowItem::keyPressEvent(QKeyEvent* event)
{
	const int count = static_cast<int>(m_cards.size());
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
				emit Activated(m_current);
				return;
			}
			break;
		default: break;
	}
	event->ignore();
}

void HeroRowItem::hoverMoveEvent(QHoverEvent* event)
{
	m_hover_zoom.SetFocused(IndexAt(event->position()));
}

void HeroRowItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	m_hover_zoom.SetFocused(-1);
}

void HeroRowItem::mousePressEvent(QMouseEvent* event)
{
	const int index = IndexAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	if (event->button() == Qt::RightButton)
	{
		emit MenuRequested(index);
		return;
	}
	SetCurrentIndex(index);
	forceActiveFocus(Qt::MouseFocusReason);
	emit Activated(index);
}

void HeroRowItem::wheelEvent(QWheelEvent* event)
{
	// A vertical wheel scrolls the row sideways.
	const auto  pixels = event->pixelDelta();
	const auto  angle  = event->angleDelta();
	const qreal delta =
	    !pixels.isNull() ? (pixels.x() != 0 ? pixels.x() : pixels.y()) : (angle.x() != 0 ? angle.x() : angle.y()) / 120.0 * Pitch() / 2;
	m_scroll.ScrollTo(m_scroll.Position() - delta, MaxContentX(), false);
}

QSGNode* HeroRowItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RowNode*>(old_node);
	if (node == nullptr)
	{
		node = new RowNode;
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal dpr   = window()->effectiveDevicePixelRatio();
	const auto  card  = CardSize();
	const qreal room  = RING_ROOM * u;
	node->SetRect(boundingRect().adjusted(-room, -room, room, room));
	node->UpdateShade(window(), card, dpr);

	// Only the cards inside the clip are built.
	const qreal scroll = m_scroll.Position();
	const int   first  = std::max(0, static_cast<int>(std::floor((scroll - room) / Pitch())));
	const int   last   = std::min(static_cast<int>(m_cards.size()), static_cast<int>(std::ceil((scroll + width() + room) / Pitch())));
	const int   shown  = std::max(0, last - first);
	while (node->cards.size() > shown)
	{
		delete node->cards.takeLast();
	}
	while (node->cards.size() < shown)
	{
		node->cards << new CardNode(window(), node->shade);
		node->appendChildNode(node->cards.last());
	}

	for (int i = 0; i < shown; i++)
	{
		const int   index = first + i;
		const auto& game  = m_cards.at(index);
		auto*       slot  = node->cards.at(i);

		QMatrix4x4 matrix;
		matrix.translate(static_cast<float>(index * Pitch() - scroll), 0);
		slot->setMatrix(matrix);

		CardContent content;
		content.title = game.title;
		content.size  = card;
		content.u     = u;
		content.shade = node->shade;
		slot->SetContent(content);

		const auto&     id = game.picture_id.isEmpty() ? game.icon_id : game.picture_id;
		CoverNode::Look look;
		look.rect = QRectF(QPointF(0, 0), card);
		look.fill = theme->Surface();
		look.zoom = std::max(m_zoom.Zoom(index), m_hover_zoom.Zoom(index));
		look.ring = hasActiveFocus() && index == m_current;
		look.dpr  = dpr;
		slot->cover->Update(window(), look, node->textures.Get(window(), id, m_art.Find(id)));
	}
	node->textures.Prune();
	return node;
}
