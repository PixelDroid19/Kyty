#include "EmulatorRunner.h"

#include "Configuration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#if defined(_WIN32)
#include <Q_STARTUPINFO>
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

#if defined(_WIN32)
constexpr char SCRIPT_EXE[] = "fc_script.exe";
#else
constexpr char SCRIPT_EXE[] = "fc_script";
#endif

#if defined(_WIN32)
constexpr char CMD_EXE[]    = "cmd.exe";
constexpr char CONEMU_EXE[] = "C:/Program Files/ConEmu/ConEmu64.exe";
#elif defined(__APPLE__)
constexpr char MAC_OPEN[]     = "open";
constexpr char MAC_TERMINAL[] = "Terminal";
#endif
#if !defined(_WIN32)
constexpr char KYTY_BASH_FILE[] = "kyty_run.sh";
#endif
constexpr char KYTY_MOUNT[]            = "kyty_mount";
constexpr char KYTY_EXECUTE[]          = "kyty_execute";
constexpr char KYTY_LOAD_ELF[]         = "kyty_load_elf";
constexpr char KYTY_LOAD_SYMBOLS_ALL[] = "kyty_load_symbols_all";
constexpr char KYTY_LOAD_PARAM_SFO[]   = "kyty_load_param_sfo";
constexpr char KYTY_LOAD_PARAM_JSON[]  = "kyty_load_param_json";
constexpr char KYTY_INIT[]             = "kyty_init";
constexpr char KYTY_LUA_FILE[]         = "kyty_run.lua";
constexpr char KYTY_LOG_FILE[]         = "kyty_run.log";
#if defined(_WIN32)
constexpr DWORD CMD_X_CHARS = 175;
constexpr DWORD CMD_Y_CHARS = 1000;
#endif

// A probe that runs longer than this is considered hung and is killed.
constexpr int PROBE_TIMEOUT_MS = 10000;
constexpr int START_TIMEOUT_MS = 3000;
// Time a game gets to close after Stop() before it is killed.
constexpr int STOP_TIMEOUT_MS = 3000;

namespace {

QStringList CandidatePaths()
{
	// Search order: application dir, current dir, parent dir (robust for double-click, shortcuts, and CLI)
	QStringList candidates;
	candidates << QCoreApplication::applicationDirPath() + QDir::separator() + SCRIPT_EXE;
	candidates << QDir(".").absoluteFilePath(SCRIPT_EXE);
	candidates << QDir("..").absoluteFilePath(SCRIPT_EXE);
#if defined(__APPLE__)
	// Inside .app bundle, binary is in Contents/MacOS, fc_script may be alongside .app
	candidates << QDir(QCoreApplication::applicationDirPath() + "/../..").absoluteFilePath(SCRIPT_EXE);
	candidates << QDir(QCoreApplication::applicationDirPath() + "/../../..").absoluteFilePath(SCRIPT_EXE);
#endif
	return candidates;
}

// Paths come from the user and the file system, so quotes, backslashes and line breaks must not end the Lua string early.
QString LuaQuote(QString text)
{
	text.replace('\\', "\\\\").replace('\'', "\\'").replace('\n', "\\n").replace('\r', "\\r");
	return "'" + text + "'";
}

bool CreateLuaScript(const Kyty::Configuration& info, const QString& file_name)
{
	QFile file(file_name);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return false;
	}
	QTextStream s(&file);

