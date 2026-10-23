#ifndef LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_
#define LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_

#include "PageItem.h"

// The global emulator settings, the emulator program, the run mode, the controller and where the settings are
// stored.
class SettingsPageItem: public PageItem
{
	Q_OBJECT
	QML_NAMED_ELEMENT(SettingsPage)
	KYTY_QT_CLASS_NO_COPY(SettingsPageItem);

signals:
	// QML maps only signals that start in lower case to on... handlers.
	void emulatorSettingsRequested(); // NOLINT(readability-identifier-naming)
	// The user wants to pick the emulator program in a file dialog.
	void programRequested(); // NOLINT(readability-identifier-naming)

public:
	explicit SettingsPageItem(QQuickItem* parent = nullptr);
	~SettingsPageItem() override = default;

	[[nodiscard]] QVariantList Hints() const override;

protected:
	void Compose(PageLayout* layout) override;
	void Press(int id) override;

private:
	void AddHeading(PageLayout* layout, const QString& text) const;
	void AddSummary(PageLayout* layout) const;
	void AddProgram(PageLayout* layout) const;
};

#endif /* LAUNCHER_INCLUDE_SETTINGS_PAGE_ITEM_H_ */
