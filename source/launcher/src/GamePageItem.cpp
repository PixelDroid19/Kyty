#include "GamePageItem.h"

#include "GameArtItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QDateTime>
#include <QKeyEvent>
#include <QLocale>

#include <algorithm>
#include <cmath>
#include <tuple>

// Sizes in units.
constexpr qreal MAX_WIDTH      = 1040;
constexpr qreal TOP            = 40;
constexpr qreal SPACING        = 22;
constexpr qreal ART_SIZE       = 168;
constexpr qreal HEADER_SPACING = 28;
constexpr qreal LINE_SPACING   = 8;
constexpr qreal NAME_FONT      = 38;
constexpr qreal CARD_SPACING   = 16;
constexpr qreal CARD_PADDING   = 18;
constexpr qreal CARD_LINES     = 6;
constexpr int   TROPHY_COLUMNS = 2;

constexpr int PLAY     = 0;
constexpr int STOP     = 1;
constexpr int SETTINGS = 2;
constexpr int FOLDER   = 3;
constexpr int OPTIONS  = 4;

namespace {

QString GameName(const QVariantMap& game)
{
	const auto title = game.value("title").toString();
	return title.isEmpty() ? game.value("name").toString() : title;
}

// played: the game was started at least once, which a run ended before play time was kept leaves without seconds.
QString PlayTimeText(qint64 seconds, bool played)
{
	const auto minutes = seconds / 60;
	if (seconds == 0)
	{
		return played ? GamePageItem::tr("Not recorded yet") : GamePageItem::tr("Not played yet");
	}
	if (minutes < 1)
	{
		return GamePageItem::tr("Under a minute");
	}
	return minutes < 60 ? GamePageItem::tr("%1 min").arg(minutes) : GamePageItem::tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
}

QString GradeText(QChar grade)
{
	switch (grade.unicode())
	{
		case 'P': return GamePageItem::tr("Platinum");
		case 'G': return GamePageItem::tr("Gold");
		case 'S': return GamePageItem::tr("Silver");
		case 'B': return GamePageItem::tr("Bronze");
		default: return {};
	}
}

QString TrophyStatus(const GameTrophy& trophy)
{
	if (trophy.unlocked > 0)
	{
		return GamePageItem::tr("Unlocked %1")
		    .arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(trophy.unlocked), QLocale::ShortFormat));
	}
	if (trophy.progress >= 0 && trophy.target > 0)
	{
		return GamePageItem::tr("Progress %1 of %2").arg(std::min(trophy.progress, trophy.target)).arg(trophy.target);
	}
	return GamePageItem::tr("Locked");
}

} // namespace

GamePageItem::GamePageItem(QQuickItem* parent): PageItem(MAX_WIDTH, TOP, SPACING, parent)
{
	m_art_clip = new QQuickItem(this);
	m_art_clip->setClip(true);
	m_art = new GameArtItem(m_art_clip);
}

QVariantList GamePageItem::Hints() const
{
	return {QVariantMap {{"pad", QStringLiteral("✕")}, {"key", QStringLiteral("Enter")}, {"text", tr("Select")}},
	        QVariantMap {{"pad", QStringLiteral("△")}, {"key", QStringLiteral("O")}, {"text", tr("Options")}},
	        QVariantMap {{"pad", QStringLiteral("□")}, {"key", QStringLiteral("E")}, {"text", tr("Game settings")}},
	        QVariantMap {{"pad", QStringLiteral("○")}, {"key", QStringLiteral("Esc")}, {"text", tr("Back")}}};
}

void GamePageItem::SetGame(const QVariantMap& game, bool latest)
{
	const bool same_game = game == m_game && latest == m_latest;
	m_game               = game;
	m_latest             = latest;
	// Read again for the same game too: a run unlocks trophies without changing the game's entry.
	m_trophies = LauncherBackend::Instance()->Trophies(Row());
	if (!same_game)
	{
		m_art->SetGame(game.value("icon").toString(), GameName(game));
	}
	Recompose();
}

