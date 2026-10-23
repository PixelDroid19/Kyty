#include "TextFieldItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QStyleHints>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal INSET      = 14;
constexpr qreal RADIUS     = 12;
constexpr qreal RING_WIDTH = 3;

namespace {

class FieldNode: public GroupNode
{
public:
	explicit FieldNode(QQuickWindow* window)
	{
		box = new RoundedBoxNode;
		appendChildNode(box);
		clip = new RectClipNode;
		appendChildNode(clip);
		text = window->createTextNode();
		clip->appendChildNode(text);
		cursor = window->createRectangleNode();
		clip->appendChildNode(cursor);
	}

	RoundedBoxNode*   box    = nullptr;
	RectClipNode*     clip   = nullptr;
	QSGTextNode*      text   = nullptr;
	QSGRectangleNode* cursor = nullptr;
};

QFont FieldFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

qreal CursorWidth()
{
	return std::max<qreal>(1, std::round(Theme::Instance()->U()));
}

// Pasted text keeps only its first line, since the field holds one.
QString FirstLine(const QString& text)
{
	return text.section(QLatin1Char('\n'), 0, 0).remove(QLatin1Char('\r'));
}

} // namespace

TextFieldItem::TextFieldItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemAcceptsInputMethod);
	setAcceptedMouseButtons(Qt::LeftButton);
	setCursor(Qt::IBeamCursor);

	QTextOption option;
	option.setWrapMode(QTextOption::NoWrap);
	// Without it a trailing space has no width and the cursor would not move past it.
	option.setFlags(QTextOption::IncludeTrailingSpaces);
	m_layout.setTextOption(option);
	m_layout.setCacheEnabled(true);

	connect(&m_blink, &QTimer::timeout, this,
	        [this]()
	        {
		        m_cursor_shown = !m_cursor_shown;
		        update();
	        });
	connect(Theme::Instance(), &Theme::UChanged, this, &TextFieldItem::Relayout);
	connect(Theme::Instance(), &Theme::LiveChanged, this, &TextFieldItem::RestartBlink);
	Relayout();
}

void TextFieldItem::SetText(const QString& text)
{
	if (text == m_text)
	{
		return;
	}
	m_text   = text;
	m_cursor = m_anchor = static_cast<int>(text.size());
	Relayout();
}

void TextFieldItem::Relayout()
{
	m_layout.clearLayout();
	m_layout.setText(m_text);
	m_layout.setFont(FieldFont());
	m_layout.setPreeditArea(m_preedit.isEmpty() ? -1 : m_cursor, m_preedit);
	m_layout.beginLayout();
	m_layout.createLine();
	m_layout.endLayout();
	CursorMoved();
}

bool TextFieldItem::IsNumberText(const QString& text, int max_digits)
{
	return text.size() <= max_digits && std::all_of(text.begin(), text.end(), [](QChar c) { return c >= '0' && c <= '9'; });
}

bool TextFieldItem::Accepts(const QString& text) const
{
	return m_max_digits == 0 || IsNumberText(text, m_max_digits);
}

QString TextFieldItem::SelectedText() const
{
	const int start = std::min(m_anchor, m_cursor);
	return m_text.mid(start, std::abs(m_cursor - m_anchor));
}

QRectF TextFieldItem::InnerRect() const
{
	const qreal inset = INSET * Theme::Instance()->U();
	return {inset, 0, std::max<qreal>(0, width() - 2 * inset), height()};
}

QRectF TextFieldItem::CursorRect() const
{
	const auto  line = m_layout.lineAt(0);
	const qreal top  = CentredBaseline(FieldFont(), boundingRect()) - line.ascent();
	const qreal x    = InnerRect().left() - m_scroll + line.cursorToX(m_cursor + (m_preedit.isEmpty() ? 0 : m_preedit_cursor));
	return {std::round(x), std::round(top), CursorWidth(), std::round(line.height())};
}

int TextFieldItem::PositionAt(qreal x) const
{
	return m_layout.lineAt(0).xToCursor(x - InnerRect().left() + m_scroll, QTextLine::CursorBetweenCharacters);
}

void TextFieldItem::CursorMoved()
{
	// Scrolls the least that shows the cursor, and never past the end of the text.
	const auto  line         = m_layout.lineAt(0);
	const qreal cursor_width = CursorWidth();
	const qreal visible      = std::max(InnerRect().width(), cursor_width);
	const qreal cursor_x     = line.cursorToX(m_cursor + (m_preedit.isEmpty() ? 0 : m_preedit_cursor));
	const qreal max_scroll   = std::max<qreal>(0, line.naturalTextWidth() + cursor_width - visible);
	m_scroll                 = std::clamp(m_scroll, std::max<qreal>(0, cursor_x + cursor_width - visible), std::min(cursor_x, max_scroll));

	RestartBlink();
	if (hasActiveFocus())
	{
		QGuiApplication::inputMethod()->update(Qt::ImQueryInput);
	}
	update();
}

