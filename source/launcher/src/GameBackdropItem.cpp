#include "GameBackdropItem.h"

#include "ArtworkProvider.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QEasingCurve>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGTexture>
#include <QSGTransformNode>

#include <algorithm>
#include <cmath>

// The focus must rest this long before a picture is decoded.
constexpr int SETTLE_MS = 180;
// The new picture fades in while settling from a slight zoom, which reads as a camera move, not a cut.
constexpr int   DISSOLVE_MS = 900;
constexpr int   FADE_IN_MS  = 420;
constexpr qreal ZOOM_FROM   = 1.04;
constexpr int   VANISH_MS   = 250;

namespace {

// One picture in the scene: opacity, then the zoom around the centre, then the image when there is one.
struct LayerSlot
{
	QSGOpacityNode*   opacity   = nullptr;
	QSGTransformNode* transform = nullptr;
	QSGImageNode*     image     = nullptr;
	quint64           key       = 0;

	void Create(QSGNode* parent)
	{
		opacity   = new QSGOpacityNode;
		transform = new QSGTransformNode;
		opacity->appendChildNode(transform);
		parent->appendChildNode(opacity);
	}

	void SetTexture(QQuickWindow* window, QSGTexture* texture)
	{
		delete image;
		image = nullptr;
		if (texture != nullptr)
		{
			image = CreateTextureNode(window, texture, {});
			transform->appendChildNode(image);
		}
	}

	QSGTexture* TakeTexture()
	{
		auto* texture = image->texture();
		image->setOwnsTexture(false);
		delete image;
		image = nullptr;
		key   = 0;
		return texture;
	}

	// The covered slot takes the front slot's texture when the picture it shows is the one that was on top.
	void Assign(QQuickWindow* window, const GameBackdropItem::Layer& layer, LayerSlot* donor)
	{
		if (key == layer.key)
		{
			return;
		}
		QSGTexture* texture = nullptr;
		if (layer.key != 0 && donor != nullptr && donor->key == layer.key)
		{
			texture = donor->TakeTexture();
		} else if (layer.key != 0)
		{
			texture = window->createTextureFromImage(layer.image, layer.image.hasAlphaChannel() ? QQuickWindow::TextureHasAlphaChannel
			                                                                                    : QQuickWindow::CreateTextureOptions());
		}
		SetTexture(window, texture);
		key = layer.key;
	}

	// Fills bounds with the middle of the texture, as PreserveAspectCrop does, scaled around the centre.
	void Layout(const QRectF& bounds, const GameBackdropItem::Layer& layer)
	{
		opacity->setOpacity(layer.opacity);
		if (image == nullptr || bounds.isEmpty())
		{
			return;
		}
		const QSizeF texture = image->texture()->textureSize();
		const qreal  aspect  = bounds.width() / bounds.height();
		QSizeF       source  = texture;
		if (texture.width() / texture.height() > aspect)
		{
			source.setWidth(texture.height() * aspect);
		} else
		{
			source.setHeight(texture.width() / aspect);
		}
		image->setRect(bounds);
		image->setSourceRect(QRectF(QPointF((texture.width() - source.width()) / 2, (texture.height() - source.height()) / 2), source));

		QMatrix4x4 matrix;
		const auto centre = bounds.center();
		const auto scale  = static_cast<float>(layer.scale);
		matrix.translate(static_cast<float>(centre.x()), static_cast<float>(centre.y()));
		matrix.scale(scale, scale);
		matrix.translate(static_cast<float>(-centre.x()), static_cast<float>(-centre.y()));
		transform->setMatrix(matrix);
	}
};

class BackdropNode: public GroupNode
{
public:
	explicit BackdropNode(QQuickWindow* window)
	{
		covered.Create(this);
		front.Create(this);
		scrim_opacity              = new QSGOpacityNode;
		// The page background over the art, so the page's text keeps its contrast in either palette.
		QColor top    = Theme::Instance()->Background();
		QColor bottom = top;
		top.setAlphaF(0.78F);
		bottom.setAlphaF(0.93F);
		const QGradientStops stops = {{0.0, top}, {1.0, bottom}};
		scrim                      = CreateTextureNode(window, CreateGradientTexture(window, stops, Qt::Vertical), {});
		scrim_opacity->appendChildNode(scrim);
		appendChildNode(scrim_opacity);
	}

	LayerSlot       covered;
	LayerSlot       front;
	QSGOpacityNode* scrim_opacity = nullptr;
	QSGImageNode*   scrim         = nullptr;
};

} // namespace

