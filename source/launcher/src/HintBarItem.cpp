#include "HintBarItem.h"

#include "SceneNodes.h"
#include "Theme.h"

#include <QQuickWindow>
#include <QSGTextNode>
#include <QVariantMap>

#include <algorithm>

// Sizes in units.
constexpr qreal HINT_SPACING = 26;
constexpr qreal LABEL_GAP    = 8;
constexpr qreal GLYPH_MIN    = 26;
constexpr qreal GLYPH_PAD    = 12;
constexpr qreal GLYPH_FONT   = 14;
constexpr qreal KEY_PAD      = 16;
constexpr qreal KEY_HEIGHT   = 24;
constexpr qreal KEY_RADIUS   = 6;

namespace {

// The hints change only with the page or the controller, so the whole bar is rebuilt then.
class BarNode: public GroupNode
{
public:
	int layout = -1;
};

} // namespace

HintBarItem::HintBarItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	connect(Theme::Instance(), &Theme::UChanged, this,
	        [this]()
	        {
		        m_layout++;
		        update();
	        });
}

void HintBarItem::SetHints(const QVariantList& hints)
{
	if (hints == m_hints)
	{
		return;
	}
	m_hints = hints;
	m_layout++;
	update();
}

void HintBarItem::SetPad(bool pad)
{
	if (pad == m_pad)
	{
		return;
	}
	m_pad = pad;
	m_layout++;
	update();
}

void HintBarItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		m_layout++;
		update();
	}
}

QSGNode* HintBarItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<BarNode*>(old_node);
	if (node == nullptr)
	{
		node = new BarNode;
	}
	if (node->layout == m_layout)
	{
		return node;
	}
	node->layout = m_layout;
	while (auto* child = node->firstChild())
	{
		delete child;
	}

	const auto* theme      = Theme::Instance();
	const qreal u          = theme->U();
	const qreal dpr        = window()->effectiveDevicePixelRatio();
	const qreal middle     = height() / 2;
	const auto  glyph_font = SceneFont(GLYPH_FONT * u);
	const auto  key_font   = SceneFont(theme->FontSmall());
	const auto  label_font = SceneFont(theme->FontBody());
	qreal       x          = theme->Margin();
	for (const auto& value: std::as_const(m_hints))
	{
		const auto    hint  = value.toMap();
		const bool    glyph = m_pad && hint.contains("pad");
		const QString cap   = hint.value(glyph ? "pad" : "key").toString();
		const QString text  = hint.value("text").toString();
		const auto&   font  = glyph ? glyph_font : key_font;

		const qreal  cap_width = glyph ? std::max(GLYPH_MIN * u, TextWidth(cap, font) + GLYPH_PAD * u) : TextWidth(cap, font) + KEY_PAD * u;
		const qreal  cap_height = glyph ? cap_width : KEY_HEIGHT * u;
		const QRectF cap_rect(x, middle - cap_height / 2, cap_width, cap_height);

		auto*                 box = new RoundedBoxNode;
		RoundedBoxNode::Style style;
		style.fill         = glyph ? theme->GlassStrong() : theme->Glass();
		style.border       = theme->GlassBorder();
		style.border_width = 1;
		style.radius       = glyph ? cap_height / 2 : KEY_RADIUS * u;
		style.dpr          = dpr;
		box->Update(window(), cap_rect, style);
		node->appendChildNode(box);

		auto* cap_text = window()->createTextNode();
		SetTextLine(cap_text, cap, font, theme->TextPrimary(), cap_rect, Qt::AlignHCenter);
		node->appendChildNode(cap_text);

		const qreal label_x     = x + cap_width + LABEL_GAP * u;
		const qreal label_width = TextWidth(text, label_font);
		auto*       label       = window()->createTextNode();
		SetTextLine(label, text, label_font, theme->TextSecondary(), QRectF(label_x, 0, label_width + 1, height()));
		node->appendChildNode(label);

		x = label_x + label_width + HINT_SPACING * u;
	}
	return node;
}