void GamePageItem::SetShowsArt(bool shows)
{
	m_shows_art = shows;
	Recompose();
}

// The icon beside the game's name and details, centred on each other.
void GamePageItem::AddHeader(PageLayout* layout)
{
	const auto* theme = Theme::Instance();
	const qreal u     = layout->U();
	const qreal art   = m_shows_art ? ART_SIZE * u : 0;
	const qreal x     = m_shows_art ? art + HEADER_SPACING * u : 0;
	const qreal width = std::max<qreal>(0, layout->Width() - x);

	QVector<PageLayout::Text> lines;
	if (m_latest)
	{
		PageLayout::Text label;
		label.text  = tr("Continue where you left off");
		label.font  = SceneFont(theme->FontSmall(), QFont::DemiBold);
		label.color = theme->Accent();
		lines << label;
	}
	PageLayout::Text name;
	name.text  = GameName(m_game);
	name.font  = SceneFont(NAME_FONT * u, QFont::DemiBold);
	name.color = theme->TextPrimary();
	lines << name;

	QStringList details;
	for (const char* key: {"serial", "region", "version"})
	{
		const auto value = m_game.value(key).toString();
		if (!value.isEmpty())
		{
			details << value;
		}
	}
	if (m_game.value("favorite").toBool())
	{
		details.prepend(QStringLiteral("★"));
	}
	if (!details.isEmpty())
	{
		PageLayout::Text line;
		line.text  = details.join(QStringLiteral("  ·  "));
		line.font  = SceneFont(theme->FontBody());
		line.color = theme->TextSecondary();
		lines << line;
	}

	qreal column = 0;
	for (auto& line: lines)
	{
		line.rect = QRectF(x, 0, width, TextBlockHeight(line.text, line.font, width));
		column += line.rect.height();
	}
	column += LINE_SPACING * u * static_cast<qreal>(lines.size() - 1);
	const qreal height = std::max(art, column);
	qreal       y      = layout->Y() + std::round((height - column) / 2);
	for (auto& line: lines)
	{
		line.rect.moveTop(y);
		y += line.rect.height() + LINE_SPACING * u;
		layout->AddText(line);
	}

	m_art_slot = QRectF(0, layout->Y() + std::round((height - art) / 2), art, art);
	layout->Advance(height);
}

