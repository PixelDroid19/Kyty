#include "PageLayout.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <cmath>

// Sizes in units.
constexpr qreal BUTTON_HEIGHT  = 40;
constexpr qreal BUTTON_PADDING = 36;
constexpr qreal BUTTON_SPACING = 10;

QRectF PageLayout::AddParagraph(const QString& text, const QFont& font, const QColor& color, QTextOption::WrapMode wrap)
{
	const QRectF rect(0, m_y, m_width, TextBlockHeight(text, font, m_width, wrap));
	Text         block;
	block.rect  = rect;
	block.text  = text;
	block.font  = font;
	block.color = color;
	block.wrap  = wrap;
	m_texts << block;
	Advance(rect.height());
	return rect;
}

void PageLayout::AddButtonRow(const QVector<ButtonSpec>& buttons)
{
	const qreal height = ButtonHeight(m_u);
	qreal       x      = 0;
	for (const auto& spec: buttons)
	{
		Button button;
		button.spec = spec;
		button.rect = QRectF(x, m_y, std::ceil(TextWidth(spec.text, ButtonFont(spec.primary)) + ButtonPadding(m_u)), height);
		button.row  = m_rows;
		m_buttons << button;
		x += button.rect.width() + BUTTON_SPACING * m_u;
	}
	m_rows++;
	Advance(height);
}

qreal PageLayout::ButtonHeight(qreal u)
{
	return BUTTON_HEIGHT * u;
}

qreal PageLayout::ButtonPadding(qreal u)
{
	return BUTTON_PADDING * u;
}

QFont PageLayout::ButtonFont(bool primary)
{
	return SceneFont(Theme::Instance()->FontBody(), primary ? QFont::DemiBold : QFont::Normal);
}
