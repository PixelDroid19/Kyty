#include "EditorPageItem.h"

#include "FieldRowItem.h"
#include "FocusButtonItem.h"
#include "LauncherBackend.h"
#include "SceneNodes.h"
#include "TextFieldItem.h"
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
constexpr qreal SCROLLBAR_ROOM = 12;
constexpr qreal SHOW_MARGIN    = 16;
constexpr qreal WHEEL_STEP     = 48;

namespace {

class EditorNode: public GroupNode
{
public:
	int generation = -1;
};

bool Is(const QVariantMap& values, const char* key, const char* value)
{
	return values.value(key).toString() == QLatin1String(value);
}

const EditorField GAME_FIELDS[] = {
    {"name", QT_TRANSLATE_NOOP("EditorPageItem", "Name"), FieldType::Text, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "General")},
    {"folder", QT_TRANSLATE_NOOP("EditorPageItem", "Folder (group)"), FieldType::Text},
    {"basedir", QT_TRANSLATE_NOOP("EditorPageItem", "Game folder"), FieldType::Path, nullptr, true},
    {"param_file", QT_TRANSLATE_NOOP("EditorPageItem", "Parameters file"), FieldType::Path},
    {"custom_settings", QT_TRANSLATE_NOOP("EditorPageItem", "Custom emulator settings"), FieldType::Toggle},
};

const EditorField EMULATOR_FIELDS[] = {
    {"screen_width", QT_TRANSLATE_NOOP("EditorPageItem", "Screen width"), FieldType::Number, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Display")},
    {"screen_height", QT_TRANSLATE_NOOP("EditorPageItem", "Screen height"), FieldType::Number},
    {"graphics_preset", QT_TRANSLATE_NOOP("EditorPageItem", "Graphics preset"), FieldType::Choice, "graphics_preset"},
    {"render_resolution_mode", QT_TRANSLATE_NOOP("EditorPageItem", "Render resolution"), FieldType::Choice, "render_resolution_mode"},
    {"render_resolution_scope", QT_TRANSLATE_NOOP("EditorPageItem", "Scaled surfaces"), FieldType::Choice, "render_resolution_scope", false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"render_resolution_width", QT_TRANSLATE_NOOP("EditorPageItem", "Render width"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"render_resolution_height", QT_TRANSLATE_NOOP("EditorPageItem", "Render height"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "render_resolution_mode", "Fixed"); }},
    {"presentation_filter", QT_TRANSLATE_NOOP("EditorPageItem", "Presentation filter"), FieldType::Choice, "presentation_filter"},
    {"presentation_fsr_sharpness", QT_TRANSLATE_NOOP("EditorPageItem", "FSR sharpness (0–100)"), FieldType::Number, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "presentation_filter", "Fsr"); }},
    {"texture_anisotropy", QT_TRANSLATE_NOOP("EditorPageItem", "Texture anisotropy"), FieldType::Choice, "texture_anisotropy"},
    {"neo", QT_TRANSLATE_NOOP("EditorPageItem", "Neo mode"), FieldType::Toggle},

    {"shader_optimization_type", QT_TRANSLATE_NOOP("EditorPageItem", "Optimization"), FieldType::Choice, "shader_optimization_type", false,
	 nullptr, QT_TRANSLATE_NOOP("EditorPageItem", "Shaders and validation")},
    {"shader_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Shader validation"), FieldType::Toggle},
    {"vulkan_validation_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Vulkan validation"), FieldType::Toggle},
    {"spirv_debug_printf_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "SPIR-V debug printf"), FieldType::Toggle},

    {"printf_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Log output"), FieldType::Choice, "printf_direction", false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Logs and dumps")},
    {"printf_level", QT_TRANSLATE_NOOP("EditorPageItem", "Log level"), FieldType::Choice, "printf_level", false,
	 [](const QVariantMap& f) { return !Is(f, "printf_direction", "Silent"); }},
    {"printf_output_file", QT_TRANSLATE_NOOP("EditorPageItem", "Log file"), FieldType::Path, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "printf_direction", "File"); }},
    {"printf_output_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Log folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return Is(f, "printf_direction", "Directory"); }},
    {"shader_log_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Shader log"), FieldType::Choice, "shader_log_direction"},
    {"shader_log_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Shader log folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return Is(f, "shader_log_direction", "File"); }},
    {"command_buffer_dump_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Command buffer dump"), FieldType::Toggle},
    {"command_buffer_dump_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Dump folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return f.value("command_buffer_dump_enabled").toBool(); }},
    {"pipeline_dump_enabled", QT_TRANSLATE_NOOP("EditorPageItem", "Pipeline dump"), FieldType::Toggle},
    {"pipeline_dump_folder", QT_TRANSLATE_NOOP("EditorPageItem", "Pipeline dump folder"), FieldType::Path, nullptr, true,
	 [](const QVariantMap& f) { return f.value("pipeline_dump_enabled").toBool(); }},
    {"profiler_direction", QT_TRANSLATE_NOOP("EditorPageItem", "Profiler output"), FieldType::Choice, "profiler_direction"},
    {"profiler_output_file", QT_TRANSLATE_NOOP("EditorPageItem", "Profiler file"), FieldType::Path, nullptr, false,
	 [](const QVariantMap& f) { return Is(f, "profiler_direction", "File") || Is(f, "profiler_direction", "FileAndNetwork"); }},
};

