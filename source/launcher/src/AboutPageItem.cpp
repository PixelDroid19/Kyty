#include "AboutPageItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal MAX_WIDTH      = 820;
constexpr qreal TOP            = 40;
constexpr qreal SPACING        = 14;
constexpr qreal MASCOT_WIDTH   = 96;
constexpr qreal HEADER_SPACING = 28;
constexpr qreal TITLE_SPACING  = 6;
constexpr qreal TABLE_PADDING  = 16;
constexpr qreal COLUMN_SPACING = 18;
constexpr qreal ROW_SPACING    = 10;

constexpr int BACK = 0;

namespace {

struct Control
{
	const char* action;
	const char* keys;
	const char* pad;
	const char* mouse;
};

// Untranslated cells are key and button names.
const Control CONTROLS[] = {
    {QT_TRANSLATE_NOOP("AboutPageItem", "Move"), QT_TRANSLATE_NOOP("AboutPageItem", "Arrow keys"),
	 QT_TRANSLATE_NOOP("AboutPageItem", "D-pad or left stick"), QT_TRANSLATE_NOOP("AboutPageItem", "Click")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Run a game"), QT_TRANSLATE_NOOP("AboutPageItem", "Enter"), "✕",
	 QT_TRANSLATE_NOOP("AboutPageItem", "Double-click")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Options"), "O", "△ / Start", QT_TRANSLATE_NOOP("AboutPageItem", "Right-click")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Edit a game"), "E", "□", QT_TRANSLATE_NOOP("AboutPageItem", "Options, then Edit")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Switch folder"), "PgUp / PgDn", "L1 / R1", QT_TRANSLATE_NOOP("AboutPageItem", "Click a folder")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Save in the editor"), "O", "△", QT_TRANSLATE_NOOP("AboutPageItem", "Save")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Test run in the editor"), "E", "□", QT_TRANSLATE_NOOP("AboutPageItem", "Test run")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Type text"), QT_TRANSLATE_NOOP("AboutPageItem", "Type"),
	 QT_TRANSLATE_NOOP("AboutPageItem", "Enter opens the on-screen keyboard"), QT_TRANSLATE_NOOP("AboutPageItem", "Type")},
    {QT_TRANSLATE_NOOP("AboutPageItem", "Back or cancel"), "Esc", "○", QT_TRANSLATE_NOOP("AboutPageItem", "Click outside")},
};

constexpr int COLUMNS = 4;
// The action column is a little wider than the others.
constexpr qreal COLUMN_WEIGHTS[COLUMNS] = {210, 190, 190, 190};

qreal LineHeight(const QFont& font)
{
	return std::ceil(QFontMetricsF(font).height());
}

} // namespace

AboutPageItem::AboutPageItem(QQuickItem* parent): PageItem(MAX_WIDTH, TOP, SPACING, parent)
{
	m_mascot_clip = new QQuickItem(this);
	m_mascot_clip->setClip(true);
}

QVariantList AboutPageItem::Hints() const
{
	return {QVariantMap {{"pad", QStringLiteral("○")}, {"key", QStringLiteral("Esc")}, {"text", tr("Back")}}};
}

void AboutPageItem::SetMascot(QQuickItem* mascot)
{
	if (mascot == m_mascot)
	{
		return;
	}
	if (m_mascot != nullptr)
	{
		disconnect(m_mascot, nullptr, this, nullptr);
	}
	m_mascot = mascot;
	if (mascot != nullptr)
	{
		mascot->setParentItem(m_mascot_clip);
		connect(mascot, &QQuickItem::heightChanged, this, &AboutPageItem::Recompose);
	}
	Recompose();
	emit MascotChanged();
}

