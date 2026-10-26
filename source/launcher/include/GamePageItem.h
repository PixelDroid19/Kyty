#ifndef LAUNCHER_INCLUDE_GAME_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_GAME_PAGE_ITEM_H_

#include "PageItem.h"

#include <QVariantMap>

class GameArtItem;

// One game over its own artwork: its icon and name, Play (Continue once it has been played), its settings and its
// options a button away, then cards with its play time, its settings and its folder.
class GamePageItem: public PageItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(GamePageItem);

signals:
	void RunRequested(int row);
	void EditRequested(int row);
	void MenuRequested(int row);
	// stop or folder, as the library's options menu names them.
	void ActionRequested(const QString& action, int row);

public:
	explicit GamePageItem(QQuickItem* parent = nullptr);
	~GamePageItem() override = default;

	[[nodiscard]] QVariantList Hints() const override;
	[[nodiscard]] QVariantMap  Game() const { return m_game; }
	[[nodiscard]] int          Row() const { return m_game.value("row", -1).toInt(); }
	// The game shown, from the backend's game maps; latest marks the game played last.
	void SetGame(const QVariantMap& game, bool latest);
	// Leaves the icon out of the header, for a layout that shows it right above the page.
	void SetShowsArt(bool shows);

protected:
	void Compose(PageLayout* layout) override;
	void Press(int id) override;
	void Placed() override;
	void keyPressEvent(QKeyEvent* event) override;

private:
	void AddHeader(PageLayout* layout);
	void AddCards(PageLayout* layout) const;

	QVariantMap  m_game;
	bool         m_latest    = false;
	bool         m_shows_art = true;
	GameArtItem* m_art       = nullptr;
	// Clips the artwork to the scrolled area, as the content's nodes are.
	QQuickItem* m_art_clip = nullptr;
	// Where the artwork goes, in content coordinates.
	QRectF m_art_slot;
};

#endif /* LAUNCHER_INCLUDE_GAME_PAGE_ITEM_H_ */
