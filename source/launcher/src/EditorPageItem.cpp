#include "EditorPageItem.h"

#include "EditorFields.h"
#include "FieldFormItem.h"
#include "FocusButtonItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextPadItem.h"
#include "Theme.h"

#include <QKeyEvent>
#include <QQuickWindow>
#include <QSGTextNode>
#include <QTimer>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal HEADER_TOP     = 26;
constexpr qreal HEADER_HEIGHT  = 40;
constexpr qreal HEADER_SPACING = 6;
constexpr qreal TITLE_GAP      = 12;
constexpr qreal BUTTON_SPACING = 10;
constexpr qreal FORM_GAP       = 12;
constexpr qreal FORM_BOTTOM    = 8;

namespace {

class EditorNode: public GroupNode
{
public:
	int generation = -1;
};

PageLayout::Text TextAt(const QRectF& rect, const QString& text, const QFont& font, const QColor& color, QTextOption::WrapMode wrap)
{
	PageLayout::Text line;
	line.rect  = rect;
	line.text  = text;
	line.font  = font;
	line.color = color;
	line.wrap  = wrap;
	return line;
}

QVariantMap Hint(const QString& pad, const QString& key, const QString& text)
{
	return {{"pad", pad}, {"key", key}, {"text", text}};
}

} // namespace

EditorPageItem::EditorPageItem(QQuickItem* parent): QQuickItem(parent)
{
	setFlag(ItemHasContents);
	setFlag(ItemIsFocusScope);

	auto add_button = [this](const QString& text, bool primary, void (EditorPageItem::*action)())
	{
		auto* button = new FocusButtonItem(this);
		button->SetText(text);
		button->SetPrimary(primary);
		connect(button, &FocusButtonItem::Activated, this, action);
		return button;
	};
	m_save   = add_button(tr("Save"), true, &EditorPageItem::Save);
	m_test   = add_button(tr("Test run"), false, &EditorPageItem::TestRun);
	m_cancel = add_button(tr("Cancel"), false, &EditorPageItem::Cancelled);

	// The keyboard covers the whole page, so it is created after the form to draw over it.
	m_pad  = new TextPadItem;
	m_form = new FieldFormItem(m_pad, this);
	m_pad->setParentItem(this);
	m_pad->setParent(this);
	m_form->AddFields(EditorFields(FieldGroup::Game));
	// A game shows its emulator settings only while it has custom ones; otherwise the global ones apply.
	m_form->AddFields(EditorFields(FieldGroup::Display), true);
	m_form->AddFields(EditorFields(FieldGroup::Shaders), true);
	m_form->AddFields(EditorFields(FieldGroup::Logs), true);
	m_form->AddFields(EditorFields(FieldGroup::Modules));
	// The note under the title depends on whether the game has custom settings.
	connect(m_form, &FieldFormItem::Edited, this, &QQuickItem::polish);
	connect(m_form, &FieldFormItem::TopReached, this, [this]() { m_save->forceActiveFocus(Qt::OtherFocusReason); });
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);
}

QVariantList EditorPageItem::Hints() const
{
	return {Hint(QStringLiteral("△"), QStringLiteral("O"), tr("Save")), Hint(QStringLiteral("□"), QStringLiteral("E"), tr("Test run")),
	        Hint(QStringLiteral("○"), QStringLiteral("Esc"), tr("Cancel"))};
}

void EditorPageItem::OpenGame(int row, const QVariantMap& fields)
{
	auto values = fields;
	if (!values.value("basedir").toString().isEmpty())
	{
		values["elfs"] = LauncherBackend::Instance()->ScanElfs(values.value("basedir").toString(), values.value("elfs").toList());
	}
	m_row = row;
	m_error.clear();
	m_status.clear();
	m_pad->Close();
	m_form->SetValues(values);
	polish();
	// The page is shown after this returns; another form may have been scrolled, a new one starts at its top.
	QTimer::singleShot(0, m_form, &FieldFormItem::FocusFirst);
}

void EditorPageItem::Save()
{
	const auto result = LauncherBackend::Instance()->SaveGame(m_row, m_form->Values());
	m_error           = result.value("error").toString();
	polish();
	if (m_error.isEmpty())
	{
		emit Saved(result.value("row").toInt());
	}
}

void EditorPageItem::TestRun()
{
	m_error  = LauncherBackend::Instance()->TestGame(m_form->Values());
	m_status = m_error.isEmpty() ? tr("Test run started in a separate window. Nothing is saved.") : QString();
	polish();
}

