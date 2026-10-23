#ifndef LAUNCHER_INCLUDE_LAUNCHER_WINDOW_H_
#define LAUNCHER_INCLUDE_LAUNCHER_WINDOW_H_

#include "Common.h"
#include "FileChooser.h"

#include <QQuickWindow>
#include <QTimer>
#include <QVariantMap>

class AboutPageItem;
class BackgroundItem;
class EditorPageItem;
class GameBackdropItem;
class HintBarItem;
class KitMascotItem;
class LibraryPageItem;
class NoticeItem;
class SettingsPageItem;
class SidebarItem;

// The launcher's window and page switcher. Each page handles its own keys; the sidebar moves between pages, and
// Left that no page uses moves to the sidebar.
class LauncherWindow: public QQuickWindow
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(LauncherWindow);

public:
	LauncherWindow();
	~LauncherWindow() override = default;

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	enum class Page
	{
		Library,
		Editor,
		Settings,
		About
	};

	void Layout();
	void StateChanged();
	void UpdateLive();
	void UpdatePage();
	void UpdateBackdrop();
	void ShowPage(Page page);
	void FocusPage();
	void OpenEditor(int row, const QVariantMap& fields);
	void AddFolder(const QString& folder);
	void RunGame(int row);
	void HandleAction(const QString& action, int row);
	void ReportGameFinished(const QString& message);
	void ReportFolderImported(int added, int found, const QString& error);
	void RestoreGeometry();

	Page              m_page    = Page::Library;
	bool              m_running = false;
	// Visibility to return to when the game closes, so a maximized launcher comes back maximized.
	Visibility        m_restore = Windowed;
	BackgroundItem*   m_background = nullptr;
	GameBackdropItem* m_backdrop = nullptr;
	SidebarItem*      m_sidebar  = nullptr;
	KitMascotItem*    m_kit      = nullptr;
	LibraryPageItem*  m_library  = nullptr;
	EditorPageItem*   m_editor   = nullptr;
	SettingsPageItem* m_settings = nullptr;
	AboutPageItem*    m_about    = nullptr;
	HintBarItem*      m_hints    = nullptr;
	NoticeItem*       m_notice   = nullptr;
	FileChooser       m_add_chooser;
	FileChooser       m_program_chooser;
	// Lets Kit's hop finish before the window steps aside for a game.
	QTimer m_step_aside;
};

#endif /* LAUNCHER_INCLUDE_LAUNCHER_WINDOW_H_ */
