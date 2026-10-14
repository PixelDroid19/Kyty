#include "MainDialog.h"

#include "Common.h"
#include "Configuration.h"
#include "ConfigurationItem.h"
#include "ConfigurationListWidget.h"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTextStream>
#include <QVariant>
#include <QtCore>
#if defined(_WIN32)
#include <Q_STARTUPINFO>
#endif

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char SCRIPT_EXE[]     = "fc_script.exe";
#else
constexpr char SCRIPT_EXE[] = "fc_script";
#endif

#if defined(_WIN32)
constexpr char CMD_EXE[]    = "cmd.exe";
constexpr char CONEMU_EXE[] = "C:/Program Files/ConEmu/ConEmu64.exe";
#else
constexpr char GNOME[]          = "gnome-terminal";
constexpr char XTERM[]          = "xterm";
constexpr char KYTY_BASH_FILE[] = "kyty_run.sh";
#if defined(__APPLE__)
constexpr char MAC_OPEN[] = "open";
#endif
#endif
constexpr char KYTY_MOUNT[]    = "kyty_mount";
constexpr char KYTY_EXECUTE[]  = "kyty_execute";
constexpr char KYTY_LOAD_ELF[] = "kyty_load_elf";
constexpr char KYTY_LOAD_SYMBOLS_ALL[] = "kyty_load_symbols_all";
constexpr char KYTY_LOAD_PARAM_SFO[]   = "kyty_load_param_sfo";
constexpr char KYTY_LOAD_PARAM_JSON[]  = "kyty_load_param_json";
constexpr char KYTY_INIT[]             = "kyty_init";
constexpr char KYTY_LUA_FILE[]         = "kyty_run.lua";
#if defined(_WIN32)
constexpr DWORD CMD_X_CHARS = 175;
constexpr DWORD CMD_Y_CHARS = 1000;
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";

class DetachableProcess: public QProcess
{
	Q_OBJECT;

public:
	explicit DetachableProcess(QObject* parent = nullptr): QProcess(parent) {}
	void Detach()
	{
		this->waitForStarted();
		setProcessState(QProcess::NotRunning);
	}
};

class MainDialogPrivate: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(MainDialogPrivate);

public:
	MainDialogPrivate() = default;
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run();
	void CompleteRun();

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }

	static void WriteSettings(QSettings* s);
	static void ReadSettings(QSettings* s);

private:
	static QByteArray g_last_geometry;

	Ui::MainDialog* m_ui          = {nullptr};
	MainDialog*     m_main_dialog = nullptr;
	QString         m_interpreter;

	/*DetachableProcess*/ QProcess m_process;

	ConfigurationItem* m_running_item = nullptr;
};

QByteArray MainDialogPrivate::g_last_geometry;

MainDialog::MainDialog(QWidget* parent): QDialog(parent), m_p(new MainDialogPrivate)
{
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate()
{
	delete m_ui;
}

void MainDialogPrivate::Setup(MainDialog* main_dialog)
{
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);

	setParent(main_dialog);
	m_main_dialog = main_dialog;
	m_ui->widget->SetMainDialog(main_dialog);

	main_dialog->setWindowFlags(Qt::Dialog /*| Qt::MSWindowsFixedSizeDialogHint*/);

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter, Qt::QueuedConnection);
	connect(m_ui->pushButton_Run, &QPushButton::clicked, this, &MainDialogPrivate::Run);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, &MainDialogPrivate::Update);
	connect(m_ui->widget, &ConfigurationListWidget::Run, this, &MainDialogPrivate::Run);
	connect(main_dialog, &MainDialog::Resize,
	        [=]()
	        {
		        g_last_geometry = m_main_dialog->saveGeometry();
		        m_ui->widget->WriteSettings();
	        });

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	connect(&m_process, &QProcess::finished, this,
	        [=](int exit_code, QProcess::ExitStatus exit_status)
	        {
		        const bool interpreter_process = m_process.program() == m_interpreter;
		        CompleteRun();

		        if (interpreter_process && exit_status == QProcess::CrashExit)
		        {
			        QMessageBox::critical(m_main_dialog, tr("Error"), tr("The emulator process crashed"));
		        } else if (interpreter_process && exit_code != 0)
		        {
			        QMessageBox::critical(m_main_dialog, tr("Error"), tr("The emulator exited with code %1").arg(exit_code));
		        }
	        });
#else
	connect(&m_process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
	        [=](int exit_code, QProcess::ExitStatus exit_status)
	        {
		        const bool interpreter_process = m_process.program() == m_interpreter;
		        CompleteRun();

		        if (interpreter_process && exit_status == QProcess::CrashExit)
		        {
			        QMessageBox::critical(m_main_dialog, tr("Error"), tr("The emulator process crashed"));
		        } else if (interpreter_process && exit_code != 0)
		        {
			        QMessageBox::critical(m_main_dialog, tr("Error"), tr("The emulator exited with code %1").arg(exit_code));
		        }
	        });
