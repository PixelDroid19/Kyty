#include "TextPadItem.h"

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
#include <iterator>

// Sizes in units.
constexpr qreal PADDING     = 20;
constexpr qreal SPACING     = 14;
constexpr qreal CELL_WIDTH  = 60;
constexpr qreal CELL_HEIGHT = 46;
constexpr qreal KEY_GAP     = 6;
constexpr qreal KEY_RADIUS  = 10;
constexpr qreal BOX_HEIGHT  = 40;
constexpr qreal BOX_RADIUS  = 10;
constexpr qreal BOX_INSET   = 14;
constexpr qreal RING_WIDTH  = 3;

constexpr int COLUMNS = 10;

namespace {

const char* const TOKENS[] = {"1",
                              "2",
                              "3",
                              "4",
                              "5",
                              "6",
                              "7",
                              "8",
                              "9",
                              "0",
                              "q",
                              "w",
                              "e",
                              "r",
                              "t",
                              "y",
                              "u",
                              "i",
                              "o",
                              "p",
                              "a",
                              "s",
                              "d",
                              "f",
                              "g",
                              "h",
                              "j",
                              "k",
                              "l",
                              "-",
                              "z",
                              "x",
                              "c",
                              "v",
                              "b",
                              "n",
                              "m",
                              ".",
                              "_",
                              "/",
                              ":",
                              "+",
                              "(",
                              ")",
                              "@",
                              "&",
                              "?",
                              "'",
                              "~",
                              "#",
                              QT_TRANSLATE_NOOP("TextPadItem", "Shift"),
                              QT_TRANSLATE_NOOP("TextPadItem", "Space"),
                              QT_TRANSLATE_NOOP("TextPadItem", "Del"),
                              QT_TRANSLATE_NOOP("TextPadItem", "Done")};

constexpr int TOKEN_COUNT = static_cast<int>(std::size(TOKENS));
constexpr int ROWS        = (TOKEN_COUNT + COLUMNS - 1) / COLUMNS;

class KeyNode: public GroupNode
{
public:
	explicit KeyNode(QQuickWindow* window)
	{
		box = new RoundedBoxNode;
		appendChildNode(box);
		label = window->createTextNode();
		appendChildNode(label);
	}

	RoundedBoxNode* box   = nullptr;
	QSGTextNode*    label = nullptr;
};

class PadNode: public GroupNode
{
public:
	explicit PadNode(QQuickWindow* window)
	{
		scrim = window->createRectangleNode();
		appendChildNode(scrim);
		panel = new RoundedBoxNode;
		appendChildNode(panel);
		title = window->createTextNode();
		appendChildNode(title);
		box = new RoundedBoxNode;
		appendChildNode(box);
		text = window->createTextNode();
		appendChildNode(text);
		for (auto*& key: keys)
		{
			key = new KeyNode(window);
			appendChildNode(key);
		}
	}

	QSGRectangleNode* scrim = nullptr;
	RoundedBoxNode*   panel = nullptr;
	QSGTextNode*      title = nullptr;
	RoundedBoxNode*   box   = nullptr;
	QSGTextNode*      text  = nullptr;
	KeyNode*          keys[TOKEN_COUNT] {};
};

QFont BodyFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

QFont SmallFont()
{
	return SceneFont(Theme::Instance()->FontSmall());
}

} // namespace

TextPadItem::TextPadItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);
	setAcceptedMouseButtons(Qt::AllButtons);
	setAcceptHoverEvents(true);
	setVisible(false);
	setZ(20);
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::update);
}

void TextPadItem::SetText(const QString& text)
{
	if (text != m_text)
	{
		m_text = text;
		update();
	}
}

void TextPadItem::OpenFor(const QString& key, const QString& title)
{
	m_key     = key;
	m_title   = title;
	m_upper   = false;
	m_current = 0;
	m_shown   = true;
	setVisible(true);
	forceActiveFocus(Qt::OtherFocusReason);
	emit ShownChanged();
	update();
}

void TextPadItem::Close()
{
	if (!m_shown)
	{
		return;
	}
	m_shown = false;
	setVisible(false);
	emit ShownChanged();
}

QString TextPadItem::Label(int index) const
{
	const QString token = QString::fromLatin1(TOKENS[index]);
	// Word keys are named in the launcher's language; Press still matches them by their source name.
	if (token.size() > 1)
	{
		return tr(TOKENS[index]);
	}
	return m_upper && token.size() == 1 && token.at(0).isLower() ? token.toUpper() : token;
}

void TextPadItem::Press(int index)
{
	const QLatin1String token(TOKENS[index]);
	if (token == QLatin1String("Shift"))
	{
		m_upper = !m_upper;
		update();
	} else if (token == QLatin1String("Del"))
	{
		emit Edited(m_text.left(m_text.size() - 1));
	} else if (token == QLatin1String("Space"))
	{
		emit Edited(m_text + QLatin1Char(' '));
	} else if (token == QLatin1String("Done"))
	{
		emit Accepted();
		Close();
	} else
	{
		emit Edited(m_text + Label(index));
	}
}

void TextPadItem::MoveCurrent(int index)
{
	m_current = index;
	update();
}

