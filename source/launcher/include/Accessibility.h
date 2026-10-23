#ifndef LAUNCHER_INCLUDE_ACCESSIBILITY_H_
#define LAUNCHER_INCLUDE_ACCESSIBILITY_H_

#include <QAccessible>
#include <QString>

class QQuickItem;

// Screen readers see the launcher's painted items through these: what an item is, what it shows now, and whether it is
// on. Call them on the GUI thread whenever the shown value changes.
void SetAccessible(QQuickItem* item, QAccessible::Role role, const QString& name, const QString& description = {});
void SetAccessibleChecked(QQuickItem* item, bool checked);

#endif /* LAUNCHER_INCLUDE_ACCESSIBILITY_H_ */