void TextFieldItem::RestartBlink()
{
	m_cursor_shown       = true;
	const int flash_time = QGuiApplication::styleHints()->cursorFlashTime();
	if (hasActiveFocus() && Theme::Instance()->Live() && flash_time > 0)
	{
		m_blink.start(flash_time / 2);
	} else
	{
		m_blink.stop();
	}
	update();
}

bool TextFieldItem::MoveCursor(int position, bool select)
{
	position = std::clamp(position, 0, static_cast<int>(m_text.size()));
	if (position == m_cursor && (select || !HasSelection()))
	{
		return false;
	}
	m_cursor = position;
	if (!select)
	{
		m_anchor = position;
	}
	CursorMoved();
	return true;
}

void TextFieldItem::Commit(const QString& text, int cursor)
{
	const bool changed = text != m_text;
	m_text             = text;
	m_cursor = m_anchor = cursor;
	Relayout();
	if (changed)
	{
		emit Edited(m_text);
	}
}

bool TextFieldItem::Insert(const QString& text)
{
	const int start = std::min(m_anchor, m_cursor);
	auto      next  = m_text;
	next.replace(start, std::abs(m_cursor - m_anchor), text);
	if (!Accepts(next))
	{
		return false;
	}
	Commit(next, start + static_cast<int>(text.size()));
	return true;
}

bool TextFieldItem::Erase(int position)
{
	const int start = HasSelection() ? std::min(m_anchor, m_cursor) : std::min(position, m_cursor);
	const int end   = HasSelection() ? std::max(m_anchor, m_cursor) : std::max(position, m_cursor);
	if (start == end)
	{
		return false;
	}
	Commit(QString(m_text).remove(start, end - start), start);
	return true;
}

bool TextFieldItem::EditKey(QKeyEvent* event)
{
	const int  length = static_cast<int>(m_text.size());
	const int  start  = std::min(m_anchor, m_cursor);
	const int  end    = std::max(m_anchor, m_cursor);
	const auto words  = QTextLayout::SkipWords;

	if (event->matches(QKeySequence::MoveToPreviousChar))
	{
		return MoveCursor(HasSelection() ? start : m_layout.previousCursorPosition(m_cursor), false);
	}
	if (event->matches(QKeySequence::MoveToNextChar))
	{
		return MoveCursor(HasSelection() ? end : m_layout.nextCursorPosition(m_cursor), false);
	}
	if (event->matches(QKeySequence::SelectPreviousChar))
	{
		return MoveCursor(m_layout.previousCursorPosition(m_cursor), true);
	}
	if (event->matches(QKeySequence::SelectNextChar))
	{
		return MoveCursor(m_layout.nextCursorPosition(m_cursor), true);
	}
	if (event->matches(QKeySequence::MoveToPreviousWord) || event->matches(QKeySequence::SelectPreviousWord))
	{
		return MoveCursor(m_layout.previousCursorPosition(m_cursor, words), event->matches(QKeySequence::SelectPreviousWord));
	}
	if (event->matches(QKeySequence::MoveToNextWord) || event->matches(QKeySequence::SelectNextWord))
	{
		return MoveCursor(m_layout.nextCursorPosition(m_cursor, words), event->matches(QKeySequence::SelectNextWord));
	}
	if (event->matches(QKeySequence::MoveToStartOfLine) || event->matches(QKeySequence::SelectStartOfLine))
	{
		return MoveCursor(0, event->matches(QKeySequence::SelectStartOfLine));
	}
	if (event->matches(QKeySequence::MoveToEndOfLine) || event->matches(QKeySequence::SelectEndOfLine))
	{
		return MoveCursor(length, event->matches(QKeySequence::SelectEndOfLine));
	}
	if (event->matches(QKeySequence::SelectAll))
	{
		m_anchor = 0;
		m_cursor = length;
		CursorMoved();
		return true;
	}
	if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut))
	{
		if (HasSelection())
		{
			QGuiApplication::clipboard()->setText(SelectedText());
		}
		if (event->matches(QKeySequence::Cut))
		{
			Erase(m_cursor);
		}
		return true;
	}
	if (event->matches(QKeySequence::Paste))
	{
		Insert(FirstLine(QGuiApplication::clipboard()->text()));
		return true;
	}
	if (event->matches(QKeySequence::DeleteStartOfWord))
	{
		Erase(m_layout.previousCursorPosition(m_cursor, words));
		return true;
	}
	if (event->matches(QKeySequence::DeleteEndOfWord))
	{
		Erase(m_layout.nextCursorPosition(m_cursor, words));
		return true;
	}
	switch (event->key())
	{
		case Qt::Key_Backspace: Erase(m_layout.previousCursorPosition(m_cursor)); return true;
		case Qt::Key_Delete: Erase(m_layout.nextCursorPosition(m_cursor)); return true;
		case Qt::Key_Return:
		case Qt::Key_Enter: emit PadRequested(); return true;
		default: break;
	}

	// Shortcuts with Control are not text; AltGr combinations are.
	const auto modifiers = event->modifiers() & ~Qt::KeypadModifier;
	const auto text      = event->text();
	if (text.isEmpty() || !text.at(0).isPrint() || modifiers == Qt::ControlModifier ||
	    modifiers == (Qt::ControlModifier | Qt::ShiftModifier))
	{
		return false;
	}
	// A rejected character goes on to the page, so the page's letter keys still work from a number field.
	return Insert(text);
}

