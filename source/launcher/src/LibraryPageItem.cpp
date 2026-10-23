#include "LibraryPageItem.h"

#include "ActionMenuItem.h"
#include "ArtworkSet.h"
#include "FocusButtonItem.h"
#include "FolderTabsItem.h"
#include "GameGridItem.h"
#include "GameTableItem.h"
#include "HeroRowItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextFieldItem.h"
#include "TextPadItem.h"
#include "Theme.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGRectangleNode>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal HEADER_TOP       = 26;
constexpr qreal HEADER_SPACING   = 4;
constexpr qreal STATUS_TOP       = 34;
constexpr qreal STATUS_SPACING   = 22;
constexpr qreal DOT              = 8;
constexpr qreal DOT_GAP          = 8;
constexpr qreal HERO_TOP         = 20;
constexpr qreal HERO_BOX         = 176;
constexpr qreal HERO_GAP         = 12;
constexpr qreal HERO_HEIGHT      = 136;
constexpr qreal TOOLBAR_TOP      = 18;
constexpr qreal TOOLBAR_HEIGHT   = 40;
constexpr qreal SEARCH_WIDTH     = 280;
constexpr qreal SEARCH_ICON      = 18;
constexpr qreal SEARCH_ICON_GAP  = 10;
constexpr qreal SWITCH_WIDTH     = 36;
constexpr qreal SWITCH_SPACING   = 4;
constexpr qreal SWITCH_ICON      = 20;
constexpr qreal SWITCH_MARK      = 16;
constexpr qreal SWITCH_MARK_LINE = 2;
// Cells pad each cover by 12 units, so the covers line up with the title above.
constexpr qreal GRID_INSET    = 12;
constexpr qreal GRID_TOP      = 14;
constexpr qreal TABLE_INSET   = 6;
constexpr qreal TABLE_TOP     = 16;
constexpr qreal EMPTY_PADDING = 40;
constexpr qreal EMPTY_SPACING = 16;

namespace {

enum Icon
{
	SEARCH_ICON_INDEX,
	GRID_ICON_INDEX,
	TABLE_ICON_INDEX,
	ICON_COUNT
};

const char* const ICON_FILES[ICON_COUNT] = {":/assets/icons/search.svg", ":/assets/icons/library.svg", ":/assets/icons/list.svg"};
constexpr qreal   ICON_SIZES[ICON_COUNT] = {SEARCH_ICON, SWITCH_ICON, SWITCH_ICON};

QFont TitleFont()
{
	return SceneFont(Theme::Instance()->FontTitle(), QFont::DemiBold);
}

QFont SmallFont()
{
	return SceneFont(Theme::Instance()->FontSmall());
}

QFont BodyFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

class LibraryNode: public GroupNode
{
public:
	explicit LibraryNode(QQuickWindow* window)
	{
		for (auto** text: {&title, &details, &hero_label, &placeholder, &empty})
		{
			*text = window->createTextNode();
			appendChildNode(*text);
		}
		for (int i = 0; i < 2; i++)
		{
			dots[i] = new RoundedBoxNode;
			appendChildNode(dots[i]);
			status[i] = window->createTextNode();
			appendChildNode(status[i]);
		}
		underline = window->createRectangleNode();
		appendChildNode(underline);
		switch_mark = new RoundedBoxNode;
		appendChildNode(switch_mark);
		for (auto*& opacity: icon_opacity)
		{
			opacity = new QSGOpacityNode;
			appendChildNode(opacity);
		}
	}

	ArtworkTextures   textures;
	QSGTextNode*      title       = nullptr;
	QSGTextNode*      details     = nullptr;
	QSGTextNode*      hero_label  = nullptr;
	QSGTextNode*      placeholder = nullptr;
	QSGTextNode*      empty       = nullptr;
	RoundedBoxNode*   dots[2] {};
	QSGTextNode*      status[2] {};
	QSGRectangleNode* underline   = nullptr;
	RoundedBoxNode*   switch_mark = nullptr;
	QSGOpacityNode*   icon_opacity[ICON_COUNT] {};
	QSGImageNode*     icons[ICON_COUNT] {};
};

} // namespace

