#ifndef LAUNCHER_INCLUDE_HINT_BAR_ITEM_H_
#define LAUNCHER_INCLUDE_HINT_BAR_ITEM_H_

#include "Common.h"

#include <QQuickItem>
#include <QVariantList>

// Bottom strip listing what the current page's buttons do: each hint is a key cap and its action. While a
// controller is connected, hints with a pad glyph show it instead of the key.
class HintBarItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(HintBarItem);

public:
	explicit HintBarItem(QQuickItem* parent = nullptr);
	~HintBarItem() override = default;

	// Each hint: { pad: "✕", key: "Enter", text: "Run" }; pad is optional.
	[[nodiscard]] QVariantList Hints() const { return m_hints; }
	void                       SetHints(const QVariantList& hints);
	[[nodiscard]] bool         Pad() const { return m_pad; }
	void                       SetPad(bool pad);

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

private:
	QVariantList m_hints;
	bool         m_pad = false;
	// Bumped whenever the hints must be laid out again.
	int m_layout = 0;
};

#endif /* LAUNCHER_INCLUDE_HINT_BAR_ITEM_H_ */
