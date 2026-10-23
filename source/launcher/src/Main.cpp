#include "ArtworkProvider.h"
#include "LauncherBackend.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>

int main(int argc, char* argv[])
{
	QGuiApplication app(argc, argv);

	LauncherBackend backend;

	QQmlApplicationEngine engine;
	engine.addImageProvider(ArtworkProvider::ID, new ArtworkProvider);
	engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
	engine.loadFromModule("Kyty.Launcher", "Main");
	if (engine.rootObjects().isEmpty())
	{
		return 1;
	}

	if (auto* window = qobject_cast<QWindow*>(engine.rootObjects().first()))
	{
		backend.AttachWindow(window);
	}

	return QGuiApplication::exec();
}
