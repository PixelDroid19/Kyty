#include "LauncherWindow.h"

#include "AboutPageItem.h"
#include "ActionMenuItem.h"
#include "BackgroundItem.h"
#include "EditorPageItem.h"
#include "GameBackdropItem.h"
#include "HintBarItem.h"
#include "KitMascotItem.h"
#include "LauncherBackend.h"
#include "LibraryPageItem.h"
#include "NoticeItem.h"
#include "SettingsPageItem.h"
#include "SidebarItem.h"
#include "Theme.h"
#include "TopBarItem.h"
#include "UiSounds.h"

#include <QCoreApplication>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QProcess>

#include <algorithm>

constexpr int BASE_WIDTH     = 1280;
constexpr int BASE_HEIGHT    = 720;
constexpr int MINIMUM_WIDTH  = 960;
constexpr int MINIMUM_HEIGHT = 540;
constexpr int STEP_ASIDE_MS  = 450;
constexpr int REVEAL_MS      = 200;
// A page comes in from a little smaller than its size.
constexpr qreal REVEAL_SCALE = 0.985;

// Sizes in units.
constexpr qreal SIDEBAR_WIDTH = 96;
constexpr qreal KIT_TOP       = 30;
constexpr qreal KIT_WIDTH     = 44;
constexpr qreal KIT_GAP       = 18;
constexpr qreal HINTS_HEIGHT  = 56;
constexpr qreal TOP_BAR       = 64;
constexpr qreal NOTICE_GAP    = 12;

namespace {

// Builds the item before it joins the scene, so its own itemChange sees it arrive.
template <typename Item>
Item* AddItem(QQuickItem* parent)
{
	auto* item = new Item;
	item->setParent(parent);
	item->setParentItem(parent);
	return item;
}

// The sound of a key the window used: arrows move, Enter accepts, Escape goes back.
void PlayKeySound(int key)
{
	auto* sounds = LauncherBackend::Instance()->Sounds();
	switch (key)
	{
		case Qt::Key_Up:
		case Qt::Key_Down:
		case Qt::Key_Left:
		case Qt::Key_Right:
		case Qt::Key_PageUp:
		case Qt::Key_PageDown: sounds->Play(UiSounds::Sound::Move); break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_PowerOff: sounds->Play(UiSounds::Sound::Accept); break;
		case Qt::Key_Escape: sounds->Play(UiSounds::Sound::Back); break;
		default: break;
	}
}

} // namespace

LauncherWindow::LauncherWindow()
{
	setMinimumSize(QSize(MINIMUM_WIDTH, MINIMUM_HEIGHT));
	resize(BASE_WIDTH, BASE_HEIGHT);

	auto* backend = LauncherBackend::Instance();
	connect(backend, &LauncherBackend::StateChanged, this, &LauncherWindow::StateChanged);
	connect(backend, &LauncherBackend::GameFinished, this, &LauncherWindow::ReportGameFinished);
	connect(backend, &LauncherBackend::FolderImported, this, &LauncherWindow::ReportFolderImported);
	connect(backend, &LauncherBackend::SaveFailed, this,
	        [this](const QString& file) { m_notice->Show(tr("The settings could not be saved to %1.").arg(file)); });
	connect(backend, &LauncherBackend::ThemeFailed, this, [this](const QString& message) { m_notice->Show(message); });
	// Queued: the change may come from a key the page being replaced is still handling.
	connect(backend, &LauncherBackend::LanguageChanged, this, &LauncherWindow::Rebuild, Qt::QueuedConnection);
	connect(backend, &LauncherBackend::AppearanceChanged, this, &LauncherWindow::Rebuild, Qt::QueuedConnection);
	connect(backend, &LauncherBackend::AppearanceChanged, this, &LauncherWindow::ApplyWindowMode);
	connect(Theme::Instance(), &Theme::PaletteChanged, this, &LauncherWindow::Rebuild, Qt::QueuedConnection);
	connect(this, &QWindow::activeChanged, this, &LauncherWindow::UpdateLive);
	connect(this, &QWindow::visibilityChanged, this, &LauncherWindow::UpdateLive);
	// A maximized or full-screen size would make the next plain window cover the screen.
	connect(this, &QQuickWindow::closing, this,
	        [this]()
	        {
		        if (windowStates() == Qt::WindowNoState)
		        {
			        LauncherBackend::Instance()->SaveWindowGeometry(x(), y(), width(), height());
		        }
	        });

	m_step_aside.setSingleShot(true);
	m_step_aside.setInterval(STEP_ASIDE_MS);
	connect(&m_step_aside, &QTimer::timeout, this,
	        [this]()
	        {
		        showMinimized();
		        releaseResources();
	        });

	m_reveal.setDuration(REVEAL_MS);
	m_reveal.setStartValue(0.0);
	m_reveal.setEndValue(1.0);
	m_reveal.setEasingCurve(QEasingCurve::OutCubic);
	connect(&m_reveal, &QVariantAnimation::valueChanged, this,
	        [this](const QVariant& value)
	        {
		        const qreal t    = value.toReal();
		        auto*       page = PageFor(m_page);
		        page->setOpacity(t);
		        page->setScale(REVEAL_SCALE + (1 - REVEAL_SCALE) * t);
	        });

	connect(&m_add_chooser, &FileChooser::Chosen, this, &LauncherWindow::AddFolder);
	connect(&m_program_chooser, &FileChooser::Chosen, backend, &LauncherBackend::SetEmulatorPath);

	BuildItems();
	RestoreGeometry();
	ApplyWindowMode();
	Layout();
	StateChanged();
	FocusPage();
	OpenLastGame();
}

