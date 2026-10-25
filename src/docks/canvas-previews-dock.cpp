#include "canvas-previews-dock.hpp"
#include "../utils/widgets/display-helpers.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMetaObject>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

#include <graphics/vec4.h>

CanvasPreviewsDock::CanvasPreviewsDock(QWidget *parent) : QFrame(parent)
{
	/* Accept whatever height DockState restored; never force the dock taller. */
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	setMinimumSize(0, 0);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(2, 2, 2, 2);
	outer->setSpacing(2);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto *rowHost = new QWidget(scroll);
	rowLayout = new QHBoxLayout(rowHost);
	rowLayout->setContentsMargins(0, 0, 0, 0);
	rowLayout->setSpacing(4);
	scroll->setWidget(rowHost);
	outer->addWidget(scroll, 1);

	obs_frontend_add_event_callback(FrontendEvent, this);
	Rebuild();
}

CanvasPreviewsDock::~CanvasPreviewsDock()
{
	obs_frontend_remove_event_callback(FrontendEvent, this);
	ClearSlots();
}

void CanvasPreviewsDock::FrontendEvent(enum obs_frontend_event event, void *private_data)
{
	auto *dock = static_cast<CanvasPreviewsDock *>(private_data);
	if (!dock) {
		return;
	}

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
	case OBS_FRONTEND_EVENT_CANVAS_ADDED:
	case OBS_FRONTEND_EVENT_CANVAS_REMOVED:
		QMetaObject::invokeMethod(dock, "Rebuild", Qt::QueuedConnection);
		break;
	default:
		break;
	}
}

void CanvasPreviewsDock::ClearSlots()
{
	for (auto &slot : previewSlots) {
		if (slot->display) {
			obs_display_t *disp = slot->display->GetDisplay();
			if (disp) {
				obs_display_remove_draw_callback(disp, RenderSlot, slot.get());
			}
		}
		if (slot->container) {
			rowLayout->removeWidget(slot->container);
			slot->container->deleteLater();
		}
	}
	previewSlots.clear();
}

void CanvasPreviewsDock::AddMainSlot()
{
	auto slot = std::make_unique<PreviewSlot>();
	slot->isMain = true;

	auto *container = new QWidget(this);
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto *label = new QLabel(QString::fromUtf8(obs_module_text("MainCanvas")), container);
	label->setProperty("class", QStringLiteral("label-preview-title"));
	layout->addWidget(label);

	auto *display = new OBSQTDisplay(container);
	/* Keep mins tiny so Rebuild cannot override the saved dock height. */
	display->setMinimumSize(24, 24);
	display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(display, 1);

	slot->container = container;
	slot->display = display;
	slot->label = label;

	PreviewSlot *raw = slot.get();
	connect(display, &OBSQTDisplay::DisplayCreated, this, [raw](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(), RenderSlot, raw);
	});

	rowLayout->addWidget(container, 1);
	previewSlots.push_back(std::move(slot));
}

void CanvasPreviewsDock::AddCanvasSlot(obs_canvas_t *canvas)
{
	if (!canvas) {
		return;
	}

	obs_canvas_t *main = obs_get_main_canvas();
	if (canvas == main) {
		obs_canvas_release(main);
		return;
	}
	obs_canvas_release(main);

	const uint32_t flags = obs_canvas_get_flags(canvas);
	if ((flags & EPHEMERAL) && !(flags & SCENE_REF) && !(flags & ACTIVATE)) {
		return;
	}

	auto slot = std::make_unique<PreviewSlot>();
	slot->weakCanvas = OBSGetWeakRef(canvas);

	auto *container = new QWidget(this);
	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	const char *name = obs_canvas_get_name(canvas);
	auto *label = new QLabel(QString::fromUtf8(name && *name ? name : "Canvas"), container);
	label->setProperty("class", QStringLiteral("label-preview-title"));
	layout->addWidget(label);

	auto *display = new OBSQTDisplay(container);
	display->setMinimumSize(24, 24);
	display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(display, 1);

	slot->container = container;
	slot->display = display;
	slot->label = label;

	PreviewSlot *raw = slot.get();
	connect(display, &OBSQTDisplay::DisplayCreated, this, [raw](OBSQTDisplay *window) {
		obs_display_add_draw_callback(window->GetDisplay(), RenderSlot, raw);
	});

	rowLayout->addWidget(container, 1);
	previewSlots.push_back(std::move(slot));
}

bool CanvasPreviewsDock::EnumCanvas(void *param, obs_canvas_t *canvas)
{
	auto *dock = static_cast<CanvasPreviewsDock *>(param);
	dock->AddCanvasSlot(canvas);
	return true;
}

void CanvasPreviewsDock::Rebuild()
{
	ClearSlots();
	AddMainSlot();
	obs_enum_canvases(EnumCanvas, this);
}

void CanvasPreviewsDock::RenderSlot(void *data, uint32_t cx, uint32_t cy)
{
	auto *slot = static_cast<PreviewSlot *>(data);
	if (!slot) {
		return;
	}

	uint32_t sourceCX = 1;
	uint32_t sourceCY = 1;
	obs_canvas_t *canvas = nullptr;

	if (slot->isMain) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			sourceCX = ovi.base_width ? ovi.base_width : 1;
			sourceCY = ovi.base_height ? ovi.base_height : 1;
		}
		canvas = obs_get_main_canvas();
	} else {
		OBSCanvas strong = OBSGetStrongRef(slot->weakCanvas);
		if (!strong) {
			return;
		}
		struct obs_video_info ovi;
		if (obs_canvas_get_video_info(strong, &ovi)) {
			sourceCX = ovi.base_width ? ovi.base_width : 1;
			sourceCY = ovi.base_height ? ovi.base_height : 1;
		}
		canvas = obs_canvas_get_ref(strong);
	}

	if (!canvas) {
		return;
	}

	int x, y;
	float scale;
	GetScaleAndCenterPos(int(sourceCX), int(sourceCY), int(cx), int(cy), x, y, scale);
	const float newCX = scale * float(sourceCX);
	const float newCY = scale * float(sourceCY);

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(float(-x), newCX + float(x), float(-y), newCY + float(y), -100.0f, 100.0f);
	gs_reset_viewport();

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(nullptr, 0, uint32_t(newCX), uint32_t(newCY));
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	const bool previous = gs_set_linear_srgb(true);
	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, int(newCX), int(newCY));
	obs_canvas_render(canvas);
	gs_set_linear_srgb(previous);

	gs_projection_pop();
	gs_viewport_pop();

	obs_canvas_release(canvas);
}
