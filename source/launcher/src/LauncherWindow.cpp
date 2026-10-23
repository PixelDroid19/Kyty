#include "LauncherWindow.h"

#include "AboutPageItem.h"
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

#include <QKeyEvent>

#include <algorithm>

constexpr int BASE_WIDTH     = 1280;
constexpr int BASE_HEIGHT    = 720;
constexpr int MINIMUM_WIDTH  = 960;
constexpr int MINIMUM_HEIGHT = 540;
constexpr int STEP_ASIDE_MS  = 450;

// Sizes in units.
constexpr qreal SIDEBAR_WIDTH = 96;
constexpr qreal KIT_TOP       = 30;
constexpr qreal KIT_WIDTH     = 44;
constexpr qreal KIT_GAP       = 18;
constexpr qreal HINTS_HEIGHT  = 56;
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

} // namespace

LauncherWindow::LauncherWindow()
{
	setTitle(tr("Kyty Launcher"));
	setColor(Theme::Instance()->Background());
	setMinimumSize(QSize(MINIMUM_WIDTH, MINIMUM_HEIGHT));
	resize(BASE_WIDTH, BASE_HEIGHT);

	auto* root   = contentItem();
	m_background = AddItem<BackgroundItem>(root);
	m_backdrop   = AddItem<GameBackdropItem>(root);
	m_library    = AddItem<LibraryPageItem>(root);
	m_editor     = AddItem<EditorPageItem>(root);
	m_settings   = AddItem<SettingsPageItem>(root);
	m_about      = AddItem<AboutPageItem>(root);
	auto* mascot = new KitMascotItem;
	mascot->setParent(m_about);
	m_about->SetMascot(mascot);
	m_hints = AddItem<HintBarItem>(root);
	// Its labels open over the page beside it.
	m_sidebar = AddItem<SidebarItem>(root);
	m_kit     = AddItem<KitMascotItem>(m_sidebar);
	m_kit->SetFloating(false);
	m_notice = AddItem<NoticeItem>(root);

	auto* backend = LauncherBackend::Instance();
	connect(backend, &LauncherBackend::StateChanged, this, &LauncherWindow::StateChanged);
	connect(backend, &LauncherBackend::GameFinished, this, &LauncherWindow::ReportGameFinished);
	connect(backend, &LauncherBackend::FolderImported, this, &LauncherWindow::ReportFolderImported);
	connect(this, &QWindow::activeChanged, this, &LauncherWindow::UpdateLive);
	connect(this, &QWindow::visibilityChanged, this, &LauncherWindow::UpdateLive);
	connect(this, &QQuickWindow::closing, this, [this]() { LauncherBackend::Instance()->SaveWindowGeometry(x(), y(), width(), height()); });

	m_step_aside.setSingleShot(true);
	m_step_aside.setInterval(STEP_ASIDE_MS);
	connect(&m_step_aside, &QTimer::timeout, this,
	        [this]()
	        {
		        showMinimized();
		        releaseResources();
	        });

	const auto add_title = tr("Choose a game folder or a folder of games");
	connect(m_sidebar, &SidebarItem::LibraryRequested, this, [this]() { ShowPage(Page::Library); });
	connect(m_sidebar, &SidebarItem::AddGameRequested, this, [this, add_title]() { m_add_chooser.ChooseFolder(add_title); });
	connect(m_sidebar, &SidebarItem::SettingsRequested, this, [this]() { ShowPage(Page::Settings); });
	connect(m_sidebar, &SidebarItem::AboutRequested, this, [this]() { ShowPage(Page::About); });
	connect(m_sidebar, &SidebarItem::QuitRequested, this, &QWindow::close);
	connect(m_kit, &QQuickItem::heightChanged, this, &LauncherWindow::Layout);

	connect(m_library, &LibraryPageItem::RunRequested, this, &LauncherWindow::RunGame);
	connect(m_library, &LibraryPageItem::EditRequested, this, [this](int row) { OpenEditor(row, LauncherBackend::Instance()->FieldsFor(row)); });
	connect(m_library, &LibraryPageItem::AddRequested, this, [this, add_title]() { m_add_chooser.ChooseFolder(add_title); });
	connect(m_library, &LibraryPageItem::ActionRequested, this, &LauncherWindow::HandleAction);
	connect(m_library, &LibraryPageItem::FocusedGameChanged, this, &LauncherWindow::UpdateBackdrop);

	connect(m_editor, &EditorPageItem::Saved, this,
	        [this](int row)
	        {
		        if (m_editor->Global())
		        {
			        ShowPage(Page::Settings);
			        return;
		        }
		        ShowPage(Page::Library);
		        m_library->SelectRow(row);
	        });
	connect(m_editor, &EditorPageItem::Cancelled, this, [this]() { ShowPage(m_editor->Global() ? Page::Settings : Page::Library); });
	connect(m_editor, &EditorPageItem::ModeChanged, this, &LauncherWindow::UpdatePage);

	connect(m_settings, &SettingsPageItem::Closed, this, [this]() { ShowPage(Page::Library); });
	connect(m_settings, &SettingsPageItem::EmulatorSettingsRequested, this,
	        [this]()
	        {
		        m_editor->OpenGlobal();
		        m_page = Page::Editor;
		        UpdatePage();
	        });
	connect(m_settings, &SettingsPageItem::ProgramRequested, this,
	        [this]() { m_program_chooser.ChooseFile(tr("Choose the emulator program")); });
	connect(m_about, &AboutPageItem::Closed, this, [this]() { ShowPage(Page::Library); });

	connect(&m_add_chooser, &FileChooser::Chosen, this, &LauncherWindow::AddFolder);
	connect(&m_program_chooser, &FileChooser::Chosen, backend, &LauncherBackend::SetEmulatorPath);
	connect(m_notice, &QQuickItem::widthChanged, this, &LauncherWindow::Layout);
	connect(m_notice, &QQuickItem::heightChanged, this, &LauncherWindow::Layout);

	RestoreGeometry();
	Layout();
	StateChanged();
	FocusPage();
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

void LauncherWindow::resizeEvent(QResizeEvent* event)
{
	QQuickWindow::resizeEvent(event);
	Layout();
}

void LauncherWindow::Layout()
{
	const qreal w = width();
	const qreal h = height();
	// Scales every size with the window, from 0.75 on small windows to 1.5 on large ones.
	auto* theme = Theme::Instance();
	theme->SetU(std::clamp(std::min(w / BASE_WIDTH, h / BASE_HEIGHT), 0.75, 1.5));
	const qreal u = theme->U();

	m_background->setSize(QSizeF(w, h));
	m_backdrop->setSize(QSizeF(w, h));
	const qreal sidebar = SIDEBAR_WIDTH * u;
	m_sidebar->setSize(QSizeF(sidebar, h));
	m_kit->setWidth(KIT_WIDTH * u);
	m_kit->setPosition(QPointF((sidebar - m_kit->width()) / 2, KIT_TOP * u));
	m_sidebar->SetEntriesTop(m_kit->y() + m_kit->height() + KIT_GAP * u);

	const QRectF page(sidebar, 0, std::max<qreal>(0, w - sidebar), std::max<qreal>(0, h - HINTS_HEIGHT * u));
	for (QQuickItem* item: {static_cast<QQuickItem*>(m_library), static_cast<QQuickItem*>(m_editor), static_cast<QQuickItem*>(m_settings),
	                        static_cast<QQuickItem*>(m_about)})
	{
		item->setPosition(page.topLeft());
		item->setSize(page.size());
	}
	m_hints->setPosition(QPointF(sidebar, page.bottom()));
	m_hints->setSize(QSizeF(page.width(), HINTS_HEIGHT * u));
	m_notice->setPosition(QPointF(std::round(page.center().x() - m_notice->width() / 2), page.bottom() - NOTICE_GAP * u - m_notice->height()));
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
			m_step_aside.start();
		} else
		{
			m_step_aside.stop();
			setVisibility(m_restore);
			raise();
			requestActivate();
		}
	}
	m_hints->SetPad(!LauncherBackend::Instance()->ControllerName().isEmpty());
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

