#include "FieldFormItem.h"

#include "FieldRowItem.h"
#include "LauncherBackend.h"
#include "TextFieldItem.h"
#include "TextPadItem.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QKeyEvent>

#include <algorithm>

// Sizes in units.
constexpr qreal ROW_SPACING    = 10;
constexpr qreal SECTION_GAP    = 22;
constexpr qreal SCROLLBAR_ROOM = 12;
constexpr qreal SHOW_MARGIN    = 16;
constexpr qreal WHEEL_STEP     = 48;

namespace {

// Editing any of these by hand leaves the chosen graphics preset.
const char* const PRESET_KEYS[] = {"screen_width",
                                   "screen_height",
                                   "render_resolution_mode",
                                   "render_resolution_scope",
                                   "render_resolution_width",
                                   "render_resolution_height",
                                   "presentation_filter",
                                   "texture_anisotropy"};

} // namespace

FieldFormItem::FieldFormItem(TextPadItem* pad, QQuickItem* parent): QQuickItem(parent), m_pad(pad)
{
	setFlag(ItemIsFocusScope);
	setClip(true);
	m_content = new QQuickItem(this);

	connect(m_pad, &TextPadItem::Edited, this, &FieldFormItem::PadEdited);
	connect(m_pad, &TextPadItem::ShownChanged, this,
	        [this]()
	        {
		        if (!m_pad->Shown() && RowFor(m_pad->Key()) != nullptr)
		        {
			        FocusKey(m_pad->Key());
		        }
	        });
	connect(&m_chooser, &FileChooser::Chosen, this, [this](const QString& path) { SetField(m_browse_key, path); });
	connect(&m_scroll, &ViewScroll::Moved, this, [this]() { m_content->setY(-m_scroll.Position()); });
	connect(Theme::Instance(), &Theme::UChanged, this, &QQuickItem::polish);
}

bool FieldFormItem::HasControls() const
{
	return !FocusItems().isEmpty();
}

void FieldFormItem::Clear()
{
	for (auto* row: m_rows)
	{
		// The row may be the one handling the key that cleared the form, so it goes once the event is over.
		row->setParentItem(nullptr);
		row->deleteLater();
	}
	m_rows.clear();
	m_custom_only.clear();
	polish();
}

void FieldFormItem::AddFields(const QVector<EditorField>& fields, bool custom_only)
{
	for (const auto& field: fields)
	{
		auto* row = new FieldRowItem(field, m_content);
		connect(row, &FieldRowItem::Edited, this, &FieldFormItem::SetField);
		connect(row, &FieldRowItem::PadRequested, this, &FieldFormItem::OpenPad);
		connect(row, &FieldRowItem::BrowseRequested, this, &FieldFormItem::Browse);
		m_rows << row;
		m_custom_only << custom_only;
	}
	polish();
}

void FieldFormItem::SetValues(const QVariantMap& values)
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

void FieldFormItem::SetField(const QString& key, const QVariant& value)
{
	auto* backend = LauncherBackend::Instance();
	if (key == QLatin1String("graphics_preset"))
	{
		auto values               = backend->GraphicsPreset(m_values, value.toString());
		values["graphics_preset"] = value;
		SetValues(values);
		emit Edited(key);
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
	emit Edited(key);
}

void FieldFormItem::OpenPad(const QString& key, const QString& title)
{
	m_pad->SetText(m_values.value(key).toString());
	m_pad->OpenFor(key, title);
}

void FieldFormItem::PadEdited(const QString& text)
{
	const auto  key = m_pad->Key();
	const auto* row = RowFor(key);
	if (row == nullptr)
	{
		return;
	}
	if (row->Field().type == FieldType::Number)
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

void FieldFormItem::Browse(const QString& key, bool folder)
{
	const auto* row = RowFor(key);
	if (row == nullptr)
	{
		return;
	}
	m_browse_key     = key;
	const auto title = QCoreApplication::translate("EditorPageItem", row->Field().label);
	if (folder)
	{
		m_chooser.ChooseFolder(title);
	} else
	{
		m_chooser.ChooseFile(title);
	}
}

QVector<QQuickItem*> FieldFormItem::FocusItems() const
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

FieldRowItem* FieldFormItem::RowFor(const QString& key) const
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

qreal FieldFormItem::ScrollLimit() const
{
	return std::max<qreal>(0, m_content->height() - height());
}

void FieldFormItem::FocusItem(QQuickItem* item)
{
	item->forceActiveFocus(Qt::OtherFocusReason);
	// A field shown or hidden by the last edit moves the ones below it.
	ensurePolished();
	const qreal  margin = SHOW_MARGIN * Theme::Instance()->U();
	const QRectF rect   = item->mapRectToItem(m_content, item->boundingRect());
	m_scroll.Show(rect.top() - margin, rect.bottom() + margin, height(), ScrollLimit(), true);
}

void FieldFormItem::ScrollToTop()
{
	ensurePolished();
	m_scroll.ScrollTo(0, ScrollLimit(), false);
}

void FieldFormItem::FocusFirst()
{
	ScrollToTop();
	const auto items = FocusItems();
	if (!items.isEmpty())
	{
		FocusItem(items.first());
	}
}

void FieldFormItem::FocusKey(const QString& key)
{
	const auto* row = RowFor(key);
	if (row != nullptr && row->Shown() && !row->FocusItems().isEmpty())
	{
		FocusItem(row->FocusItems().first());
		return;
	}
	FocusFirst();
}

void FieldFormItem::MoveFocus(int delta)
{
	const auto items = FocusItems();
	const auto found = std::find_if(items.begin(), items.end(), [](const QQuickItem* item) { return item->hasActiveFocus(); });
	if (found == items.end())
	{
		FocusFirst();
		return;
	}
	const auto index = std::distance(items.begin(), found);
	if (index == 0 && delta < 0)
	{
		emit TopReached();
		return;
	}
	FocusItem(items.at(std::clamp<qsizetype>(index + delta, 0, items.size() - 1)));
}

void FieldFormItem::keyPressEvent(QKeyEvent* event)
{
	switch (event->key())
	{
		case Qt::Key_Up: MoveFocus(-1); return;
		case Qt::Key_Down: MoveFocus(1); return;
		default: break;
	}
	event->ignore();
}

void FieldFormItem::wheelEvent(QWheelEvent* event)
{
	const qreal step  = WHEEL_STEP * Theme::Instance()->U();
	const qreal delta = !event->pixelDelta().isNull() ? event->pixelDelta().y() : event->angleDelta().y() / 120.0 * step;
	m_scroll.ScrollTo(m_scroll.Position() - delta, ScrollLimit(), false);
}

void FieldFormItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry)
{
	QQuickItem::geometryChange(new_geometry, old_geometry);
	if (new_geometry.size() != old_geometry.size())
	{
		polish();
	}
}

void FieldFormItem::updatePolish()
{
	const qreal u          = Theme::Instance()->U();
	const qreal form_width = std::max<qreal>(0, width() - SCROLLBAR_ROOM * u);
	qreal       y          = 0;
	for (auto* row: m_rows)
	{
		if (!row->Shown())
		{
			row->Layout(form_width);
			continue;
		}
		// A new section after other rows leaves room above its heading.
		if (y > 0)
		{
			y += (row->Field().heading != nullptr ? SECTION_GAP : ROW_SPACING) * u;
		}
		row->setPosition(QPointF(0, y));
		y += row->Layout(form_width);
	}
	m_content->setSize(QSizeF(form_width, y));
	m_scroll.ScrollTo(m_scroll.Position(), ScrollLimit(), false);
	m_content->setY(-m_scroll.Position());
}
