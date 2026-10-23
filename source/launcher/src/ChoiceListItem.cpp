#include "ChoiceListItem.h"

#include "Accessibility.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal ROW_HEIGHT  = 38;
constexpr qreal PADDING     = 6;
constexpr qreal ANCHOR_GAP  = 6;
constexpr qreal EDGE_MARGIN = 12;
constexpr qreal TEXT_INSET  = 14;
constexpr qreal CHECK_WIDTH = 28;
constexpr qreal SHADOW      = 8;
constexpr qreal BAR_WIDTH   = 3;

constexpr int MAX_ROWS    = 8;
constexpr int WHEEL_NOTCH = 120;

namespace {

class ListNode: public GroupNode
{
public:
	explicit ListNode()
	{
		shadow = new RoundedBoxNode;
		appendChildNode(shadow);
		panel = new RoundedBoxNode;
		appendChildNode(panel);
		highlight = new RoundedBoxNode;
		appendChildNode(highlight);
		bar = new RoundedBoxNode;
		appendChildNode(bar);
		texts = new GroupNode;
		appendChildNode(texts);
	}

	RoundedBoxNode* shadow    = nullptr;
	RoundedBoxNode* panel     = nullptr;
	RoundedBoxNode* highlight = nullptr;
	RoundedBoxNode* bar       = nullptr;
	GroupNode*      texts     = nullptr;
};

} // namespace

ChoiceListItem::ChoiceListItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::AllButtons);
	setAcceptHoverEvents(true);
	setVisible(false);
	setZ(10);
	m_fade.setStartValue(0.0);
	m_fade.setEndValue(1.0);
	connect(&m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { setOpacity(value.toReal()); });
}

void ChoiceListItem::OpenFor(const QRectF& anchor, const QStringList& texts, int current)
{
	if (texts.isEmpty())
	{
		return;
	}
	m_texts   = texts;
	m_anchor  = anchor;
	m_chosen  = std::clamp<int>(current, 0, static_cast<int>(texts.size()) - 1);
	m_current = m_chosen;
	m_first   = 0;
	m_wheel   = 0;
	MoveCurrent(m_chosen);
	setPosition(QPointF(0, 0));
	setSize(parentItem()->size());
	setVisible(true);
	forceActiveFocus(Qt::OtherFocusReason);
	if (Theme::Instance()->Animation() > 0)
	{
		m_fade.setDuration(Theme::Instance()->Animation());
		m_fade.start();
	} else
	{
		setOpacity(1);
	}
	update();
}

void ChoiceListItem::Close(bool picked)
{
	m_fade.stop();
	setVisible(false);
	if (picked)
	{
		emit Chosen(m_current);
	} else
	{
		emit Cancelled();
	}
}

int ChoiceListItem::VisibleRows() const
{
	return std::min<int>(static_cast<int>(m_texts.size()), MAX_ROWS);
}

QRectF ChoiceListItem::PanelRect() const
{
	const qreal u      = Theme::Instance()->U();
	const qreal margin = EDGE_MARGIN * u;
	const qreal gap    = ANCHOR_GAP * u;
	const qreal h      = VisibleRows() * ROW_HEIGHT * u + 2 * PADDING * u;
	// Below the field when it fits, else above it when that fits, else as low as the page allows.
	qreal top = m_anchor.bottom() + gap;
	if (top + h > height() - margin)
	{
		top = m_anchor.top() - gap - h >= margin ? m_anchor.top() - gap - h : std::max(margin, height() - margin - h);
	}
	return {m_anchor.left(), top, m_anchor.width(), h};
}

QRectF ChoiceListItem::RowRect(int index) const
{
	const qreal  u     = Theme::Instance()->U();
	const QRectF panel = PanelRect().adjusted(PADDING * u, PADDING * u, -PADDING * u, -PADDING * u);
	return {panel.left(), panel.top() + (index - m_first) * ROW_HEIGHT * u, panel.width(), ROW_HEIGHT * u};
}

