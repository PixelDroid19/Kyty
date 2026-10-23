#ifndef LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_
#define LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_

#include "Common.h"
#include "Configuration.h"
#include "EmulatorRunner.h"
#include "GameLibrary.h"

#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTranslator>
#include <QVariantList>
#include <QVariantMap>

class GamepadInput;
class QWindow;

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

public:
	explicit LauncherBackend(QObject* parent = nullptr);
	~LauncherBackend() override;

	// The one backend of the process, created before the UI.
	static LauncherBackend* Instance();

	void AttachWindow(QWindow* window);

	[[nodiscard]] QVariantList Games() const;
	[[nodiscard]] QVariantList Folders() const;
	[[nodiscard]] QVariantList Recent() const;
	[[nodiscard]] bool         IsEmulatorReady() const { return m_emulator_ready; }
	[[nodiscard]] QString      EmulatorPath() const { return m_runner.Path(); }
	[[nodiscard]] QString      EmulatorVersion() const { return m_emulator_version; }
	[[nodiscard]] QString      EmulatorMessage() const { return m_emulator_message; }
	[[nodiscard]] bool         IsRunning() const { return m_runner.IsBusy(); }
	[[nodiscard]] int          RunningRow() const { return m_running_row; }
	[[nodiscard]] QString      ControllerName() const;
	[[nodiscard]] bool         IsTableView() const { return m_table_view; }
	[[nodiscard]] QString      LogPath() const { return m_runner.LogPath(); }
	[[nodiscard]] QString      SettingsFile() const { return m_settings_file; }
	// The emulator settings of every game without custom settings, in the editor's field form.
	[[nodiscard]] QVariantMap  GlobalFields() const;
	// The launcher's own settings in the same form: run mode, colour scheme, language, look and library layout.
	[[nodiscard]] QVariantMap LauncherFields() const;
	// One of the look and library settings: accent, interface_size, animations, background, cover_size,
	// show_titles, show_recent or on_launch.
	[[nodiscard]] QVariant Look(const QString& key) const { return m_look.value(key); }
	// How much interface_size grows or shrinks every size.
	[[nodiscard]] qreal InterfaceScale() const;

	QVariantMap FieldsFor(int row) const;
	QVariantMap Choices() const;
	QVariantMap GraphicsPreset(const QVariantMap& fields, const QString& preset) const;
	QVariantMap SaveGame(int row, const QVariantMap& fields);
	// Returns an error text, or an empty string once the global emulator settings are saved.
	QString     SaveGlobal(const QVariantMap& fields);
	void        DeleteGame(int row);
	void        MoveGame(int row, int delta);
	void        SetFavorite(int row, bool favorite);
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
	void         Save() const;
	QVariantMap  GameToMap(int row) const;
	QVariantMap  FolderToMap(const QString& title, const QString& folder, bool all) const;
	void         OnProbed(bool found, const QString& version, const QString& message);
	void         OnFinished(int exit_code, bool crashed);
	QString      StartGame(const QVariantMap& fields, int row);
	void         AddImported(const QList<QVariantMap>& games, int found);
	void         ApplyColorScheme();
	void         ApplyLanguage();
	void               ApplyLook();
	[[nodiscard]] bool IsValidLook(const QString& key, const QVariant& value) const;

	GameLibrary        m_library;
	Kyty::Configuration m_global;
	EmulatorRunner     m_runner;
	GamepadInput*      m_gamepad = nullptr;
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