// Empty while a game runs, so the launcher holds no picture the emulator could use.
void LauncherWindow::UpdateBackdrop()
{
	m_backdrop->SetPicture(m_running ? QString() : m_library->FocusedGame().value("picture").toString());
}

void LauncherWindow::UpdatePage()
{
	m_library->setVisible(m_page == Page::Library);
	m_editor->setVisible(m_page == Page::Editor);
	m_settings->setVisible(m_page == Page::Settings);
	m_about->setVisible(m_page == Page::About);

	switch (m_page)
	{
		case Page::Library:
			m_sidebar->SetCurrent(QStringLiteral("library"));
			m_sidebar->SetRightTarget(m_library->GameView());
			m_hints->SetHints(LibraryPageItem::Hints());
			break;
		case Page::Editor:
			m_sidebar->SetCurrent(m_editor->Global() ? QStringLiteral("settings") : QStringLiteral("library"));
			m_sidebar->SetRightTarget(nullptr);
			m_hints->SetHints(m_editor->Hints());
			break;
		case Page::Settings:
			m_sidebar->SetCurrent(QStringLiteral("settings"));
			m_sidebar->SetRightTarget(m_settings);
			m_hints->SetHints(m_settings->Hints());
			break;
		case Page::About:
			m_sidebar->SetCurrent(QStringLiteral("about"));
			m_sidebar->SetRightTarget(m_about);
			m_hints->SetHints(m_about->Hints());
			break;
	}
}

void LauncherWindow::ShowPage(Page page)
{
	m_page = page;
	UpdatePage();
	// After the page is shown, since a hidden item cannot take the focus.
	QTimer::singleShot(0, this, &LauncherWindow::FocusPage);
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
	m_page = Page::Editor;
	UpdatePage();
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
	const auto error = LauncherBackend::Instance()->RunGame(row);
	if (!error.isEmpty())
	{
		m_notice->Show(error);
	}
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

void LauncherWindow::keyPressEvent(QKeyEvent* event)
{
	QQuickWindow::keyPressEvent(event);
	// The editor uses Left inside its fields, so only the other pages hand it to the sidebar.
	if (!event->isAccepted() && event->key() == Qt::Key_Left && m_page != Page::Editor)
	{
		m_sidebar->forceActiveFocus(Qt::TabFocusReason);
		event->accept();
	}
}
