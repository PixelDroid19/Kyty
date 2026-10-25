#ifndef LAUNCHER_INCLUDE_ARTWORK_SET_H_
#define LAUNCHER_INCLUDE_ARTWORK_SET_H_

#include "Common.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

class QQuickWindow;
class QSGTexture;

// The decoded artwork a view shows now. Missing pictures are asked from the artwork loader; pictures the view no
// longer shows are dropped, so a long library keeps only about a screen of artwork in memory.
class ArtworkSet: public QObject
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(ArtworkSet);

signals:
	// A picture asked for by Keep() arrived.
	void Loaded();

public:
	explicit ArtworkSet(QObject* parent = nullptr): QObject(parent) {}
	~ArtworkSet() override = default;

	// Keeps the pictures of these image:// ids (the address without "image://art/") decoded to fill size, asks
	// for the missing ones and drops the rest. A new size drops every picture.
	void Keep(const QStringList& ids, const QSize& size);
	// The picture of id, or a null image while it loads or when it could not be loaded.
	[[nodiscard]] QImage Find(const QString& id) const { return m_images.value(id); }

private:
	void Request(const QString& id);

	QSize                  m_size;
	QHash<QString, QImage> m_images;
	// Results of jobs started for an older size are ignored.
	quint64 m_generation = 0;
};

// The textures of a view's artwork, uploaded once per decoded picture. Lives on the view's root node, on the
// render thread, and deletes its textures with it.
class ArtworkTextures
{
public:
	ArtworkTextures() = default;
	~ArtworkTextures();

	KYTY_QT_CLASS_NO_COPY(ArtworkTextures);

	// The texture of image, uploaded the first time it is asked for; null for a null image.
	[[nodiscard]] QSGTexture* Get(QQuickWindow* window, const QString& id, const QImage& image);
	// Deletes the textures not asked for since the last call.
	void Prune();

private:
	struct Entry
	{
		qint64      image_key = 0;
		QSGTexture* texture   = nullptr;
		bool        used      = false;
	};

	QHash<QString, Entry> m_entries;
};

#endif /* LAUNCHER_INCLUDE_ARTWORK_SET_H_ */