QVector<FocusButtonItem*> EditorPageItem::HeaderButtons() const
{
	return {m_save, m_test, m_cancel};
}

bool EditorPageItem::HeaderFocused() const
{
	const auto header = HeaderButtons();
	return std::any_of(header.begin(), header.end(), [](const FocusButtonItem* button) { return button->hasActiveFocus(); });
}

bool EditorPageItem::MoveAcross(int delta)
{
	const auto header = HeaderButtons();
	const auto found  = std::find_if(header.begin(), header.end(), [](const FocusButtonItem* button) { return button->hasActiveFocus(); });
	if (found == header.end())
	{
		return false;
	}
	const auto index = std::distance(header.begin(), found) + delta;
	if (index >= 0 && index < header.size())
	{
		header.at(index)->forceActiveFocus(Qt::OtherFocusReason);
	}
	return true;
}

void EditorPageItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up:
			if (HeaderFocused())
			{
				return;
			}
			break;
		case Qt::Key_Down:
			if (HeaderFocused())
			{
				m_form->FocusFirst();
				return;
			}
			break;
		case Qt::Key_Left:
		case Qt::Key_Right:
			if (MoveAcross(event->key() == Qt::Key_Left ? -1 : 1))
			{
				return;
			}
			break;
		case Qt::Key_Escape: emit Cancelled(); return;
		case Qt::Key_O: Save(); return;
		case Qt::Key_E: TestRun(); return;
		default: break;
	}
	event->ignore();
}

void EditorPageItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		m_pad->setSize(new_geometry.size());
		polish();
	}
}

void EditorPageItem::updatePolish()
{
	const auto* theme  = Theme::Instance();
	const qreal u      = theme->U();
	const qreal margin = theme->Margin();
	const qreal inner  = std::max<qreal>(0, width() - 2 * margin);
	const qreal top    = HEADER_TOP * u;

	// The header's buttons sit at its right end, the title takes the rest of the line.
	qreal      left    = margin + inner;
	const auto buttons = HeaderButtons();
	for (auto it = buttons.rbegin(); it != buttons.rend(); ++it)
	{
		auto* button = *it;
		left -= button->implicitWidth();
		button->setPosition(QPointF(left, top));
		button->setSize(QSizeF(button->implicitWidth(), button->implicitHeight()));
		left -= BUTTON_SPACING * u;
	}
	const auto title = m_row < 0 ? tr("New game") : tr("Edit game");
	m_texts.clear();
	m_texts << TextAt(QRectF(margin, top, std::max<qreal>(0, left + BUTTON_SPACING * u - TITLE_GAP * u - margin), HEADER_HEIGHT * u), title,
	                  SceneFont(theme->FontTitle(), QFont::DemiBold), theme->TextPrimary(), QTextOption::NoWrap);

	qreal      y         = top + HEADER_HEIGHT * u;
	const auto small     = SceneFont(theme->FontSmall());
	auto       add_block = [&](const QString& text, const QColor& color)
	{
		if (text.isEmpty())
		{
			return;
		}
		y += HEADER_SPACING * u;
		const qreal height = TextBlockHeight(text, small, inner);
		m_texts << TextAt(QRectF(margin, y, inner, height), text, small, color, QTextOption::WordWrap);
		y += height;
	};
	if (!m_form->Values().value("custom_settings").toBool())
	{
		add_block(
		    tr("This game runs with the emulator settings from Settings. Turn on custom emulator settings to change them for this game "
			   "only."),
		    theme->TextMuted());
	}
	add_block(m_error, theme->Danger());
	add_block(m_status, theme->Ok());

	y += FORM_GAP * u;
	m_form->setPosition(QPointF(margin, y));
	m_form->setSize(QSizeF(inner, std::max<qreal>(0, height() - FORM_BOTTOM * u - y)));

	m_generation++;
	update();
}

QSGNode* EditorPageItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<EditorNode*>(old_node);
	if (node == nullptr)
	{
		node = new EditorNode;
	}
	if (node->generation == m_generation)
	{
		return node;
	}
	node->generation = m_generation;
	while (auto* child = node->firstChild())
	{
		node->removeChildNode(child);
		delete child;
	}
	for (const auto& text: m_texts)
	{
		auto* text_node = window()->createTextNode();
		if (text.wrap == QTextOption::NoWrap)
		{
			SetTextLine(text_node, text.text, text.font, text.color, text.rect);
		} else
		{
			SetTextBlock(text_node, text.text, text.font, text.color, text.rect, text.alignment, text.wrap);
		}
		node->appendChildNode(text_node);
	}
	return node;
}