const EditorField MODULE_FIELDS[] = {
    {"elfs", QT_TRANSLATE_NOOP("EditorPageItem", "ELF and PRX files"), FieldType::Modules, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Modules")},
    {"param_file", QT_TRANSLATE_NOOP("EditorPageItem", "param.sfo"), FieldType::Params, nullptr, false, nullptr,
	 QT_TRANSLATE_NOOP("EditorPageItem", "Parameters")},
};

// Editing any of these by hand leaves the chosen graphics preset.
const char* const PRESET_KEYS[] = {"screen_width",
                                   "screen_height",
                                   "render_resolution_mode",
                                   "render_resolution_scope",
                                   "render_resolution_width",
                                   "render_resolution_height",
                                   "presentation_filter",
                                   "texture_anisotropy"};

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
	m_save     = add_button(tr("Save"), true, &EditorPageItem::Save);
	m_test     = add_button(tr("Test run"), false, &EditorPageItem::TestRun);
	m_defaults = add_button(tr("Restore defaults"), false, &EditorPageItem::RestoreDefaults);
	m_cancel   = add_button(tr("Cancel"), false, &EditorPageItem::Cancelled);

	m_viewport = new QQuickItem(this);
	m_viewport->setClip(true);
	m_content = new QQuickItem(m_viewport);

	m_pad = new TextPadItem(this);
	connect(m_pad, &TextPadItem::Edited, this, &EditorPageItem::PadEdited);
	connect(m_pad, &TextPadItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_pad->Shown())
		        {
			        FocusKey(m_pad->Key());
		        }
	        });

	connect(&m_chooser, &FileChooser::Chosen, this, [this](const QString& path) { SetField(m_browse_key, path); });
	connect(&m_scroll, &ViewScroll::Moved, this, [this]() { m_content->setY(-m_scroll.Position()); });
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);
}

QVariantList EditorPageItem::Hints() const
{
	QVariantList hints {Hint(QStringLiteral("△"), QStringLiteral("O"), tr("Save"))};
	if (!m_global)
	{
		hints << Hint(QStringLiteral("□"), QStringLiteral("E"), tr("Test run"));
	}
	hints << Hint(QStringLiteral("○"), QStringLiteral("Esc"), tr("Cancel"));
	return hints;
}

void EditorPageItem::OpenGame(int row, const QVariantMap& fields)
{
	auto values = fields;
	if (!values.value("basedir").toString().isEmpty())
	{
		values["elfs"] = LauncherBackend::Instance()->ScanElfs(values.value("basedir").toString(), values.value("elfs").toList());
	}
	Open(false, row, values);
}

void EditorPageItem::OpenGlobal()
{
	Open(true, -1, LauncherBackend::Instance()->GlobalFields());
}

void EditorPageItem::Open(bool global, int row, const QVariantMap& values)
{
	const bool mode_changed = global != m_global || !m_rows_built;
	m_global                = global;
	m_row                   = row;
	m_error.clear();
	m_status.clear();
	m_pad->Close();
	m_test->setVisible(!global);
	m_defaults->setVisible(global);
	if (mode_changed)
	{
		BuildRows();
	}
	SetValues(values);
	emit ModeChanged();
	// The page is shown after this returns; another form may have been scrolled, a new one starts at its top.
	QTimer::singleShot(0, this, &EditorPageItem::FocusFirst);
}

void EditorPageItem::BuildRows()
{
	for (auto* row: m_rows)
	{
		row->setParentItem(nullptr);
		row->deleteLater();
	}
	m_rows.clear();
	m_custom_only.clear();
	if (!m_global)
	{
		for (const auto& field: GAME_FIELDS)
		{
			AddRow(field, false);
		}
	}
	// A game shows its emulator settings only while it has custom ones; otherwise the global ones apply.
	for (const auto& field: EMULATOR_FIELDS)
	{
		AddRow(field, !m_global);
	}
	if (!m_global)
	{
		for (const auto& field: MODULE_FIELDS)
		{
			AddRow(field, false);
		}
	}
	m_rows_built = true;
}

