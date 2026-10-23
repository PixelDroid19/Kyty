#ifndef LAUNCHER_INCLUDE_FILE_CHOOSER_H_
#define LAUNCHER_INCLUDE_FILE_CHOOSER_H_

#include "Common.h"

#include <QFileDialog>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Picks a file or a folder with the platform's file dialog, without blocking the event loop; the launcher's
// window stays drawn but takes no input until the dialog closes.
class FileChooser: public QObject
{
	Q_OBJECT
	QML_ELEMENT
	KYTY_QT_CLASS_NO_COPY(FileChooser);

signals:
	// QML maps only signals that start in lower case to on... handlers.
	void chosen(const QString& path); // NOLINT(readability-identifier-naming)

public:
	explicit FileChooser(QObject* parent = nullptr);
	~FileChooser() override;

	Q_INVOKABLE void chooseFile(const QString& title);   // NOLINT(readability-identifier-naming)
	Q_INVOKABLE void chooseFolder(const QString& title); // NOLINT(readability-identifier-naming)

private:
	void Open(const QString& title, QFileDialog::FileMode mode);

	QPointer<QFileDialog> m_dialog;
};

#endif /* LAUNCHER_INCLUDE_FILE_CHOOSER_H_ */