int ChoiceListItem::RowAt(QPointF point) const
{
	for (int i = m_first; i < m_first + VisibleRows(); i++)
	{
		if (RowRect(i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

// Moves the highlight and scrolls just enough to keep it in view.
void ChoiceListItem::MoveCurrent(int index)
{
	m_current = std::clamp<int>(index, 0, static_cast<int>(m_texts.size()) - 1);
	m_first   = std::clamp(m_first, m_current - VisibleRows() + 1, m_current);
	SetAccessible(this, QAccessible::List, m_texts.value(m_current));
	update();
}

void ChoiceListItem::ScrollBy(int rows)
{
	m_first = std::clamp(m_first + rows, 0, static_cast<int>(m_texts.size()) - VisibleRows());
	update();
}

void ChoiceListItem::keyPressEvent(QKeyEvent* event)
{
	// The list is modal: no key reaches the page below while it is open.
	switch (event->key())
	{
		case Qt::Key_Up: MoveCurrent(m_current - 1); return;
		case Qt::Key_Down: MoveCurrent(m_current + 1); return;
		case Qt::Key_PageUp: MoveCurrent(m_current - VisibleRows()); return;
		case Qt::Key_PageDown: MoveCurrent(m_current + VisibleRows()); return;
		case Qt::Key_Home: MoveCurrent(0); return;
		case Qt::Key_End: MoveCurrent(static_cast<int>(m_texts.size()) - 1); return;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_Space: Close(true); return;
		case Qt::Key_Escape:
		case Qt::Key_Back: Close(false); return;
		default: return;
	}
}

void ChoiceListItem::mousePressEvent(QMouseEvent* /*event*/) {}

void ChoiceListItem::mouseReleaseEvent(QMouseEvent* event)
{
	const int index = event->button() == Qt::LeftButton ? RowAt(event->position()) : -1;
	if (index >= 0)
	{
		m_current = index;
		Close(true);
		return;
	}
	if (!PanelRect().contains(event->position()))
	{
		Close(false);
	}
}

void ChoiceListItem::hoverMoveEvent(QHoverEvent* event)
{
	const int index = RowAt(event->position());
	if (index >= 0 && index != m_current)
	{
		MoveCurrent(index);
	}
}

void ChoiceListItem::wheelEvent(QWheelEvent* event)
{
	// Whole rows per notch; touchpads add up small steps until they make one.
	m_wheel += event->angleDelta().y();
	const int rows = m_wheel / WHEEL_NOTCH;
	m_wheel -= rows * WHEEL_NOTCH;
	if (rows != 0)
	{
		ScrollBy(-rows);
	}
}

QSGNode* ChoiceListItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<ListNode*>(old_node);
	if (node == nullptr)
	{
		node = new ListNode;
	}
	const auto*  theme = Theme::Instance();
	const qreal  u     = theme->U();
	const qreal  dpr   = window()->effectiveDevicePixelRatio();
	const QRectF panel = PanelRect();

	RoundedBoxNode::Style shadow;
	shadow.fill   = QColor::fromRgbF(0, 0, 0, theme->IsDark() ? 0.35F : 0.12F);
	shadow.radius = theme->RadiusSmall() + SHADOW * u / 2;
	shadow.dpr    = dpr;
	node->shadow->Update(window(), panel.adjusted(-SHADOW * u / 2, 0, SHADOW * u / 2, SHADOW * u), shadow);

	RoundedBoxNode::Style box;
	box.fill         = theme->Panel();
	box.border       = theme->GlassBorder();
	box.border_width = 1;
	box.radius       = theme->RadiusSmall();
	box.dpr          = dpr;
	node->panel->Update(window(), panel, box);

	RoundedBoxNode::Style highlight;
	highlight.fill   = theme->GlassStrong();
	highlight.border = theme->Accent();
	// The ring shows where the keys are; the pointer's row is the same row.
	highlight.border_width = hasActiveFocus() ? 2 : 0;
	highlight.radius       = theme->RadiusSmall() - PADDING * u / 2;
	highlight.dpr          = dpr;
	node->highlight->Update(window(), RowRect(m_current), highlight);

	// A thin bar on the right shows where a long list is scrolled to.
	RoundedBoxNode::Style bar;
	bar.fill          = m_texts.size() > VisibleRows() ? theme->GlassBorder() : QColor(Qt::transparent);
	bar.radius        = BAR_WIDTH * u / 2;
	bar.dpr           = dpr;
	const qreal track = panel.height() - 2 * PADDING * u;
	const qreal share = static_cast<qreal>(VisibleRows()) / std::max<qsizetype>(1, m_texts.size());
	node->bar->Update(window(),
	                  QRectF(panel.right() - PADDING * u / 2 - BAR_WIDTH * u,
	                         panel.top() + PADDING * u + track * m_first / std::max<qsizetype>(1, m_texts.size()), BAR_WIDTH * u,
	                         track * share),
	                  bar);

	while (auto* child = node->texts->firstChild())
	{
		node->texts->removeChildNode(child);
		delete child;
	}
	for (int i = m_first; i < m_first + VisibleRows(); i++)
	{
		const bool   chosen = i == m_chosen;
		const QRectF row    = RowRect(i).adjusted(TEXT_INSET * u, 0, -TEXT_INSET * u, 0);
		const auto   font   = SceneFont(theme->FontBody(), chosen ? QFont::DemiBold : QFont::Normal);
		auto*        text   = window()->createTextNode();
		SetTextLine(text, m_texts.at(i), font, chosen ? theme->Accent() : theme->TextPrimary(), row.adjusted(0, 0, -CHECK_WIDTH * u, 0));
		node->texts->appendChildNode(text);
		if (chosen)
		{
			auto* check = window()->createTextNode();
			SetTextLine(check, QStringLiteral("✓"), font, theme->Accent(), row, Qt::AlignRight);
			node->texts->appendChildNode(check);
		}
	}
	return node;
}