void EditorPageItem::AddRow(const EditorField& field, bool custom_only)
{
	auto* row = new FieldRowItem(field, m_content);
	connect(row, &FieldRowItem::Edited, this, &EditorPageItem::SetField);
	connect(row, &FieldRowItem::PadRequested, this, &EditorPageItem::OpenPad);
	connect(row, &FieldRowItem::BrowseRequested, this, &EditorPageItem::Browse);
	m_rows << row;
	m_custom_only << custom_only;
}

void EditorPageItem::SetValues(const QVariantMap& values)
{
	m_values          = values;
	const bool custom = values.value("custom_settings").toBool();
	for (int i = 0; i < m_rows.size(); i++)
	{
		const auto& field = m_rows.at(i)->Field();
		const bool  shown = (!m_custom_only.at(i) || custom) && (field.when == nullptr || field.when(values));
		m_rows.at(i)->SetValues(values, shown);
	}
	polish();
}

void EditorPageItem::SetField(const QString& key, const QVariant& value)
{
	auto* backend = LauncherBackend::Instance();
	if (key == QLatin1String("graphics_preset"))
	{
		auto values               = backend->GraphicsPreset(m_values, value.toString());
		values["graphics_preset"] = value;
		SetValues(values);
		return;
	}
	auto values = m_values;
	values[key] = value;
	if (std::any_of(std::begin(PRESET_KEYS), std::end(PRESET_KEYS),
	                [&](const char* preset_key) { return key == QLatin1String(preset_key); }))
	{
		values["graphics_preset"] = QStringLiteral("Custom");
	}
	if (key == QLatin1String("basedir"))
	{
		values["elfs"] = backend->ScanElfs(value.toString(), values.value("elfs").toList());
	}
	SetValues(values);
}

void EditorPageItem::OpenPad(const QString& key, const QString& title)
{
	m_pad->SetText(m_values.value(key).toString());
	m_pad->OpenFor(key, title);
}

void EditorPageItem::PadEdited(const QString& text)
{
	const auto  key = m_pad->Key();
	const auto* row = RowFor(key);
	if (row != nullptr && row->Field().type == FieldType::Number)
	{
		// Like typing in the field: the pad may empty it, but the value keeps the last number.
		if (!TextFieldItem::IsNumberText(text, NUMBER_DIGITS))
		{
			return;
		}
		m_pad->SetText(text);
		if (!text.isEmpty())
		{
			SetField(key, text.toInt());
		}
		return;
	}
	m_pad->SetText(text);
	SetField(key, text);
}

void EditorPageItem::Browse(const QString& key, bool folder)
{
	m_browse_key = key;
	if (folder)
	{
		m_chooser.ChooseFolder(tr("Choose the game folder"));
	} else
	{
		m_chooser.ChooseFile(tr("Choose a file"));
	}
}

void EditorPageItem::Save()
{
	auto* backend = LauncherBackend::Instance();
	if (m_global)
	{
		m_error = backend->SaveGlobal(m_values);
		polish();
		if (m_error.isEmpty())
		{
			emit Saved(-1);
		}
		return;
	}
	const auto result = backend->SaveGame(m_row, m_values);
	m_error           = result.value("error").toString();
	polish();
	if (m_error.isEmpty())
	{
		emit Saved(result.value("row").toInt());
	}
}

void EditorPageItem::TestRun()
{
	m_error  = LauncherBackend::Instance()->TestGame(m_values);
	m_status = m_error.isEmpty() ? tr("Test run started in a separate window. Nothing is saved.") : QString();
	polish();
}

void EditorPageItem::RestoreDefaults()
{
	m_error.clear();
	m_status = tr("Default settings loaded. Save to keep them.");
	SetValues(LauncherBackend::Instance()->FieldsFor(-1));
}

QVector<FocusButtonItem*> EditorPageItem::HeaderButtons() const
{
	return {m_save, m_global ? m_defaults : m_test, m_cancel};
}

QVector<QQuickItem*> EditorPageItem::FocusItems() const
{
	QVector<QQuickItem*> items;
	for (const auto* row: m_rows)
	{
		if (row->Shown())
		{
			items << row->FocusItems();
		}
	}
	return items;
}

