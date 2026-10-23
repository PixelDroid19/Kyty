#include "ArtworkProvider.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <cmath>

// Artwork larger than this is refused instead of decoded; a PS5 pic0.png is about 16 MiB at 3840 x 2160.
constexpr qint64 MAX_ARTWORK_BYTES  = 32LL * 1024 * 1024;
constexpr int    MAX_ARTWORK_SIDE   = 8192;
constexpr int    MAX_CACHED_ARTWORK = 512;
// Requested sizes are rounded up to these long sides, so resizing the window does not decode the artwork again.
constexpr int MIN_BUCKET = 128;
constexpr int MAX_BUCKET = 1024;
// Corner radius of rounded artwork, as a share of its shorter side.
constexpr double CORNER_RADIUS = 0.08;

namespace {

QSize BucketSize(const QSize& requested)
{
	const int longest = std::max(requested.width(), requested.height());
	if (longest <= 0)
	{
		return {MAX_BUCKET / 2, MAX_BUCKET / 2};
	}
	int bucket = MIN_BUCKET;
	while (bucket < longest && bucket < MAX_BUCKET)
	{
		bucket *= 2;
	}
	const double factor = static_cast<double>(bucket) / longest;
	return {static_cast<int>(std::ceil(requested.width() * factor)), static_cast<int>(std::ceil(requested.height() * factor))};
}

QString CacheFile(const QString& cache_dir, const QFileInfo& info, const QSize& box, bool rounded)
{
	QCryptographicHash hash(QCryptographicHash::Sha1);
	hash.addData(info.absoluteFilePath().toUtf8());
	hash.addData(QByteArray::number(info.size()));
	hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
	hash.addData(QByteArray::number(box.width()) + "x" + QByteArray::number(box.height()));
	hash.addData(rounded ? "round" : "square");
	return QDir(cache_dir).filePath(QString::fromLatin1(hash.result().toHex()) + ".png");
}

// Cuts the image to the box's shape from its centre, as PreserveAspectCrop shows it, so the rounded corners
// land on the edges that are drawn.
QImage RoundCorners(const QImage& image, const QSize& box)
{
	const double ratio  = static_cast<double>(box.width()) / box.height();
	int          width  = image.width();
	int          height = std::max(1, qRound(width / ratio));
	if (height > image.height())
	{
		height = image.height();
		width  = std::max(1, qRound(height * ratio));
	}

	QImage rounded(width, height, QImage::Format_ARGB32_Premultiplied);
	rounded.fill(Qt::transparent);
	QPainter painter(&rounded);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(image.copy((image.width() - width) / 2, (image.height() - height) / 2, width, height));
	const double radius = CORNER_RADIUS * std::min(width, height);
	painter.drawRoundedRect(QRectF(0, 0, width, height), radius, radius);
	return rounded;
}

// Keeps the newest files; the rest belong to artwork that was replaced, removed or drawn at an old size.
void PruneCache(const QString& cache_dir)
{
	const auto files = QDir(cache_dir).entryInfoList({"*.png"}, QDir::Files, QDir::Time);
	for (int i = MAX_CACHED_ARTWORK; i < files.size(); i++)
	{
		QFile::remove(files.at(i).absoluteFilePath());
	}
}

ArtworkLoader* g_loader = nullptr;

} // namespace

ArtworkLoader::ArtworkLoader()
    : m_cache_dir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/Kyty/launcher-artwork")
{
	Q_ASSERT(g_loader == nullptr);
	g_loader = this;
	m_pool.setMaxThreadCount(1);
	m_pool.setThreadPriority(QThread::IdlePriority);
	QDir().mkpath(m_cache_dir);
	m_pool.start([dir = m_cache_dir] { PruneCache(dir); });
}

ArtworkLoader::~ArtworkLoader()
{
	m_pool.waitForDone();
	g_loader = nullptr;
}

ArtworkLoader* ArtworkLoader::Instance()
{
	Q_ASSERT(g_loader != nullptr);
	return g_loader;
}

ArtworkJob* ArtworkLoader::CreateJob(const QString& id, const QSize& size) const
{
	const auto prefix  = QLatin1String(ArtworkProvider::ROUNDED_PREFIX);
	const bool rounded = id.startsWith(prefix);
	const auto encoded = rounded ? id.mid(prefix.size()) : id;
	const auto path    = QString::fromUtf8(QByteArray::fromBase64(encoded.toLatin1(), QByteArray::Base64UrlEncoding));
	return new ArtworkJob(path, size, rounded, m_cache_dir);
}

void ArtworkLoader::Start(ArtworkJob* job)
{
	m_pool.start(job);
}

QString ArtworkProvider::Url(const QString& path)
{
	if (path.isEmpty())
	{
		return {};
	}
	const auto id = path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	return QString("image://%1/%2").arg(ID, QString::fromLatin1(id));
}

QString ArtworkProvider::IdFromUrl(const QString& url)
{
	const auto prefix = QString("image://%1/").arg(ID);
	return url.startsWith(prefix) ? url.mid(prefix.size()) : QString();
}

QQuickImageResponse* ArtworkProvider::requestImageResponse(const QString& id, const QSize& requested_size)
{
	auto* job      = m_loader->CreateJob(id, requested_size);
	auto* response = new ArtworkResponse(job);
	m_loader->Start(job);
	return response;
}

void ArtworkJob::run()
{
	const QFileInfo info(m_path);
	if (!info.isFile() || info.size() > MAX_ARTWORK_BYTES)
	{
		emit Done({}, QString("Artwork is missing or larger than %1 bytes: %2").arg(MAX_ARTWORK_BYTES).arg(m_path));
		return;
	}

	const auto box        = BucketSize(m_size);
	const auto cache_file = CacheFile(m_cache_dir, info, box, m_rounded);
	if (QImage cached(cache_file); !cached.isNull())
	{
		emit Done(cached, {});
		return;
	}

	QImageReader reader(m_path);
	const auto   original = reader.size();
	if (!original.isValid() || original.width() > MAX_ARTWORK_SIDE || original.height() > MAX_ARTWORK_SIDE)
	{
		emit Done({}, QString("Artwork has an unsupported size: %1").arg(m_path));
		return;
	}
	// Fills the box like PreserveAspectCrop does, never enlarging.
	const double scale = std::min(1.0, std::max(static_cast<double>(box.width()) / original.width(),
	                                            static_cast<double>(box.height()) / original.height()));
	reader.setScaledSize(QSize(std::max(1, qRound(original.width() * scale)), std::max(1, qRound(original.height() * scale))));

	QImage image = reader.read();
	if (image.isNull())
	{
		emit Done({}, reader.errorString());
		return;
	}
	if (m_rounded)
	{
		image = RoundCorners(image, box);
	}

	// A failed write only costs a decode next time, so the image is still shown.
	QSaveFile file(cache_file);
	if (file.open(QIODevice::WriteOnly) && image.save(&file, "PNG"))
	{
		file.commit();
	}
	emit Done(image, {});
}

ArtworkResponse::ArtworkResponse(ArtworkJob* job)
{
	connect(job, &ArtworkJob::Done, this,
	        [this](const QImage& image, const QString& error)
	        {
		        m_image = image;
		        m_error = error;
		        emit finished();
	        },
	        Qt::QueuedConnection);
}

QQuickTextureFactory* ArtworkResponse::textureFactory() const
{
	return QQuickTextureFactory::textureFactoryForImage(m_image);
}