// Continue where you left off: the launcher opens on the page of the game played last.
void LauncherWindow::OpenLastGame()
{
	auto* backend = LauncherBackend::Instance();
	if (backend->Look("start_on") != QLatin1String("LastGame"))
	{
		return;
	}
	const auto recent = backend->Recent();
	if (!recent.isEmpty())
	{
		m_library->OpenGamePage(recent.first().toMap().value("row").toInt());
	}
}

// The items bake the palette and the texts into their nodes, so a new theme or language builds them again.
void LauncherWindow::BuildItems()
{
	setTitle(tr("Kyty Launcher"));
	setColor(Theme::Instance()->Background());

	auto* root   = contentItem();
	m_background = AddItem<BackgroundItem>(root);
	m_backdrop   = AddItem<GameBackdropItem>(root);
	m_library    = AddItem<LibraryPageItem>(root);
	m_editor     = AddItem<EditorPageItem>(root);
	m_settings   = AddItem<SettingsPageItem>(root);
	m_about      = AddItem<AboutPageItem>(root);
	m_backdrop->SetDimmed(LauncherBackend::Instance()->Look("background") == QLatin1String("Dimmed"));
	auto* mascot = new KitMascotItem;
	mascot->setParent(m_about);
	m_about->SetMascot(mascot);
	m_hints = AddItem<HintBarItem>(root);
	// Its labels open over the page beside it.
	m_sidebar = AddItem<SidebarItem>(root);
	m_kit     = AddItem<KitMascotItem>(m_sidebar);
	m_kit->SetFloating(false);
	m_topbar = AddItem<TopBarItem>(root);
	m_notice = AddItem<NoticeItem>(root);
	m_power  = AddItem<ActionMenuItem>(root);
	m_console = LauncherBackend::Instance()->LayoutName() == QLatin1String("Console");
	m_sidebar->setVisible(!m_console);
	if (m_console)
	{
		m_topbar->SetTabs({tr("Games"), tr("Settings"), tr("About")});
	}

	const auto add_title = tr("Choose a game folder or a folder of games");
	connect(m_sidebar, &SidebarItem::LibraryRequested, this, [this]() { ShowPage(Page::Library); });
	connect(m_sidebar, &SidebarItem::AddGameRequested, this, [this, add_title]() { m_add_chooser.ChooseFolder(add_title); });
	connect(m_sidebar, &SidebarItem::SettingsRequested, this, [this]() { ShowPage(Page::Settings); });
	connect(m_sidebar, &SidebarItem::AboutRequested, this, [this]() { ShowPage(Page::About); });
	connect(m_sidebar, &SidebarItem::QuitRequested, this, &LauncherWindow::OpenPowerMenu);
	connect(m_topbar, &TopBarItem::TabRequested, this,
	        [this](int index) { ShowPage(index == 1 ? Page::Settings : index == 2 ? Page::About : Page::Library, false); });
	connect(m_topbar, &TopBarItem::SearchRequested, this,
	        [this]()
	        {
		        ShowPage(Page::Library, false);
		        // After the page is shown, since a hidden item cannot take the focus.
		        QTimer::singleShot(0, m_library, &LibraryPageItem::FocusSearch);
	        });
	connect(m_topbar, &TopBarItem::PowerRequested, this, &LauncherWindow::OpenPowerMenu);
	connect(m_topbar, &TopBarItem::DownRequested, this, &LauncherWindow::FocusPage);
	connect(m_power, &ActionMenuItem::Chosen, this, [this](const QString& action, int /*row*/) { PowerChosen(action); });
	connect(m_power, &ActionMenuItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_power->Shown() && m_power_return != nullptr)
		        {
			        m_power_return->forceActiveFocus();
		        }
	        });
	connect(m_kit, &QQuickItem::heightChanged, this, &LauncherWindow::Layout);

	connect(m_library, &LibraryPageItem::RunRequested, this, &LauncherWindow::RunGame);
	connect(m_library, &LibraryPageItem::EditRequested, this, [this](int row) { OpenEditor(row, LauncherBackend::Instance()->FieldsFor(row)); });
	connect(m_library, &LibraryPageItem::AddRequested, this, [this, add_title]() { m_add_chooser.ChooseFolder(add_title); });
	connect(m_library, &LibraryPageItem::ActionRequested, this, &LauncherWindow::HandleAction);
	connect(m_library, &LibraryPageItem::FocusedGameChanged, this, &LauncherWindow::UpdateBackdrop);
	connect(m_library, &LibraryPageItem::HintsChanged, this, &LauncherWindow::UpdatePage);

	connect(m_editor, &EditorPageItem::Saved, this,
	        [this](int row)
	        {
		        ShowPage(Page::Library);
		        m_library->SelectRow(row);
	        });
	connect(m_editor, &EditorPageItem::Cancelled, this, [this]() { ShowPage(Page::Library); });

	connect(m_settings, &SettingsPageItem::Closed, this, [this]() { ShowPage(Page::Library); });
	connect(m_settings, &SettingsPageItem::ProgramRequested, this,
	        [this]() { m_program_chooser.ChooseFile(tr("Choose the emulator program")); });
	connect(m_about, &AboutPageItem::Closed, this, [this]() { ShowPage(Page::Library); });

	connect(m_notice, &QQuickItem::widthChanged, this, &LauncherWindow::Layout);
	connect(m_notice, &QQuickItem::heightChanged, this, &LauncherWindow::Layout);
}