	s << "local cfg = {\n";
	s << "\t ScreenWidth = " << info.screen_width << ";\n";
	s << "\t ScreenHeight = " << info.screen_height << ";\n";
	s << "\t RenderResolutionMode = '" << EnumToText(info.render_resolution_mode) << "';\n";
	s << "\t RenderResolutionScope = '" << EnumToText(info.render_resolution_scope) << "';\n";
	s << "\t RenderResolutionWidth = " << info.render_resolution_width << ";\n";
	s << "\t RenderResolutionHeight = " << info.render_resolution_height << ";\n";
	s << "\t PresentationFilter = '" << EnumToText(info.presentation_filter) << "';\n";
	s << "\t PresentationFsrSharpness = " << info.presentation_fsr_sharpness << ";\n";
	s << "\t TextureAnisotropy = '" << EnumToText(info.texture_anisotropy) << "';\n";
	s << "\t Neo = " << (info.neo ? "true" : "false") << ";\n";
	s << "\t VulkanValidationEnabled = " << (info.vulkan_validation_enabled ? "true" : "false") << ";\n";
	s << "\t ShaderValidationEnabled = " << (info.shader_validation_enabled ? "true" : "false") << ";\n";
	s << "\t ShaderOptimizationType = '" << EnumToText(info.shader_optimization_type) << "';\n";
	s << "\t ShaderLogDirection = '" << EnumToText(info.shader_log_direction) << "';\n";
	s << "\t ShaderLogFolder = " << LuaQuote(info.shader_log_folder) << ";\n";
	s << "\t CommandBufferDumpEnabled = " << (info.command_buffer_dump_enabled ? "true" : "false") << ";\n";
	s << "\t CommandBufferDumpFolder = " << LuaQuote(info.command_buffer_dump_folder) << ";\n";
	s << "\t PrintfDirection = '" << EnumToText(info.printf_direction) << "';\n";
	s << "\t PrintfLevel = '" << EnumToText(info.printf_level) << "';\n";
	s << "\t PrintfOutputFile = " << LuaQuote(info.printf_output_file) << ";\n";
	s << "\t PrintfOutputFolder = " << LuaQuote(info.printf_output_folder) << ";\n";
	s << "\t ProfilerDirection = '" << EnumToText(info.profiler_direction) << "';\n";
	s << "\t ProfilerOutputFile = " << LuaQuote(info.profiler_output_file) << ";\n";
	s << "\t SpirvDebugPrintfEnabled = " << (info.spirv_debug_printf_enabled ? "true" : "false") << ";\n";
	s << "\t PipelineDumpEnabled = " << (info.pipeline_dump_enabled ? "true" : "false") << ";\n";
	s << "\t PipelineDumpFolder = " << LuaQuote(info.pipeline_dump_folder) << ";\n";
	s << "}\n";

	s << KYTY_INIT << "(cfg);\n";
	s << KYTY_MOUNT << "(" << LuaQuote(info.basedir) << ", '/app0');\n";

	// Param file: detect .json vs .sfo
	if (!info.param_file.isEmpty())
	{
		if (info.param_file.endsWith(".json", Qt::CaseInsensitive))
		{
			s << KYTY_LOAD_PARAM_JSON << "(" << LuaQuote(info.param_file) << ");\n";
		} else
		{
			s << KYTY_LOAD_PARAM_SFO << "(" << LuaQuote(info.param_file) << ");\n";
		}
	}

	for (const auto& elf: info.elfs_selected)
	{
		s << KYTY_LOAD_ELF << "(" << LuaQuote("/app0/" + elf) << ");\n";
	}

	s << KYTY_LOAD_SYMBOLS_ALL << "();\n";

	s << KYTY_EXECUTE << "();\n";

	file.close();
	return true;
}

#if !defined(_WIN32)
bool CreateBashScript(const QString& interpreter, const QString& lua_file_name, const QString& file_name)
{
	QFile file(file_name);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return false;
	}
	QTextStream s(&file);

	s << "#!/bin/bash\n";
	s << "'" << interpreter << "' '" << lua_file_name << "'\n";
	s << "echo Press any key...\n";
	s << "read -n1\n";

	file.close();

	return file.setPermissions(file.permissions() | QFile::ExeUser | QFile::ExeOwner | QFile::ExeGroup);
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
struct Terminal
{
	const char* program;
	// Arguments placed before "bash <script>". Each keeps the terminal in the foreground, so the launcher sees when the game ends.
	QStringList arguments;
};

