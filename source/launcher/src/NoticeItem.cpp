#include "NoticeItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QQuickWindow>
#include <QSGTextNode>

#include <cmath>

// Sizes in units.
constexpr qreal PAD_X        = 20;
constexpr qreal PAD_Y        = 10;
constexpr qreal PARENT_INSET = 80;

constexpr int SHOWN_MS = 4000;

namespace {

class NoticeNode: public GroupNode
{
public:
	explicit NoticeNode(QQuickWindow* window)
	{
		pill = new RoundedBoxNode;
		appendChildNode(pill);
		text = window->createTextNode();
		appendChildNode(text);
	}

	RoundedBoxNode* pill = nullptr;
	QSGTextNode*    text = nullptr;
};

QFont NoticeFont()
{
	return SceneFont(Theme::Instance()->FontBody());
}

} // namespace

NoticeItem::NoticeItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setVisible(false);
	m_hide_timer.setSingleShot(true);
	m_hide_timer.setInterval(SHOWN_MS);
	connect(&m_hide_timer, &QTimer::timeout, this, [this]() { Show(QString()); });
	connect(Theme::Instance(), &Theme::UChanged, this, &NoticeItem::Resize);
}

void NoticeItem::Show(const QString& text)
{
	m_text = text;
	setVisible(!text.isEmpty());
	if (text.isEmpty())
	{
		m_hide_timer.stop();
		return;
	}
	m_hide_timer.start();
	Resize();
}

void NoticeItem::Resize()
{
	if (m_text.isEmpty())
	{
		return;
	}
	const qreal u          = Theme::Instance()->U();
	const auto  font       = NoticeFont();
	const qreal max_width  = parentItem() != nullptr ? parentItem()->width() - PARENT_INSET * u : TextWidth(m_text, font) + 2 * PAD_X * u;
	const qreal pill_width = std::min(std::ceil(TextWidth(m_text, font)) + 2 * PAD_X * u, max_width);
	const qreal text_width = std::max<qreal>(0, pill_width - 2 * PAD_X * u);
	setImplicitSize(pill_width, TextBlockHeight(m_text, font, text_width) + 2 * PAD_Y * u);
	update();
}

void NoticeItem::itemChange(ItemChange change, const ItemChangeData& value)
{
	QQuickItem::itemChange(change, value);
	if (change != ItemParentHasChanged)
	{
		return;
	}
	disconnect(m_parent_width);
	if (value.item != nullptr)
	{
		m_parent_width = connect(value.item, &QQuickItem::widthChanged, this, &NoticeItem::Resize);
	}
	Resize();
}

QSGNode* NoticeItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<NoticeNode*>(old_node);
	if (node == nullptr)
	{
		node = new NoticeNode(window());
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();

	RoundedBoxNode::Style style;
	style.fill         = theme->GlassStrong();
	style.border       = theme->GlassBorder();
	style.border_width = 1;
	style.radius       = height() / 2;
	style.dpr          = window()->effectiveDevicePixelRatio();
	node->pill->Update(window(), boundingRect(), style);

	const QRectF text_rect(PAD_X * u, PAD_Y * u, std::max<qreal>(0, width() - 2 * PAD_X * u), height() - 2 * PAD_Y * u);
	SetTextBlock(node->text, m_text, NoticeFont(), theme->TextPrimary(), text_rect, Qt::AlignHCenter);
	return node;
}
