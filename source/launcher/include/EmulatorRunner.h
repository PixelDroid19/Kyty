#ifndef LAUNCHER_INCLUDE_EMULATOR_RUNNER_H_
#define LAUNCHER_INCLUDE_EMULATOR_RUNNER_H_

#include "Common.h"

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace Kyty {
class Configuration;
} // namespace Kyty

// Finds the fc_script interpreter, checks that it exposes the Lua API, and starts games with it.
class EmulatorRunner: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(EmulatorRunner);

signals:
	// message is the reason the emulator is unusable; empty when found.
	void Probed(bool found, const QString& version, const QString& message);
	// crashed is true when the process did not exit normally; exit_code is only meaningful otherwise.
	void Finished(int exit_code, bool crashed);

public:
	explicit EmulatorRunner(QObject* parent = nullptr);
	~EmulatorRunner() override;

	// Searches for the interpreter, using override_path first when it is set. Result arrives through Probed().
	void Probe(const QString& override_path);

	[[nodiscard]] const QString& Path() const { return m_path; }
	[[nodiscard]] bool           IsBusy() const { return m_process.state() != QProcess::NotRunning; }
	// The file that receives the emulator's output in direct mode; empty before the first run.
	[[nodiscard]] const QString& LogPath() const { return m_log_path; }

	// Writes the run scripts for info and starts them with the given mode from RunModes().
	// Returns an error text, or an empty string on success.
	QString Start(const Kyty::Configuration& info, const QString& mode);

	// Asks the running game to close, and kills it if it does not.
	void Stop();

	// Index 0 is always DIRECT_MODE; the rest are the terminals found on this host.
	static QStringList RunModes();

	static constexpr char DIRECT_MODE[] = "Direct";

private:
	void OnProbeFinished();
	void OnProbeFailed();
	void OnProcessFinished(int exit_code, QProcess::ExitStatus status);
	// Points m_process at the terminal named mode, running the run script inside it.
	QString PrepareTerminal(const QString& mode, const QString& lua_file_name);

	QString     m_path;
	QString     m_log_path;
	QProcess    m_probe;
	QTimer      m_probe_timeout;
	QProcess    m_process;
	QTimer      m_stop_timeout;
};

#endif /* LAUNCHER_INCLUDE_EMULATOR_RUNNER_H_ */
