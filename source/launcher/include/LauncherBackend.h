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
#include <QVariantList>
#include <QVariantMap>

class GamepadInput;
class QWindow;

// The object the pages use: the library, the editor, the emulator, the controller and persistence.
class LauncherBackend: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(LauncherBackend);

	Q_PROPERTY(QVariantList games READ Games NOTIFY StateChanged)
	Q_PROPERTY(QVariantList folders READ Folders NOTIFY StateChanged)
	Q_PROPERTY(QVariantList recent READ Recent NOTIFY StateChanged)
	Q_PROPERTY(bool emulatorReady READ IsEmulatorReady NOTIFY StateChanged)
	Q_PROPERTY(QString emulatorPath READ EmulatorPath NOTIFY StateChanged)
	Q_PROPERTY(QString emulatorVersion READ EmulatorVersion NOTIFY StateChanged)
	Q_PROPERTY(QString emulatorMessage READ EmulatorMessage NOTIFY StateChanged)
	Q_PROPERTY(bool running READ IsRunning NOTIFY StateChanged)
	Q_PROPERTY(int runningRow READ RunningRow NOTIFY StateChanged)
	Q_PROPERTY(QString controllerName READ ControllerName NOTIFY StateChanged)
	Q_PROPERTY(QStringList runModes READ RunModes CONSTANT)
	Q_PROPERTY(int runModeIndex READ RunModeIndex NOTIFY StateChanged)
	Q_PROPERTY(bool tableView READ IsTableView NOTIFY StateChanged)
	Q_PROPERTY(QString logPath READ LogPath NOTIFY StateChanged)
	Q_PROPERTY(QString settingsFile READ SettingsFile CONSTANT)
	// The emulator settings of every game without custom settings, in the editor's field form.
	Q_PROPERTY(QVariantMap globalFields READ GlobalFields NOTIFY StateChanged)

signals:
	void StateChanged();
	// message is empty when the game closed normally, otherwise it says how it ended.
	void GameFinished(const QString& message);
	// Result of ImportFolder: games added, game folders found (already listed ones included), and an error text.
	void FolderImported(int added, int found, const QString& error);

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
	[[nodiscard]] QStringList  RunModes() const { return m_run_modes; }
	[[nodiscard]] int          RunModeIndex() const;
	[[nodiscard]] bool         IsTableView() const { return m_table_view; }
	[[nodiscard]] QString      LogPath() const { return m_runner.LogPath(); }
	[[nodiscard]] QString      SettingsFile() const { return m_settings_file; }
	[[nodiscard]] QVariantMap  GlobalFields() const;

	Q_INVOKABLE QVariantMap   FieldsFor(int row) const;
	Q_INVOKABLE QVariantMap   Choices() const;
	Q_INVOKABLE QVariantMap   GraphicsPreset(const QVariantMap& fields, const QString& preset) const;
	Q_INVOKABLE QVariantMap   SaveGame(int row, const QVariantMap& fields);
	// Returns an error text, or an empty string once the global emulator settings are saved.
	Q_INVOKABLE QString       SaveGlobal(const QVariantMap& fields);
	Q_INVOKABLE void          DeleteGame(int row);
	Q_INVOKABLE void          MoveGame(int row, int delta);
	Q_INVOKABLE QString       RunGame(int row);
	Q_INVOKABLE QString       TestGame(const QVariantMap& fields);
	Q_INVOKABLE QVariantMap   DetectGame(const QString& folder) const;
	Q_INVOKABLE bool          IsGameFolder(const QString& folder) const;
	// Finds the games below folder in the background and adds the ones not listed yet; reports FolderImported().
	Q_INVOKABLE QString       ImportFolder(const QString& folder);
	Q_INVOKABLE QVariantList  ScanElfs(const QString& basedir, const QVariantList& current) const;
	Q_INVOKABLE QVariantList  ParamEntries(const QString& param_file) const;
	Q_INVOKABLE void          SetEmulatorPath(const QString& path);
	Q_INVOKABLE void          SetRunModeIndex(int index);
	// Shows the library as a table instead of a grid of covers; remembered between starts.
	Q_INVOKABLE void          SetTableView(bool table);
	Q_INVOKABLE void          StopGame();
	Q_INVOKABLE void          OpenLog() const;
	Q_INVOKABLE void          OpenGameFolder(int row) const;
	Q_INVOKABLE QVariantMap   WindowGeometry() const;
	Q_INVOKABLE void          SaveWindowGeometry(int x, int y, int width, int height);

private:
	void         Load();
	void         Save() const;
	QVariantMap  GameToMap(int row) const;
	QVariantMap  FolderToMap(const QString& title, const QString& folder, bool all) const;
	void         OnProbed(bool found, const QString& version, const QString& message);
	void         OnFinished(int exit_code, bool crashed);
	QString      StartGame(const QVariantMap& fields, int row);
	void         AddImported(const QList<QVariantMap>& games, int found);

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
	int                m_running_row       = -1;
	QRect              m_window_geometry;
	bool               m_importing         = false;
	bool               m_table_view        = false;
	// Declared last so it is destroyed first: its destructor waits for a running import, which calls DetectGame().
	QThreadPool        m_import_pool;
};

#endif /* LAUNCHER_INCLUDE_LAUNCHER_BACKEND_H_ */
