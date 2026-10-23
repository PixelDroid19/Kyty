#ifndef LAUNCHER_INCLUDE_TEXT_FIELD_ITEM_H_
#define LAUNCHER_INCLUDE_TEXT_FIELD_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QString>
#include <QTextLayout>
#include <QTimer>

// One line of editable text in a framed box: a blinking cursor, a selection made with Shift and the arrow keys or
// by dragging, the clipboard keys, word jumps and input methods. Enter and Return ask for the on-screen keyboard;
// keys the field does not use (Up, Down, Escape, and Left or Right at an end) go on to the page.
class TextFieldItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(TextFieldItem);

signals:
	// The user changed the text; SetText does not report.
	void Edited(const QString& text);
	void PadRequested();

public:
	explicit TextFieldItem(QQuickItem* parent = nullptr);
	~TextFieldItem() override = default;

	[[nodiscard]] QString Text() const { return m_text; }
	// Replaces the text, leaving the cursor at its end.
	void SetText(const QString& text);
	// From now on only whole numbers of at most max_digits digits are accepted.
	void SetDigitsOnly(int max_digits) { m_max_digits = max_digits; }

	// Whether text is empty or a whole number of at most max_digits digits.
	[[nodiscard]] static bool IsNumberText(const QString& text, int max_digits);

	QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     inputMethodEvent(QInputMethodEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseMoveEvent(QMouseEvent* event) override;

private:
	// Lays the text and any input method preedit out again, for a new text or font size.
	void Relayout();
	// Handles the cursor and editing keys; false for a key the field does not use here.
	bool EditKey(QKeyEvent* event);
	// Moves the cursor, extending the selection when select is set; false when nothing would change.
	bool MoveCursor(int position, bool select);
	// Replaces the selection with text; false when the result is not acceptable.
	bool Insert(const QString& text);
	// Deletes the selection, or the text between the cursor and position; false when nothing would change.
	bool Erase(int position);
	void Commit(const QString& text, int cursor);
	void CursorMoved();
	void RestartBlink();

	[[nodiscard]] bool    Accepts(const QString& text) const;
	[[nodiscard]] bool    HasSelection() const { return m_anchor != m_cursor; }
	[[nodiscard]] QString SelectedText() const;
	[[nodiscard]] QRectF  InnerRect() const;
	[[nodiscard]] QRectF  CursorRect() const;
	[[nodiscard]] int     PositionAt(qreal x) const;

	QString     m_text;
	QString     m_preedit;
	int         m_preedit_cursor = 0;
	int         m_cursor         = 0;
	int         m_anchor         = 0;
	int         m_max_digits     = 0;
	qreal       m_scroll         = 0;
	bool        m_cursor_shown   = true;
	QTextLayout m_layout;
	QTimer      m_blink;
};

#endif /* LAUNCHER_INCLUDE_TEXT_FIELD_ITEM_H_ */