TextPadItem::Geometry TextPadItem::Layout() const
{
	const qreal  u          = Theme::Instance()->U();
	const qreal  grid_width = COLUMNS * CELL_WIDTH * u;
	const qreal  title_h    = std::ceil(QFontMetricsF(SmallFont()).height());
	const qreal  content_h  = title_h + BOX_HEIGHT * u + ROWS * CELL_HEIGHT * u + 2 * SPACING * u;
	const QSizeF panel_size(grid_width + 2 * PADDING * u, content_h + 2 * PADDING * u);

	Geometry geometry;
	geometry.panel =
	    QRectF(QPointF(std::round((width() - panel_size.width()) / 2), std::round((height() - panel_size.height()) / 2)), panel_size);
	const QPointF content = geometry.panel.topLeft() + QPointF(PADDING * u, PADDING * u);
	geometry.title        = QRectF(content, QSizeF(grid_width, title_h));
	geometry.text         = QRectF(content.x(), geometry.title.bottom() + SPACING * u, grid_width, BOX_HEIGHT * u);
	geometry.keys         = QPointF(content.x(), geometry.text.bottom() + SPACING * u);
	return geometry;
}

QRectF TextPadItem::KeyRect(const Geometry& geometry, int index) const
{
	const qreal u = Theme::Instance()->U();
	return {geometry.keys.x() + (index % COLUMNS) * CELL_WIDTH * u, geometry.keys.y() + (index / COLUMNS) * CELL_HEIGHT * u,
	        (CELL_WIDTH - KEY_GAP) * u, (CELL_HEIGHT - KEY_GAP) * u};
}

int TextPadItem::KeyAt(QPointF point) const
{
	const auto geometry = Layout();
	for (int i = 0; i < TOKEN_COUNT; i++)
	{
		if (KeyRect(geometry, i).contains(point))
		{
			return i;
		}
	}
	return -1;
}

void TextPadItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

void TextPadItem::keyPressEvent(QKeyEvent* event)
{
	// The pad is modal: no key reaches the page below while it is open.
	const int column = m_current % COLUMNS;
	switch (event->key())
	{
		case Qt::Key_Left:
			if (column > 0)
			{
				MoveCurrent(m_current - 1);
			}
			return;
		case Qt::Key_Right:
			if (column + 1 < COLUMNS && m_current + 1 < TOKEN_COUNT)
			{
				MoveCurrent(m_current + 1);
			}
			return;
		case Qt::Key_Up:
			if (m_current >= COLUMNS)
			{
				MoveCurrent(m_current - COLUMNS);
			}
			return;
		case Qt::Key_Down:
			// The last row is shorter; from above its end the focus goes to its last key.
			if (m_current / COLUMNS + 1 < ROWS)
			{
				MoveCurrent(std::min(m_current + COLUMNS, TOKEN_COUNT - 1));
			}
			return;
		case Qt::Key_Return:
		case Qt::Key_Enter: Press(m_current); return;
		case Qt::Key_Escape: Close(); return;
		case Qt::Key_Backspace: emit Edited(m_text.left(m_text.size() - 1)); return;
		default: break;
	}
	const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
	const auto text      = event->text();
	if (!text.isEmpty() && text.at(0).isPrint() && modifiers != Qt::ControlModifier &&
	    modifiers != (Qt::ControlModifier | Qt::ShiftModifier))
	{
		emit Edited(m_text + text);
		// Typing on a keyboard leaves the pad's keys alone, so Return then means Done.
		MoveCurrent(TOKEN_COUNT - 1);
	}
}

void TextPadItem::mousePressEvent(QMouseEvent* /*event*/) {}

void TextPadItem::mouseReleaseEvent(QMouseEvent* event)
{
	const int index = event->button() == Qt::LeftButton ? KeyAt(event->position()) : -1;
	if (index >= 0)
	{
		MoveCurrent(index);
		Press(index);
		return;
	}
	if (!Layout().panel.contains(event->position()))
	{
		Close();
	}
}

// Accepting hover and the wheel keeps the page below from reacting to the pointer.
void TextPadItem::hoverMoveEvent(QHoverEvent* /*event*/) {}

void TextPadItem::wheelEvent(QWheelEvent* /*event*/) {}

QSGNode* TextPadItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<PadNode*>(old_node);
	if (node == nullptr)
	{
		node = new PadNode(window());
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

	SetTextLine(node->title, m_title, SmallFont(), theme->TextSecondary(), geometry.title);

	RoundedBoxNode::Style box;
	box.fill         = theme->Glass();
	box.border       = theme->GlassBorder();
	box.border_width = 1;
	box.radius       = BOX_RADIUS * u;
	box.dpr          = dpr;
	node->box->Update(window(), geometry.text, box);
	// The end of the text, where the next character goes, stays in view.
	const auto text_rect = geometry.text.adjusted(BOX_INSET * u, 0, -BOX_INSET * u, 0);
	const auto shown     = QFontMetricsF(BodyFont()).elidedText(m_text + QLatin1Char('|'), Qt::ElideLeft, text_rect.width());
	SetTextLine(node->text, shown, BodyFont(), theme->TextPrimary(), text_rect);

	for (int i = 0; i < TOKEN_COUNT; i++)
	{
		const bool            current = i == m_current;
		RoundedBoxNode::Style style;
		style.fill         = current ? theme->GlassStrong() : theme->Glass();
		style.border       = current ? theme->Accent() : theme->GlassBorder();
		style.border_width = current ? RING_WIDTH * u : 1;
		style.radius       = KEY_RADIUS * u;
		style.dpr          = dpr;
		const auto rect    = KeyRect(geometry, i);
		node->keys[i]->box->Update(window(), rect, style);
		const auto label = Label(i);
		SetTextLine(node->keys[i]->label, label, label.size() > 1 ? SmallFont() : BodyFont(), theme->TextPrimary(), rect, Qt::AlignHCenter);
	}
	return node;
}
