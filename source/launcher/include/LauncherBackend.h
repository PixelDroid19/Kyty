#ifndef LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_
#define LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_

#include "Common.h"
#include "Configuration.h"
#include "EmulatorRunner.h"
#include "GameLibrary.h"
#include "ThemePack.h"

#include <QFont>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTranslator>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class GamepadInput;
class QWindow;
class UiSounds;

// The object the pages use: the library, the editor, the emulator, the controller and persistence.
class LauncherBackend: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(LauncherBackend);

signals:
	void StateChanged();
	// message is empty when the game closed normally, otherwise it says how it ended.
	void GameFinished(const QString& message);
	// Result of ImportFolder: games added, game folders found (already listed ones included), and an error text.
	void FolderImported(int added, int found, const QString& error);
	// The launcher's texts are now in another language; pages built before still show the old one.
	void LanguageChanged();
	// The launcher's look or library layout changed; pages built before still show the old one.
	void AppearanceChanged();
	// The settings could not be written to file, so the last changes are lost when the launcher closes.
	void SaveFailed(const QString& file);
	// The chosen add-on theme cannot be used, so the launcher's own look is back; message says why.
	void ThemeFailed(const QString& message);

public:
	explicit LauncherBackend(QObject* parent = nullptr);
	~LauncherBackend() override;

	// The one backend of the process, created before the UI.
	static LauncherBackend* Instance();

	void AttachWindow(QWindow* window);

	[[nodiscard]] QVariantList Games() const;
	// The library's tabs, each { title, kind, section, count }: kind is all, favorites, section or unfiled.
	[[nodiscard]] QVariantList Folders() const;
	[[nodiscard]] QVariantList Recent() const;
	[[nodiscard]] bool         IsEmulatorReady() const { return m_emulator_ready; }
	[[nodiscard]] QString      EmulatorPath() const { return m_runner.Path(); }
	[[nodiscard]] QString      EmulatorVersion() const { return m_emulator_version; }
	[[nodiscard]] QString      EmulatorMessage() const { return m_emulator_message; }
	[[nodiscard]] bool         IsRunning() const { return m_runner.IsBusy(); }
	[[nodiscard]] int          RunningRow() const { return m_running_row; }
	[[nodiscard]] QString      ControllerName() const;
	// text with the PlayStation button symbols it is written with (✕ ○ □ △ L1 R1) shown as the connected
	// controller prints them.
	[[nodiscard]] QString PadText(const QString& text) const;
	// The controller's charge in percent, or -1 when unknown or none is connected.
	[[nodiscard]] int  ControllerBattery() const;
	[[nodiscard]] bool ControllerCharging() const;
	// The interface sounds, at the volume the settings ask for.
	[[nodiscard]] UiSounds* Sounds() const { return m_sounds; }
	// Sidebar or Console: the add-on theme's layout when it names one, otherwise the layout setting.
	[[nodiscard]] QString      LayoutName() const;
	[[nodiscard]] bool         IsTableView() const { return m_table_view; }
	[[nodiscard]] QString      LogPath() const { return m_runner.LogPath(); }
	[[nodiscard]] QString      SettingsFile() const { return m_settings_file; }
	// The emulator settings of every game without custom settings, in the editor's field form.
	[[nodiscard]] QVariantMap  GlobalFields() const;
	// The launcher's own settings in the same form: run mode, colour scheme, language, look and library layout.
	[[nodiscard]] QVariantMap LauncherFields() const;
	// One of the look and library settings: accent, interface_size, animations, background, cover_size,
	// show_titles, show_recent, on_launch, sort, window_mode, start_on, theme, layout, ui_sounds or ui_volume.
	[[nodiscard]] QVariant Look(const QString& key) const { return m_look.value(key); }
	// How much interface_size grows or shrinks every size.
	[[nodiscard]] qreal InterfaceScale() const;
	// The name shown for a choice the launcher does not list itself, such as an add-on theme, or an empty string.
	[[nodiscard]] QString ChoiceLabel(const QString& value) const;
	// The add-on theme in use, or null for the launcher's own look.
	[[nodiscard]] const ThemePack* CurrentTheme() const;

	QVariantMap FieldsFor(int row) const;
	QVariantMap Choices() const;
	QVariantMap GraphicsPreset(const QVariantMap& fields, const QString& preset) const;
	QVariantMap SaveGame(int row, const QVariantMap& fields);
	// Returns an error text, or an empty string once the global emulator settings are saved.
	QString     SaveGlobal(const QVariantMap& fields);
	void        DeleteGame(int row);
	void        MoveGame(int row, int delta);
	void        SetFavorite(int row, bool favorite);
	// Adds the game to the section named section, or takes it out; a section exists while a game is in it.
	void        SetInSection(int row, const QString& section, bool in);
	QString     RunGame(int row);
	QString     TestGame(const QVariantMap& fields);
	QVariantMap DetectGame(const QString& folder) const;
	bool        IsGameFolder(const QString& folder) const;
	// Finds the games below folder in the background and adds the ones not listed yet; reports FolderImported().
	QString      ImportFolder(const QString& folder);
	QVariantList ScanElfs(const QString& basedir, const QVariantList& current) const;
	QVariantList ParamEntries(const QString& param_file) const;
	void         SetEmulatorPath(const QString& path);
	// Applies and remembers one of LauncherFields().
	void SetLauncherField(const QString& key, const QVariant& value);
	// Shows the library as a table instead of a grid of covers; remembered between starts.
	void        SetTableView(bool table);
	void        StopGame();
	void        OpenLog() const;
	void        OpenGameFolder(int row) const;
	QVariantMap WindowGeometry() const;
	void        SaveWindowGeometry(int x, int y, int width, int height);

private:
	void         Load();
	void               Save();
	QVariantMap  GameToMap(int row) const;
	QVariantMap        FolderToMap(const QString& title, const QString& kind, const QString& section,
	                               const std::function<bool(const Kyty::Configuration&)>& shows) const;
	void         OnProbed(bool found, const QString& version, const QString& message);
	void         OnFinished(int exit_code, bool crashed);
	QString      StartGame(const QVariantMap& fields, int row);
	void         AddImported(const QList<QVariantMap>& games, int found);
	void         ApplyColorScheme();
	void         ApplyLanguage();
	void               ApplyLook();
	void               ApplyTheme();
	void               ThemeUnusable(const QString& message);
	[[nodiscard]] bool IsValidLook(const QString& key, const QVariant& value) const;

	GameLibrary        m_library;
	Kyty::Configuration m_global;
	EmulatorRunner     m_runner;
	GamepadInput*      m_gamepad = nullptr;
	UiSounds*          m_sounds  = nullptr;
	QString            m_settings_file;
	QString            m_emulator_override;
	QString            m_emulator_version;
	QString            m_emulator_message;
	bool               m_emulator_ready    = false;
	QStringList        m_run_modes;
	QString            m_run_mode;
	QString             m_color_scheme;
	QString             m_language;
	QVariantMap         m_look;
	QVector<ThemePack>  m_themes;
	// The font of a theme added to the application, and the font the launcher had before any theme.
	int                 m_theme_font = -1;
	QFont               m_default_font;
	QTranslator         m_translator;
	int                m_running_row       = -1;
	// When the running library game started, in milliseconds since the epoch; its play time grows when it ends.
	qint64             m_run_started = 0;
	QRect              m_window_geometry;
	bool               m_importing         = false;
	bool               m_table_view        = false;
	// Declared last so it is destroyed first: its destructor waits for a running import, which calls DetectGame().
	QThreadPool        m_import_pool;
};

#endif /* LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_ */
