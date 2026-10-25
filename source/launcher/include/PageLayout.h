#ifndef LAUNCHER_INCLUDE_PAGE_LAYOUT_H_
#define LAUNCHER_INCLUDE_PAGE_LAYOUT_H_

#include "Common.h"

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>
#include <QTextOption>
#include <QVector>

#include <algorithm>

// A page's content placed from the top down in content coordinates: texts, boxes and buttons. A page describes
// itself with it and PageItem draws it, scrolls it and moves the focus between its buttons. Every Add... call
// below the cursor moves the cursor past what it placed and the spacing.
class PageLayout
{
public:
	struct Text
	{
		QRectF                rect;
		QString               text;
		QFont                 font;
		QColor                color;
		Qt::Alignment         alignment = Qt::AlignLeft;
		QTextOption::WrapMode wrap      = QTextOption::WordWrap;
	};

	struct Box
	{
		QRectF rect;
		QColor fill;
		QColor border;
		qreal  radius = 0;
	};

	// What a page asks for; the layout sizes and places it.
	struct ButtonSpec
	{
		QString text;
		int     id      = 0;
		bool    primary = false;
		bool    danger  = false;
		bool    allowed = true;
	};

	struct Button
	{
		ButtonSpec spec;
		QRectF     rect;
		// Buttons placed by the same AddButtonRow share a row; the focus moves between rows with Up and Down.
		int row = 0;
	};

	PageLayout(qreal width, qreal u, qreal spacing): m_width(width), m_u(u), m_spacing(spacing) {}

	[[nodiscard]] qreal Width() const { return m_width; }
	[[nodiscard]] qreal U() const { return m_u; }
	[[nodiscard]] qreal Y() const { return m_y; }
	// Bottom of the content, without the spacing after the last block.
	[[nodiscard]] qreal Height() const { return std::max<qreal>(0, m_y - m_spacing); }

	[[nodiscard]] const QVector<Text>&   Texts() const { return m_texts; }
	[[nodiscard]] const QVector<Box>&    Boxes() const { return m_boxes; }
	[[nodiscard]] const QVector<Button>& Buttons() const { return m_buttons; }

	// Places without moving the cursor, for blocks a page arranges itself.
	void AddText(const Text& text) { m_texts << text; }
	void AddBox(const Box& box) { m_boxes << box; }
	// Moves the cursor past a block of the given height and the spacing.
	void Advance(qreal height) { m_y += height + m_spacing; }

	// Text across the whole width, broken into lines; returns where it went.
	QRectF AddParagraph(const QString& text, const QFont& font, const QColor& color, QTextOption::WrapMode wrap = QTextOption::WordWrap);
	// Buttons side by side, each as wide as its text.
	void AddButtonRow(const QVector<ButtonSpec>& buttons);

	// Height and the width a text adds to a button.
	[[nodiscard]] static qreal ButtonHeight(qreal u);
	[[nodiscard]] static qreal ButtonPadding(qreal u);
	[[nodiscard]] static QFont ButtonFont(bool primary);

private:
	qreal           m_width   = 0;
	qreal           m_u       = 1;
	qreal           m_spacing = 0;
	qreal           m_y       = 0;
	int             m_rows    = 0;
	QVector<Text>   m_texts;
	QVector<Box>    m_boxes;
	QVector<Button> m_buttons;
};

#endif /* LAUNCHER_INCLUDE_PAGE_LAYOUT_H_ */