LibraryPageItem::LibraryPageItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setAcceptedMouseButtons(Qt::LeftButton);
	setAcceptHoverEvents(true);

	m_hero = new HeroRowItem(this);
	m_tabs = new FolderTabsItem(this);
	m_search = new TextFieldItem(this);
	m_search->SetFramed(false);
	m_grid  = new GameGridItem(this);
	m_table = new GameTableItem(this);
	m_add   = new FocusButtonItem(this);
	m_add->SetText(tr("Add game"));
	m_add->SetPrimary(true);
	// Over everything else on the page.
	m_menu = new ActionMenuItem(this);
	m_pad  = new TextPadItem(this);

	auto* backend = LauncherBackend::Instance();
	connect(m_hero, &HeroRowItem::Activated, this,
	        [this](int index) { emit RunRequested(m_recent.value(index).toMap().value("row").toInt()); });
	connect(m_hero, &HeroRowItem::MenuRequested, this, [this](int index) { OpenMenu(m_recent.value(index).toMap().value("row").toInt()); });
	connect(m_hero, &HeroRowItem::CurrentIndexChanged, this, &LibraryPageItem::UpdateFocusedGame);
	connect(m_hero, &QQuickItem::activeFocusChanged, this, &LibraryPageItem::UpdateFocusedGame);
	connect(m_tabs, &FolderTabsItem::CurrentIndexChanged, this, &LibraryPageItem::Filter);
	connect(m_tabs, &FolderTabsItem::TabPaddingChanged, this, &QQuickItem::polish);
	connect(m_search, &TextFieldItem::Edited, this, &LibraryPageItem::Filter);
	connect(m_search, &TextFieldItem::PadRequested, this, &LibraryPageItem::SearchReturn);
	connect(m_search, &QQuickItem::activeFocusChanged, this, [this]() { update(); });
	connect(m_grid, &GameGridItem::CurrentIndexChanged, this, &LibraryPageItem::UpdateFocusedGame);
	connect(m_table, &GameTableItem::CurrentIndexChanged, this, &LibraryPageItem::UpdateFocusedGame);
	auto run  = [this](int index) { emit RunRequested(m_visible_games.value(index).toMap().value("row").toInt()); };
	auto menu = [this](int index) { OpenMenu(m_visible_games.value(index).toMap().value("row").toInt()); };
	connect(m_grid, &GameGridItem::Activated, this, run);
	connect(m_grid, &GameGridItem::MenuRequested, this, menu);
	connect(m_table, &GameTableItem::Activated, this, run);
	connect(m_table, &GameTableItem::MenuRequested, this, menu);
	connect(m_add, &FocusButtonItem::Activated, this, &LibraryPageItem::AddRequested);
	connect(m_menu, &ActionMenuItem::Chosen, this, &LibraryPageItem::MenuChosen);
	connect(m_menu, &ActionMenuItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_menu->Shown())
		        {
			        FocusGames();
		        }
	        });
	connect(m_pad, &TextPadItem::Edited, this,
	        [this](const QString& text)
	        {
		        m_search->SetText(text);
		        Filter();
	        });
	connect(m_pad, &TextPadItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_pad->Shown())
		        {
			        m_search->forceActiveFocus();
		        }
	        });
	connect(backend, &LauncherBackend::StateChanged, this, &LibraryPageItem::Refresh);
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);
	Refresh();
}

QVariantList LibraryPageItem::Hints()
{
	return {QVariantMap {{"pad", QStringLiteral("✕")}, {"key", QStringLiteral("Enter")}, {"text", tr("Run")}},
	        QVariantMap {{"pad", QStringLiteral("△")}, {"key", QStringLiteral("O")}, {"text", tr("Options")}},
	        QVariantMap {{"pad", QStringLiteral("□")}, {"key", QStringLiteral("E")}, {"text", tr("Edit")}},
	        QVariantMap {{"pad", QStringLiteral("L1 R1")}, {"key", QStringLiteral("PgUp/PgDn")}, {"text", tr("Switch folder")}},
	        QVariantMap {{"key", QStringLiteral("Ctrl+F")}, {"text", tr("Search")}},
	        QVariantMap {{"key", QStringLiteral("V")}, {"text", tr("Grid / table")}}};
}

QQuickItem* LibraryPageItem::GameView() const
{
	if (LauncherBackend::Instance()->IsTableView())
	{
		return m_table;
	}
	return m_grid;
}

