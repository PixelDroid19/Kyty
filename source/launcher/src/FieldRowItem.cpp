#include "FieldRowItem.h"

#include "FieldControls.h"
#include "FocusButtonItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextFieldItem.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QFontMetricsF>
#include <QQuickWindow>
#include <QSGTextNode>

#include <algorithm>
#include <cmath>

// Sizes in units.
constexpr qreal LABEL_WIDTH     = 230;
constexpr qreal LABEL_GAP       = 16;
constexpr qreal CONTROL_HEIGHT  = 40;
constexpr qreal HEADING_GAP     = 6;
constexpr qreal BUTTON_GAP      = 8;
constexpr qreal TOGGLE_WIDTH    = 200;
constexpr qreal MODULE_HEIGHT   = 36;
constexpr qreal MODULE_SPACING  = 6;
constexpr qreal PARAM_SPACING   = 4;
constexpr qreal PARAM_KEY_WIDTH = 200;
constexpr qreal PARAM_GAP       = 12;

constexpr int MAX_PARAMS = 200;

namespace {

class RowNode: public GroupNode
{
public:
	int generation = -1;
};

QString Translated(const char* text)
{
	return QCoreApplication::translate("EditorPageItem", text);
}

QFont SmallFont()
{
	return SceneFont(Theme::Instance()->FontSmall());
}

PageLayout::Text Line(const QRectF& rect, const QString& text, const QFont& font, const QColor& color)
{
	PageLayout::Text line;
	line.rect  = rect;
	line.text  = text;
	line.font  = font;
	line.color = color;
	line.wrap  = QTextOption::NoWrap;
	return line;
}

} // namespace

FieldRowItem::FieldRowItem(const EditorField& field, QQuickItem* parent): QQuickItem(parent), m_field(field)
{
	setFlag(ItemHasContents);
	const QString key = field.key;
	switch (field.type)
	{
		case FieldType::Text:
		case FieldType::Path:
		case FieldType::Number:
			m_input = new TextFieldItem(this);
			connect(m_input, &TextFieldItem::PadRequested, this, [this, key]() { emit PadRequested(key, Translated(m_field.label)); });
			break;
		case FieldType::Toggle:
			m_toggle = new ToggleItem(this);
			connect(m_toggle, &FieldControlItem::Activated, this, [this, key]() { emit Edited(key, !m_value.toBool()); });
			break;
		case FieldType::Choice:
			m_choices = LauncherBackend::Instance()->Choices().value(field.choices).toStringList();
			m_choice  = new ChoiceItem(this);
			connect(m_choice, &FieldControlItem::Activated, this, &FieldRowItem::Step);
			break;
		case FieldType::Modules:
			m_button = new FocusButtonItem(this);
			m_button->SetText(tr("Rescan files"));
			connect(m_button, &FocusButtonItem::Activated, this,
			        [this, key]() { emit Edited(key, LauncherBackend::Instance()->ScanElfs(m_basedir, m_value.toList())); });
			break;
		case FieldType::Params: break;
	}

	if (field.type == FieldType::Text || field.type == FieldType::Path)
	{
		connect(m_input, &TextFieldItem::Edited, this, [this, key](const QString& text) { emit Edited(key, text); });
	}
	if (field.type == FieldType::Path)
	{
		m_button = new FocusButtonItem(this);
		m_button->SetText(tr("Browse…"));
		connect(m_button, &FocusButtonItem::Activated, this, [this, key]() { emit BrowseRequested(key, m_field.folder); });
	}
	if (field.type == FieldType::Number)
	{
		m_input->SetDigitsOnly(NUMBER_DIGITS);
		// An emptied field keeps the last number until a new one is typed, and shows it again once left.
		connect(m_input, &TextFieldItem::Edited, this,
		        [this, key](const QString& text)
		        {
			        if (!text.isEmpty())
			        {
				        emit Edited(key, text.toInt());
			        }
		        });
		connect(m_input, &QQuickItem::activeFocusChanged, this, &FieldRowItem::ShowValue);
		m_minus = new FocusButtonItem(this);
		m_minus->SetText(QStringLiteral("−10"));
		connect(m_minus, &FocusButtonItem::Activated, this, [this]() { Step(-10); });
		m_plus = new FocusButtonItem(this);
		m_plus->SetText(QStringLiteral("+10"));
		connect(m_plus, &FocusButtonItem::Activated, this, [this]() { Step(10); });
	}
}