void LauncherWindow::Rebuild()
{
	// The editor may hold changes not saved yet; it is rebuilt once the user leaves it.
	if (m_page == Page::Editor)
	{
		m_rebuild_pending = true;
		return;
	}
	m_rebuild_pending = false;
	const auto place  = m_settings->CurrentPlace();
	const int  row    = m_library->FocusedGame().value("row", -1).toInt();
	const int  opened = m_library->GamePageRow();
	for (QQuickItem* item: contentItem()->childItems())
	{
		item->setParentItem(nullptr);
		item->deleteLater();
	}
	BuildItems();
	Layout();
	StateChanged();
	if (opened >= 0)
	{
		m_library->OpenGamePage(opened);
	} else if (row >= 0)
	{
		m_library->SelectRow(row);
	}
	if (m_page == Page::Settings)
	{
		QTimer::singleShot(0, m_settings, [settings = m_settings, place]() { settings->GoTo(place); });
		return;
	}
	QTimer::singleShot(0, this, &LauncherWindow::FocusPage);
}

void LauncherWindow::RestoreGeometry()
{
	const auto geometry = LauncherBackend::Instance()->WindowGeometry();
	if (!geometry.contains("width"))
	{
		return;
	}
	setPosition(geometry.value("x").toInt(), geometry.value("y").toInt());
	resize(std::max(MINIMUM_WIDTH, geometry.value("width").toInt()), std::max(MINIMUM_HEIGHT, geometry.value("height").toInt()));
}