QString LibraryPageItem::SearchText() const
{
	return m_search->Text().trimmed().toLower();
}

// The backend changed: games, folders, recently played, the view or the status line.
void LibraryPageItem::Refresh()
{
	const auto* backend = LauncherBackend::Instance();
	m_games             = backend->Games();
	m_recent            = backend->Recent();
	m_hero->SetGames(m_recent);
	m_hero->setVisible(HasRecent());
	m_tabs->SetFolders(backend->Folders());
	m_add->setVisible(m_games.isEmpty());
	Filter();
}

void LibraryPageItem::Filter()
{
	const auto folders = LauncherBackend::Instance()->Folders();
	m_folder           = folders.value(m_tabs->CurrentIndex()).toMap();
	const bool all     = m_folder.isEmpty() || m_folder.value("all").toBool();
	const auto folder  = m_folder.value("folder").toString();
	const auto query   = SearchText();

	m_visible_games.clear();
	for (const auto& entry: m_games)
	{
		const auto game = entry.toMap();
		if (!all && game.value("folder").toString() != folder)
		{
			continue;
		}
		auto name = game.value("title").toString();
		if (name.isEmpty())
		{
			name = game.value("name").toString();
		}
		if (query.isEmpty() || name.toLower().contains(query) || game.value("serial").toString().toLower().contains(query))
		{
			m_visible_games.append(game);
		}
	}

	const bool table = LauncherBackend::Instance()->IsTableView();
	const bool shown = !m_visible_games.isEmpty();
	m_grid->SetGames(m_visible_games);
	m_table->SetGames(m_visible_games);
	m_grid->setVisible(!table && shown);
	m_table->setVisible(table && shown);
	UpdateFocusedGame();
	polish();
	update();
}

void LibraryPageItem::UpdateFocusedGame()
{
	QVariantMap game;
	if (m_hero->hasActiveFocus())
	{
		game = m_recent.value(m_hero->CurrentIndex()).toMap();
	} else
	{
		const int index = LauncherBackend::Instance()->IsTableView() ? m_table->CurrentIndex() : m_grid->CurrentIndex();
		game            = m_visible_games.value(index).toMap();
	}
	if (game == m_focused_game)
	{
		return;
	}
	m_focused_game = game;
	update();
	emit FocusedGameChanged();
}

int LibraryPageItem::IndexOfRow(int row) const
{
	for (int i = 0; i < m_visible_games.size(); i++)
	{
		if (m_visible_games.at(i).toMap().value("row").toInt() == row)
		{
			return i;
		}
	}
	return -1;
}

void LibraryPageItem::SelectRow(int row)
{
	int index = IndexOfRow(row);
	if (index < 0)
	{
		m_tabs->SetCurrentIndex(0);
		index = IndexOfRow(row);
	}
	const int current = std::max(0, index);
	m_grid->SetCurrentIndex(current);
	m_table->SetCurrentIndex(current);
}

void LibraryPageItem::FocusDefault()
{
	FocusGames();
}

void LibraryPageItem::FocusGames()
{
	if (!m_visible_games.isEmpty())
	{
		GameView()->forceActiveFocus();
	} else if (m_add->isVisible())
	{
		m_add->forceActiveFocus();
	} else
	{
		m_tabs->forceActiveFocus();
	}
}

void LibraryPageItem::FocusSearch()
{
	m_search->forceActiveFocus();
	m_search->SelectAll();
}

void LibraryPageItem::ToggleView()
{
	auto*     backend = LauncherBackend::Instance();
	const int index   = backend->IsTableView() ? m_table->CurrentIndex() : m_grid->CurrentIndex();
	backend->SetTableView(!backend->IsTableView());
	m_grid->SetCurrentIndex(index);
	m_table->SetCurrentIndex(index);
	GameView()->forceActiveFocus();
}

void LibraryPageItem::ChangeFolder(int delta)
{
	const int count = static_cast<int>(LauncherBackend::Instance()->Folders().size());
	if (count == 0)
	{
		return;
	}
	m_tabs->SetCurrentIndex((m_tabs->CurrentIndex() + delta + count) % count);
}

// A controller cannot type, so Enter opens the on-screen pad while one is connected.
void LibraryPageItem::SearchReturn()
{
	if (LauncherBackend::Instance()->ControllerName().isEmpty())
	{
		FocusGames();
		return;
	}
	m_pad->SetText(m_search->Text());
	m_pad->OpenFor(QStringLiteral("search"), tr("Search games"));
}