GameBackdropItem::GameBackdropItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);

	m_settle.setSingleShot(true);
	m_settle.setInterval(SETTLE_MS);
	connect(&m_settle, &QTimer::timeout, this, &GameBackdropItem::Show);

	m_dissolve.setStartValue(0.0);
	m_dissolve.setEndValue(1.0);
	m_dissolve.setDuration(DISSOLVE_MS);
	connect(&m_dissolve, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { StepDissolve(value.toReal()); });
	connect(&m_dissolve, &QVariantAnimation::finished, this, &GameBackdropItem::FinishDissolve);

	m_vanish.setStartValue(1.0);
	m_vanish.setEndValue(0.0);
	m_vanish.setDuration(VANISH_MS);
	m_vanish.setEasingCurve(QEasingCurve::InQuad);
	connect(&m_vanish, &QVariantAnimation::valueChanged, this,
	        [this](const QVariant& value)
	        {
		        m_front.opacity = m_vanish_from * value.toReal();
		        update();
	        });
	connect(&m_vanish, &QVariantAnimation::finished, this, &GameBackdropItem::ReleaseFront);
}

void GameBackdropItem::SetPicture(const QString& picture)
{
	if (picture == m_picture)
	{
		return;
	}
	m_picture = picture;
	m_settle.start();
}

void GameBackdropItem::Show()
{
	if (m_front.url == m_picture && m_front.key != 0)
	{
		// Coming back to the picture that was fading out keeps it.
		if (m_vanish.state() == QAbstractAnimation::Running)
		{
			m_vanish.stop();
			m_front.opacity = 1;
			update();
		}
		return;
	}
	m_vanish.stop();
	// A dissolve still running ends at once, so its picture becomes the one to dissolve from.
	if (m_dissolve.state() == QAbstractAnimation::Running)
	{
		FinishDissolve();
	}
	if (m_picture.isEmpty())
	{
		if (m_front.key != 0)
		{
			m_vanish_from = m_front.opacity;
			m_vanish.start();
		}
		return;
	}

	const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	const QSize size(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr)));
	auto*       job = ArtworkLoader::Instance()->CreateJob(ArtworkProvider::IdFromUrl(m_picture), size);
	connect(
	    job, &ArtworkJob::Done, this,
	    [this, url = m_picture](const QImage& image, const QString& error)
	    {
		    if (!error.isEmpty())
		    {
			    qWarning("Backdrop artwork: %s", qPrintable(error));
		    }
		    Reveal(url, image);
	    },
	    Qt::QueuedConnection);
	ArtworkLoader::Instance()->Start(job);
}

void GameBackdropItem::Reveal(const QString& url, const QImage& image)
{
	// The focus moved on while this one decoded, or the same picture arrived twice.
	if (url != m_picture || image.isNull() || url == m_front.url)
	{
		return;
	}
	m_vanish.stop();
	if (m_dissolve.state() == QAbstractAnimation::Running)
	{
		FinishDissolve();
	}
	m_covered = m_front.key != 0 ? m_front : Layer();
	m_front   = {url, image, m_next_key++, 0, ZOOM_FROM};
	m_dissolve.start();
	update();
}

void GameBackdropItem::StepDissolve(qreal progress)
{
	const qreal        elapsed_ms = progress * DISSOLVE_MS;
	const QEasingCurve out_cubic(QEasingCurve::OutCubic);
	m_front.opacity = out_cubic.valueForProgress(std::min<qreal>(1, elapsed_ms / FADE_IN_MS));
	m_front.scale   = ZOOM_FROM - (ZOOM_FROM - 1) * out_cubic.valueForProgress(progress);
	update();
}

void GameBackdropItem::FinishDissolve()
{
	m_dissolve.stop();
	m_front.opacity = 1;
	m_front.scale   = 1;
	// The covered picture is released as soon as it cannot be seen.
	m_covered = Layer();
	update();
}

void GameBackdropItem::ReleaseFront()
{
	m_front = Layer();
	update();
}

void GameBackdropItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		update();
	}
}

QSGNode* GameBackdropItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<BackdropNode*>(old_node);
	if (node == nullptr)
	{
		node = new BackdropNode(window());
	}
	// The covered slot goes first, so it can take the texture the front slot is about to replace.
	node->covered.Assign(window(), m_covered, &node->front);
	node->front.Assign(window(), m_front, nullptr);

	const QRectF bounds = boundingRect();
	node->covered.Layout(bounds, m_covered);
	node->front.Layout(bounds, m_front);
	node->scrim->setRect(bounds);
	node->scrim_opacity->setOpacity(
	    std::max(node->covered.image != nullptr ? m_covered.opacity : 0, node->front.image != nullptr ? m_front.opacity : 0));
	return node;
}