// Play time, settings and folder side by side, each a heading, a value and a note in a glass panel of one height.
void GamePageItem::AddCards(PageLayout* layout) const
{
	const auto* theme = Theme::Instance();
	const qreal u     = layout->U();

	const auto last_run = m_game.value("lastRun").toLongLong();
	QString    last_text;
	if (last_run > 0)
	{
		last_text = tr("Last played %1").arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(last_run), QLocale::ShortFormat));
	}
	const auto sections = m_game.value("sections").toStringList();
	const bool custom   = m_game.value("customSettings").toBool();

	struct Card
	{
		QString heading;
		QString value;
		QString note;
	};
	const Card cards[] = {
	    {tr("Play time"), PlayTimeText(m_game.value("playTime").toLongLong(), last_run > 0), last_text},
	    {tr("Game settings"), custom ? tr("Its own settings") : tr("The global settings"),
	     tr("Screen %1").arg(m_game.value("screen").toString())},
	    {tr("Folder and data"), sections.isEmpty() ? tr("In no section") : sections.join(QStringLiteral(", ")),
	     m_game.value("basedir").toString()}};
	constexpr int count = sizeof(cards) / sizeof(cards[0]);

	const qreal spacing = CARD_SPACING * u;
	const qreal padding = CARD_PADDING * u;
	const qreal card_w  = std::floor((layout->Width() - (count - 1) * spacing) / count);
	const qreal inner   = std::max<qreal>(0, card_w - 2 * padding);
	const auto  heading = SceneFont(theme->FontSmall(), QFont::DemiBold);
	const auto  value   = SceneFont(theme->FontBody() * 1.2, QFont::DemiBold);
	const auto  note    = SceneFont(theme->FontSmall());
	const qreal top     = layout->Y();

	QVector<PageLayout::Text> texts;
	qreal                     height = 0;
	for (int i = 0; i < count; i++)
	{
		const qreal left = i * (card_w + spacing) + padding;
		qreal       y    = top + padding;
		for (const auto& [text, font, color]: {std::tuple {cards[i].heading, heading, theme->TextMuted()},
		                                       std::tuple {cards[i].value, value, theme->TextPrimary()},
		                                       std::tuple {cards[i].note, note, theme->TextSecondary()}})
		{
			if (text.isEmpty())
			{
				continue;
			}
			PageLayout::Text line;
			line.text  = text;
			line.font  = font;
			line.color = color;
			// A long folder path breaks anywhere instead of leaving the card.
			line.wrap = QTextOption::WrapAtWordBoundaryOrAnywhere;
			line.rect = QRectF(left, y, inner, TextBlockHeight(text, font, inner, line.wrap));
			texts << line;
			y += line.rect.height() + CARD_LINES * u;
		}
		height = std::max(height, y - CARD_LINES * u + padding - top);
	}
	for (int i = 0; i < count; i++)
	{
		PageLayout::Box panel;
		panel.rect   = QRectF(i * (card_w + spacing), top, card_w, height);
		panel.fill   = theme->Glass();
		panel.border = theme->GlassBorder();
		panel.radius = theme->Radius();
		layout->AddBox(panel);
	}
	for (const auto& text: texts)
	{
		layout->AddText(text);
	}
	layout->Advance(height);
}

// The game's trophies as glass tiles in columns under a count of the unlocked ones; a locked hidden trophy keeps its
// name and description to itself.
void GamePageItem::AddTrophies(PageLayout* layout) const
{
	if (m_trophies.isEmpty())
	{
		return;
	}
	const auto* theme = Theme::Instance();
	const qreal u     = layout->U();
	const auto  unlocked =
	    std::count_if(m_trophies.begin(), m_trophies.end(), [](const GameTrophy& trophy) { return trophy.unlocked > 0; });
	layout->AddParagraph(tr("Trophies  ·  %1 of %2 unlocked").arg(unlocked).arg(m_trophies.size()),
	                     SceneFont(theme->FontBody() * 1.2, QFont::DemiBold), theme->TextPrimary());

	const qreal spacing = CARD_SPACING * u;
	const qreal padding = CARD_PADDING * u;
	const qreal tile_w  = std::floor((layout->Width() - (TROPHY_COLUMNS - 1) * spacing) / TROPHY_COLUMNS);
	const qreal inner   = std::max<qreal>(0, tile_w - 2 * padding);
	const auto  name    = SceneFont(theme->FontBody(), QFont::DemiBold);
	const auto  small   = SceneFont(theme->FontSmall());
	qreal       top     = layout->Y();
	for (int first = 0; first < m_trophies.size(); first += TROPHY_COLUMNS)
	{
		QVector<PageLayout::Text> texts;
		qreal                     height = 0;
		const int                 last   = std::min<int>(first + TROPHY_COLUMNS, static_cast<int>(m_trophies.size()));
		for (int i = first; i < last; i++)
		{
			const auto& trophy = m_trophies[i];
			const bool  secret = trophy.hidden && trophy.unlocked == 0;
			const bool  earned = trophy.unlocked > 0;
			const auto  title  = secret ? tr("Hidden trophy") : trophy.name;
			const qreal left   = (i - first) * (tile_w + spacing) + padding;
			qreal       y      = top + padding;
			const auto  head   = GradeText(trophy.grade) + QStringLiteral("  ·  ") + title;
			for (const auto& [text, font, color]: {std::tuple {head, name, earned ? theme->TextPrimary() : theme->TextSecondary()},
			                                       std::tuple {secret ? QString() : trophy.detail, small, theme->TextSecondary()},
			                                       std::tuple {TrophyStatus(trophy), small, earned ? theme->Accent() : theme->TextMuted()}})
			{
				if (text.isEmpty())
				{
					continue;
				}
				PageLayout::Text line;
				line.text  = text;
				line.font  = font;
				line.color = color;
				line.wrap  = QTextOption::WrapAtWordBoundaryOrAnywhere;
				line.rect  = QRectF(left, y, inner, TextBlockHeight(text, font, inner, line.wrap));
				texts << line;
				y += line.rect.height() + CARD_LINES * u;
			}
			height = std::max(height, y - CARD_LINES * u + padding - top);
		}
		for (int i = first; i < last; i++)
		{
			PageLayout::Box tile;
			tile.rect   = QRectF((i - first) * (tile_w + spacing), top, tile_w, height);
			tile.fill   = theme->Glass();
			tile.border = theme->GlassBorder();
			tile.radius = theme->Radius();
			layout->AddBox(tile);
		}
		for (const auto& text: texts)
		{
			layout->AddText(text);
		}
		layout->Advance(height);
		top = layout->Y();
	}
}