void LibraryPageItem::OpenMenu(int row)
{
	const auto* backend = LauncherBackend::Instance();
	const auto  game    = m_games.value(row).toMap();
	if (game.isEmpty())
	{
		return;
	}
	const auto         last = m_games.size() - 1;
	const QVariantList items {
	    QVariantMap {{"label", tr("Run")}, {"action", "run"}, {"allowed", backend->IsEmulatorReady() && !backend->IsRunning()}},
	    QVariantMap {{"label", tr("Stop game")}, {"action", "stop"}, {"allowed", backend->IsRunning()}, {"danger", true}},
	    QVariantMap {{"label", tr("Edit")}, {"action", "edit"}},
	    QVariantMap {{"label", tr("Move up")}, {"action", "up"}, {"allowed", row > 0}},
	    QVariantMap {{"label", tr("Move down")}, {"action", "down"}, {"allowed", row < last}},
	    QVariantMap {{"label", tr("Open game folder")}, {"action", "folder"}},
	    QVariantMap {{"label", tr("Delete")}, {"action", "delete"}, {"danger", true}},
	    QVariantMap {{"label", tr("Close")}, {"action", "close"}}};
	m_menu->OpenFor(game, row, items);
}

void LibraryPageItem::MenuChosen(const QString& action, int row)
{
	if (action == QLatin1String("run"))
	{
		emit RunRequested(row);
	} else if (action == QLatin1String("edit"))
	{
		emit EditRequested(row);
	} else
	{
		emit ActionRequested(action, row);
	}
}

// Moves between the hero row, the tabs, the search box and the games.
bool LibraryPageItem::NavigateKey(QKeyEvent* event)
{
	const auto* focus = window() != nullptr ? window()->activeFocusItem() : nullptr;
	const int   key   = event->key();
	auto        move  = [](QQuickItem* target)
	{
		target->forceActiveFocus(Qt::TabFocusReason);
		return true;
	};
	if (focus == m_hero)
	{
		return key == Qt::Key_Down && move(m_tabs);
	}
	if (focus == m_tabs)
	{
		if (key == Qt::Key_Right)
		{
			return move(m_search);
		}
		if (key == Qt::Key_Up && HasRecent())
		{
			return move(m_hero);
		}
		if (key == Qt::Key_Down)
		{
			FocusGames();
			return true;
		}
		return false;
	}
	if (focus == m_search)
	{
		if (key == Qt::Key_Left)
		{
			return move(m_tabs);
		}
		if (key == Qt::Key_Up && HasRecent())
		{
			return move(m_hero);
		}
		if (key == Qt::Key_Down)
		{
			FocusGames();
			return true;
		}
		// Escape clears the search first, and only an empty search lets it through.
		if (key == Qt::Key_Escape && !m_search->Text().isEmpty())
		{
			m_search->SetText(QString());
			Filter();
			return true;
		}
		return false;
	}
	return (focus == m_grid || focus == m_table) && key == Qt::Key_Up && move(m_tabs);
}

// Keys of the grid and the table that act on the selected game or the page.
bool LibraryPageItem::GameKey(QKeyEvent* event)
{
	const int  key = event->key();
	const auto row = m_visible_games.value(GameView() == m_table ? m_table->CurrentIndex() : m_grid->CurrentIndex()).toMap();
	if (key == Qt::Key_E && !row.isEmpty())
	{
		emit EditRequested(row.value("row").toInt());
	} else if ((key == Qt::Key_O || key == Qt::Key_Menu) && !row.isEmpty())
	{
		OpenMenu(row.value("row").toInt());
	} else if (key == Qt::Key_V)
	{
		ToggleView();
	} else
	{
		return false;
	}
	return true;
}

