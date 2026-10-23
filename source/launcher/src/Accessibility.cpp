#include "Accessibility.h"

#include <QMetaType>
#include <QQmlEngine>
#include <QQuickItem>

// Qt Quick exposes an item to assistive technology through its attached Accessible object. Its class is private, so
// it is reached by name through the meta-object system: its properties then run Qt's own code, whatever its version.
static QObject* Attached(QQuickItem* item)
{
	static const auto attached_properties = []() -> QQmlAttachedPropertiesFunc
	{
		// Qt Quick registers its QML types when its module is first looked up, and the launcher loads no QML.
		qmlTypeId("QtQuick", 2, 0, "Accessible");
		const auto* meta = QMetaType::fromName("QQuickAccessibleAttached*").metaObject();
		if (meta == nullptr)
		{
			qWarning("Qt Quick has no Accessible attached type; screen readers will not see the launcher");
			return nullptr;
		}
		return qmlAttachedPropertiesFunction(nullptr, meta);
	}();
	return attached_properties == nullptr ? nullptr : qmlAttachedPropertiesObject(item, attached_properties, true);
}

void SetAccessible(QQuickItem* item, QAccessible::Role role, const QString& name, const QString& description)
{
	auto* attached = Attached(item);
	if (attached == nullptr)
	{
		return;
	}
	// Setting the role again would reset the states that come with it.
	if (attached->property("role").value<QAccessible::Role>() != role)
	{
		attached->setProperty("role", QVariant::fromValue(role));
	}
	attached->setProperty("name", name);
	attached->setProperty("description", description);
}

void SetAccessibleChecked(QQuickItem* item, bool checked)
{
	auto* attached = Attached(item);
	if (attached == nullptr)
	{
		return;
	}
	attached->setProperty("checkable", true);
	attached->setProperty("checked", checked);
}
