#include "SettingsPageItem.h"

#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal MAX_WIDTH       = 720;
constexpr qreal TOP             = 26;
constexpr qreal SPACING         = 10;
constexpr qreal HEADING_PADDING = 14;
constexpr qreal COLUMN_SPACING  = 24;
constexpr qreal ROW_SPACING     = 8;
constexpr qreal DOT             = 8;
constexpr qreal DOT_SPACING     = 10;
constexpr qreal BACK_GAP        = 6;

namespace {

enum Button
{
	EmulatorSettings,
	ChooseProgram,
	SearchProgram,
	RunMode,
	OpenLog,
	Back
};

} // namespace

SettingsPageItem::SettingsPageItem(QQuickItem* parent): PageItem(MAX_WIDTH, TOP, SPACING, parent)
{
	connect(LauncherBackend::Instance(), &LauncherBackend::StateChanged, this, &SettingsPageItem::Recompose);
}

QVariantList SettingsPageItem::Hints() const
{
	return {QVariantMap {{"pad", QStringLiteral("✕")}, {"key", QStringLiteral("Enter")}, {"text", tr("Select")}},
	        QVariantMap {{"pad", QStringLiteral("○")}, {"key", QStringLiteral("Esc")}, {"text", tr("Back")}}};
}

void SettingsPageItem::AddHeading(PageLayout* layout, const QString& text) const
{
	auto font = SceneFont(Theme::Instance()->FontSmall(), QFont::DemiBold);
	font.setCapitalization(QFont::AllUppercase);
	const qreal      padding = HEADING_PADDING * layout->U();
	PageLayout::Text heading;
	heading.rect  = QRectF(0, layout->Y() + padding, layout->Width(), LineHeight(font));
	heading.text  = text;
	heading.font  = font;
	heading.color = Theme::Instance()->TextMuted();
	layout->AddText(heading);
	layout->Advance(padding + heading.rect.height());
}

// What every game without custom settings runs with, one line per area: labels in the first column, values in
// the second.
void SettingsPageItem::AddSummary(PageLayout* layout) const
{
	const auto f  = LauncherBackend::Instance()->GlobalFields();
	const auto on = [](bool value) { return value ? tr("On") : tr("Off"); };
	const auto at = [&f](const char* key) { return f.value(QLatin1String(key)).toString(); };

	const QString resolution = at("render_resolution_mode") == QLatin1String("Fixed")
	                               ? QStringLiteral("%1 × %2 · %3")
	                                     .arg(at("render_resolution_width"), at("render_resolution_height"), at("render_resolution_scope"))
	                               : tr("Native");
	const QString filter     = at("presentation_filter") == QLatin1String("Fsr")
	                               ? QStringLiteral("FSR · %1").arg(at("presentation_fsr_sharpness"))
	                               : at("presentation_filter");
	const QString log        = at("printf_direction") == QLatin1String("Silent")
	                               ? at("printf_direction")
	                               : QStringLiteral("%1 · %2").arg(at("printf_direction"), at("printf_level"));
	const bool    validation = f.value("vulkan_validation_enabled").toBool() || f.value("shader_validation_enabled").toBool();

	const QVector<QPair<QString, QString>> rows = {
	    {tr("Screen"), QStringLiteral("%1 × %2").arg(at("screen_width"), at("screen_height"))},
	    {tr("Render resolution"), resolution},
	    {tr("Presentation filter"), filter},
	    {tr("Texture anisotropy"), at("texture_anisotropy")},
	    {tr("Neo mode"), on(f.value("neo").toBool())},
	    {tr("Shader optimization"), at("shader_optimization_type")},
	    {tr("Log output"), log},
	    {tr("Validation"), on(validation)},
	};

	const auto* theme       = Theme::Instance();
	const qreal u           = layout->U();
	const auto  font        = SceneFont(theme->FontBody());
	const qreal line        = LineHeight(font);
	qreal       label_width = 0;
	for (const auto& row: rows)
	{
		label_width = std::max(label_width, std::ceil(TextWidth(row.first, font)));
	}
	const qreal value_x = label_width + COLUMN_SPACING * u;
	qreal       y       = layout->Y();
	for (const auto& row: rows)
	{
		PageLayout::Text label;
		label.rect  = QRectF(0, y, label_width, line);
		label.text  = row.first;
		label.font  = font;
		label.color = theme->TextSecondary();
		label.wrap  = QTextOption::NoWrap;
		layout->AddText(label);

		PageLayout::Text value = label;
		value.rect             = QRectF(value_x, y, std::max<qreal>(0, layout->Width() - value_x), line);
		value.text             = row.second;
		value.color            = theme->TextPrimary();
		layout->AddText(value);
		y += line + ROW_SPACING * u;
	}
	layout->Advance(y - layout->Y() - ROW_SPACING * u);
}