void LibraryPageItem::keyPressEvent(QKeyEvent* event)
{
	const auto* focus      = window() != nullptr ? window()->activeFocusItem() : nullptr;
	const bool  games_view = focus == m_grid || focus == m_table;
	// L1 and R1 (PageUp and PageDown) switch folders from the tabs and the games.
	if ((games_view || focus == m_tabs) && (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown))
	{
		ChangeFolder(event->key() == Qt::Key_PageUp ? -1 : 1);
		return;
	}
	// Search works from anywhere on the page; the search field keeps the slash it types.
	if (event->key() == Qt::Key_Slash || (event->key() == Qt::Key_F && (event->modifiers() & Qt::ControlModifier) != 0))
	{
		FocusSearch();
		return;
	}
	if (focus == m_hero && (event->key() == Qt::Key_O || event->key() == Qt::Key_Menu))
	{
		OpenMenu(m_recent.value(m_hero->CurrentIndex()).toMap().value("row").toInt());
		return;
	}
	if (NavigateKey(event) || (games_view && GameKey(event)))
	{
		return;
	}
	event->ignore();
}

int LibraryPageItem::SwitchAt(QPointF point) const
{
	for (int i = 0; i < 2; i++)
	{
		if (m_geometry.switches[i].contains(point))
		{
			return i;
		}
	}
	return -1;
}

void LibraryPageItem::mousePressEvent(QMouseEvent* event)
{
	const int index = SwitchAt(event->position());
	if (index < 0)
	{
		event->ignore();
		return;
	}
	if ((index == 1) != LauncherBackend::Instance()->IsTableView())
	{
		ToggleView();
	}
}

void LibraryPageItem::hoverMoveEvent(QHoverEvent* event)
{
	SetHoveredSwitch(SwitchAt(event->position()));
}

void LibraryPageItem::hoverLeaveEvent(QHoverEvent* /*event*/)
{
	SetHoveredSwitch(-1);
}

void LibraryPageItem::SetHoveredSwitch(int index)
{
	if (index == m_hovered_switch)
	{
		return;
	}
	m_hovered_switch = index;
	if (index >= 0)
	{
		setCursor(Qt::PointingHandCursor);
	} else
	{
		unsetCursor();
	}
	update();
}

void LibraryPageItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		polish();
	}
}

void LibraryPageItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
	{
		polish();
	}
}

QStringList LibraryPageItem::StatusTexts() const
{
	const auto* backend    = LauncherBackend::Instance();
	const auto  controller = backend->ControllerName();
	return {backend->IsEmulatorReady() ? tr("Emulator ready") : tr("Emulator not found"),
	        controller.isEmpty() ? tr("No controller") : controller};
}

QString LibraryPageItem::EmptyText() const
{
	if (m_games.isEmpty())
	{
		return tr("No games yet. Add a game folder to start your library.");
	}
	if (!SearchText().isEmpty())
	{
		return tr("No games match “%1”.").arg(m_search->Text().trimmed());
	}
	return tr("No games in this folder.");
}