// Applied at start and when the setting changes, so another look change does not undo the user's own resizing.
void LauncherWindow::ApplyWindowMode()
{
	const auto mode = LauncherBackend::Instance()->Look("window_mode").toString();
	if (mode == m_window_mode)
	{
		return;
	}
	m_window_mode = mode;
	if (mode == QLatin1String("Maximized"))
	{
		setWindowStates(Qt::WindowMaximized);
	} else if (mode == QLatin1String("FullScreen"))
	{
		setWindowStates(Qt::WindowFullScreen);
	} else
	{
		setWindowStates(Qt::WindowNoState);
	}
}

void LauncherWindow::resizeEvent(QResizeEvent* event)
{
	QQuickWindow::resizeEvent(event);
	Layout();
}

void LauncherWindow::Layout()
{
	const qreal w = width();
	const qreal h = height();
	// Scales every size with the window, from 0.75 on small windows to 1.5 on large ones, then by the chosen
	// interface size.
	auto* theme = Theme::Instance();
	theme->SetU(std::clamp(std::min(w / BASE_WIDTH, h / BASE_HEIGHT), 0.75, 1.5) * LauncherBackend::Instance()->InterfaceScale());
	const qreal u = theme->U();

	m_background->setSize(QSizeF(w, h));
	m_backdrop->setSize(QSizeF(w, h));
	m_power->setSize(QSizeF(w, h));
	const qreal sidebar = m_console ? 0 : SIDEBAR_WIDTH * u;
	m_sidebar->setSize(QSizeF(sidebar, h));
	m_kit->setWidth(KIT_WIDTH * u);
	m_kit->setPosition(QPointF((sidebar - m_kit->width()) / 2, KIT_TOP * u));
	m_sidebar->SetEntriesTop(m_kit->y() + m_kit->height() + KIT_GAP * u);

	// The Sidebar layout's pages keep their whole height and the bar lies over their top right corner.
	const qreal  bar  = TOP_BAR * u;
	const qreal  top  = m_console ? bar : 0;
	const QRectF page(sidebar, top, std::max<qreal>(0, w - sidebar), std::max<qreal>(0, h - top - HINTS_HEIGHT * u));
	m_topbar->setPosition(QPointF(sidebar, 0));
	m_topbar->setSize(QSizeF(page.width(), bar));
	for (QQuickItem* item: {static_cast<QQuickItem*>(m_library), static_cast<QQuickItem*>(m_editor), static_cast<QQuickItem*>(m_settings),
	                        static_cast<QQuickItem*>(m_about)})
	{
		item->setPosition(page.topLeft());
		item->setSize(page.size());
	}
	m_hints->setPosition(QPointF(sidebar, page.bottom()));
	m_hints->setSize(QSizeF(page.width(), HINTS_HEIGHT * u));
	// Centred on the page, but a notice wider than the page moves left over the sidebar instead of leaving the window.
	const qreal notice_x = std::min(page.center().x() - m_notice->width() / 2, w - NOTICE_GAP * u - m_notice->width());
	m_notice->setPosition(QPointF(std::round(notice_x), page.bottom() - NOTICE_GAP * u - m_notice->height()));
}

void LauncherWindow::StateChanged()
{
	const bool running = LauncherBackend::Instance()->IsRunning();
	if (running != m_running)
	{
		m_running = running;
		// The launcher steps aside while a game runs and frees its GPU resources, so the emulator gets the machine.
		if (running)
		{
			m_restore = visibility() == Minimized ? Windowed : visibility();
			m_kit->Hop();
			if (LauncherBackend::Instance()->Look("on_launch") == QLatin1String("Minimize"))
			{
				m_step_aside.start();
			}
		} else
		{
			m_step_aside.stop();
			setVisibility(m_restore);
			raise();
			requestActivate();
		}
	}
	m_hints->SetPad(!LauncherBackend::Instance()->ControllerName().isEmpty());
	m_topbar->Refresh();
	UpdateLive();
	UpdatePage();
	UpdateBackdrop();
}