// A status dot beside the program's path, or beside why no program is usable.
void SettingsPageItem::AddProgram(PageLayout* layout) const
{
	const auto* theme   = Theme::Instance();
	const auto* backend = LauncherBackend::Instance();
	const qreal u       = layout->U();
	const bool  ready   = backend->IsEmulatorReady();

	PageLayout::Text text;
	text.text         = ready ? backend->EmulatorPath() : backend->EmulatorMessage();
	text.font         = SceneFont(theme->FontBody());
	text.color        = ready ? theme->TextPrimary() : theme->Danger();
	text.wrap         = QTextOption::WrapAnywhere;
	const qreal x     = (DOT + DOT_SPACING) * u;
	const qreal width = std::max<qreal>(0, layout->Width() - x);
	text.rect         = QRectF(x, layout->Y(), width, TextBlockHeight(text.text, text.font, width, text.wrap));
	layout->AddText(text);

	PageLayout::Box dot;
	dot.rect   = QRectF(0, std::round(text.rect.center().y() - DOT * u / 2), DOT * u, DOT * u);
	dot.fill   = ready ? theme->Ok() : theme->Danger();
	dot.radius = DOT * u / 2;
	layout->AddBox(dot);
	layout->Advance(text.rect.height());
}

void SettingsPageItem::Compose(PageLayout* layout)
{
	const auto* theme   = Theme::Instance();
	const auto* backend = LauncherBackend::Instance();
	const auto  small   = SceneFont(theme->FontSmall());
	const auto  body    = SceneFont(theme->FontBody());

	layout->AddParagraph(tr("Settings"), SceneFont(theme->FontTitle(), QFont::DemiBold), theme->TextPrimary());

	AddHeading(layout, tr("Emulator settings"));
	layout->AddParagraph(tr("Every game runs with these unless it has custom emulator settings of its own."), small, theme->TextMuted());
	AddSummary(layout);
	layout->AddButtonRow({{tr("Change emulator settings…"), EmulatorSettings, true}});

	AddHeading(layout, tr("Emulator program"));
	AddProgram(layout);
	if (!backend->EmulatorVersion().isEmpty())
	{
		layout->AddParagraph(backend->EmulatorVersion(), small, theme->TextMuted());
	}
	layout->AddButtonRow({{tr("Choose program…"), ChooseProgram}, {tr("Search automatically"), SearchProgram}});

	AddHeading(layout, tr("Run mode"));
	const auto modes = backend->RunModes();
	const auto mode  = modes.value(backend->RunModeIndex());
	if (backend->LogPath().isEmpty())
	{
		layout->AddButtonRow({{tr("Run: %1").arg(mode), RunMode}});
	} else
	{
		layout->AddButtonRow({{tr("Run: %1").arg(mode), RunMode}, {tr("Open last log"), OpenLog}});
	}
	layout->AddParagraph(tr("Direct runs the emulator without a window of its own and writes its output to a log file. A terminal shows "
	                        "the output live, for debugging."),
	                     small, theme->TextMuted());

	AddHeading(layout, tr("Controller"));
	layout->AddParagraph(!backend->ControllerName().isEmpty() ? backend->ControllerName()
	                                                          : tr("No controller connected. Keyboard and mouse work everywhere."),
	                     body, theme->TextSecondary());

	AddHeading(layout, tr("Settings file"));
	layout->AddParagraph(backend->SettingsFile(), small, theme->TextSecondary(), QTextOption::WrapAnywhere);

	layout->Advance(BACK_GAP * layout->U());
	layout->AddButtonRow({{tr("Back"), Back}});
}

void SettingsPageItem::Press(int id)
{
	auto* backend = LauncherBackend::Instance();
	switch (id)
	{
		case EmulatorSettings: emit emulatorSettingsRequested(); break;
		case ChooseProgram: emit programRequested(); break;
		case SearchProgram: backend->SetEmulatorPath(QString()); break;
		case RunMode: backend->SetRunModeIndex((backend->RunModeIndex() + 1) % std::max<int>(1, backend->RunModes().size())); break;
		case OpenLog: backend->OpenLog(); break;
		case Back: emit closed(); break;
		default: break;
	}
}
