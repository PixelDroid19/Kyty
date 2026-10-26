#include "GameArtItem.h"

#include "ArtworkProvider.h"
#include "CoverNode.h"
#include "GameCard.h"
#include "SceneNodes.h"
#include "Theme.h"

#include <QQuickWindow>
#include <QSGTextNode>

#include <cmath>

namespace {

class ArtNode: public GroupNode
{
public:
	explicit ArtNode(QQuickWindow* window)
	{
		cover = new CoverNode;
		appendChildNode(cover);
		initials = window->createTextNode();
		cover->Overlay()->appendChildNode(initials);
	}

	ArtworkTextures textures;
	CoverNode*      cover    = nullptr;
	QSGTextNode*    initials = nullptr;
};

} // namespace

GameArtItem::GameArtItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	connect(&m_art, &ArtworkSet::Loaded, this, &QQuickItem::update);
}

void GameArtItem::SetGame(const QString& url, const QString& title)
{
	const auto id = ArtworkProvider::IdFromUrl(url);
	const auto rounded = id.isEmpty() ? QString() : QString(ArtworkProvider::ROUNDED_PREFIX) + id;
	if (rounded == m_id && title == m_title)
	{
		return;
	}
	m_id    = rounded;
	m_title = title;
	polish();
	update();
}

void GameArtItem::updatePolish()
{
	const qreal dpr = window() != nullptr ? window()->effectiveDevicePixelRatio() : 1;
	if (m_id.isEmpty() || width() <= 0 || height() <= 0)
	{
		m_art.Keep({}, {});
		return;
	}
	m_art.Keep({m_id}, QSize(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr))));
}

void GameArtItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		polish();
		update();
	}
}

QSGNode* GameArtItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<ArtNode*>(old_node);
	if (node == nullptr)
	{
		node = new ArtNode(window());
	}
	const auto* theme = Theme::Instance();
	const auto  art   = m_art.Find(m_id);

	CoverNode::Look look;
	look.rect = boundingRect();
	look.fill = theme->Surface();
	look.dpr  = window()->effectiveDevicePixelRatio();
	node->cover->Update(window(), look, node->textures.Get(window(), m_id, art));

	node->initials->clear();
	if (m_id.isEmpty())
	{
		SetTextLine(node->initials, Initials(m_title), SceneFont(height() * 0.3, QFont::DemiBold), theme->TextPrimary(), boundingRect(),
		            Qt::AlignHCenter);
	}
	node->textures.Prune();
	return node;
}
