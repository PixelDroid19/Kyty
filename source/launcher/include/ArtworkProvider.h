#ifndef LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_
#define LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_

#include "Common.h"

#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QQuickImageResponse>
#include <QRunnable>
#include <QSize>
#include <QString>
#include <QThreadPool>

// Serves game artwork scaled down to the size the launcher draws it, and keeps those small copies on disk.
// A 4K pic0.png is then decoded once instead of on every start, on one idle-priority thread that never
// competes with a running game.
class ArtworkProvider: public QQuickAsyncImageProvider
{
public:
	static constexpr char ID[] = "art";
	// Put before the encoded path, it asks for the artwork cut to the requested box with rounded corners.
	static constexpr char ROUNDED_PREFIX[] = "round/";

	ArtworkProvider();
	~ArtworkProvider() override = default;

	KYTY_QT_CLASS_NO_COPY(ArtworkProvider);

	// The image:// address QML uses for the artwork file at path; empty when path is empty.
	static QString Url(const QString& path);

	QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requested_size) override;

private:
	QThreadPool m_pool;
	QString     m_cache_dir;
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

class ArtworkResponse: public QQuickImageResponse
{
	Q_OBJECT

public:
	explicit ArtworkResponse(ArtworkJob* job);

	[[nodiscard]] QQuickTextureFactory* textureFactory() const override;
	[[nodiscard]] QString               errorString() const override { return m_error; }

private:
	QImage  m_image;
	QString m_error;
};

#endif /* LAUNCHER_INCLUDE_ARTWORK_PROVIDER_H_ */