FieldRowItem* EditorPageItem::RowFor(const QString& key) const
{
	for (auto* row: m_rows)
	{
		if (key == QLatin1String(row->Field().key))
		{
			return row;
		}
	}
	return nullptr;
}

qreal EditorPageItem::ScrollLimit() const
{
	return std::max<qreal>(0, m_content->height() - m_viewport->height());
}

void EditorPageItem::FocusItem(QQuickItem* item)
{
	item->forceActiveFocus(Qt::OtherFocusReason);
	if (item->parentItem() == this)
	{
		return;
	}
	// A field shown or hidden by the last edit moves the ones below it.
	ensurePolished();
	const qreal  margin = SHOW_MARGIN * Theme::Instance()->U();
	const QRectF rect   = item->mapRectToItem(m_content, item->boundingRect());
	m_scroll.Show(rect.top() - margin, rect.bottom() + margin, m_viewport->height(), ScrollLimit(), true);
}

void EditorPageItem::FocusFirst()
{
	ensurePolished();
	m_scroll.ScrollTo(0, ScrollLimit(), false);
	const auto items = FocusItems();
	FocusItem(items.isEmpty() ? m_save : items.first());
}

void EditorPageItem::FocusKey(const QString& key)
{
	for (auto* row: m_rows)
	{
		if (key == QLatin1String(row->Field().key) && row->Shown() && !row->FocusItems().isEmpty())
		{
			FocusItem(row->FocusItems().first());
			return;
		}
	}
	FocusFirst();
}

void EditorPageItem::MoveFocus(int delta)
{
	const auto header = HeaderButtons();
	const auto items  = FocusItems();
	if (std::any_of(header.begin(), header.end(), [](const FocusButtonItem* button) { return button->hasActiveFocus(); }))
	{
		if (delta > 0 && !items.isEmpty())
		{
			FocusItem(items.first());
		}
		return;
	}
	const auto found = std::find_if(items.begin(), items.end(), [](const QQuickItem* item) { return item->hasActiveFocus(); });
	if (found == items.end())
	{
		FocusFirst();
		return;
	}
	const auto index = std::distance(items.begin(), found);
	if (index == 0 && delta < 0)
	{
		FocusItem(m_save);
		return;
	}
	FocusItem(items.at(std::clamp<qsizetype>(index + delta, 0, items.size() - 1)));
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
		FocusItem(header.at(index));
	}
	return true;
}

void EditorPageItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up: MoveFocus(-1); return;
		case Qt::Key_Down: MoveFocus(1); return;
		case Qt::Key_Left:
		case Qt::Key_Right:
			if (MoveAcross(event->key() == Qt::Key_Left ? -1 : 1))
			{
				return;
			}
			break;
		case Qt::Key_Escape: emit Cancelled(); return;
		case Qt::Key_O: Save(); return;
		case Qt::Key_E:
			if (!m_global)
			{
				TestRun();
				return;
			}
			break;
		default: break;
	}
	event->ignore();
}

void EditorPageItem::wheelEvent(QWheelEvent* event)
{
	const qreal step  = WHEEL_STEP * Theme::Instance()->U();
	const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().y() : event->angleDelta().y() / 120.0 * step;
	m_scroll.ScrollTo(m_scroll.Position() - delta, ScrollLimit(), false);
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
	const auto title = m_global ? tr("Emulator settings") : m_row < 0 ? tr("New game") : tr("Edit game");
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
	if (m_global)
	{
		add_block(tr("Every game runs with these settings unless it has custom emulator settings of its own."), theme->TextMuted());
	} else if (!m_values.value("custom_settings").toBool())
	{
		add_block(
		    tr("This game runs with the emulator settings from Settings. Turn on custom emulator settings to change them for this game "
			   "only."),
		    theme->TextMuted());
	}
	add_block(m_error, theme->Danger());
	add_block(m_status, theme->Ok());

	y += FORM_GAP * u;
	m_viewport->setPosition(QPointF(margin, y));
	m_viewport->setSize(QSizeF(inner, std::max<qreal>(0, height() - FORM_BOTTOM * u - y)));
	const qreal form_width = std::max<qreal>(0, inner - SCROLLBAR_ROOM * u);
	qreal       row_y      = 0;
	for (auto* row: m_rows)
	{
		row->setPosition(QPointF(0, row_y));
		row_y += row->Layout(form_width);
	}
	m_content->setSize(QSizeF(form_width, row_y));
	m_scroll.ScrollTo(m_scroll.Position(), ScrollLimit(), false);
	m_content->setY(-m_scroll.Position());

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