// Animations run only while someone can see the launcher; otherwise the scene graph stays idle.
void LauncherWindow::UpdateLive()
{
	const auto shown = visibility();
	Theme::Instance()->SetLive(!m_running && isActive() && shown != Minimized && shown != Hidden);
}

// Empty while a game runs, so the launcher holds no picture the emulator could use, and with a plain background.
void LauncherWindow::UpdateBackdrop()
{
	const bool plain = LauncherBackend::Instance()->Look("background") == QLatin1String("Plain");
	m_backdrop->SetPicture(m_running || plain ? QString() : m_library->FocusedGame().value("picture").toString());
}

void LauncherWindow::UpdatePage()
{
	m_library->setVisible(m_page == Page::Library);
	m_editor->setVisible(m_page == Page::Editor);
	m_settings->setVisible(m_page == Page::Settings);
	m_about->setVisible(m_page == Page::About);
	m_topbar->SetCurrentTab(m_page == Page::Settings ? 1 : m_page == Page::About ? 2 : 0);
	// The Sidebar layout's editor keeps its own buttons where the bar would lie.
	m_topbar->setVisible(m_console || m_page != Page::Editor);

	switch (m_page)
	{
		case Page::Library:
			m_sidebar->SetCurrent(QStringLiteral("library"));
			m_sidebar->SetRightTarget(m_library->FocusTarget());
			m_hints->SetHints(m_library->Hints());
			break;
		case Page::Editor:
			m_sidebar->SetCurrent(QStringLiteral("library"));
			m_sidebar->SetRightTarget(nullptr);
			m_hints->SetHints(m_editor->Hints());
			break;
		case Page::Settings:
			m_sidebar->SetCurrent(QStringLiteral("settings"));
			m_sidebar->SetRightTarget(m_settings);
			m_hints->SetHints(SettingsPageItem::Hints());
			break;
		case Page::About:
			m_sidebar->SetCurrent(QStringLiteral("about"));
			m_sidebar->SetRightTarget(m_about);
			m_hints->SetHints(m_about->Hints());
			break;
	}
}

void LauncherWindow::ShowPage(Page page, bool focus_page)
{
	const Page previous = m_page;
	m_page              = page;
	if (m_rebuild_pending)
	{
		Rebuild();
		return;
	}
	UpdatePage();
	Reveal(previous);
	if (focus_page)
	{
		// After the page is shown, since a hidden item cannot take the focus.
		QTimer::singleShot(0, this, &LauncherWindow::FocusPage);
	}
}

void LauncherWindow::FocusPage()
{
	switch (m_page)
	{
		case Page::Library: m_library->FocusDefault(); break;
		case Page::Settings: m_settings->FocusDefault(); break;
		case Page::About: m_about->FocusDefault(); break;
		case Page::Editor: break;
	}
}

void LauncherWindow::OpenEditor(int row, const QVariantMap& fields)
{
	m_editor->OpenGame(row, fields);
	const Page previous = m_page;
	m_page              = Page::Editor;
	UpdatePage();
	Reveal(previous);
}

QQuickItem* LauncherWindow::PageFor(Page page) const
{
	switch (page)
	{
		case Page::Library: return m_library;
		case Page::Editor: return m_editor;
		case Page::Settings: return m_settings;
		case Page::About: return m_about;
	}
	return m_library;
}

// Fades the page just shown in with a slight zoom, so a page change reads as one motion; previous is left as it rests.
void LauncherWindow::Reveal(Page previous)
{
	m_reveal.stop();
	auto* left = PageFor(previous);
	left->setOpacity(1);
	left->setScale(1);
	if (previous == m_page || Theme::Instance()->Animation() == 0)
	{
		return;
	}
	m_reveal.start();
}

// A game folder opens the editor; any other folder adds every game found inside it.
void LauncherWindow::AddFolder(const QString& folder)
{
	auto* backend = LauncherBackend::Instance();
	if (backend->IsGameFolder(folder))
	{
		OpenEditor(-1, backend->DetectGame(folder));
		return;
	}
	const auto error = backend->ImportFolder(folder);
	m_notice->Show(error.isEmpty() ? tr("Looking for games…") : error);
}

