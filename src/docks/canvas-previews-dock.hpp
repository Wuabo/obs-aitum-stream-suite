#pragma once

#include "../utils/widgets/qt-display.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <memory>
#include <vector>

class CanvasPreviewsDock : public QFrame {
	Q_OBJECT

	struct PreviewSlot {
		OBSWeakCanvas weakCanvas;
		bool isMain = false;
		QPointer<QWidget> container;
		QPointer<OBSQTDisplay> display;
		QPointer<QLabel> label;
	};

	QHBoxLayout *rowLayout = nullptr;
	std::vector<std::unique_ptr<PreviewSlot>> previewSlots;

	void ClearSlots();
	void AddMainSlot();
	void AddCanvasSlot(obs_canvas_t *canvas);
	static void RenderSlot(void *data, uint32_t cx, uint32_t cy);
	static bool EnumCanvas(void *param, obs_canvas_t *canvas);
	static void FrontendEvent(enum obs_frontend_event event, void *private_data);

public:
	explicit CanvasPreviewsDock(QWidget *parent = nullptr);
	~CanvasPreviewsDock() override;

public slots:
	void Rebuild();
};
