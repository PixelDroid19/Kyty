#include "FileChooser.h"

#include <QGuiApplication>
#include <QWindow>

FileChooser::FileChooser(QObject* parent): QObject(parent) {}

FileChooser::~FileChooser()
{
	delete m_dialog;
}

void FileChooser::chooseFile(const QString& title)
{
	Open(title, QFileDialog::ExistingFile);
}

void FileChooser::chooseFolder(const QString& title)
{
	Open(title, QFileDialog::Directory);
}

void FileChooser::Open(const QString& title, QFileDialog::FileMode mode)
{
	if (m_dialog != nullptr)
	{
		m_dialog->raise();
		m_dialog->activateWindow();
		return;
	}
	auto* dialog = new QFileDialog(nullptr, title);
	m_dialog     = dialog;
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setFileMode(mode);
	if (mode == QFileDialog::Directory)
	{
		dialog->setOption(QFileDialog::ShowDirsOnly);
	}
	dialog->setWindowModality(Qt::ApplicationModal);
	// A Qt Quick window cannot parent a widget, so the dialog is tied to it as a transient window instead; that
	// keeps it above the launcher, the platform's own dialogs too.
	if (auto* parent = QGuiApplication::focusWindow())
	{
		dialog->winId();
		dialog->windowHandle()->setTransientParent(parent);
	}
	connect(dialog, &QFileDialog::fileSelected, this, &FileChooser::chosen);
	dialog->open();
}