const QList<Terminal>& Terminals()
{
	static const QList<Terminal> terminals = {
	    {"konsole", {"--separate", "-e"}}, {"kitty", {}},   {"alacritty", {"-e"}},
	    {"foot", {}},                      {"gnome-terminal", {"--wait", "--"}}, {"xterm", {"-e"}},
	};
	return terminals;
}
#endif

} // namespace

EmulatorRunner::EmulatorRunner(QObject* parent): QObject(parent)
{
	m_probe_timeout.setSingleShot(true);
	m_probe_timeout.setInterval(PROBE_TIMEOUT_MS);
	connect(&m_probe_timeout, &QTimer::timeout, &m_probe, &QProcess::kill);

	connect(&m_probe, &QProcess::finished, this, &EmulatorRunner::OnProbeFinished);
	connect(&m_probe, &QProcess::errorOccurred, this, &EmulatorRunner::OnProbeFailed);

	m_stop_timeout.setSingleShot(true);
	m_stop_timeout.setInterval(STOP_TIMEOUT_MS);
	connect(&m_stop_timeout, &QTimer::timeout, &m_process, &QProcess::kill);

	connect(&m_process, &QProcess::finished, this, &EmulatorRunner::OnProcessFinished);
}

EmulatorRunner::~EmulatorRunner()
{
	// A game must not outlive the launcher as an orphan whose window nobody tracks.
	disconnect(&m_process, nullptr, this, nullptr);
	if (IsBusy())
	{
		m_process.kill();
		m_process.waitForFinished(STOP_TIMEOUT_MS);
	}
}

void EmulatorRunner::Probe(const QString& override_path)
{
	if (m_probe.state() != QProcess::NotRunning)
	{
		return;
	}
	m_path.clear();

	QStringList candidates;
	if (!override_path.isEmpty())
	{
		candidates << override_path;
	} else
	{
		candidates = CandidatePaths();
	}

	for (const auto& cand: candidates)
	{
		if (QFile::exists(cand))
		{
			m_path = cand;
			break;
		}
	}

	if (m_path.isEmpty())
	{
		emit Probed(false, {}, QCoreApplication::translate("Launcher", "Emulator not found. Set its path in Settings."));
		return;
	}

	m_probe.setProgram(m_path);
	m_probe.start();
	m_probe_timeout.start();
}

void EmulatorRunner::OnProbeFinished()
{
	m_probe_timeout.stop();

	const auto output = QString::fromUtf8(m_probe.readAllStandardOutput());
	const auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

	const QStringList required = {KYTY_MOUNT, KYTY_EXECUTE, KYTY_LOAD_ELF, KYTY_LOAD_SYMBOLS_ALL, KYTY_LOAD_PARAM_SFO, KYTY_INIT};
	bool              found    = lines.count() >= 2;
	for (const auto& fn: required)
	{
		found = found && lines.contains(QString("Lua function: ") + fn);
	}

	if (!found)
	{
		m_path.clear();
		emit Probed(false, {}, QCoreApplication::translate("Launcher", "The file is not a compatible emulator."));
		return;
	}

	const auto version = lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0);
	emit Probed(true, version, {});
}

void EmulatorRunner::OnProbeFailed()
{
	if (m_probe.state() != QProcess::NotRunning)
	{
		return;
	}
	m_probe_timeout.stop();
	m_path.clear();
	emit Probed(false, {}, QCoreApplication::translate("Launcher", "The emulator could not be started."));
}