#endif
	connect(&m_process, &QProcess::errorOccurred, this,
	        [=](QProcess::ProcessError error)
	        {
		        if (error != QProcess::FailedToStart)
		        {
			        return;
		        }

		        const auto program     = m_process.program();
		        const auto error_text = m_process.errorString();
		        CompleteRun();
		        QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't start process: ") + program + QLatin1Char('\n') + error_text);
	        });

	// connect(main_dialog, &MainDialog::Quit, [=]() { m_process.Detach(); });

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	m_main_dialog->restoreGeometry(g_last_geometry);

	Update();
}

void MainDialogPrivate::FindInterpreter()
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
	for (const auto& cand : candidates)
	{
		if (QFile::exists(cand))
		{
			m_interpreter = cand;
			break;
		}
	}
	// Fallback legacy check
	if (m_interpreter.isEmpty())
	{
		QFile file(QDir(".").absoluteFilePath(SCRIPT_EXE));
		if (file.exists())
		{
			m_interpreter = file.fileName();
		} else
		{
			QFile file2(QDir("..").absoluteFilePath(SCRIPT_EXE));
			if (file2.exists())
			{
				m_interpreter = file2.fileName();
			}
		}
	}

	bool found = QFile::exists(m_interpreter);

	if (found)
	{
		m_ui->label_Interpreter->setText(tr("Emulator: ") + m_interpreter);

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() >= 2)
		{
			m_ui->label_Version->setText(tr("Version: ") + (lines.at(0).startsWith("exe_name") ? lines.at(1) : lines.at(0)));
		} else
		{
			found = false;
		}

		found = found && lines.contains(QString("Lua function: ") + KYTY_MOUNT);
		found = found && lines.contains(QString("Lua function: ") + KYTY_EXECUTE);
		found = found && lines.contains(QString("Lua function: ") + KYTY_LOAD_ELF);
		found = found && lines.contains(QString("Lua function: ") + KYTY_LOAD_SYMBOLS_ALL);
		found = found && lines.contains(QString("Lua function: ") + KYTY_LOAD_PARAM_SFO);
		found = found && lines.contains(QString("Lua function: ") + KYTY_INIT);
	}

	if (!found)
	{
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

#if defined(_WIN32)
	QFile console(CONEMU_EXE);
	bool  con_emu = console.exists();
#else
	bool con_emu = true;
#endif

	m_ui->radioButton_Cmd->setEnabled(true);
	m_ui->radioButton_Cmd->setChecked(true);
	m_ui->radioButton_ConEmu->setEnabled(con_emu);
	m_ui->radioButton_ConEmu->setChecked(false);

#if !defined(_WIN32)
#if defined(__APPLE__)
	m_ui->radioButton_Cmd->setText(QCoreApplication::translate("MainDialog", "Terminal (open)", nullptr));
	m_ui->radioButton_ConEmu->setText(QCoreApplication::translate("MainDialog", "Direct", nullptr));
#else
	m_ui->radioButton_Cmd->setText(QCoreApplication::translate("MainDialog", "gnome-terminal", nullptr));
	m_ui->radioButton_ConEmu->setText(QCoreApplication::translate("MainDialog", "xterm", nullptr));
#endif
#endif

	Update();
}

static QString LuaStringLiteral(const QString& value);