void GamePageItem::Compose(PageLayout* layout)
{
	if (m_game.isEmpty())
	{
		return;
	}
	const auto* backend = LauncherBackend::Instance();
	AddHeader(layout);
	const bool running = m_game.value("running").toBool();
	const bool played  = m_game.value("lastRun").toLongLong() > 0;
	QVector<PageLayout::ButtonSpec> buttons;
	if (running)
	{
		buttons << PageLayout::ButtonSpec {tr("Stop game"), STOP, true, true};
	} else
	{
		buttons << PageLayout::ButtonSpec {played ? tr("Continue") : tr("Play"), PLAY, true, false,
		                                   backend->IsEmulatorReady() && !backend->IsRunning()};
	}
	buttons << PageLayout::ButtonSpec {tr("Game settings"), SETTINGS} << PageLayout::ButtonSpec {tr("Open game folder"), FOLDER}
	        << PageLayout::ButtonSpec {QStringLiteral("…"), OPTIONS};
	layout->AddButtonRow(buttons);
	AddCards(layout);
	AddTrophies(layout);
}

void GamePageItem::Press(int id)
{
	const int row = Row();
	switch (id)
	{
		case PLAY: emit RunRequested(row); break;
		case STOP: emit ActionRequested(QStringLiteral("stop"), row); break;
		case SETTINGS: emit EditRequested(row); break;
		case FOLDER: emit ActionRequested(QStringLiteral("folder"), row); break;
		case OPTIONS: emit MenuRequested(row); break;
		default: break;
	}
}

void GamePageItem::keyPressEvent(QKeyEvent* event)
{
	if (m_game.isEmpty())
	{
		PageItem::keyPressEvent(event);
		return;
	}
	if (event->key() == Qt::Key_O || event->key() == Qt::Key_Menu)
	{
		emit MenuRequested(Row());
		return;
	}
	if (event->key() == Qt::Key_E)
	{
		emit EditRequested(Row());
		return;
	}
	PageItem::keyPressEvent(event);
}

void GamePageItem::Placed()
{
	const auto* theme = Theme::Instance();
	const qreal top   = TOP * theme->U();
	m_art_clip->setPosition(QPointF(0, top));
	m_art_clip->setSize(QSizeF(width(), std::max<qreal>(0, height() - top - theme->Margin())));
	m_art->setVisible(m_shows_art && !m_game.isEmpty());
	m_art->setSize(m_art_slot.size());
	m_art->setPosition(ContentOrigin() + m_art_slot.topLeft() - QPointF(0, top));
}