QString EmulatorRunner::Start(const Kyty::Configuration& info, const QString& mode)
{
	if (IsBusy())
	{
		return QCoreApplication::translate("Launcher", "A game is already running.");
	}
	if (m_path.isEmpty())
	{
		return QCoreApplication::translate("Launcher", "Emulator not found. Set its path in Settings.");
	}

	const QFileInfo f(m_path);
	const auto      dir = f.absoluteDir();

	const auto lua_file_name = dir.filePath(KYTY_LUA_FILE);
	if (!CreateLuaScript(info, lua_file_name))
	{
		return QCoreApplication::translate("Launcher", "Can't create file:\n") + lua_file_name;
	}

	m_process.setWorkingDirectory(dir.path());
	m_process.setStandardInputFile(QProcess::nullDevice());
	m_process.setProcessChannelMode(QProcess::MergedChannels);

	if (mode == DIRECT_MODE)
	{
		// The output goes straight to a file, so the launcher never reads a pipe while the game runs.
		m_log_path = dir.filePath(KYTY_LOG_FILE);
		m_process.setStandardOutputFile(m_log_path, QIODevice::Truncate);
		m_process.setProgram(m_path);
		m_process.setArguments({lua_file_name});
#if defined(_WIN32)
		m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args)
		                                            { args->flags |= static_cast<uint32_t>(CREATE_NO_WINDOW); });
#endif
	} else
	{
		m_process.setStandardOutputFile(QProcess::nullDevice());
		const auto error = PrepareTerminal(mode, lua_file_name);
		if (!error.isEmpty())
		{
			return error;
		}
	}

	m_process.start();
	if (!m_process.waitForStarted(START_TIMEOUT_MS))
	{
		return QCoreApplication::translate("Launcher", "The emulator could not be started: %1").arg(m_process.errorString());
	}
	return {};
}

QString EmulatorRunner::PrepareTerminal(const QString& mode, const QString& lua_file_name)
{
#if defined(_WIN32)
	m_process.setProgram(mode == "ConEmu" ? CONEMU_EXE : CMD_EXE);
	m_process.setArguments(mode == "ConEmu" ? QStringList {"-run", m_path, lua_file_name} : QStringList {"/K", m_path, lua_file_name});
	m_process.setCreateProcessArgumentsModifier(
	    [](QProcess::CreateProcessArguments* args)
	    {
		    args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
		    args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		    args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		    args->startupInfo->dwXCountChars = CMD_X_CHARS;
		    args->startupInfo->dwYCountChars = CMD_Y_CHARS;
	    });
	return {};
#else
	const auto bash_file_name = QFileInfo(lua_file_name).absoluteDir().filePath(KYTY_BASH_FILE);
	if (!CreateBashScript(m_path, lua_file_name, bash_file_name))
	{
		return QCoreApplication::translate("Launcher", "Can't create file:\n") + bash_file_name;
	}
#if defined(__APPLE__)
	Q_UNUSED(mode);
	// -W keeps "open" running until Terminal quits, the closest the launcher gets to the game's lifetime there.
	m_process.setProgram(MAC_OPEN);
	m_process.setArguments({"-W", "-a", MAC_TERMINAL, bash_file_name});
	return {};
#else
	for (const auto& terminal: Terminals())
	{
		if (mode == terminal.program)
		{
			m_process.setProgram(terminal.program);
			m_process.setArguments(terminal.arguments + QStringList {"bash", bash_file_name});
			return {};
		}
	}
	return QCoreApplication::translate("Launcher", "The terminal %1 is not available.").arg(mode);
#endif
#endif
}

void EmulatorRunner::Stop()
{
	if (!IsBusy())
	{
		return;
	}
	m_process.terminate();
	m_stop_timeout.start();
}

void EmulatorRunner::OnProcessFinished(int exit_code, QProcess::ExitStatus status)
{
	m_stop_timeout.stop();
	emit Finished(exit_code, status == QProcess::CrashExit);
}

QStringList EmulatorRunner::RunModes()
{
	QStringList modes = {DIRECT_MODE};
#if defined(_WIN32)
	modes << "cmd";
	if (QFile::exists(CONEMU_EXE))
	{
		modes << "ConEmu";
	}
#elif defined(__APPLE__)
	modes << MAC_TERMINAL;
#else
	for (const auto& terminal: Terminals())
	{
		if (!QStandardPaths::findExecutable(terminal.program).isEmpty())
		{
			modes << terminal.program;
		}
	}
#endif
	return modes;
}