static bool CreateLuaScript(Kyty::Configuration* info, const QString& file_name)
{
	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QTextStream s(&file);

		s << "local cfg = {\n";
		s << "\t ScreenWidth = " << info->screen_width << ";\n";
		s << "\t ScreenHeight = " << info->screen_height << ";\n";
		s << "\t RenderResolutionMode = " << LuaStringLiteral(EnumToText(info->render_resolution_mode)) << ";\n";
		s << "\t RenderResolutionScope = " << LuaStringLiteral(EnumToText(info->render_resolution_scope)) << ";\n";
		s << "\t RenderResolutionWidth = " << info->render_resolution_width << ";\n";
		s << "\t RenderResolutionHeight = " << info->render_resolution_height << ";\n";
		s << "\t PresentationFilter = " << LuaStringLiteral(EnumToText(info->presentation_filter)) << ";\n";
		s << "\t PresentationFsrSharpness = " << info->presentation_fsr_sharpness << ";\n";
		s << "\t TextureAnisotropy = " << LuaStringLiteral(EnumToText(info->texture_anisotropy)) << ";\n";
		s << "\t Neo = " << (info->neo ? "true" : "false") << ";\n";
		s << "\t VulkanValidationEnabled = " << (info->vulkan_validation_enabled ? "true" : "false") << ";\n";
		s << "\t ShaderValidationEnabled = " << (info->shader_validation_enabled ? "true" : "false") << ";\n";
		s << "\t ShaderOptimizationType = " << LuaStringLiteral(EnumToText(info->shader_optimization_type)) << ";\n";
		s << "\t ShaderLogDirection = " << LuaStringLiteral(EnumToText(info->shader_log_direction)) << ";\n";
		s << "\t ShaderLogFolder = " << LuaStringLiteral(info->shader_log_folder) << ";\n";
		s << "\t CommandBufferDumpEnabled = " << (info->command_buffer_dump_enabled ? "true" : "false") << ";\n";
		s << "\t CommandBufferDumpFolder = " << LuaStringLiteral(info->command_buffer_dump_folder) << ";\n";
		s << "\t PrintfDirection = " << LuaStringLiteral(EnumToText(info->printf_direction)) << ";\n";
		s << "\t PrintfLevel = " << LuaStringLiteral(EnumToText(info->printf_level)) << ";\n";
		s << "\t PrintfOutputFile = " << LuaStringLiteral(info->printf_output_file) << ";\n";
		s << "\t PrintfOutputFolder = " << LuaStringLiteral(info->printf_output_folder) << ";\n";
		s << "\t ProfilerDirection = " << LuaStringLiteral(EnumToText(info->profiler_direction)) << ";\n";
		s << "\t ProfilerOutputFile = " << LuaStringLiteral(info->profiler_output_file) << ";\n";
		s << "\t SpirvDebugPrintfEnabled = " << (info->spirv_debug_printf_enabled ? "true" : "false") << ";\n";
		s << "\t PipelineDumpEnabled = " << (info->pipeline_dump_enabled ? "true" : "false") << ";\n";
		s << "\t PipelineDumpFolder = " << LuaStringLiteral(info->pipeline_dump_folder) << ";\n";
		s << "}\n";

		s << KYTY_INIT << "(cfg);\n";
		s << KYTY_MOUNT << "(" << LuaStringLiteral(info->basedir) << ", '/app0');\n";

		// Param file: detect .json vs .sfo
		if (!info->param_file.isEmpty())
		{
			if (info->param_file.endsWith(".json", Qt::CaseInsensitive))
			{
				s << KYTY_LOAD_PARAM_JSON << "(" << LuaStringLiteral(info->param_file) << ");\n";
			} else
			{
				s << KYTY_LOAD_PARAM_SFO << "(" << LuaStringLiteral(info->param_file) << ");\n";
			}
		}

		for (const auto& elf: info->elfs_selected)
		{
			s << KYTY_LOAD_ELF << "(" << LuaStringLiteral(QStringLiteral("/app0/") + elf) << ");\n";
		}

		//		for (const auto& lib: info->libs_selected)
		//		{
		//			s << KYTY_LOAD_SYMBOLS << "('" << lib << "');\n";
		//		}

		s << KYTY_LOAD_SYMBOLS_ALL << "();\n";

		s << KYTY_EXECUTE << "();\n";

		file.close();
		return true;
	}
	return false;
}

static QString LuaStringLiteral(const QString& value)
{
	QString result;
	result.reserve(value.size() + 2);
	result += QLatin1Char('"');

	for (const QChar character: value)
	{
		const ushort code = character.unicode();
		switch (code)
		{
			case '\\': result += QStringLiteral("\\\\"); break;
			case '"': result += QStringLiteral("\\\""); break;
			case '\n': result += QStringLiteral("\\n"); break;
			case '\r': result += QStringLiteral("\\r"); break;
			case '\t': result += QStringLiteral("\\t"); break;
			case '\b': result += QStringLiteral("\\b"); break;
			case '\f': result += QStringLiteral("\\f"); break;
			default:
				if (code < 0x20u || code == 0x7fu)
				{
					result += QLatin1Char('\\');
					result += QString::number(code).rightJustified(3, QLatin1Char('0'));
				} else
				{
					result += character;
				}
				break;
		}
	}

	result += QLatin1Char('"');
	return result;
}

static QString ShellQuote(const QString& value)
{
	QString result = QStringLiteral("'");
	for (const QChar character: value)
	{
		if (character == QLatin1Char('\''))
		{
			result += QStringLiteral("'\\''");
		} else
		{
			result += character;
		}
	}
	result += QLatin1Char('\'');
	return result;
}