void LauncherWindow::RunGame(int row)
{
	auto*      backend = LauncherBackend::Instance();
	const auto error   = backend->RunGame(row);
	if (!error.isEmpty())
	{
		m_notice->Show(error);
		return;
	}
	backend->Sounds()->Play(UiSounds::Sound::Start);
}

void LauncherWindow::HandleAction(const QString& action, int row)
{
	auto* backend = LauncherBackend::Instance();
	if (action == QLatin1String("delete"))
	{
		backend->DeleteGame(row);
	} else if (action == QLatin1String("up") || action == QLatin1String("down"))
	{
		const int delta = action == QLatin1String("up") ? -1 : 1;
		backend->MoveGame(row, delta);
		m_library->SelectRow(row + delta);
	} else if (action == QLatin1String("favorite") || action == QLatin1String("unfavorite"))
	{
		backend->SetFavorite(row, action == QLatin1String("favorite"));
	} else if (action == QLatin1String("folder"))
	{
		backend->OpenGameFolder(row);
	} else if (action == QLatin1String("stop"))
	{
		backend->StopGame();
	}
}

void LauncherWindow::ReportGameFinished(const QString& message)
{
	if (message.isEmpty())
	{
		return;
	}
	const auto log = LauncherBackend::Instance()->LogPath();
	m_notice->Show(log.isEmpty() ? message : tr("%1 Log: %2").arg(message, log));
}

void LauncherWindow::ReportFolderImported(int added, int found, const QString& error)
{
	QString message;
	if (found == 0)
	{
		message = tr("No games were found in that folder.");
	} else if (added == 0)
	{
		message = tr("All %1 games in that folder are already in the library.").arg(found);
	} else
	{
		message = tr("Added %1 of the %2 games found.").arg(added).arg(found);
	}
	if (!error.isEmpty())
	{
		message += QLatin1Char(' ') + error;
	}
	m_notice->Show(message);
}

// Opened by the power buttons and by holding the controller's back button.
void LauncherWindow::OpenPowerMenu()
{
	if (m_power->Shown())
	{
		return;
	}
	m_power_return              = activeFocusItem();
	const bool         full     = (windowStates() & Qt::WindowFullScreen) != 0;
	const QVariantList items {QVariantMap {{"label", tr("Quit")}, {"action", "quit"}},
	                          QVariantMap {{"label", full ? tr("Leave full screen") : tr("Full screen")}, {"action", "full-screen"}},
	                          QVariantMap {{"label", tr("Restart the launcher")}, {"action", "restart"}},
	                          QVariantMap {{"label", tr("Close")}, {"action", "close"}}};
	m_power->OpenFor({{"title", tr("Power")}}, -1, items);
}

void LauncherWindow::PowerChosen(const QString& action)
{
	if (action == QLatin1String("quit"))
	{
		close();
	} else if (action == QLatin1String("full-screen"))
	{
		setWindowStates(windowStates() ^ Qt::WindowFullScreen);
	} else if (action == QLatin1String("restart"))
	{
		if (QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1)))
		{
			close();
			return;
		}
		m_notice->Show(tr("The launcher could not be started again."));
	}
}

void LauncherWindow::keyPressEvent(QKeyEvent* event)
{
	QQuickWindow::keyPressEvent(event);
	const int key = event->key();
	if (event->isAccepted())
	{
		PlayKeySound(key);
		return;
	}
	// The editor uses Left and Up inside its fields, so only the other pages hand them on.
	const bool editing = m_page == Page::Editor;
	if (key == Qt::Key_PowerOff)
	{
		OpenPowerMenu();
	} else if (key == Qt::Key_Left && !editing && !m_console)
	{
		m_sidebar->forceActiveFocus(Qt::TabFocusReason);
	} else if (key == Qt::Key_Up && !editing && !m_power->Shown())
	{
		m_topbar->forceActiveFocus(Qt::TabFocusReason);
	} else
	{
		return;
	}
	event->accept();
	PlayKeySound(key);
}
