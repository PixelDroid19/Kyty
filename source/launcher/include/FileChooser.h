#ifndef LAUNCHER_INCLUDE_FILE_CHOOSER_H_
#define LAUNCHER_INCLUDE_FILE_CHOOSER_H_

#include "Common.h"

#include <QFileDialog>
#include <QObject>
#include <QPointer>
#include <QString>

// Picks a file or a folder with the platform's file dialog, without blocking the event loop; the launcher's
// window stays drawn but takes no input until the dialog closes.
class FileChooser: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(FileChooser);

signals:
	void Chosen(const QString& path);

public:
	explicit FileChooser(QObject* parent = nullptr);
	~FileChooser() override;

	void ChooseFile(const QString& title);
	void ChooseFolder(const QString& title);

private:
	void Open(const QString& title, QFileDialog::FileMode mode);

	QPointer<QFileDialog> m_dialog;
};

#endif /* LAUNCHER_INCLUDE_FILE_CHOOSER_H_ */