#if !defined(_WIN32)
static bool CreateBashScript(const QString& interpreter, const QString& lua_file_name, const QString& file_name)
{
	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		QTextStream s(&file);

		s << "#!/bin/bash\n";
		s << ShellQuote(interpreter) << " " << ShellQuote(lua_file_name) << "\n";
		s << "emulator_exit=$?\n";
		s << "echo Press any key...\n";
		s << "read -n1\n";
		s << "exit \"$emulator_exit\"\n";

		file.close();

		return file.setPermissions(file.permissions() | QFile::ExeUser | QFile::ExeOwner | QFile::ExeGroup);
	}
	return false;
}
#endif

void MainDialog::RunInterpreter(QProcess* process, Kyty::Configuration* info, [[maybe_unused]] bool con_emu)
{
	const auto& interpreter = m_p->GetInterpreter();

	QFileInfo f(interpreter);
	auto      dir = f.absoluteDir();

	auto lua_file_name = dir.filePath(KYTY_LUA_FILE);
	if (!CreateLuaScript(info, lua_file_name))
	{
		QMessageBox::critical(this, tr("Error"), tr("Can't create file:\n") + lua_file_name);
		QApplication::quit();
		return;
	}

#if !defined(_WIN32)
	auto bash_file_name = dir.filePath(KYTY_BASH_FILE);
	if (!CreateBashScript(interpreter, lua_file_name, bash_file_name))
	{
		QMessageBox::critical(this, tr("Error"), tr("Can't create file:\n") + bash_file_name);
		QApplication::quit();
		return;
	}

#if defined(__APPLE__)
	// macOS: try to open Terminal via 'open', fallback to direct execution
	if (con_emu)
	{
		// Direct execution without terminal wrapper
		process->setProgram(interpreter);
		process->setArguments({lua_file_name});
	} else
	{
		// Use 'open -a Terminal' to launch bash script in Terminal.app
		// If that fails, QProcess will error and we fallback to direct
		process->setProgram(MAC_OPEN);
		process->setArguments({"-a", "Terminal", bash_file_name});
		// Fallback check: if open fails, use direct
	}
#else
	if (con_emu)
	{
		process->setProgram(XTERM);
		process->setArguments({"-e", "bash", bash_file_name});
	} else
	{
		process->setProgram(GNOME);
		process->setArguments({"--wait", "--", "bash", bash_file_name});
	}
#endif
#else
	if (con_emu)
	{
		process->setProgram(CONEMU_EXE);
		process->setArguments({"-run", interpreter, lua_file_name});
	} else
	{
		process->setProgram(CMD_EXE);
		process->setArguments({"/K", interpreter, lua_file_name});
	}
#endif
	process->setWorkingDirectory(dir.path());
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier(
	    [](QProcess::CreateProcessArguments* args)
	    {
		    args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
		    args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		    args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		    args->startupInfo->dwXCountChars = CMD_X_CHARS;
		    args->startupInfo->dwYCountChars = CMD_Y_CHARS;
	    });
#endif
	process->start();
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings* s)
{
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings* s)
{
	MainDialogPrivate::ReadSettings(s);
}

void MainDialog::resizeEvent(QResizeEvent* event)
{
	emit Resize();
	QDialog::resizeEvent(event);
}

void MainDialogPrivate::WriteSettings(QSettings* s)
{
	s->beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty())
	{
		s->setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}

	s->endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings* s)
{
	s->beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s->value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();

	s->endGroup();
}

void MainDialogPrivate::Run()
{
	auto* selected_item = m_ui->widget->GetSelectedItem();
	if (selected_item == nullptr)
	{
		Update();
		return;
	}

	m_running_item = selected_item;

	m_running_item->SetRunning(true);

	m_main_dialog->RunInterpreter(&m_process, m_running_item->GetInfo(), m_ui->radioButton_ConEmu->isChecked());

	if (m_process.state() == QProcess::NotRunning)
	{
		CompleteRun();
	} else
	{
		Update();
	}
}

void MainDialogPrivate::CompleteRun()
{
	if (m_running_item == nullptr)
	{
		return;
	}

	m_running_item->SetRunning(false);
	m_running_item = nullptr;
	Update();
}

void MainDialogPrivate::Update()
{
	const auto* item = m_ui->widget->GetSelectedItem();

	bool run_enabled = (m_process.state() == QProcess::NotRunning && item != nullptr);

	if (run_enabled && item != nullptr)
	{
		const auto* info = item->GetInfo();
		auto        dir  = info->basedir;
		run_enabled      = !dir.isEmpty() && QDir(dir).exists();
	}

	m_ui->pushButton_Run->setEnabled(run_enabled);
	m_ui->widget->SetRunEnabled(run_enabled);
}

#include "MainDialog.moc"
