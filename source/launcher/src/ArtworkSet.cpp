#include "ArtworkSet.h"

#include "ArtworkProvider.h"

#include <QQuickWindow>
#include <QSGTexture>
#include <QSet>

void ArtworkSet::Keep(const QStringList& ids, const QSize& size)
{
	if (size != m_size)
	{
		m_size = size;
		m_images.clear();
		m_generation++;
	}
	const QSet<QString> wanted(ids.begin(), ids.end());
	for (auto it = m_images.begin(); it != m_images.end();)
	{
		it = wanted.contains(it.key()) ? std::next(it) : m_images.erase(it);
	}
	for (const auto& id: wanted)
	{
		if (!id.isEmpty() && !m_images.contains(id))
		{
			Request(id);
		}
	}
}

void ArtworkSet::Request(const QString& id)
{
	// The null image marks the picture as asked for, so it is not asked for again while it loads.
	m_images.insert(id, QImage());
	auto* job = ArtworkLoader::Instance()->CreateJob(id, m_size);
	connect(
	    job, &ArtworkJob::Done, this,
	    [this, id, generation = m_generation](const QImage& image, const QString& error)
	    {
		    if (!error.isEmpty())
		    {
			    qWarning("Artwork: %s", qPrintable(error));
		    }
		    // Dropped while it loaded, or asked for at another size.
		    if (generation != m_generation || !m_images.contains(id) || image.isNull())
		    {
			    return;
		    }
		    m_images.insert(id, image);
		    emit Loaded();
	    },
	    Qt::QueuedConnection);
	ArtworkLoader::Instance()->Start(job);
}

ArtworkTextures::~ArtworkTextures()
{
	for (const auto& entry: std::as_const(m_entries))
	{
		delete entry.texture;
	}
}

QSGTexture* ArtworkTextures::Get(QQuickWindow* window, const QString& id, const QImage& image)
{
	if (image.isNull())
	{
		return nullptr;
	}
	auto& entry = m_entries[id];
	entry.used  = true;
	if (entry.texture == nullptr || entry.image_key != image.cacheKey())
	{
		delete entry.texture;
		entry.texture   = window->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
		entry.image_key = image.cacheKey();
	}
	return entry.texture;
}

void ArtworkTextures::Prune()
{
	for (auto it = m_entries.begin(); it != m_entries.end();)
	{
		if (!it->used)
		{
			delete it->texture;
			it = m_entries.erase(it);
			continue;
		}
		it->used = false;
		++it;
	}
}