// Places the child items and works out where the page's own drawing goes.
void LibraryPageItem::updatePolish()
{
	const auto* theme  = Theme::Instance();
	const qreal u      = theme->U();
	const qreal margin = theme->Margin();
	const qreal gap    = theme->Gap();
	const qreal w      = width();
	const qreal h      = height();
	Geometry    g;

	const auto  small        = SmallFont();
	const auto  texts        = StatusTexts();
	const qreal status_line  = LineHeight(small);
	qreal       status_width = STATUS_SPACING * u;
	for (const auto& text: texts)
	{
		status_width += (DOT + DOT_GAP) * u + TextWidth(text, small);
	}
	g.status = QRectF(w - margin - status_width, STATUS_TOP * u, status_width, status_line);

	const qreal header_width = std::max<qreal>(0, g.status.left() - gap - margin);
	g.title                  = QRectF(margin, HEADER_TOP * u, header_width, LineHeight(TitleFont()));
	g.details = QRectF(margin, g.title.bottom() + HEADER_SPACING * u, header_width, std::ceil(theme->FontSmall() * 1.5));

	qreal toolbar_top = g.details.bottom();
	if (HasRecent())
	{
		const qreal top = g.details.bottom() + HERO_TOP * u;
		g.hero_label    = QRectF(margin, top, w - margin, status_line);
		m_hero->setPosition(QPointF(margin, g.hero_label.bottom() + HERO_GAP * u));
		m_hero->setSize(QSizeF(w - margin, HERO_HEIGHT * u));
		toolbar_top = top + HERO_BOX * u;
	}
	const QRectF toolbar(margin, toolbar_top + TOOLBAR_TOP * u, std::max<qreal>(0, w - 2 * margin), TOOLBAR_HEIGHT * u);

	for (int i = 0; i < 2; i++)
	{
		const qreal x  = toolbar.right() - (2 - i) * SWITCH_WIDTH * u - (1 - i) * SWITCH_SPACING * u;
		g.switches[i] = QRectF(x, toolbar.top(), SWITCH_WIDTH * u, toolbar.height());
	}
	const qreal search_width = std::min(SEARCH_WIDTH * u, toolbar.width() * 0.4);
	g.search                 = QRectF(g.switches[0].left() - gap - search_width, toolbar.top(), search_width, toolbar.height());
	g.search_icon            = QRectF(g.search.left(), g.search.center().y() - SEARCH_ICON * u / 2, SEARCH_ICON * u, SEARCH_ICON * u);
	const qreal input_left   = g.search_icon.right() + SEARCH_ICON_GAP * u;
	m_search->setPosition(QPointF(input_left, g.search.top()));
	m_search->setSize(QSizeF(std::max<qreal>(0, g.search.right() - input_left), g.search.height()));

	// The tabs start one tab padding to the left, so the first name lines up with the title above.
	const qreal tabs_left = toolbar.left() - m_tabs->TabPadding();
	m_tabs->setPosition(QPointF(tabs_left, toolbar.top()));
	m_tabs->setSize(QSizeF(std::max<qreal>(0, g.search.left() - gap - tabs_left), toolbar.height()));

	const QRectF grid(margin - GRID_INSET * u, toolbar.bottom() + GRID_TOP * u, std::max<qreal>(0, w - 2 * (margin - GRID_INSET * u)),
	                  std::max<qreal>(0, h - toolbar.bottom() - GRID_TOP * u));
	m_grid->setPosition(grid.topLeft());
	m_grid->setSize(grid.size());
	const qreal table_left = margin - TABLE_INSET * u;
	const qreal table_top  = toolbar.bottom() + TABLE_TOP * u;
	m_table->setPosition(QPointF(table_left, table_top));
	m_table->setSize(QSizeF(std::max<qreal>(0, w - margin - table_left), std::max<qreal>(0, h - table_top)));

	// The empty state, centred in the grid's place.
	const qreal empty_width  = std::max<qreal>(0, grid.width() - 2 * EMPTY_PADDING * u);
	const qreal text_height  = TextBlockHeight(EmptyText(), BodyFont(), empty_width);
	const qreal button       = m_add->isVisible() ? EMPTY_SPACING * u + m_add->height() : 0;
	const qreal empty_top    = std::round(grid.center().y() - (text_height + button) / 2);
	g.empty_text             = QRectF(grid.center().x() - empty_width / 2, empty_top, empty_width, text_height);
	m_add->setPosition(QPointF(std::round(grid.center().x() - m_add->width() / 2), g.empty_text.bottom() + EMPTY_SPACING * u));

	m_menu->setSize(size());
	m_pad->setSize(size());
	m_geometry = g;

	const qreal dpr        = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const qreal icon_scale = u * dpr;
	if (icon_scale != m_icon_scale)
	{
		m_icon_scale = icon_scale;
		for (int i = 0; i < ICON_COUNT; i++)
		{
			m_icons[i] =
			    PaintIcon(ICON_FILES[i], static_cast<int>(std::ceil(ICON_SIZES[i] * icon_scale)), Theme::Instance()->TextPrimary());
		}
	}
	update();
}

