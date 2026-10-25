#ifndef LAUNCHER_INCLUDE_KIT_MASCOT_ITEM_H_
#define LAUNCHER_INCLUDE_KIT_MASCOT_ITEM_H_

#include "Common.h"

#include <QElapsedTimer>
#include <QImage>
#include <QQuickItem>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector>

// Kit, the project's mascot, built from the website's SVG layers and animated the same way: float, blink,
// red/cyan glitch slices, and petting with happy eyes, a purr shake and rising hearts. Its height follows its
// width. The loops run only while the theme is live, so a hidden launcher or a running game costs no frames.
class KitMascotItem: public QQuickItem
{
	Q_OBJECT
	KYTY_QT_CLASS_NO_COPY(KitMascotItem);

public:
	explicit KitMascotItem(QQuickItem* parent = nullptr);
	~KitMascotItem() override = default;

	// Whether Kit bobs up and down while idle.
	void SetFloating(bool floating);
	// Short jump with happy eyes, used when a game starts.
	void Hop();

protected:
	QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* data) override;
	void     updatePolish() override;
	void     geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
	void     itemChange(ItemChange change, const ItemChangeData& value) override;
	void     mousePressEvent(QMouseEvent* event) override;
	void     mouseMoveEvent(QMouseEvent* event) override;
	void     mouseReleaseEvent(QMouseEvent* event) override;
	void     mouseUngrabEvent() override;

private:
	struct Heart
	{
		qint64 start = 0;
		// In units of the 120 x 160 viewBox.
		qreal size = 0;
		qreal dx   = 0;
	};

	void Tick();
	// Runs the frame clock while anything moves, and stops it when nothing does.
	void UpdateClock();
	void StartPet();
	void StopPet();
	void SetPetted(bool petted);
	void SpawnHeart();

	[[nodiscard]] qint64 Now() const { return m_time.elapsed(); }
	[[nodiscard]] bool   Hopping() const;
	// How far the "purrr…" text has faded in, from 0 to 1.
	[[nodiscard]] qreal PurrLevel() const;

	bool           m_floating = true;
	bool           m_petted   = false;
	QElapsedTimer  m_time;
	// The time of the frame being drawn, in milliseconds of m_time.
	qint64         m_now        = 0;
	qint64         m_hop_start  = -1;
	qint64         m_pet_start  = 0;
	qint64         m_purr_start = -1;
	qreal          m_purr_from  = 0;
	QVector<Heart> m_hearts;
	// Its only job is to tick once per frame while something moves.
	QVariantAnimation m_clock;
	QTimer            m_heart_timer;
	QTimer            m_unpet_timer;
	QVector<QImage>   m_layers;
	QImage            m_heart;
	QSize             m_layer_px;
};

#endif /* LAUNCHER_INCLUDE_KIT_MASCOT_ITEM_H_ */
