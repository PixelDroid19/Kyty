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
class GameGridItem;
class GameTableItem;
class HeroRowItem;
class TextFieldItem;
class TextPadItem;

// Main page: recently played, the folders as tabs and the games of the selected folder as a grid or a table,
// narrowed by the search box. Everything sits straight on the focused game's artwork, without panels. Keys the
// page does not use, such as Left from the first column, are left to the window.
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

public:
	explicit LibraryPageItem(QQuickItem* parent = nullptr);
	~LibraryPageItem() override = default;

	// What the page's keys do, for the hint bar; see HintBarItem.
	[[nodiscard]] static QVariantList Hints();
	// The game in the recently played row while it has the focus, otherwise the one selected in the list.
	[[nodiscard]] QVariantMap FocusedGame() const { return m_focused_game; }
	// The grid or the table, whichever shows the games.
	[[nodiscard]] QQuickItem* GameView() const;

	void FocusDefault();
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
		QRectF status;
		QRectF hero_label;
		QRectF search;
		QRectF search_icon;
		QRectF switches[2]; // Grid, then table.
		QRectF empty_text;
	};

	void Refresh();
	void Filter();
	static void SortGames(QVariantList* games);
	void UpdateFocusedGame();
	void SetHoveredSwitch(int index);
	void OpenMenu(int row);
	void MenuChosen(const QString& action, int row);
	void ToggleView();
	void ChangeFolder(int delta);
	void FocusSearch();
	// The game view when it shows games, otherwise the add button when there is one.
	void FocusGames();
	void SearchReturn();
	bool NavigateKey(QKeyEvent* event);
	bool GameKey(QKeyEvent* event);

	[[nodiscard]] int        IndexOfRow(int row) const;
	[[nodiscard]] int        SwitchAt(QPointF point) const;
	[[nodiscard]] bool        ShowsRecent() const { return m_shows_recent; }
	[[nodiscard]] QString    EmptyText() const;
	[[nodiscard]] QString    SearchText() const;
	[[nodiscard]] QStringList StatusTexts() const;

	QVariantList     m_games;
	QVariantList     m_recent;
	QVariantList     m_visible_games;
	QVariantMap      m_folder;
	QVariantMap      m_focused_game;
	Geometry         m_geometry;
	int              m_hovered_switch = -1;
	bool             m_shows_recent   = false;
	HeroRowItem*     m_hero           = nullptr;
	FolderTabsItem*  m_tabs           = nullptr;
	TextFieldItem*   m_search         = nullptr;
	GameGridItem*    m_grid           = nullptr;
	GameTableItem*   m_table          = nullptr;
	FocusButtonItem* m_add            = nullptr;
	ActionMenuItem*  m_menu           = nullptr;
	TextPadItem*     m_pad            = nullptr;
	// Search, grid and table icons, painted for the current size.
	QImage m_icons[3];
	qreal  m_icon_scale = 0;
};

#endif /* LAUNCHER_INCLUDE_LIBRARY_PAGE_ITEM_H_ */