QSGNode* LibraryPageItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<LibraryNode*>(old_node);
	if (node == nullptr)
	{
		node = new LibraryNode(window());
	}
	const auto* theme   = Theme::Instance();
	const auto* backend = LauncherBackend::Instance();
	const qreal u       = theme->U();
	const qreal dpr     = window()->effectiveDevicePixelRatio();
	const auto& g       = m_geometry;
	const auto  small   = SmallFont();

	// The focused game's name over its own artwork, as on the console's home screen.
	auto title = m_focused_game.value("title").toString();
	if (title.isEmpty())
	{
		title = m_focused_game.value("name").toString();
	}
	if (title.isEmpty())
	{
		title = m_folder.value("title").toString();
	}
	SetTextLine(node->title, title, TitleFont(), theme->TextPrimary(), g.title);
	QStringList details;
	for (const char* key: {"serial", "region", "version"})
	{
		const auto value = m_focused_game.value(key).toString();
		if (!value.isEmpty())
		{
			details.append(value);
		}
	}
	SetTextLine(node->details, details.join(QStringLiteral("  ·  ")), small, theme->TextSecondary(), g.details);

	const auto   texts = StatusTexts();
	const QColor dots[2] {backend->IsEmulatorReady() ? theme->Ok() : theme->Danger(),
	                      backend->ControllerName().isEmpty() ? theme->TextMuted() : theme->Ok()};
	qreal        x = g.status.left();
	for (int i = 0; i < 2; i++)
	{
		const qreal           dot = DOT * u;
		RoundedBoxNode::Style style;
		style.fill   = dots[i];
		style.radius = dot / 2;
		style.dpr    = dpr;
		node->dots[i]->Update(window(), QRectF(x, g.status.center().y() - dot / 2, dot, dot), style);
		const qreal text_x = x + dot + DOT_GAP * u;
		const qreal width  = TextWidth(texts.at(i), small);
		SetTextLine(node->status[i], texts.at(i), small, theme->TextSecondary(), QRectF(text_x, g.status.top(), width, g.status.height()));
		x = text_x + width + STATUS_SPACING * u;
	}

	node->hero_label->clear();
	if (HasRecent())
	{
		SetTextLine(node->hero_label, tr("Recently played"), small, theme->TextSecondary(), g.hero_label);
	}

	const bool searching = m_search->hasActiveFocus();
	const auto line      = searching ? 2 * u : 1;
	node->underline->setRect(QRectF(g.search.left(), g.search.bottom() - line, g.search.width(), line));
	node->underline->setColor(searching ? theme->Accent() : theme->GlassBorder());
	node->placeholder->clear();
	if (m_search->Text().isEmpty())
	{
		SetTextLine(node->placeholder, tr("Search by name or serial"), BodyFont(), theme->TextMuted(), m_search->boundingRect().translated(m_search->position()));
	}

	const bool table = backend->IsTableView();
	QRectF     icon_rects[ICON_COUNT] {g.search_icon};
	qreal      opacities[ICON_COUNT] {searching ? 1 : 0.6};
	for (int i = 0; i < 2; i++)
	{
		const bool  active = (i == 1) == table;
		const qreal icon   = SWITCH_ICON * u;
		icon_rects[i + 1]  = QRectF(g.switches[i].center().x() - icon / 2, g.switches[i].center().y() - icon / 2, icon, icon);
		opacities[i + 1]   = active ? 1 : (m_hovered_switch == i ? 0.8 : 0.45);
	}
	for (int i = 0; i < ICON_COUNT; i++)
	{
		auto* texture = node->textures.Get(window(), QString::fromLatin1(ICON_FILES[i]), m_icons[i]);
		if (texture == nullptr)
		{
			delete node->icons[i];
			node->icons[i] = nullptr;
			continue;
		}
		if (node->icons[i] == nullptr)
		{
			node->icons[i] = window()->createImageNode();
			node->icons[i]->setOwnsTexture(false);
			node->icons[i]->setFiltering(QSGTexture::Linear);
			// The software renderer reads the texture as soon as the node joins a shown tree.
			node->icons[i]->setTexture(texture);
			node->icon_opacity[i]->appendChildNode(node->icons[i]);
		}
		node->icons[i]->setTexture(texture);
		node->icons[i]->setRect(icon_rects[i]);
		node->icon_opacity[i]->setOpacity(opacities[i]);
	}
	const auto&           active = g.switches[table ? 1 : 0];
	RoundedBoxNode::Style mark;
	mark.fill   = theme->Accent();
	mark.radius = SWITCH_MARK_LINE * u / 2;
	mark.dpr    = dpr;
	node->switch_mark->Update(window(),
	                          QRectF(active.center().x() - SWITCH_MARK * u / 2, active.bottom() - SWITCH_MARK_LINE * u, SWITCH_MARK * u,
	                                 SWITCH_MARK_LINE * u),
	                          mark);

	node->empty->clear();
	if (m_visible_games.isEmpty())
	{
		SetTextBlock(node->empty, EmptyText(), BodyFont(), theme->TextSecondary(), g.empty_text, Qt::AlignHCenter);
	}
	node->textures.Prune();
	return node;
}