// The mascot beside the launcher's name and a line about the mascot, centred on each other.
void AboutPageItem::AddHeader(PageLayout* layout)
{
	const auto* theme  = Theme::Instance();
	const qreal u      = layout->U();
	const qreal mascot = m_mascot != nullptr ? m_mascot->height() : 0;
	const qreal x      = (MASCOT_WIDTH + HEADER_SPACING) * u;
	const qreal width  = std::max<qreal>(0, layout->Width() - x);

	PageLayout::Text title;
	title.text  = tr("Kyty Launcher");
	title.font  = SceneFont(theme->FontTitle(), QFont::DemiBold);
	title.color = theme->TextPrimary();
	title.rect  = QRectF(x, 0, width, LineHeight(title.font));

	PageLayout::Text line;
	line.text  = tr("Kit is the Kyty mascot. Hold the mouse on him to pet him.");
	line.font  = SceneFont(theme->FontSmall());
	line.color = theme->TextMuted();
	line.rect  = QRectF(x, 0, width, TextBlockHeight(line.text, line.font, width));

	const qreal column = title.rect.height() + TITLE_SPACING * u + line.rect.height();
	const qreal height = std::max(mascot, column);
	const qreal top    = layout->Y() + std::round((height - column) / 2);
	title.rect.moveTop(top);
	line.rect.moveTop(top + title.rect.height() + TITLE_SPACING * u);
	layout->AddText(title);
	layout->AddText(line);

	m_mascot_slot = QRectF(0, layout->Y() + std::round((height - mascot) / 2), MASCOT_WIDTH * u, mascot);
	layout->Advance(height);
}

// One row of four cells per control under a header row, in a glass panel.
void AboutPageItem::AddControls(PageLayout* layout) const
{
	const auto* theme   = Theme::Instance();
	const qreal u       = layout->U();
	const qreal padding = TABLE_PADDING * u;
	const qreal spacing = COLUMN_SPACING * u;
	const qreal inner   = std::max<qreal>(0, layout->Width() - 2 * padding - (COLUMNS - 1) * spacing);
	qreal       total   = 0;
	for (const qreal weight: COLUMN_WEIGHTS)
	{
		total += weight;
	}
	qreal widths[COLUMNS] {};
	qreal lefts[COLUMNS] {};
	qreal x = padding;
	for (int c = 0; c < COLUMNS; c++)
	{
		widths[c] = std::floor(inner * COLUMN_WEIGHTS[c] / total);
		lefts[c]  = x;
		x += widths[c] + spacing;
	}

	auto          header_font     = SceneFont(theme->FontSmall(), QFont::DemiBold);
	auto          body_font       = SceneFont(theme->FontBody());
	const QString header[COLUMNS] = {tr("Action"), tr("Keyboard"), tr("Controller"), tr("Mouse")};

	const qreal top     = layout->Y();
	qreal       y       = top + padding;
	auto        add_row = [&](const QString* cells, const QFont& font, const QColor& color)
	{
		qreal height = 0;
		for (int c = 0; c < COLUMNS; c++)
		{
			PageLayout::Text cell;
			cell.text  = cells[c];
			cell.font  = font;
			cell.color = color;
			cell.rect  = QRectF(lefts[c], y, widths[c], TextBlockHeight(cell.text, font, widths[c]));
			height     = std::max(height, cell.rect.height());
			layout->AddText(cell);
		}
		y += height + ROW_SPACING * u;
	};
	add_row(header, header_font, theme->TextMuted());
	for (const auto& control: CONTROLS)
	{
		const QString cells[COLUMNS] = {tr(control.action), tr(control.keys), tr(control.pad), tr(control.mouse)};
		add_row(cells, body_font, theme->TextPrimary());
	}
	const qreal height = y - ROW_SPACING * u + padding - top;

	PageLayout::Box panel;
	panel.rect   = QRectF(0, top, layout->Width(), height);
	panel.fill   = theme->Glass();
	panel.border = theme->GlassBorder();
	panel.radius = theme->Radius();
	layout->AddBox(panel);
	layout->Advance(height);
}

void AboutPageItem::Compose(PageLayout* layout)
{
	const auto* theme = Theme::Instance();
	AddHeader(layout);
	layout->AddParagraph(tr("Games are started by the Kyty emulator. The launcher only keeps the list of games and their settings."),
	                     SceneFont(theme->FontBody()), theme->TextSecondary());
	AddControls(layout);
	layout->AddButtonRow({{tr("Back"), BACK}});
}

void AboutPageItem::Press(int id)
{
	if (id == BACK)
	{
		emit closed();
	}
}

void AboutPageItem::Placed()
{
	const qreal u   = Theme::Instance()->U();
	const qreal top = TOP * u;
	m_mascot_clip->setPosition(QPointF(0, top));
	m_mascot_clip->setSize(QSizeF(width(), std::max<qreal>(0, height() - top - Theme::Instance()->Margin())));
	if (m_mascot == nullptr)
	{
		return;
	}
	m_mascot->setWidth(MASCOT_WIDTH * u);
	m_mascot->setPosition(ContentOrigin() + m_mascot_slot.topLeft() - QPointF(0, top));
}