void TextFieldItem::keyPressEvent(QKeyEvent* event)
{
	if (!EditKey(event))
	{
		event->ignore();
	}
}

void TextFieldItem::inputMethodEvent(QInputMethodEvent* event)
{
	const auto& commit = event->commitString();
	if (!commit.isEmpty() || event->replacementLength() > 0)
	{
		auto next   = m_text;
		int  cursor = std::min(m_anchor, m_cursor);
		next.remove(cursor, std::abs(m_cursor - m_anchor));
		const int from  = std::clamp(cursor + event->replacementStart(), 0, static_cast<int>(next.size()));
		const int count = std::min(event->replacementLength(), static_cast<int>(next.size()) - from);
		next.remove(from, count);
		next.insert(from, commit);
		cursor = from + static_cast<int>(commit.size());
		if (Accepts(next))
		{
			Commit(next, cursor);
		}
	}

	m_preedit        = event->preeditString();
	m_preedit_cursor = static_cast<int>(m_preedit.size());
	for (const auto& attribute: event->attributes())
	{
		if (attribute.type == QInputMethodEvent::Cursor)
		{
			m_preedit_cursor = attribute.start;
		}
	}
	Relayout();
	event->accept();
}

QVariant TextFieldItem::inputMethodQuery(Qt::InputMethodQuery query) const
{
	switch (query)
	{
		case Qt::ImEnabled: return true;
		case Qt::ImHints: return static_cast<int>(m_max_digits > 0 ? Qt::ImhDigitsOnly | Qt::ImhNoPredictiveText : Qt::ImhNoPredictiveText);
		case Qt::ImCursorRectangle: return CursorRect();
		case Qt::ImCursorPosition: return m_cursor;
		case Qt::ImAnchorPosition: return m_anchor;
		case Qt::ImSurroundingText: return m_text;
		case Qt::ImCurrentSelection: return SelectedText();
		case Qt::ImMaximumTextLength: return m_max_digits > 0 ? QVariant(m_max_digits) : QVariant();
		default: return QQuickItem::inputMethodQuery(query);
	}
}

void TextFieldItem::mousePressEvent(QMouseEvent* event)
{
	forceActiveFocus(Qt::MouseFocusReason);
	MoveCursor(PositionAt(event->position().x()), (event->modifiers() & Qt::ShiftModifier) != 0);
}

void TextFieldItem::mouseMoveEvent(QMouseEvent* event)
{
	MoveCursor(PositionAt(event->position().x()), true);
}

void TextFieldItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		CursorMoved();
	}
}

void TextFieldItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change != ItemActiveFocusHasChanged)
	{
		return;
	}
	if (!value.boolValue)
	{
		m_anchor = m_cursor;
	}
	RestartBlink();
}

QSGNode* TextFieldItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<FieldNode*>(old_node);
	if (node == nullptr)
	{
		node = new FieldNode(window());
	}
	const auto* theme   = Theme::Instance();
	const qreal u       = theme->U();
	const bool  focused = hasActiveFocus();

	RoundedBoxNode::Style style;
	style.fill         = theme->Glass();
	style.border       = focused ? theme->Accent() : theme->GlassBorder();
	style.border_width = focused ? RING_WIDTH * u : 1;
	style.radius       = RADIUS * u;
	style.dpr          = window()->effectiveDevicePixelRatio();
	node->box->Update(window(), boundingRect(), style);

	const auto inner = InnerRect();
	node->clip->SetRect(inner.adjusted(-CursorWidth(), 0, CursorWidth(), 0));

	const auto  line = m_layout.lineAt(0);
	const qreal top  = CentredBaseline(FieldFont(), boundingRect()) - line.ascent();
	node->text->clear();
	node->text->setColor(theme->TextPrimary());
	node->text->setSelectionColor(theme->Accent());
	node->text->setSelectionTextColor(theme->Background());
	// The node takes the selection's last character, not its length, whatever the parameter is called.
	const bool selected = focused && m_preedit.isEmpty() && HasSelection();
	node->text->addTextLayout(QPointF(std::round(inner.left() - m_scroll), std::round(top)), &m_layout,
	                          selected ? std::min(m_anchor, m_cursor) : -1, selected ? std::max(m_anchor, m_cursor) - 1 : -1);

	node->cursor->setRect(CursorRect());
	node->cursor->setColor(focused && m_cursor_shown ? theme->TextPrimary() : Qt::transparent);
	return node;
}