QVector<QQuickItem*> FieldRowItem::FocusItems() const
{
	QVector<QQuickItem*> items;
	for (QQuickItem* item: {static_cast<QQuickItem*>(m_input), static_cast<QQuickItem*>(m_toggle), static_cast<QQuickItem*>(m_choice),
	                        static_cast<QQuickItem*>(m_minus), static_cast<QQuickItem*>(m_plus), static_cast<QQuickItem*>(m_button)})
	{
		if (item != nullptr)
		{
			items << item;
		}
	}
	for (auto* module: m_modules)
	{
		items << module;
	}
	return items;
}

void FieldRowItem::SetValues(const QVariantMap& values, bool shown)
{
	m_shown   = shown;
	m_value   = values.value(m_field.key);
	m_basedir = values.value("basedir").toString();
	setVisible(shown);
	ShowValue();
}

void FieldRowItem::ShowValue()
{
	switch (m_field.type)
	{
		case FieldType::Text:
		case FieldType::Path: m_input->SetText(m_value.toString()); break;
		case FieldType::Number:
			if (!m_input->hasActiveFocus())
			{
				m_input->SetText(m_value.toString());
			}
			break;
		case FieldType::Toggle: m_toggle->SetOn(m_value.toBool()); break;
		case FieldType::Choice: m_choice->SetValue(m_value.toString()); break;
		case FieldType::Modules: ShowModules(m_value.toList()); break;
		case FieldType::Params:
			// Read again only when the file changes, since every edit in the form passes through here.
			if (m_value.toString() != m_params_file)
			{
				m_params_file = m_value.toString();
				m_params =
				    m_params_file.isEmpty() ? QVariantList() : LauncherBackend::Instance()->ParamEntries(m_params_file).mid(0, MAX_PARAMS);
			}
			break;
	}
}

void FieldRowItem::ShowModules(const QVariantList& modules)
{
	while (m_modules.size() > modules.size())
	{
		// The row may be the one handling the key that changed the list, so it goes once the event is over.
		auto* module = m_modules.takeLast();
		module->setParentItem(nullptr);
		module->deleteLater();
	}
	while (m_modules.size() < modules.size())
	{
		const int index  = static_cast<int>(m_modules.size());
		auto*     module = new ModuleRowItem(this);
		connect(module, &FieldControlItem::Activated, this, [this, index]() { ToggleModule(index); });
		m_modules << module;
	}
	for (int i = 0; i < modules.size(); i++)
	{
		const auto module = modules.at(i).toMap();
		m_modules.at(i)->SetModule(module.value("path").toString(), module.value("present").toBool(), module.value("selected").toBool());
	}
}

void FieldRowItem::Step(int delta)
{
	if (m_field.type == FieldType::Number)
	{
		emit Edited(m_field.key, std::max(0, m_value.toInt() + delta));
		return;
	}
	if (m_choices.isEmpty())
	{
		return;
	}
	const auto count = m_choices.size();
	const auto index = std::max<qsizetype>(0, m_choices.indexOf(m_value.toString()));
	emit       Edited(m_field.key, m_choices.at((index + delta + count) % count));
}

void FieldRowItem::ToggleModule(int index)
{
	auto modules = m_value.toList();
	if (index >= modules.size() || !modules.at(index).toMap().value("present").toBool())
	{
		return;
	}
	auto module        = modules.at(index).toMap();
	module["selected"] = !module.value("selected").toBool();
	modules[index]     = module;
	emit Edited(m_field.key, modules);
}

