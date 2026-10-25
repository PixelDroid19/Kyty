#ifndef LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_
#define LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_

#include "Common.h"

#include <QImage>
#include <QRunnable>
#include <QSize>
#include <QString>
#include <QThreadPool>

class ArtworkJob;

// Loads game artwork scaled down to the size the launcher draws it, and keeps those small copies on disk.
// A 4K pic0.png is then decoded once instead of on every start, on one idle-priority thread that never
// competes with a running game. The scene items ask it for the artwork they draw.
class ArtworkLoader
{
public:
	ArtworkLoader();
	~ArtworkLoader();

	KYTY_QT_CLASS_NO_COPY(ArtworkLoader);

	// The one loader of the process, created before the UI.
	static ArtworkLoader* Instance();

	// A job for the artwork an image:// id names (the address without "image://art/"), sized to fill size.
	// Connect to its Done signal, then Start it; the job deletes itself after it runs.
	[[nodiscard]] ArtworkJob* CreateJob(const QString& id, const QSize& size) const;
	void                      Start(ArtworkJob* job);

private:
	QThreadPool m_pool;
	QString     m_cache_dir;
};

// The image:// addresses that name game artwork in the backend's game maps.
class ArtworkProvider
{
public:
	static constexpr char ID[] = "art";
	// Put before the encoded path, it asks for the artwork cut to the requested box with rounded corners.
	static constexpr char ROUNDED_PREFIX[] = "round/";

	// The image:// address of the artwork file at path; empty when path is empty.
	static QString Url(const QString& path);
	// The id inside an image:// address made by Url, or empty when url is not one.
	static QString IdFromUrl(const QString& url);
};

// Loads one artwork file on the provider's thread and reports the scaled image back.
class ArtworkJob: public QObject, public QRunnable
{
	Q_OBJECT

signals:
	void Done(const QImage& image, const QString& error);

public:
	ArtworkJob(QString path, QSize size, bool rounded, QString cache_dir)
	    : m_path(std::move(path)), m_size(size), m_rounded(rounded), m_cache_dir(std::move(cache_dir))
	{
	}

	void run() override;

private:
	QString m_path;
	QSize   m_size;
	bool    m_rounded;
	QString m_cache_dir;
};

#endif /* LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_ */
