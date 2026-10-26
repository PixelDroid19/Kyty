#ifndef LAUNCHER_INCLUDE_LIBRARY_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_LIBRARY_PAGE_ITEM_H_

#include "Common.h"

#include <QImage>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>

class ActionMenuItem;
class FocusButtonItem;
class FolderTabsItem;
class GamePageItem;
class GameGridItem;
class GameTableItem;
class HeroRowItem;
class TextFieldItem;
class TextPadItem;
class TileRowItem;

// Main page: recently played, the folders as tabs and the games of the selected folder as a grid or a table,
// narrowed by the search box. Opening a game shows its page in their place until it is closed. In the Console
// layout the games are one row of tiles instead, with the selected game's page always below it; Down opens it and
// Up goes back to the row. Everything sits straight on the focused game's artwork, without panels. Keys the page
// does not use, such as Left from the first column, are left to the window.
class LibraryPageItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(LibraryPageItem);

signals:
	void RunRequested(int row);
	void EditRequested(int row);
	void AddRequested();
	void ActionRequested(const QString& action, int row);
	// The game the user is looking at changed; the window backdrop follows it.
	void FocusedGameChanged();
	// A game's page opened or closed, so Hints() and FocusTarget() changed.
	void HintsChanged();

public:
	explicit LibraryPageItem(QQuickItem* parent = nullptr);
	~LibraryPageItem() override = default;

	// What the page's keys do, for the hint bar; see HintBarItem.
	[[nodiscard]] QVariantList Hints() const;
	// The game whose page is open, else the game in the recently played row while it has the focus, otherwise the
	// one selected in the list.
	[[nodiscard]] QVariantMap FocusedGame() const { return m_focused_game; }
	// The grid or the table, whichever shows the games.
	[[nodiscard]] QQuickItem* GameView() const;
	// Where Right from the sidebar goes: the open game's page, otherwise the game view.
	[[nodiscard]] QQuickItem* FocusTarget() const;
	// The row of the game whose page is open, or -1.
	[[nodiscard]] int GamePageRow() const;
	// Opens the page of the game in row in place of the list.
	void OpenGamePage(int row);

	void FocusDefault();
	// Puts the focus on the search: the search box, or the on-screen pad in the Console layout.
	void FocusSearch();
	// Shows the game with this row, switching to "All games" when it is not in the current folder.
	void SelectRow(int row);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     keyPressEvent(QKeyEvent* event) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     hoverMoveEvent(QHoverEvent* event) override;
	void     hoverLeaveEvent(QHoverEvent* event) override;

private:
	// Where the page's own drawing goes, worked out with the child items' places.
	struct Geometry
	{
		QRectF title;
		QRectF details;
		QRectF row_label;
		QRectF hero_label;
		QRectF search;
		QRectF search_icon;
		QRectF switches[2]; // Grid, then table.
		QRectF empty_text;
	};

	void Refresh();
	void CloseGamePage();
	[[nodiscard]] bool ShowsGamePage() const;
	void Filter();
	static void SortGames(QVariantList* games);
	void UpdateFocusedGame();
	void SetHoveredSwitch(int index);
	void OpenMenu(int row);
	void MenuChosen(const QString& action, int row);
	// The sections menu of the game in row: each section checked when the game is in it, then a new section.
	void                             OpenSectionsMenu(int row, int current);
	[[nodiscard]] static QStringList SectionNames();
	void ToggleView();
	void ChangeFolder(int delta);
	// The game view when it shows games, otherwise the add button when there is one.
	void FocusGames();
	void SearchReturn();
	void OpenSearchPad();
	// The row's selection moved: the page below it shows the newly selected game.
	void ShowRowGame();
	[[nodiscard]] QVariantList RowHints() const;
	bool                       RowKey(QKeyEvent* event);
	bool NavigateKey(QKeyEvent* event);
	bool GameKey(QKeyEvent* event);

	[[nodiscard]] int        IndexOfRow(int row) const;
	[[nodiscard]] int        SwitchAt(QPointF point) const;
	[[nodiscard]] bool        ShowsRecent() const { return m_shows_recent; }
	[[nodiscard]] QString    EmptyText() const;
	[[nodiscard]] QString    SearchText() const;

	QVariantList     m_games;
	QVariantList     m_recent;
	QVariantList     m_visible_games;
	QVariantMap      m_folder;
	QVariantMap      m_focused_game;
	Geometry         m_geometry;
	int              m_hovered_switch = -1;
	bool             m_shows_recent   = false;
	// The game a new section is being named for.
	int              m_section_row    = -1;
	HeroRowItem*     m_hero           = nullptr;
	FolderTabsItem*  m_tabs           = nullptr;
	TextFieldItem*   m_search         = nullptr;
	GameGridItem*    m_grid           = nullptr;
	GameTableItem*   m_table          = nullptr;
	FocusButtonItem* m_add            = nullptr;
	ActionMenuItem*  m_menu           = nullptr;
	TextPadItem*     m_pad            = nullptr;
	// Holds the list's items, so a game's page hides them all at once.
	QQuickItem*   m_browse    = nullptr;
	GamePageItem* m_game_page = nullptr;
	// The Console layout's row, and whether its game's page has the focus.
	bool         m_console  = false;
	bool         m_hub_open = false;
	TileRowItem* m_row      = nullptr;
	// Search, grid and table icons, painted for the current size.
	QImage m_icons[3];
	qreal  m_icon_scale = 0;
};

#endif /* LAUNCHER_INCLUDE_LIBRARY_PAGE_ITEM_H_ */