qreal FieldRowItem::Layout(qreal width)
{
	if (!m_shown)
	{
		setSize(QSizeF(width, 0));
		return 0;
	}
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	m_texts.clear();

	qreal y = 0;
	if (m_field.heading != nullptr)
	{
		auto font = SceneFont(theme->FontSmall(), QFont::DemiBold);
		font.setCapitalization(QFont::AllUppercase);
		const qreal height = std::ceil(QFontMetricsF(font).height());
		m_texts << Line(QRectF(0, 0, width, height), Translated(m_field.heading), font, theme->TextMuted());
		y = height + HEADING_GAP * u;
	}
	m_texts << Line(QRectF(0, y, LABEL_WIDTH * u, CONTROL_HEIGHT * u), Translated(m_field.label), SceneFont(theme->FontBody()),
	                theme->TextSecondary());

	const qreal control_x = (LABEL_WIDTH + LABEL_GAP) * u;
	const qreal height =
	    y + std::max(CONTROL_HEIGHT * u, PlaceControl(QRectF(control_x, y, std::max<qreal>(0, width - control_x), CONTROL_HEIGHT * u)));
	m_generation++;
	setSize(QSizeF(width, height));
	update();
	return height;
}

qreal FieldRowItem::PlaceControl(const QRectF& rect)
{
	const auto* theme = Theme::Instance();
	const qreal u     = theme->U();
	const qreal gap   = BUTTON_GAP * u;

	// Buttons go to the right end of the control's space, at their own width; the text field takes the rest.
	qreal right = rect.right();
	for (auto* button: {m_plus, m_minus, m_button})
	{
		if (button == nullptr)
		{
			continue;
		}
		right -= button->implicitWidth();
		button->setPosition(QPointF(right, rect.top()));
		button->setSize(QSizeF(button->implicitWidth(), button->implicitHeight()));
		right -= gap;
	}
	if (m_input != nullptr)
	{
		m_input->setPosition(rect.topLeft());
		m_input->setSize(
		    QSizeF(std::max<qreal>(0, (m_button != nullptr || m_plus != nullptr ? right : rect.right()) - rect.left()), rect.height()));
	}
	if (m_toggle != nullptr)
	{
		m_toggle->setPosition(rect.topLeft());
		m_toggle->setSize(QSizeF(TOGGLE_WIDTH * u, rect.height()));
	}
	if (m_choice != nullptr)
	{
		m_choice->setPosition(rect.topLeft());
		m_choice->setSize(rect.size());
	}

	if (m_field.type == FieldType::Modules)
	{
		qreal y = rect.top() + m_button->implicitHeight() + MODULE_SPACING * u;
		if (m_modules.isEmpty())
		{
			auto note = Line(QRectF(), tr("No modules found yet. Set the game folder, then rescan."), SmallFont(), theme->TextMuted());
			note.wrap = QTextOption::WordWrap;
			note.rect = QRectF(rect.left(), y, rect.width(), TextBlockHeight(note.text, note.font, rect.width()));
			m_texts << note;
			return note.rect.bottom() - rect.top();
		}
		for (auto* module: m_modules)
		{
			module->setPosition(QPointF(rect.left(), y));
			module->setSize(QSizeF(rect.width(), MODULE_HEIGHT * u));
			y += (MODULE_HEIGHT + MODULE_SPACING) * u;
		}
		return y - MODULE_SPACING * u - rect.top();
	}

	if (m_field.type == FieldType::Params)
	{
		const auto  font        = SmallFont();
		const qreal line_height = std::ceil(QFontMetricsF(font).height());
		if (m_params.isEmpty())
		{
			m_texts << Line(QRectF(rect.left(), rect.top(), rect.width(), line_height), tr("No parameters file selected."), font,
			                theme->TextMuted());
			return line_height;
		}
		qreal       y         = rect.top();
		const qreal key_width = PARAM_KEY_WIDTH * u;
		const qreal value_x   = rect.left() + key_width + PARAM_GAP * u;
		for (const auto& param: m_params)
		{
			const auto entry = param.toMap();
			m_texts << Line(QRectF(rect.left(), y, key_width, line_height), entry.value("key").toString(), font, theme->TextMuted());
			m_texts << Line(QRectF(value_x, y, std::max<qreal>(0, rect.right() - value_x), line_height), entry.value("value").toString(),
			                font, theme->TextSecondary());
			y += line_height + PARAM_SPACING * u;
		}
		return y - PARAM_SPACING * u - rect.top();
	}
	return rect.height();
}

QSGNode* FieldRowItem::updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* /*data*/)
{
	auto* node = static_cast<RowNode*>(old_node);
	if (node == nullptr)
	{
		node = new RowNode;
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
