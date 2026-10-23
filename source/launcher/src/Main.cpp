#include "ArtworkProvider.h"
#include "LauncherBackend.h"
#include "LauncherWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QProcess>

// Set to "software" by the restart below; Qt Quick reads it to pick its CPU renderer.
constexpr char BACKEND_VARIABLE[] = "QT_QUICK_BACKEND";

namespace {

// The scene graph draws through the GPU API Qt picks for the platform, on whichever GPU the system offers.
// When none of them can start, the launcher starts again on Qt Quick's CPU renderer instead of staying blank.
void RestartOnCpuRenderer(const QString& error)
{
	if (qEnvironmentVariable(BACKEND_VARIABLE) == QLatin1String("software"))
	{
		qCritical("The launcher cannot draw its window: %s", qPrintable(error));
		QCoreApplication::exit(1);
		return;
	}
	qWarning("No GPU renderer could start (%s); restarting on the CPU renderer.", qPrintable(error));

	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(BACKEND_VARIABLE, "software");
	QProcess process;
	process.setProgram(QCoreApplication::applicationFilePath());
	process.setArguments(QCoreApplication::arguments().mid(1));
	process.setProcessEnvironment(environment);
	const bool restarted = process.startDetached();
	if (!restarted)
	{
		qCritical("The launcher could not restart itself: %s", qPrintable(process.errorString()));
	}
	QCoreApplication::exit(restarted ? 0 : 1);
}

} // namespace

int main(int argc, char* argv[])
{
	// A widget application, for the file dialogs; the window itself is Qt Quick.
	QApplication app(argc, argv);

	Theme           theme;
	ArtworkLoader   artwork;
	LauncherBackend backend;

	LauncherWindow window;
	// Connected before the window is first exposed, which is when the renderer starts.
	QObject::connect(
	    &window, &QQuickWindow::sceneGraphError, &app,
	    [](QQuickWindow::SceneGraphError /*kind*/, const QString& message) { RestartOnCpuRenderer(message); }, Qt::QueuedConnection);
	backend.AttachWindow(&window);
	window.show();

	return QApplication::exec();
}
