#include "../dialogs/name-dialog.hpp"
#include "../dialogs/source-select-dialog.hpp"
#include "../utils/widgets/scene-tree.hpp"
#include "../utils/color.hpp"
#include "../utils/icon.hpp"
#include "../utils/widgets/focus-scroll-spinbox.hpp"
#include "../utils/widgets/source-toolbar.hpp"
#include "../utils/widgets/source-tree.hpp"
#include "canvas-dock.hpp"
#include "sources-dock.hpp"
#include <graphics/matrix4.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QAbstractItemDelegate>
#include <QComboBox>
#include <QDockWidget>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QToolBar>
#include <QUrlQuery>
#include <QWidgetAction>
#include <algorithm>
#include <cmath>
#include <src/utils/obs-websocket-api.h>
#include <util/dstr.h>
#include <util/platform.h>

#define HANDLE_RADIUS 4.0f
#define HELPER_ROT_BREAKPONT 45.0f
#define SPACER_LABEL_MARGIN 6.0f

std::list<CanvasDock *> canvas_docks;

extern obs_websocket_vendor vendor;

CanvasDock::CanvasDock(obs_data_t *settings_, QWidget *parent)
	: QFrame(parent),
	  preview(new OBSQTDisplay(this)),
	  settings(settings_),
	  eventFilter(BuildEventFilter())
{
	canvas_width = (uint32_t)obs_data_get_int(settings, "width");
	if (canvas_width < 1) {
		canvas_width = 1080;
	}
	canvas_height = (uint32_t)obs_data_get_int(settings, "height");
	if (canvas_height < 1) {
		canvas_height = 1920;
	}

	canvas_name = obs_data_get_string(settings, "name");
	if (canvas_name.empty()) {
		canvas_name = "Vertical";
	}

	canvas = obs_get_canvas_by_name(canvas_name.c_str());
	if (canvas && obs_canvas_removed(canvas)) {
		obs_canvas_release(canvas);
		canvas = nullptr;
	} else if (canvas && obs_canvas_get_flags(canvas) != PROGRAM) {
		obs_frontend_remove_canvas(canvas);
		obs_canvas_remove(canvas);
		obs_canvas_release(canvas);
		canvas = nullptr;
	}
	if (!canvas) {
		canvas = obs_get_canvas_by_uuid(obs_data_get_string(settings, "uuid"));
		if (canvas) {
			if (obs_canvas_removed(canvas)) {
				obs_canvas_release(canvas);
				canvas = nullptr;
			} else if (obs_canvas_get_flags(canvas) != PROGRAM) {
				obs_frontend_remove_canvas(canvas);
				obs_canvas_remove(canvas);
				obs_canvas_release(canvas);
				canvas = nullptr;
			} else {
				std::string name = obs_canvas_get_name(canvas);
				if (name != canvas_name) {
					obs_canvas_set_name(canvas, canvas_name.c_str());
				}
			}
		}
	}
	if (canvas) {
		obs_video_info ovi;
		if (!obs_canvas_get_video_info(canvas, &ovi) ||
		    (ovi.base_width != canvas_width || ovi.base_height != canvas_height || ovi.output_width != canvas_width ||
		     ovi.output_height != canvas_height)) {
			obs_get_video_info(&ovi);
			ovi.base_height = canvas_height;
			ovi.base_width = canvas_width;
			ovi.output_height = canvas_height;
			ovi.output_width = canvas_width;
			if (obs_canvas_reset_video(canvas, &ovi)) {
				blog(LOG_INFO, "[Aitum Stream Suite] Canvas '%s' reset video %ux%u", obs_canvas_get_name(canvas),
				     canvas_width, canvas_height);
			} else {
				blog(LOG_ERROR, "[Aitum Stream Suite] Failed to reset video on canvas '%s'",
				     obs_canvas_get_name(canvas));
			}
		}
		if (strcmp(obs_data_get_string(settings, "uuid"), "") == 0) {
			obs_data_set_string(settings, "uuid", obs_canvas_get_uuid(canvas));
		}
	} else {
		obs_video_info ovi;
		obs_get_video_info(&ovi);
		ovi.base_height = canvas_height;
		ovi.base_width = canvas_width;
		ovi.output_height = canvas_height;
		ovi.output_width = canvas_width;
		canvas = obs_frontend_add_canvas(canvas_name.c_str(), &ovi, PROGRAM);
		blog(LOG_INFO, "[Aitum Stream Suite] Add frontend canvas '%s' %ux%u", canvas_name.c_str(), canvas_width,
		     canvas_height);
		if (canvas) {
			obs_data_set_string(settings, "uuid", obs_canvas_get_uuid(canvas));
			obs_data_set_int(settings, "width", canvas_width);
			obs_data_set_int(settings, "height", canvas_height);
		}
	}
	if (canvas) {
		auto sh = obs_canvas_get_signal_handler(canvas);
		signal_handler_connect(sh, "source_add", source_add, this);
	}

	LoadUI();
}

CanvasDock::CanvasDock(const char *canvas_name_, QWidget *parent)
	: QFrame(parent),
	  preview(new OBSQTDisplay(this)),
	  eventFilter(BuildEventFilter()),
	  canvas_name(canvas_name_)
{
	LoadUI();
}

bool CanvasDock::save_undo_source_enum(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *p)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (obs_obj_is_private(source) && !obs_source_removed(source)) {
		return true;
	}

	obs_data_array_t *array = (obs_data_array_t *)p;

	/* check if the source is already stored in the array */
	const char *name = obs_source_get_name(source);
	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease sourceData = obs_data_array_item(array, i);
		if (strcmp(name, obs_data_get_string(sourceData, "name")) == 0) {
			return true;
		}
	}

	if (obs_source_is_group(source)) {
		obs_scene_enum_items(obs_group_from_source(source), save_undo_source_enum, p);
	}

	OBSDataAutoRelease source_data = obs_save_source(source);
	obs_data_array_push_back(array, source_data);
	return true;
}

static bool undoing = false;

void CanvasDock::undo_redo_scene(const char *json)
{
	OBSDataAutoRelease base = obs_data_create_from_json(json);
	OBSDataArrayAutoRelease array = obs_data_get_array(base, "array");
	std::vector<OBSSource> sources;

	/* create missing sources */
	const size_t count = obs_data_array_count(array);
	sources.reserve(count);

	undoing = true;

	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease data = obs_data_array_item(array, i);
		const char *name = obs_data_get_string(data, "name");
		const char *canvas_uuid = obs_data_get_string(data, "canvas_uuid");
		obs_canvas_t *canvas = nullptr;
		if (canvas_uuid && canvas_uuid[0] != '\0') {
			canvas = obs_get_canvas_by_uuid(canvas_uuid);
		}

		OBSSourceAutoRelease source = canvas ? obs_canvas_get_source_by_name(canvas, name) : obs_get_source_by_name(name);
		obs_canvas_release(canvas);
		if (!source) {
			source = obs_load_source(data);
		}

		sources.push_back(source.Get());

		/* update scene/group settings to restore their
					* contents to their saved settings */
		obs_scene_t *scene = obs_group_or_scene_from_source(source);
		if (scene) {
			OBSDataAutoRelease scene_settings = obs_data_get_obj(data, "settings");
			obs_source_update(source, scene_settings);
		}
	}

	/* actually load sources now */
	for (obs_source_t *source : sources) {
		obs_source_load2(source);
	}

	undoing = false;
}

std::string CanvasDock::backup_scene(obs_scene_t *scene)
{
	OBSDataArrayAutoRelease backup_array = obs_data_array_create();
	obs_scene_enum_items(scene, save_undo_source_enum, backup_array);
	auto scene_source = obs_scene_get_source(scene);
	OBSDataAutoRelease undo_scene_data = obs_save_source(scene_source);
	obs_data_array_push_back(backup_array, undo_scene_data);
	OBSDataAutoRelease backup_data = obs_data_create();
	obs_data_set_array(backup_data, "array", backup_array);
	return obs_data_get_json(backup_data);
}

static void StyleDockToolbar(QToolBar *toolbar)
{
	if (!toolbar)
		return;
	/* Match OBSBasic.ui scenesToolbar / sourcesToolbar. No widget
	 * stylesheet: Yami's "OBSDock QToolBar QToolButton" supplies the
	 * metrics, and the icon-* class supplies the themed icon. */
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);
	toolbar->setMovable(false);
	toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
}

/* Same path rule as OBSBasicStatusBar::GetIcon — never check availableSizes()
 * on SVG icons (Qt reports empty sizes even when theme:Dark/*.svg is valid),
 * or we fall back to :/res black glyphs and colors flip after the first polish. */
static QIcon SuiteThemeIcon(const char *fileName, const char *fallbackRes = nullptr)
{
	if (obs_frontend_is_theme_dark()) {
		return QIcon(QStringLiteral("theme:Dark/") + QString::fromUtf8(fileName));
	}
	if (fallbackRes && *fallbackRes) {
		return QIcon(QString::fromUtf8(fallbackRes));
	}
	return QIcon(QStringLiteral(":/res/images/") + QString::fromUtf8(fileName));
}

static void SetToolbarClass(QToolBar *toolbar, QAction *action, const char *cls)
{
	if (!action)
		return;
	action->setProperty("class", QString::fromUtf8(cls));
	QWidget *button = toolbar ? toolbar->widgetForAction(action) : nullptr;
	if (!button)
		return;
	button->setProperty("class", QString::fromUtf8(cls));
	button->style()->unpolish(button);
	button->style()->polish(button);
}

void CanvasDock::LoadUI()
{
	setAcceptDrops(true);
	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	box = gs_render_save();

	obs_leave_graphics();

	/* Do NOT name the dock root "contextContainer" — Yami styles that
	 * object and would pad every child button (incl. zoom). */
	setObjectName(QStringLiteral("CanvasDock"));
	/* Match Main Canvas centralwidget (QWidget): no QFrame chrome. */
	setFrameShape(QFrame::NoFrame);
	setFrameShadow(QFrame::Plain);
	setLineWidth(0);
	setMidLineWidth(0);
	setContentsMargins(0, 0, 0, 0);
	/* Yami's "OBSDock > QWidget" draws a grey4 box around dock content —
	 * that bottom stroke sits under the context bar. Main Canvas reads
	 * flush; kill only the border on this root (not a broad stylesheet). */
	setStyleSheet(QStringLiteral("QFrame#CanvasDock {"
				     "  border: none;"
				     "  border-radius: 0px;"
				     "}"));

	/* Match OBSBasic.ui centralwidget verticalLayout: margins 4,0,4,0. */
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(4, 0, 4, 0);
	outer->setSpacing(0);

	/* Match canvasEditor + previewLayout: margins 1 all, spacing 2. */
	auto *canvasEditor = new QWidget(this);
	canvasEditor->setObjectName(QStringLiteral("canvasEditor"));
	auto *previewLayout = new QHBoxLayout(canvasEditor);
	previewLayout->setContentsMargins(1, 1, 1, 1);
	previewLayout->setSpacing(2);

	canvas_split = new SwitchingSplitter(canvasEditor);
	canvas_split->setContentsMargins(0, 0, 0, 0);
	canvas_split->setOrientation(Qt::Vertical);
	previewLayout->addWidget(canvas_split);
	outer->addWidget(canvasEditor, 1);

	auto canvas_preview = new QWidget;
	canvas_preview->setObjectName(QStringLiteral("previewContainer"));
	auto cwl = new QVBoxLayout(canvas_preview);
	cwl->setContentsMargins(0, 0, 0, 0);
	cwl->setSpacing(0);
	if (!settings) {
		auto canvas_combo_layout = new QHBoxLayout;
		sceneCombo = new QComboBox;
		connect(sceneCombo, &QComboBox::currentTextChanged, [this]() { SwitchScene(sceneCombo->currentText()); });
		canvas_combo_layout->addWidget(sceneCombo);

		auto addButton = new QPushButton;
		addButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		addButton->setIcon(QIcon(":/res/images/add.png"));
		addButton->setProperty("themeID", "addIconSmall");
		addButton->setProperty("class", "icon-plus");
		addButton->setProperty("toolButton", true);
		addButton->setFlat(false);
		connect(addButton, &QPushButton::clicked, [this] { AddScene("", true); });
		canvas_combo_layout->addWidget(addButton);

		auto removeButton = new QPushButton();
		removeButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		removeButton->setIcon(QIcon(":/res/images/list_remove.png"));
		removeButton->setProperty("themeID", "removeIconSmall");
		removeButton->setProperty("class", "icon-minus");
		removeButton->setProperty("toolButton", true);
		removeButton->setFlat(false);
		connect(removeButton, &QPushButton::clicked, [this] {
			auto source = obs_canvas_get_source_by_name(canvas, sceneCombo->currentText().toUtf8().constData());
			if (!source) {
				return;
			}
			QMessageBox mb(QMessageBox::Question,
				       QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Title")),
				       QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Text"))
					       .arg(QString::fromUtf8(obs_source_get_name(source))),
				       QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No));
			mb.setDefaultButton(QMessageBox::NoButton);
			if (mb.exec() == QMessageBox::Yes) {
				obs_source_remove(source);
			}
			obs_source_release(source);
		});
		canvas_combo_layout->addWidget(removeButton);

		auto propsButton = new QPushButton();
		propsButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		propsButton->setIcon(QIcon(":/settings/images/settings/general.svg"));
		propsButton->setProperty("themeID", "propertiesIconSmall");
		propsButton->setProperty("class", "icon-gear");
		propsButton->setProperty("toolButton", true);
		propsButton->setFlat(false);
		connect(propsButton, &QPushButton::clicked, [this] {
			auto source = obs_get_source_by_name(sceneCombo->currentText().toUtf8().constData());
			if (!source) {
				return;
			}
			obs_frontend_open_source_properties(source);
		});
		canvas_combo_layout->addWidget(propsButton);

		cwl->addLayout(canvas_combo_layout);
	}
	preview->setObjectName(QStringLiteral("preview"));
	preview->setMinimumSize(QSize(32, 32));
	QSizePolicy sizePolicy1(QSizePolicy::Expanding, QSizePolicy::Expanding);
	sizePolicy1.setHorizontalStretch(0);
	sizePolicy1.setVerticalStretch(0);
	sizePolicy1.setHeightForWidth(preview->sizePolicy().hasHeightForWidth());
	preview->setSizePolicy(sizePolicy1);
	preview->setMouseTracking(true);
	preview->setFocusPolicy(Qt::ClickFocus);
	preview->installEventFilter(eventFilter.get());
	preview->show();
	connect(preview, &OBSQTDisplay::DisplayCreated,
		[this]() { obs_display_add_draw_callback(preview->GetDisplay(), DrawPreview, this); });

	previewDisabledWidget = new QFrame;
	previewDisabledWidget->setObjectName(QStringLiteral("previewDisabledWidget"));
	auto lv = new QVBoxLayout;
	lv->setContentsMargins(1, 1, 1, 1);
	auto enablePreviewButton =
		new QPushButton(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.PreviewConextMenu.Enable")));
	connect(enablePreviewButton, &QPushButton::clicked, [this] {
		preview_disabled = false;
		obs_display_set_enabled(preview->GetDisplay(), true);
		preview->setVisible(true);
		previewDisabledWidget->setVisible(false);
	});
	lv->addWidget(enablePreviewButton);
	previewDisabledWidget->setLayout(lv);
	previewDisabledWidget->setVisible(preview_disabled);

	/* Scale row inside preview container; context bar is a sibling of canvasEditor. */
	BuildPreviewChrome(cwl, outer);
	canvas_split->addWidget(canvas_preview);

	connect(preview, &OBSQTDisplay::DisplayResized, this, [this]() { ResizePreview(); });

	//obs_display_set_enabled(preview->GetDisplay(), !preview_disabled);

	auto addNudge = [this](const QKeySequence &seq, MoveDir direction, int distance) {
		QAction *nudge = new QAction(preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		preview->addAction(nudge);
		connect(nudge, &QAction::triggered, [this, distance, direction]() { Nudge(distance, direction); });
	};

	addNudge(Qt::Key_Up, MoveDir::Up, 1);
	addNudge(Qt::Key_Down, MoveDir::Down, 1);
	addNudge(Qt::Key_Left, MoveDir::Left, 1);
	addNudge(Qt::Key_Right, MoveDir::Right, 1);
	addNudge(Qt::SHIFT | Qt::Key_Up, MoveDir::Up, 10);
	addNudge(Qt::SHIFT | Qt::Key_Down, MoveDir::Down, 10);
	addNudge(Qt::SHIFT | Qt::Key_Left, MoveDir::Left, 10);
	addNudge(Qt::SHIFT | Qt::Key_Right, MoveDir::Right, 10);

	QAction *deleteAction = new QAction(preview);
	connect(deleteAction, &QAction::triggered, [this]() { RemoveSelectedSources(scene, this); });
#ifdef __APPLE__
	deleteAction->setShortcut({Qt::Key_Backspace});
#else
	deleteAction->setShortcut({Qt::Key_Delete});
#endif
	deleteAction->setShortcutContext(Qt::WidgetShortcut);
	preview->addAction(deleteAction);

	if (settings) {
		panel_split = new SwitchingSplitter;
		panel_split->setObjectName(QStringLiteral("panelSplit"));
		panel_split->setContentsMargins(0, 0, 0, 0);
		/* Plain widget, not a group box: a stylesheet on the group was
		 * blocking the Yami OBSDock toolbar metrics on the YouTube tab. */
		scenesGroup = new QWidget();
		scenesGroup->setObjectName(QStringLiteral("scenesGroup"));
		scenesGroup->setContentsMargins(0, 0, 0, 0);
		auto scenesGroupLayout = new QVBoxLayout();
		scenesGroupLayout->setContentsMargins(0, 0, 0, 0);
		scenesGroupLayout->setSpacing(0);
		scenesGroup->setLayout(scenesGroupLayout);
		sceneList = new SceneTree();
		sceneList->setObjectName(QString::fromUtf8("scenes"));
		sceneList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
		sceneList->setSpacing(0);
		sceneList->setFrameShape(QFrame::NoFrame);
		sceneList->setFrameShadow(QFrame::Plain);
		sceneList->setSelectionMode(QAbstractItemView::SingleSelection);
		sceneList->setViewMode(QListView::ListMode);
		//sceneList->setResizeMode(QListView::Fixed);
		sceneList->setSelectionMode(QAbstractItemView::SingleSelection);
		sceneList->setDragEnabled(true);
		sceneList->setDragDropMode(QAbstractItemView::InternalMove);
		sceneList->setDefaultDropAction(Qt::TargetMoveAction);
		sceneList->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(sceneList->model(), &QAbstractItemModel::rowsMoved, this, [] { obs_frontend_save(); });
		connect(sceneList, &QListWidget::customContextMenuRequested,
			[this](const QPoint &pos) { ShowScenesContextMenu(sceneList->itemAt(pos)); });
		connect(sceneList->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this](QWidget *editor) {
			auto *edit = qobject_cast<QLineEdit *>(editor);
			auto *listItem = sceneList->currentItem();
			if (!edit || !listItem || !canvas)
				return;
			std::string text = edit->text().trimmed().toUtf8().constData();
			OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, currentSceneName.toUtf8().constData());
			if (!source)
				source = obs_canvas_get_source_by_name(canvas, listItem->text().toUtf8().constData());
			if (!source)
				return;
			const char *prevName = obs_source_get_name(source);
			if (text == prevName)
				return;
			OBSSourceAutoRelease found = obs_get_source_by_name(text.c_str());
			if (!found)
				found = obs_canvas_get_source_by_name(canvas, text.c_str());
			if (found || text.empty()) {
				listItem->setText(QString::fromUtf8(prevName));
				if (found) {
					QMessageBox::warning(this, QString::fromUtf8(obs_frontend_get_locale_string("NameExists.Title")),
							     QString::fromUtf8(obs_frontend_get_locale_string("NameExists.Text")));
				} else {
					QMessageBox::warning(this,
							     QString::fromUtf8(obs_frontend_get_locale_string("NoNameEntered.Title")),
							     QString::fromUtf8(obs_frontend_get_locale_string("NoNameEntered.Text")));
				}
				return;
			}
			listItem->setText(QString::fromUtf8(text.c_str()));
			obs_source_set_name(source, text.c_str());
		});

		connect(sceneList, &QListWidget::currentItemChanged, [this]() {
			const auto item = sceneList->currentItem();
			if (!item) {
				return;
			}
			SwitchScene(item->text());
			if (!item->isSelected()) {
				item->setSelected(true);
			}
		});
		connect(sceneList, &QListWidget::itemSelectionChanged, [this] {
			const auto item = sceneList->currentItem();
			if (!item) {
				return;
			}
			if (!item->isSelected()) {
				item->setSelected(true);
			}
		});

		QAction *renameAction = new QAction(sceneList);
#ifdef __APPLE__
		renameAction->setShortcut({Qt::Key_Return});
#else
		renameAction->setShortcut({Qt::Key_F2});
#endif
		renameAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		connect(renameAction, &QAction::triggered, this, &CanvasDock::EditSceneName);
		sceneList->addAction(renameAction);

		scenesGroupLayout->addWidget(sceneList, 1);

		auto toolbar = new QToolBar();
		/* Unique name — must not collide with OBS scenesToolbar. */
		toolbar->setObjectName(QStringLiteral("AitumCanvasScenesToolbar"));
		toolbar->setIconSize(QSize(16, 16));
		toolbar->setFloatable(false);
		toolbar->setMovable(false);
		auto a = toolbar->addAction(SuiteThemeIcon("plus.svg"),
					    QString::fromUtf8(obs_frontend_get_locale_string("Add")), [this] { AddScene(); });
		a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("AddScene")));
		SetToolbarClass(toolbar, a, "icon-plus");

		a = toolbar->addAction(SuiteThemeIcon("trash.svg"),
				       QString::fromUtf8(obs_frontend_get_locale_string("Remove")), [this] {
					       auto item = sceneList->currentItem();
					       if (!item) {
						       return;
					       }
					       RemoveScene(item->text());
				       });
		a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("RemoveScene")));
		SetToolbarClass(toolbar, a, "icon-trash");
		a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
#ifdef __APPLE__
		a->setShortcut({Qt::Key_Backspace});
#else
		a->setShortcut({Qt::Key_Delete});
#endif
		sceneList->addAction(a);
		toolbar->addSeparator();
		a = toolbar->addAction(SuiteThemeIcon("filter.svg"),
				       QString::fromUtf8(obs_frontend_get_locale_string("SceneFilters")), [this] {
					       auto item = sceneList->currentItem();
					       if (!item) {
						       return;
					       }
					       auto s = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
					       if (!s) {
						       return;
					       }
					       obs_frontend_open_source_filters(s);
					       obs_source_release(s);
				       });
		a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("SceneFilters")));
		SetToolbarClass(toolbar, a, "icon-filter");
		toolbar->addSeparator();
		a = toolbar->addAction(SuiteThemeIcon("up.svg"), QString::fromUtf8(obs_frontend_get_locale_string("MoveUp")),
				       [this] { ChangeSceneIndex(true, -1, 0); });
		a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSceneUp")));
		SetToolbarClass(toolbar, a, "icon-up");
		a = toolbar->addAction(SuiteThemeIcon("down.svg"),
				       QString::fromUtf8(obs_frontend_get_locale_string("MoveDown")),
				       [this] { ChangeSceneIndex(true, 1, sceneList->count() - 1); });
		a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSceneDown")));
		SetToolbarClass(toolbar, a, "icon-down");
		StyleDockToolbar(toolbar);
		scenesGroupLayout->addWidget(toolbar);

		panel_split->addWidget(scenesGroup);
	}
	auto sourcesGroup = new QGroupBox(QString::fromUtf8(obs_module_text("Sources")));
	sourcesGroup->setObjectName(QStringLiteral("sourcesGroup"));
	sourcesGroup->setContentsMargins(0, 0, 0, 0);
	auto sourcesGroupLayout = new QVBoxLayout();
	sourcesGroupLayout->setContentsMargins(0, 0, 0, 0);
	sourcesGroup->setLayout(sourcesGroupLayout);
	sourceList = new SourceTree(
		&selectMutex, &hoveredPreviewItems, [](void *param) { return ((CanvasDock *)param)->scene; }, this, this, this);
	sourceList->setContentsMargins(0, 0, 0, 0);
	sourceList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	sourceList->setFrameShape(QFrame::NoFrame);
	sourceList->setFrameShadow(QFrame::Plain);
	sourceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	sourceList->setContextMenuPolicy(Qt::CustomContextMenu);

	sourceList->setDropIndicatorShown(true);
	sourceList->setDragEnabled(true);
	sourceList->setDragDropMode(QAbstractItemView::InternalMove);
	sourceList->setDefaultDropAction(Qt::TargetMoveAction);

	connect(sourceList, &SourceTree::customContextMenuRequested, [this] { ShowSourcesContextMenu(GetCurrentSceneItem()); });
	connect(sourceList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
		[this](const QItemSelection &, const QItemSelection &) { UpdateContextBar(); });

	auto renameAction = new QAction(sourceList);
#ifdef __APPLE__
	renameAction->setShortcut({Qt::Key_Return});
#else
	renameAction->setShortcut({Qt::Key_F2});
#endif
	renameAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameAction, &QAction::triggered, [this]() { sourceList->Edit(GetTopSelectedSourceItem()); });
	sourceList->addAction(renameAction);

	sourcesGroupLayout->addWidget(sourceList);

	auto toolbar = new QToolBar();
	toolbar->setContentsMargins(0, 0, 0, 0);
	/* Unique name — must not collide with OBS sourcesToolbar. */
	toolbar->setObjectName(QStringLiteral("AitumCanvasSourcesToolbar"));
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);
	auto a = toolbar->addAction(SuiteThemeIcon("plus.svg"), QString::fromUtf8(obs_frontend_get_locale_string("Add")),
				    [this] { OpenSourceSelectDialog(this, scene); });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("AddSource")));
	SetToolbarClass(toolbar, a, "icon-plus");

	a = toolbar->addAction(SuiteThemeIcon("trash.svg"), QString::fromUtf8(obs_frontend_get_locale_string("Remove")),
			       [this] { RemoveSelectedSources(scene, this); });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("RemoveSource")));
	SetToolbarClass(toolbar, a, "icon-trash");
	a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
#ifdef __APPLE__
	a->setShortcut({Qt::Key_Backspace});
#else
	a->setShortcut({Qt::Key_Delete});
#endif
	sourceList->addAction(a);
	toolbar->addSeparator();
	a = toolbar->addAction(SuiteThemeIcon("settings/general.svg", ":/settings/images/settings/general.svg"),
			       QString::fromUtf8(obs_frontend_get_locale_string("Properties")), [this] {
				       auto item = GetCurrentSceneItem();
				       auto source = obs_sceneitem_get_source(item);
				       if (source) {
					       obs_frontend_open_source_properties(source);
				       }
			       });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("SourceProperties")));
	SetToolbarClass(toolbar, a, "icon-gear");

	toolbar->addSeparator();
	a = toolbar->addAction(SuiteThemeIcon("up.svg"), QString::fromUtf8(obs_frontend_get_locale_string("MoveUp")), [this] {
		auto item = GetCurrentSceneItem();
		if (!item) {
			return;
		}
		auto scene = obs_sceneitem_get_scene(item);
		if (!scene) {
			return;
		}
		std::string undo_json = backup_scene(scene);
		obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
		std::string redo_json = backup_scene(scene);
		auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveUp"))
					.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))),
					     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
		obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
						  undo_json.c_str(), redo_json.c_str(), false);
	});
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSourceUp")));
	SetToolbarClass(toolbar, a, "icon-up");
	a = toolbar->addAction(SuiteThemeIcon("down.svg"), QString::fromUtf8(obs_frontend_get_locale_string("MoveDown")),
			       [this] {
				       auto item = GetCurrentSceneItem();
				       if (!item) {
					       return;
				       }
				       auto scene = obs_sceneitem_get_scene(item);
				       if (!scene) {
					       return;
				       }
				       std::string undo_json = backup_scene(scene);
				       obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
				       std::string redo_json = backup_scene(scene);
				       auto undoName =
					       QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveDown"))
						       .arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))),
							    QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
				       obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene,
									 undo_redo_scene, undo_json.c_str(), redo_json.c_str(),
									 false);
			       });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSourceDown")));
	SetToolbarClass(toolbar, a, "icon-down");

	StyleDockToolbar(toolbar);
	sourcesGroupLayout->addWidget(toolbar);

	if (settings) {
		panel_split->addWidget(sourcesGroup);
	} else {
		canvas_split->addWidget(sourcesGroup);
	}

	if (settings) {

		transitionsGroup = new QWidget();
		transitionsGroup->setObjectName(QStringLiteral("transitionsGroup"));
		transitionsGroup->setContentsMargins(0, 0, 0, 0);
		auto transitionsGroupLayout = new QVBoxLayout();
		transitionsGroupLayout->setContentsMargins(0, 0, 0, 0);
		transitionsGroupLayout->setSpacing(0);
		transitionsGroup->setLayout(transitionsGroupLayout);

		transition = new QComboBox();
		transition->setMinimumWidth(120);
		transition->setAccessibleName(QString::fromUtf8(obs_frontend_get_locale_string("Transition")));
		transitionsGroupLayout->addWidget(transition);

		auto durationRow = new QHBoxLayout();
		durationRow->setSpacing(4);
		auto durationLabel = new QLabel(QString::fromUtf8(obs_frontend_get_locale_string("Basic.TransitionDuration")));
		durationLabel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
		transitionDurationBox = new QSpinBox();
		transitionDurationBox->setSuffix(QString::fromUtf8(" ms"));
		transitionDurationBox->setMinimum(50);
		transitionDurationBox->setMaximum(20000);
		transitionDurationBox->setSingleStep(50);
		transitionDurationBox->setValue(transitionDurationMs);
		durationLabel->setBuddy(transitionDurationBox);
		durationRow->addWidget(durationLabel);
		durationRow->addWidget(transitionDurationBox, 1);
		transitionsGroupLayout->addLayout(durationRow);
		connect(transitionDurationBox, qOverload<int>(&QSpinBox::valueChanged), this,
			[this](int value) { transitionDurationMs = value; });

		auto hl = new QHBoxLayout();
		hl->setSpacing(4);
		hl->addStretch();
		auto addButton = new QPushButton();
		addButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		addButton->setAccessibleName(QString::fromUtf8(obs_frontend_get_locale_string("Basic.AddTransition")));
		addButton->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("Basic.AddTransition")));
		addButton->setIcon(QIcon(QString::fromUtf8(":/res/images/plus.svg")));
		addButton->setProperty("class", QString::fromUtf8("btn-tool icon-plus"));
		addButton->setFlat(false);
		addButton->style()->unpolish(addButton);
		addButton->style()->polish(addButton);

		connect(addButton, &QPushButton::clicked, [this] {
			auto menu = QMenu(this);
			auto subMenu = menu.addMenu(QString::fromUtf8(obs_module_text("CopyFromMain")));
			struct obs_frontend_source_list frontend_transitions = {};
			obs_frontend_get_transitions(&frontend_transitions);
			for (size_t i = 0; i < frontend_transitions.sources.num; i++) {
				auto tr = frontend_transitions.sources.array[i];
				const char *name = obs_source_get_name(tr);
				auto action = subMenu->addAction(QString::fromUtf8(name));
				if (!obs_is_source_configurable(obs_source_get_id(tr))) {
					action->setEnabled(false);
				}
				for (auto t : transitions) {
					if (strcmp(name, obs_source_get_name(t)) == 0) {
						action->setEnabled(false);
						break;
					}
				}
				connect(action, &QAction::triggered, [this, tr] {
					OBSDataAutoRelease d = obs_save_source(tr);
					OBSSourceAutoRelease t = obs_load_private_source(d);
					if (t) {
						transitions.emplace_back(t);
						auto n = QString::fromUtf8(obs_source_get_name(t));
						transition->addItem(n);
						transition->setCurrentText(n);
					}
				});
			}
			obs_frontend_source_list_free(&frontend_transitions);
			menu.addSeparator();
			size_t idx = 0;
			const char *id;
			while (obs_enum_transition_types(idx++, &id)) {
				if (!obs_is_source_configurable(id)) {
					continue;
				}
				const char *display_name = obs_source_get_display_name(id);

				auto action = menu.addAction(QString::fromUtf8(display_name));
				connect(action, &QAction::triggered, [this, id] {
					OBSSourceAutoRelease t =
						obs_source_create_private(id, obs_source_get_display_name(id), nullptr);
					if (t) {
						std::string name = obs_source_get_name(t);
						while (true) {
							if (!NameDialog::AskForName(
								    this, QString::fromUtf8(obs_module_text("TransitionName")),
								    name)) {
								obs_source_release(t);
								return;
							}
							if (name.empty()) {
								continue;
							}
							bool found = false;
							for (auto tr : transitions) {
								if (strcmp(obs_source_get_name(tr), name.c_str()) == 0) {
									found = true;
									break;
								}
							}
							if (found) {
								continue;
							}

							obs_source_set_name(t, name.c_str());
							break;
						}
						transitions.emplace_back(t);
						auto n = QString::fromUtf8(obs_source_get_name(t));
						transition->addItem(n);
						transition->setCurrentText(n);
						obs_frontend_open_source_properties(t);
					}
				});
			}
			menu.exec(QCursor::pos());
		});

		hl->addWidget(addButton);

		auto removeButton = new QPushButton();
		removeButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		removeButton->setAccessibleName(QString::fromUtf8(obs_frontend_get_locale_string("Basic.RemoveTransition")));
		removeButton->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("Basic.RemoveTransition")));
		removeButton->setIcon(QIcon(QString::fromUtf8(":/res/images/minus.svg")));
		removeButton->setProperty("class", QString::fromUtf8("btn-tool icon-trash"));
		removeButton->setFlat(false);
		removeButton->style()->unpolish(removeButton);
		removeButton->style()->polish(removeButton);

		connect(removeButton, &QPushButton::clicked, [this] {
			QMessageBox mb(QMessageBox::Question,
				       QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Title")),
				       QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Text"))
					       .arg(transition->currentText()),
				       QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No));
			mb.setDefaultButton(QMessageBox::NoButton);
			if (mb.exec() != QMessageBox::Yes) {
				return;
			}

			auto n = transition->currentText().toUtf8();
			for (auto it = transitions.begin(); it != transitions.end(); ++it) {
				if (strcmp(n.constData(), obs_source_get_name(it->Get())) == 0) {
					if (!obs_is_source_configurable(obs_source_get_id(it->Get()))) {
						return;
					}
					transitions.erase(it);
					break;
				}
			}
			transition->removeItem(transition->currentIndex());
			if (transition->currentIndex() < 0) {
				transition->setCurrentIndex(0);
			}
		});

		hl->addWidget(removeButton);

		auto propsButton = new QPushButton();
		propsButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		propsButton->setAccessibleName(QString::fromUtf8(obs_frontend_get_locale_string("Basic.TransitionProperties")));
		propsButton->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("Basic.TransitionProperties")));
		propsButton->setIcon(QIcon(QString::fromUtf8(":/settings/images/settings/general.svg")));
		propsButton->setProperty("class", QString::fromUtf8("btn-tool icon-dots-vert"));
		propsButton->setFlat(false);
		propsButton->style()->unpolish(propsButton);
		propsButton->style()->polish(propsButton);

		connect(propsButton, &QPushButton::clicked, [this] {
			auto menu = QMenu(this);
			auto action = menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Rename")));
			connect(action, &QAction::triggered, [this] {
				auto tn = transition->currentText().toUtf8();
				obs_source_t *t = GetTransition(tn.constData());
				if (!t) {
					return;
				}
				std::string name = obs_source_get_name(t);
				while (true) {
					if (!NameDialog::AskForName(this, QString::fromUtf8(obs_module_text("TransitionName")),
								    name)) {
						return;
					}
					if (name.empty()) {
						continue;
					}
					bool found = false;
					for (auto tr : transitions) {
						if (strcmp(obs_source_get_name(tr), name.c_str()) == 0) {
							found = true;
							break;
						}
					}
					if (found) {
						continue;
					}

					transition->setItemText(transition->currentIndex(), QString::fromUtf8(name.c_str()));
					obs_source_set_name(t, name.c_str());
					break;
				}
			});
			action = menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Properties")));
			connect(action, &QAction::triggered, [this] {
				auto tn = transition->currentText().toUtf8();
				auto t = GetTransition(tn.constData());
				if (!t) {
					return;
				}
				obs_frontend_open_source_properties(t);
			});
			menu.exec(QCursor::pos());
		});

		hl->addWidget(propsButton);

		transitionsGroupLayout->addLayout(hl);
		transitionsGroupLayout->addStretch();

		panel_split->addWidget(transitionsGroup);
		panel_split->setCollapsible(0, false);

		/* Keep scenes/sources/transitions off this dock — they are shown
		 * via the native Scenes/Sources/Transitions docks (canvas tabs).
		 * panel_split stays as a hidden holder for Borrow*Pane(). */
		panel_split->setParent(this);
		panel_split->hide();

		LoadTransitions();

		connect(transition, &QComboBox::currentTextChanged, [this, removeButton, propsButton] {
			auto tn = transition->currentText().toUtf8();
			auto t = GetTransition(tn.constData());
			if (!t) {
				return;
			}
			SwapTransition(t);
			bool config = obs_is_source_configurable(obs_source_get_id(t));
			removeButton->setEnabled(config);
			propsButton->setEnabled(config);
		});
	}

	if (settings) {
		LoadMode(QStringLiteral("default"));
		LoadScenes();
		LogScenes();

		auto pa = obs_data_get_array(settings, "projectors");
		LoadProjectors(pa);
		obs_data_array_release(pa);
		preview_disabled = obs_data_get_bool(settings, "preview_disabled");
		locked = obs_data_get_bool(settings, "preview_locked");
	}
	obs_display_set_enabled(preview->GetDisplay(), !preview_disabled);
	preview->setVisible(!preview_disabled);
	previewDisabledWidget->setVisible(preview_disabled);

	connect(canvas_split, &SwitchingSplitter::splitterMoved, [this] { SaveSettings(); });
	if (panel_split) {
		connect(panel_split, &SwitchingSplitter::splitterMoved, [this] { SaveSettings(); });
	}

	obs_frontend_add_save_callback(save_load, this);
}

bool WindowPositionValid(QRect rect)
{
	for (QScreen *screen : QGuiApplication::screens()) {
		if (screen->availableGeometry().intersects(rect)) {
			return true;
		}
	}
	return false;
}

void CanvasDock::LoadProjectors(obs_data_array_t *pa)
{
	if (!pa) {
		return;
	}
	auto count = obs_data_array_count(pa);
	for (size_t i = 0; i < count; i++) {
		auto p = obs_data_array_item(pa, i);
		if (!p) {
			continue;
		}

		auto monitor = obs_data_get_int(p, "monitor");
		auto source_uuid = obs_data_get_string(p, "source_uuid");
		obs_source_t *source = source_uuid[0] == '\0' ? nullptr : obs_get_source_by_uuid(source_uuid);

		OBSProjector *projector = new OBSProjector(source ? nullptr : canvas, source, monitor,
							   [this](OBSProjector *p) { DeleteProjector(p); });
		obs_source_release(source);

		auto g = obs_data_get_string(p, "geometry");
		if (g[0] != '\0' && monitor < 0) {
			QByteArray byteArray = QByteArray::fromBase64(QByteArray(g));
			projector->restoreGeometry(byteArray);

			if (!WindowPositionValid(projector->normalGeometry())) {
				QRect rect = QGuiApplication::primaryScreen()->geometry();
				projector->setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size(), rect));
			}

			if (obs_data_get_bool(p, "alwaysOnTopOverridden")) {
				projector->SetIsAlwaysOnTop(obs_data_get_bool(p, "alwaysOnTop"), true);
			}
		}
		projectors.emplace_back(projector);
		obs_data_release(p);
	}
}

void CanvasDock::GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	double windowAspect, baseAspect;
	int newCX, newCY;

	windowAspect = double(windowCX) / double(windowCY);
	baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

void CanvasDock::GetCenterPosFromFixedScale(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y,
					    float scale)
{
	x = int((float(windowCX) - float(baseCX) * scale) / 2.0f);
	y = int((float(windowCY) - float(baseCY) * scale) / 2.0f);
}

void CanvasDock::BuildPreviewChrome(QBoxLayout *previewContainerLayout, QBoxLayout *outerLayout)
{
	if (!previewContainerLayout || !outerLayout) {
		return;
	}

	/* Mirror OBSBasic.ui gridLayout: preview + Y scrollbar + scale row with X scrollbar. */
	auto *gridHost = new QWidget(this);
	auto *grid = new QGridLayout(gridHost);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setSpacing(0);

	grid->addWidget(preview, 1, 0, 1, 2);
	if (previewDisabledWidget) {
		grid->addWidget(previewDisabledWidget, 1, 0, 1, 2);
	}

	previewYScrollBar = new QScrollBar(Qt::Vertical, gridHost);
	previewYScrollBar->setObjectName(QStringLiteral("previewYScrollBar"));
	grid->addWidget(previewYScrollBar, 1, 2);

	auto *scaleRow = new QWidget(gridHost);
	/* Do NOT use previewXContainer: Yami paints a full grey6 box whose top
	 * edge reads as a separator under the preview. Main Canvas looks flush
	 * in the docked centralwidget; keep the same widgets/objectNames for
	 * zoom controls and scrollbars, but leave the scale row unboxed. */
	scaleRow->setObjectName(QStringLiteral("aitumPreviewScaleRow"));
	auto *scaleLayout = new QHBoxLayout(scaleRow);
	scaleLayout->setContentsMargins(0, 0, 0, 0);
	scaleLayout->setSpacing(0);

	previewZoomOutButton = new QPushButton(scaleRow);
	previewZoomOutButton->setObjectName(QStringLiteral("previewZoomOutButton"));
	previewZoomOutButton->setFixedSize(20, 16);
	previewZoomOutButton->setIcon(QIcon(QStringLiteral(":/res/images/minus.svg")));
	previewZoomOutButton->setIconSize(QSize(8, 8));
	previewZoomOutButton->setProperty("toolButton", true);
	previewZoomOutButton->setProperty("class", QStringLiteral("icon-minus"));
	previewZoomOutButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.ZoomOut")));
	connect(previewZoomOutButton, &QPushButton::clicked, this, [this] { DecreaseScalingLevel(); });

	previewScalePercent = new QLabel(QStringLiteral("100%"), scaleRow);
	previewScalePercent->setObjectName(QStringLiteral("previewScalePercent"));
	previewScalePercent->setAlignment(Qt::AlignCenter);
	previewScalePercent->setFixedHeight(14);

	previewZoomInButton = new QPushButton(scaleRow);
	previewZoomInButton->setObjectName(QStringLiteral("previewZoomInButton"));
	previewZoomInButton->setFixedSize(20, 16);
	previewZoomInButton->setIcon(QIcon(QStringLiteral(":/res/images/plus.svg")));
	previewZoomInButton->setIconSize(QSize(8, 8));
	previewZoomInButton->setProperty("toolButton", true);
	previewZoomInButton->setProperty("class", QStringLiteral("icon-plus"));
	previewZoomInButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.ZoomIn")));
	connect(previewZoomInButton, &QPushButton::clicked, this, [this] { IncreaseScalingLevel(); });

	previewScalingMode = new QComboBox(scaleRow);
	previewScalingMode->setObjectName(QStringLiteral("previewScalingMode"));
	previewScalingMode->setFixedHeight(14);
	previewScalingMode->addItem(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.Window")));
	previewScalingMode->addItem(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.Canvas"))
			.arg(QString::number(canvas_width), QString::number(canvas_height)));
	connect(previewScalingMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
		if (index == 0) {
			SetPreviewScalingWindow();
		} else if (index == 1) {
			SetPreviewScalingCanvas();
		} else if (index == 2) {
			SetPreviewScalingOutput();
		}
	});

	previewXScrollBar = new QScrollBar(Qt::Horizontal, scaleRow);
	previewXScrollBar->setObjectName(QStringLiteral("previewXScrollBar"));
	previewXScrollBar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
	previewXScrollBar->setFixedHeight(16);
	previewXScrollBar->setSingleStep(10);
	previewXScrollBar->setPageStep(100);

	scaleLayout->addWidget(previewZoomOutButton);
	scaleLayout->addWidget(previewScalePercent);
	scaleLayout->addWidget(previewZoomInButton);
	scaleLayout->addWidget(previewScalingMode);
	scaleLayout->addSpacing(4);
	scaleLayout->addWidget(previewXScrollBar, 1);
	grid->addWidget(scaleRow, 2, 1);

	connect(previewXScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
		if (!fixedScaling) {
			return;
		}
		scrollingOffset.x = float(-value);
		if (preview) {
			preview->update();
		}
	});
	connect(previewYScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
		if (!fixedScaling) {
			return;
		}
		scrollingOffset.y = float(-value);
		if (preview) {
			preview->update();
		}
	});

	previewContainerLayout->addWidget(gridHost, 1);

	/* Mirror OBSBasic.ui: contextContainer is sibling of canvasEditor. */
	contextContainer = new QFrame(this);
	contextContainer->setObjectName(QStringLiteral("contextContainer"));
	contextContainer->setFrameShape(QFrame::NoFrame);
	contextContainer->setFrameShadow(QFrame::Plain);
	contextContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	auto *hl = new QHBoxLayout(contextContainer);
	hl->setContentsMargins(6, 4, 6, 4);
	hl->setSpacing(6);

	auto *contextSub = new QFrame(contextContainer);
	contextSub->setObjectName(QStringLiteral("contextSubContainer"));
	contextSub->setFrameShape(QFrame::NoFrame);
	contextSub->setFrameShadow(QFrame::Plain);
	auto *sub = new QHBoxLayout(contextSub);
	sub->setContentsMargins(0, 0, 0, 0);
	sub->setSpacing(6);

	contextSourceIcon = new QLabel(contextSub);
	contextSourceIcon->setObjectName(QStringLiteral("contextSourceIcon"));
	contextSourceIcon->setFixedSize(22, 22);
	contextSourceIcon->hide();

	contextSourceLabel = new QLabel(contextSub);
	contextSourceLabel->setObjectName(QStringLiteral("contextSourceLabel"));
	contextSourceLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	contextSourceLabel->setMinimumSize(160, 22);
	contextSourceLabel->setMaximumSize(160, 22);
	contextSourceLabel->setText(
		QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.NoSelectedSource")));

	contextSourceIconSpacer = new QLabel(contextSub);
	contextSourceIconSpacer->setObjectName(QStringLiteral("contextSourceIconSpacer"));
	contextSourceIconSpacer->setFixedSize(22, 22);

	auto *sep = new QFrame(contextSub);
	sep->setFrameShape(QFrame::VLine);
	sep->setFrameShadow(QFrame::Plain);
	sep->setObjectName(QStringLiteral("line_3"));

	sourcePropertiesButton = new QPushButton(contextSub);
	sourcePropertiesButton->setObjectName(QStringLiteral("sourcePropertiesButton"));
	sourcePropertiesButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	sourcePropertiesButton->setMinimumHeight(22);
	sourcePropertiesButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("SourceProperties")));
	sourcePropertiesButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Properties")));
	sourcePropertiesButton->setIcon(SuiteThemeIcon("settings/general.svg", ":/settings/images/settings/general.svg"));
	sourcePropertiesButton->setFlat(false);
	sourcePropertiesButton->setEnabled(false);
	connect(sourcePropertiesButton, &QPushButton::clicked, this, [this] {
		obs_sceneitem_t *item = GetSelectedItem(scene);
		if (!item) {
			item = GetCurrentSceneItem();
		}
		if (!item) {
			return;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source) {
			obs_frontend_open_source_properties(source);
		}
	});

	sourceFiltersButton = new QPushButton(contextSub);
	sourceFiltersButton->setObjectName(QStringLiteral("sourceFiltersButton"));
	sourceFiltersButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	sourceFiltersButton->setMinimumHeight(22);
	sourceFiltersButton->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("SourceFilters")));
	sourceFiltersButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Filters")));
	sourceFiltersButton->setIcon(SuiteThemeIcon("filter.svg"));
	sourceFiltersButton->setFlat(false);
	sourceFiltersButton->setEnabled(false);
	connect(sourceFiltersButton, &QPushButton::clicked, this, [this] {
		obs_sceneitem_t *item = GetSelectedItem(scene);
		if (!item) {
			item = GetCurrentSceneItem();
		}
		if (!item) {
			return;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source) {
			obs_frontend_open_source_filters(source);
		}
	});

	sourceInteractButton = new QPushButton(contextSub);
	sourceInteractButton->setObjectName(QStringLiteral("sourceInteractButton"));
	sourceInteractButton->setMinimumHeight(22);
	sourceInteractButton->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("Interact")));
	sourceInteractButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Interact")));
	sourceInteractButton->setIcon(SuiteThemeIcon("interact.svg"));
	sourceInteractButton->setProperty("class", QStringLiteral("icon-touch"));
	sourceInteractButton->setFlat(false);
	sourceInteractButton->hide();
	connect(sourceInteractButton, &QPushButton::clicked, this, [this] {
		obs_sceneitem_t *item = GetSelectedItem(scene);
		if (!item) {
			item = GetCurrentSceneItem();
		}
		if (!item) {
			return;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source) {
			obs_frontend_open_source_interaction(source);
		}
	});

	/* Native order: icon → label → iconSpacer → line → Properties → Filters → Interact → line */
	auto *sepAfter = new QFrame(contextSub);
	sepAfter->setFrameShape(QFrame::VLine);
	sepAfter->setFrameShadow(QFrame::Plain);
	sepAfter->setObjectName(QStringLiteral("line"));

	sub->addWidget(contextSourceIcon);
	sub->addWidget(contextSourceLabel);
	sub->addWidget(contextSourceIconSpacer);
	sub->addWidget(sep);
	sub->addWidget(sourcePropertiesButton);
	sub->addWidget(sourceFiltersButton);
	sub->addWidget(sourceInteractButton);
	sub->addWidget(sepAfter);

	emptySpace = new QFrame(contextContainer);
	emptySpace->setObjectName(QStringLiteral("emptySpace"));
	emptySpace->setFrameShape(QFrame::NoFrame);
	emptySpace->setFrameShadow(QFrame::Plain);
	emptySpace->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	emptySpace->setMinimumHeight(22);
	auto *emptyLayout = new QHBoxLayout(emptySpace);
	emptyLayout->setContentsMargins(0, 0, 0, 0);
	emptyLayout->setSpacing(0);

	hl->addWidget(contextSub);
	hl->addWidget(emptySpace, 1);
	outerLayout->addWidget(contextContainer);

	UpdatePreviewScaleControls();
	UpdateContextBarVisibility();
	UpdateContextBar(true);
}

void CanvasDock::ClearContextBar()
{
	if (!emptySpace || !emptySpace->layout()) {
		return;
	}
	QLayout *layout = emptySpace->layout();
	while (QLayoutItem *la = layout->takeAt(0)) {
		delete la->widget();
		delete la;
	}
}

QWidget *CanvasDock::CreateContextBarWidget(obs_source_t *source)
{
	return CreateSourceContextToolbar(emptySpace, source);
}

void CanvasDock::UpdateContextBarVisibility()
{
	/* Match OBSBasic thresholds, but measure the dock itself (not the
	 * whole main window). Prefer icon-only chrome when a typed toolbar
	 * also needs room — same look as native Main Canvas with Window Capture. */
	const int width = contextContainer ? contextContainer->width() : size().width();
	ContextBarSize next;
	if (width >= 900) {
		next = ContextBarSize_Normal;
	} else if (width >= 360) {
		next = ContextBarSize_Reduced;
	} else {
		next = ContextBarSize_Minimized;
	}

	if (contextBarSize == next) {
		return;
	}
	contextBarSize = next;
	UpdateContextBar(true);
}

void CanvasDock::resizeEvent(QResizeEvent *event)
{
	QFrame::resizeEvent(event);
	UpdateContextBarVisibility();
}

void CanvasDock::UpdateContextBar(bool force)
{
	if (!contextContainer || !emptySpace) {
		return;
	}

	obs_sceneitem_t *item = GetSelectedItem(scene);
	if (!item && sourceList) {
		item = GetCurrentSceneItem();
	}

	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *id = obs_source_get_unversioned_id(source);
		const uint32_t flags = obs_source_get_output_flags(source);

		bool updateNeeded = true;
		if (QLayoutItem *la = emptySpace->layout()->itemAt(0)) {
			OBSSource current = ContextToolbarGetSource(la->widget());
			if (current == source) {
				updateNeeded = false;
			}
		}

		if (sourceInteractButton) {
			sourceInteractButton->setVisible(flags & OBS_SOURCE_INTERACTION);
		}

		/* Keep typed toolbars even in Minimized — docks are often narrow. */
		if (updateNeeded || force) {
			ClearContextBar();
			if (contextBarSize >= ContextBarSize_Reduced) {
				if (QWidget *widget = CreateContextBarWidget(source)) {
					emptySpace->layout()->addWidget(widget);
				}
			}
		}

		QIcon icon;
		if (strcmp(id, "scene") == 0) {
			icon = GetSceneIcon();
		} else if (strcmp(id, "group") == 0) {
			icon = GetGroupIcon();
		} else {
			icon = GetIconFromType(obs_source_get_icon_type(id), id);
		}
		contextSourceIcon->setPixmap(icon.pixmap(QSize(16, 16)));
		contextSourceIcon->show();
		contextSourceIconSpacer->hide();
		contextSourceLabel->setText(QString::fromUtf8(obs_source_get_name(source)));
		sourceFiltersButton->setEnabled(true);
		sourcePropertiesButton->setEnabled(obs_source_configurable(source));
	} else {
		ClearContextBar();
		contextSourceIcon->hide();
		contextSourceIconSpacer->show();
		contextSourceLabel->setText(
			QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.NoSelectedSource")));
		sourceFiltersButton->setEnabled(false);
		sourcePropertiesButton->setEnabled(false);
		if (sourceInteractButton) {
			sourceInteractButton->hide();
		}
	}

	if (contextBarSize == ContextBarSize_Normal) {
		sourcePropertiesButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Properties")));
		sourceFiltersButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Filters")));
		if (sourceInteractButton) {
			sourceInteractButton->setText(QString::fromUtf8(obs_frontend_get_locale_string("Interact")));
		}
	} else {
		sourcePropertiesButton->setText(QString());
		sourceFiltersButton->setText(QString());
		if (sourceInteractButton) {
			sourceInteractButton->setText(QString());
		}
	}
}

void CanvasDock::SetFixedScaling(bool fixed)
{
	fixedScaling = fixed;
	UpdatePreviewScaleControls();
}

void CanvasDock::SetScalingAmount(float amount)
{
	if (scalingAmount == amount) {
		return;
	}
	if (scalingAmount > 0.0f) {
		scrollingOffset.x *= amount / scalingAmount;
		scrollingOffset.y *= amount / scalingAmount;
	}
	scalingAmount = amount;
	if (!fixedScaling) {
		scalingLevel = int32_t(std::round(log(scalingAmount) / log(ZOOM_SENSITIVITY)));
	}
}

void CanvasDock::SetScalingLevel(int32_t level)
{
	level = std::clamp(level, -MAX_SCALING_LEVEL, MAX_SCALING_LEVEL);
	scalingLevel = level;
	SetScalingAmount(float(pow(ZOOM_SENSITIVITY, float(level))));
}

void CanvasDock::IncreaseScalingLevel()
{
	SetFixedScaling(true);
	SetScalingLevel(scalingLevel + 1);
	ResizePreview();
	if (preview) {
		emit preview->DisplayResized();
	}
}

void CanvasDock::DecreaseScalingLevel()
{
	SetFixedScaling(true);
	SetScalingLevel(scalingLevel - 1);
	ResizePreview();
	if (preview) {
		emit preview->DisplayResized();
	}
}

void CanvasDock::SetPreviewScalingWindow()
{
	SetFixedScaling(false);
	vec2_zero(&scrollingOffset);
	scrollX = 0.5f;
	scrollY = 0.5f;
	zoom = 1.0f;
	ResizePreview();
	if (preview) {
		emit preview->DisplayResized();
	}
}

void CanvasDock::SetPreviewScalingCanvas()
{
	SetFixedScaling(true);
	SetScalingLevel(0);
	vec2_zero(&scrollingOffset);
	ResizePreview();
	if (preview) {
		emit preview->DisplayResized();
	}
}

void CanvasDock::SetPreviewScalingOutput()
{
	obs_video_info ovi{};
	if (!obs_canvas_get_video_info(canvas, &ovi)) {
		obs_get_video_info(&ovi);
	}
	SetFixedScaling(true);
	const float amount = float(ovi.output_width) / float(ovi.base_width > 0 ? ovi.base_width : 1);
	const int32_t level = int32_t(std::round(log(amount) / log(ZOOM_SENSITIVITY)));
	scalingLevel = std::clamp(level, -MAX_SCALING_LEVEL, MAX_SCALING_LEVEL);
	SetScalingAmount(amount);
	ResizePreview();
	if (preview) {
		emit preview->DisplayResized();
	}
}

void CanvasDock::ResizePreview()
{
	if (!preview) {
		return;
	}
	const QSize target = preview->size() * preview->devicePixelRatioF();
	uint32_t sourceCX = canvas_width ? canvas_width : 1;
	uint32_t sourceCY = canvas_height ? canvas_height : 1;
	if (!settings && scene) {
		obs_source_t *source = obs_scene_get_source(scene);
		if (source) {
			const uint32_t w = obs_source_get_width(source);
			const uint32_t h = obs_source_get_height(source);
			if (w > 0) {
				sourceCX = w;
			}
			if (h > 0) {
				sourceCY = h;
			}
		}
	}

	const int usableW = std::max(1, target.width() - PREVIEW_EDGE_SIZE * 2);
	const int usableH = std::max(1, target.height() - PREVIEW_EDGE_SIZE * 2);
	int x = 0;
	int y = 0;
	float scale = 1.0f;
	if (fixedScaling) {
		scale = scalingAmount;
		GetCenterPosFromFixedScale(int(sourceCX), int(sourceCY), usableW, usableH, x, y, scale);
		x += int(scrollingOffset.x);
		y += int(scrollingOffset.y);
	} else {
		GetScaleAndCenterPos(int(sourceCX), int(sourceCY), usableW, usableH, x, y, scale);
		SetScalingAmount(scale);
	}
	x += PREVIEW_EDGE_SIZE;
	y += PREVIEW_EDGE_SIZE;
	previewScale = scale;
	previewX = x;
	previewY = y;
	/* Keep legacy zoom path in sync for any code still reading zoom. */
	zoom = fixedScaling ? (scale > 0.0f ? scalingAmount / scale : 1.0f) : 1.0f;
	UpdatePreviewScaleControls();
}

void CanvasDock::UpdatePreviewScrollBars()
{
	if (!previewXScrollBar || !previewYScrollBar || !preview) {
		return;
	}
	if (!fixedScaling) {
		QSignalBlocker bx(previewXScrollBar);
		QSignalBlocker by(previewYScrollBar);
		previewXScrollBar->setRange(0, 0);
		previewYScrollBar->setRange(0, 0);
		return;
	}

	const QSize target = preview->size() * preview->devicePixelRatioF();
	const float cx = float(canvas_width > 0 ? canvas_width : 1) * scalingAmount;
	const float cy = float(canvas_height > 0 ? canvas_height : 1) * scalingAmount;
	float offX = (cx - float(target.width())) * 0.5f;
	float offY = (cy - float(target.height())) * 0.5f;
	if (offX < 0.0f) {
		offX = 0.0f;
	}
	if (offY < 0.0f) {
		offY = 0.0f;
	}
	offX += float(target.width()) * 0.5f;
	offY += float(target.height()) * 0.5f;

	scrollingOffset.x = std::clamp(scrollingOffset.x, -offX, offX);
	scrollingOffset.y = std::clamp(scrollingOffset.y, -offY, offY);

	{
		QSignalBlocker bx(previewXScrollBar);
		previewXScrollBar->setRange(int(-offX), int(offX));
		previewXScrollBar->setPageStep(int(float(target.width()) / std::min(scalingAmount, 1.0f)));
		previewXScrollBar->setValue(int(-scrollingOffset.x));
	}
	{
		QSignalBlocker by(previewYScrollBar);
		previewYScrollBar->setRange(int(-offY), int(offY));
		previewYScrollBar->setPageStep(int(float(target.height()) / std::min(scalingAmount, 1.0f)));
		previewYScrollBar->setValue(int(-scrollingOffset.y));
	}
}

void CanvasDock::UpdatePreviewScaleControls()
{
	if (previewScalePercent) {
		previewScalePercent->setText(QString::number(int(floor(previewScale * 100.0f))) + QStringLiteral("%"));
	}
	if (previewZoomInButton) {
		previewZoomInButton->setEnabled(!fixedScaling || scalingLevel != MAX_SCALING_LEVEL);
	}
	if (previewZoomOutButton) {
		previewZoomOutButton->setEnabled(!fixedScaling || scalingLevel != -MAX_SCALING_LEVEL);
	}
	UpdatePreviewScrollBars();
	if (!previewScalingMode) {
		return;
	}
	QSignalBlocker block(previewScalingMode);
	obs_video_info ovi{};
	if (!obs_canvas_get_video_info(canvas, &ovi)) {
		obs_get_video_info(&ovi);
	}
	const bool showOutput = ovi.output_width != ovi.base_width || ovi.output_height != ovi.base_height;
	while (previewScalingMode->count() > 2) {
		previewScalingMode->removeItem(2);
	}
	previewScalingMode->setItemText(
		1, QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.Canvas"))
			   .arg(QString::number(canvas_width), QString::number(canvas_height)));
	if (showOutput) {
		previewScalingMode->addItem(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.Output"))
				.arg(QString::number(ovi.output_width), QString::number(ovi.output_height)));
	}

	const float outputScale =
		float(ovi.output_width) / float(ovi.base_width > 0 ? ovi.base_width : 1);
	if (!fixedScaling) {
		previewScalingMode->setCurrentIndex(0);
	} else if (fabsf(previewScale - 1.0f) < 0.0001f) {
		previewScalingMode->setCurrentIndex(1);
	} else if (showOutput && fabsf(previewScale - outputScale) < 0.0001f) {
		previewScalingMode->setCurrentIndex(2);
	} else {
		previewScalingMode->setCurrentIndex(-1);
		previewScalingMode->setPlaceholderText(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Scale.Manual"))
				.arg(QString::number(floor(canvas_width * previewScale)),
				     QString::number(floor(canvas_height * previewScale))));
	}
}

void CanvasDock::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);
	if (!window || !window->canvas || obs_canvas_removed(window->canvas)) {
		return;
	}

	obs_source_t *source = nullptr;

	uint32_t sourceCX = window->canvas_width;
	uint32_t sourceCY = window->canvas_height;
	if (!window->settings && window->scene) {
		source = obs_scene_get_source(window->scene);
		sourceCX = obs_source_get_width(source);
		sourceCY = obs_source_get_height(source);
	}
	if (sourceCX <= 0) {
		sourceCX = 1;
	}
	if (sourceCY <= 0) {
		sourceCY = 1;
	}

	const int usableW = std::max(1, int(cx) - PREVIEW_EDGE_SIZE * 2);
	const int usableH = std::max(1, int(cy) - PREVIEW_EDGE_SIZE * 2);
	int x, y;
	float scale;
	if (window->fixedScaling) {
		scale = window->scalingAmount;
		GetCenterPosFromFixedScale(int(sourceCX), int(sourceCY), usableW, usableH, x, y, scale);
		x += int(window->scrollingOffset.x);
		y += int(window->scrollingOffset.y);
	} else {
		GetScaleAndCenterPos(sourceCX, sourceCY, usableW, usableH, x, y, scale);
	}
	x += PREVIEW_EDGE_SIZE;
	y += PREVIEW_EDGE_SIZE;
	window->previewScale = scale;
	window->previewX = x;
	window->previewY = y;

	const int previewCX = int(scale * float(sourceCX));
	const int previewCY = int(scale * float(sourceCY));
	const float right = float(cx) - float(x);
	const float bottom = float(cy) - float(y);

	gs_viewport_push();
	gs_projection_push();

	/* Same projection pattern as OBSBasic::RenderMain — edge padding is
	 * the cleared display background (PREVIEW_EDGE_SIZE). */
	gs_ortho(-float(x), right, -float(y), bottom, -100.0f, 100.0f);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, previewCX, previewCY);
	window->DrawBackdrop(float(sourceCX), float(sourceCY));

	const bool previous = gs_set_linear_srgb(true);
	if (source) {
		obs_source_video_render(source);
	} else {
		obs_canvas_render(window->canvas);
	}
	gs_set_linear_srgb(previous);
	gs_load_vertexbuffer(nullptr);

	gs_ortho(-float(x), right, -float(y), bottom, -100.0f, 100.0f);
	gs_reset_viewport();

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	if (window->scene && !window->locked) {
		gs_matrix_push();
		gs_matrix_scale3f(scale, scale, 1.0f);
		obs_scene_enum_items(window->scene, DrawSelectedItem, data);
		gs_matrix_pop();
	}

	if (window->selectionBox) {
		if (!window->rectFill) {
			gs_render_start(true);

			gs_vertex2f(0.0f, 0.0f);
			gs_vertex2f(1.0f, 0.0f);
			gs_vertex2f(0.0f, 1.0f);
			gs_vertex2f(1.0f, 1.0f);

			window->rectFill = gs_render_save();
		}

		window->DrawSelectionBox(window->startPos.x * scale, window->startPos.y * scale, window->mousePos.x * scale,
					 window->mousePos.y * scale, window->rectFill);
	}
	gs_load_vertexbuffer(nullptr);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	if (window->drawSpacingHelpers) {
		window->DrawSpacingHelpers(window->scene, (float)x, (float)y, float(previewCX), float(previewCY), scale,
					   float(sourceCX), float(sourceCY));
	}

	gs_projection_pop();
	gs_viewport_pop();
}

CanvasDock::~CanvasDock()
{
	for (auto projector : projectors) {
		delete projector;
	}
	obs_frontend_remove_save_callback(save_load, this);
	canvas_docks.remove(this);
	obs_display_remove_draw_callback(preview->GetDisplay(), DrawPreview, this);
	SaveSettings(true);
	obs_data_release(settings);
	obs_enter_graphics();
	gs_vertexbuffer_destroy(box);
	gs_vertexbuffer_destroy(rectFill);
	gs_vertexbuffer_destroy(circleFill);
	obs_leave_graphics();
	if (canvas) {
		auto sh = obs_canvas_get_signal_handler(canvas);
		if (sh) {
			signal_handler_disconnect(sh, "source_add", source_add, this);
		}
	}
	obs_canvas_release(canvas);

	obs_source_t *oldTransition = obs_weak_source_get_source(source);
	if (oldTransition && obs_source_get_type(oldTransition) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_weak_source_release(source);
		source = nullptr;
		signal_handler_t *handler = obs_source_get_signal_handler(oldTransition);
		signal_handler_disconnect(handler, "transition_stop", transition_override_stop, this);
		obs_source_dec_showing(oldTransition);
		obs_source_dec_active(oldTransition);
	}
	obs_source_release(oldTransition);

	transitions.clear();
}

extern obs_data_t *current_profile_config;

void CanvasDock::SaveSettings(bool closing, QString mode)
{
	if (!settings) {
		if (!closing && current_profile_config) {
			auto state = canvas_split->saveState();
			auto b64 = state.toBase64();
			auto state_chars = b64.constData();
			if (mode.isEmpty()) {
				mode = QStringLiteral("default");
			}
			std::string setting_name = canvas_name + "_canvas_split_" + mode.toStdString();
			obs_data_set_string(current_profile_config, setting_name.c_str(), state_chars);
			setting_name = canvas_name + "_canvas_split_automatic_" + mode.toStdString();
			obs_data_set_bool(current_profile_config, setting_name.c_str(), canvas_split->automaticSwitching);
			setting_name = canvas_name + "_canvas_split_horizontal_" + mode.toStdString();
			obs_data_set_bool(current_profile_config, setting_name.c_str(),
					  canvas_split->orientation() == Qt::Horizontal);
			setting_name = canvas_name + "_canvas_split_order_" + mode.toStdString();
			obs_data_set_string(current_profile_config, setting_name.c_str(),
					    canvas_split->savePanelOrder().toUtf8().constData());

			obs_data_set_bool(current_profile_config, "preview_disabled", preview_disabled);
			obs_data_set_bool(current_profile_config, "preview_locked", locked);
		}
		return;
	}
	if (!closing) {
		auto pa = obs_data_array_create();
		for (auto projector : projectors) {
			auto p = obs_data_create();
			obs_data_set_string(p, "source_name", projector->GetSourceName());
			obs_data_set_string(p, "source_uuid", projector->GetSourceUuid());
			obs_data_set_int(p, "monitor", projector->GetMonitor());
			obs_data_set_string(p, "geometry", projector->saveGeometry().toBase64().constData());
			if (projector->IsAlwaysOnTopOverridden()) {
				obs_data_set_bool(p, "alwaysOnTop", projector->IsAlwaysOnTop());
			}
			obs_data_set_bool(p, "alwaysOnTopOverridden", projector->IsAlwaysOnTopOverridden());
			obs_data_array_push_back(pa, p);
			obs_data_release(p);
		}
		obs_data_set_array(settings, "projectors", pa);
		obs_data_array_release(pa);

		auto state = canvas_split->saveState();
		auto b64 = state.toBase64();
		auto state_chars = b64.constData();
		if (mode.isEmpty()) {
			mode = QStringLiteral("default");
		}
		std::string setting_name = "canvas_split_" + mode.toStdString();
		obs_data_set_string(settings, setting_name.c_str(), state_chars);
		setting_name = "canvas_split_automatic_" + mode.toStdString();
		obs_data_set_bool(settings, setting_name.c_str(), canvas_split->automaticSwitching);
		setting_name = "canvas_split_horizontal_" + mode.toStdString();
		obs_data_set_bool(settings, setting_name.c_str(), canvas_split->orientation() == Qt::Horizontal);
		setting_name = "canvas_split_order_" + mode.toStdString();
		obs_data_set_string(settings, setting_name.c_str(), canvas_split->savePanelOrder().toUtf8().constData());
		if (panel_split) {
			state = panel_split->saveState();
			b64 = state.toBase64();
			state_chars = b64.constData();
			setting_name = "panel_split_" + mode.toStdString();
			obs_data_set_string(settings, setting_name.c_str(), state_chars);
			setting_name = "panel_split_automatic_" + mode.toStdString();
			obs_data_set_bool(settings, setting_name.c_str(), panel_split->automaticSwitching);
			setting_name = "panel_split_horizontal_" + mode.toStdString();
			obs_data_set_bool(settings, setting_name.c_str(), panel_split->orientation() == Qt::Horizontal);
			setting_name = "panel_split_order_" + mode.toStdString();
			obs_data_set_string(settings, setting_name.c_str(), panel_split->savePanelOrder().toUtf8().constData());
		}

		obs_data_set_bool(settings, "preview_disabled", preview_disabled);
		obs_data_set_bool(settings, "preview_locked", locked);
	}

	obs_data_array_t *transition_array = obs_data_array_create();
	for (auto transition : transitions) {
		const char *id = obs_source_get_id(transition);
		if (!obs_is_source_configurable(id)) {
			continue;
		}
		obs_data_t *transition_data = obs_save_source(transition);
		if (!transition_data) {
			continue;
		}
		obs_data_array_push_back(transition_array, transition_data);
		obs_data_release(transition_data);
	}
	obs_data_set_array(settings, "transitions", transition_array);
	obs_data_array_release(transition_array);

	if (transition) {
		obs_data_set_string(settings, "transition", transition->currentText().toUtf8().constData());
	}
	obs_data_set_int(settings, "transition_duration", transitionDurationMs);
}

void CanvasDock::DrawBackdrop(float cx, float cy)
{
	if (!box) {
		return;
	}

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawBackdrop");

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	vec4 colorVal;
	vec4_set(&colorVal, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color, &colorVal);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_scale3f(float(cx), float(cy), 1.0f);

	gs_load_vertexbuffer(box);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);

	gs_load_vertexbuffer(nullptr);

	GS_DEBUG_MARKER_END();
}

void CanvasDock::DrawSpacingHelpers(obs_scene_t *s, float x, float y, float cx, float cy, float scale, float sourceX, float sourceY)
{
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);
	if (locked) {
		return;
	}

	OBSSceneItem item = GetSelectedItem(scene);
	if (!item) {
		return;
	}

	if (obs_sceneitem_locked(item)) {
		return;
	}

	vec2 itemSize = GetItemSize(item);
	if (itemSize.x == 0.0f || itemSize.y == 0.0f) {
		return;
	}

	obs_sceneitem_t *parentGroup = obs_sceneitem_get_group(s, item);

	if (parentGroup && obs_sceneitem_locked(parentGroup)) {
		return;
	}

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3 size;
	vec3_set(&size, sourceX, sourceY, 1.0f);

	// Init box transform side locations
	vec3 left, right, top, bottom;

	vec3_set(&left, 0.0f, 0.5f, 1.0f);
	vec3_set(&right, 1.0f, 0.5f, 1.0f);
	vec3_set(&top, 0.5f, 0.0f, 1.0f);
	vec3_set(&bottom, 0.5f, 1.0f, 1.0f);

	// Decide which side to use with box transform, based on rotation
	// Seems hacky, probably a better way to do it
	float rot = obs_sceneitem_get_rot(item);

	if (parentGroup) {

		//Correct the scene item rotation angle
		rot += obs_sceneitem_get_rot(parentGroup);

		vec2 group_scale;
		obs_sceneitem_get_scale(parentGroup, &group_scale);

		vec2 group_pos;
		obs_sceneitem_get_pos(parentGroup, &group_pos);

		// Correct the scene item box transform
		// Based on scale, rotation angle, position of parent's group
		matrix4_scale3f(&boxTransform, &boxTransform, group_scale.x, group_scale.y, 1.0f);
		matrix4_rotate_aa4f(&boxTransform, &boxTransform, 0.0f, 0.0f, 1.0f, RAD(obs_sceneitem_get_rot(parentGroup)));
		matrix4_translate3f(&boxTransform, &boxTransform, group_pos.x, group_pos.y, 0.0f);
	}

	if (rot >= HELPER_ROT_BREAKPONT) {
		for (float i = HELPER_ROT_BREAKPONT; i <= 360.0f; i += 90.0f) {
			if (rot < i) {
				break;
			}

			vec3 l = left;
			vec3 r = right;
			vec3 t = top;
			vec3 b = bottom;

			vec3_copy(&top, &l);
			vec3_copy(&right, &t);
			vec3_copy(&bottom, &r);
			vec3_copy(&left, &b);
		}
	} else if (rot <= -HELPER_ROT_BREAKPONT) {
		for (float i = -HELPER_ROT_BREAKPONT; i >= -360.0f; i -= 90.0f) {
			if (rot > i) {
				break;
			}

			vec3 l = left;
			vec3 r = right;
			vec3 t = top;
			vec3 b = bottom;

			vec3_copy(&top, &r);
			vec3_copy(&right, &b);
			vec3_copy(&bottom, &l);
			vec3_copy(&left, &t);
		}
	}
	vec2 item_scale;
	obs_sceneitem_get_scale(item, &item_scale);

	// Switch top/bottom or right/left if scale is negative
	if (item_scale.x < 0.0f) {
		vec3 l = left;
		vec3 r = right;

		vec3_copy(&left, &r);
		vec3_copy(&right, &l);
	}

	if (item_scale.y < 0.0f) {
		vec3 t = top;
		vec3 b = bottom;

		vec3_copy(&top, &b);
		vec3_copy(&bottom, &t);
	}

	// Get sides of box transform
	left = GetTransformedPos(left.x, left.y, boxTransform);
	right = GetTransformedPos(right.x, right.y, boxTransform);
	top = GetTransformedPos(top.x, top.y, boxTransform);
	bottom = GetTransformedPos(bottom.x, bottom.y, boxTransform);

	bottom.y = size.y - bottom.y;
	right.x = size.x - right.x;

	// Init viewport
	vec3 viewport;
	vec3_set(&viewport, cx, cy, 1.0f);

	vec3_div(&left, &left, &viewport);
	vec3_div(&right, &right, &viewport);
	vec3_div(&top, &top, &viewport);
	vec3_div(&bottom, &bottom, &viewport);

	vec3_mulf(&left, &left, scale);
	vec3_mulf(&right, &right, scale);
	vec3_mulf(&top, &top, scale);
	vec3_mulf(&bottom, &bottom, scale);

	// Draw spacer lines and labels
	vec3 start, end;

	float pixelRatio = 1.0f; //main->GetDevicePixelRatio();
	if (!spacerLabel[3]) {
		QMetaObject::invokeMethod(this, [this, pixelRatio]() {
			for (int i = 0; i < 4; i++) {
				if (!spacerLabel[i]) {
					spacerLabel[i] = CreateLabel(pixelRatio, i);
				}
			}
		});
		return;
	}

	vec3_set(&start, top.x, 0.0f, 1.0f);
	vec3_set(&end, top.x, top.y, 1.0f);
	RenderSpacingHelper(0, start, end, viewport, pixelRatio);

	vec3_set(&start, bottom.x, 1.0f - bottom.y, 1.0f);
	vec3_set(&end, bottom.x, 1.0f, 1.0f);
	RenderSpacingHelper(1, start, end, viewport, pixelRatio);

	vec3_set(&start, 0.0f, left.y, 1.0f);
	vec3_set(&end, left.x, left.y, 1.0f);
	RenderSpacingHelper(2, start, end, viewport, pixelRatio);

	vec3_set(&start, 1.0f - right.x, right.y, 1.0f);
	vec3_set(&end, 1.0f, right.y, 1.0f);
	RenderSpacingHelper(3, start, end, viewport, pixelRatio);
}

struct SceneFindData {
	const vec2 &pos;
	OBSSceneItem item;
	bool selectBelow;

	obs_sceneitem_t *group = nullptr;

	SceneFindData(const SceneFindData &) = delete;
	SceneFindData(SceneFindData &&) = delete;
	SceneFindData &operator=(const SceneFindData &) = delete;
	SceneFindData &operator=(SceneFindData &&) = delete;

	inline SceneFindData(const vec2 &pos_, bool selectBelow_) : pos(pos_), selectBelow(selectBelow_) {}
};

struct SceneFindBoxData {
	const vec2 &startPos;
	const vec2 &pos;
	std::vector<obs_sceneitem_t *> sceneItems;

	SceneFindBoxData(const SceneFindData &) = delete;
	SceneFindBoxData(SceneFindData &&) = delete;
	SceneFindBoxData &operator=(const SceneFindData &) = delete;
	SceneFindBoxData &operator=(SceneFindData &&) = delete;

	inline SceneFindBoxData(const vec2 &startPos_, const vec2 &pos_) : startPos(startPos_), pos(pos_) {}
};

obs_scene_item *CanvasDock::GetSelectedItem(obs_scene_t *s)
{
	vec2 pos;
	SceneFindBoxData sfbd(pos, pos);

	if (!s) {
		return nullptr;
	}
	obs_scene_enum_items(s, FindSelected, &sfbd);

	if (sfbd.sceneItems.size() != 1) {
		return nullptr;
	}

	return sfbd.sceneItems.at(0);
}

bool CanvasDock::FindSelected(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	SceneFindBoxData *data = reinterpret_cast<SceneFindBoxData *>(param);

	if (obs_sceneitem_selected(item)) {
		data->sceneItems.push_back(item);
	}

	UNUSED_PARAMETER(scene);
	return true;
}

vec2 CanvasDock::GetItemSize(obs_sceneitem_t *item)
{
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(item);
	vec2 size;

	if (boundsType != OBS_BOUNDS_NONE) {
		obs_sceneitem_get_bounds(item, &size);
	} else {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		vec2 scale;

		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_crop(item, &crop);
		size.x = float(obs_source_get_width(source) - crop.left - crop.right) * scale.x;
		size.y = float(obs_source_get_height(source) - crop.top - crop.bottom) * scale.y;
	}

	return size;
}

vec3 CanvasDock::GetTransformedPos(float x, float y, const matrix4 &mat)
{
	vec3 result;
	vec3_set(&result, x, y, 0.0f);
	vec3_transform(&result, &result, &mat);
	return result;
}

obs_source_t *CanvasDock::CreateLabel(float pixelRatio, int i)
{
	OBSDataAutoRelease settings = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();

#if defined(_WIN32)
	obs_data_set_string(font, "face", "Arial");
#elif defined(__APPLE__)
	obs_data_set_string(font, "face", "Helvetica");
#else
	obs_data_set_string(font, "face", "Monospace");
#endif
	obs_data_set_int(font, "flags", 1); // Bold text
	obs_data_set_int(font, "size", (int)(16.0f * pixelRatio));

	obs_data_set_obj(settings, "font", font);
	obs_data_set_bool(settings, "outline", true);

#ifdef _WIN32
	obs_data_set_int(settings, "outline_color", 0x000000);
	obs_data_set_int(settings, "outline_size", 3);
	const char *text_source_id = "text_gdiplus";
#else
	const char *text_source_id = "text_ft2_source";
#endif

	struct dstr name;
	dstr_init(&name);
	dstr_printf(&name, "Aitum Stream Suite Preview spacing label %d", i);
	auto v_id = obs_get_latest_input_type_id(text_source_id);
	OBSSource txtSource = obs_source_create_private(v_id, name.array, settings);
	dstr_free(&name);
	return txtSource;
}

void CanvasDock::RenderSpacingHelper(int sourceIndex, vec3 &start, vec3 &end, vec3 &viewport, float pixelRatio)
{
	bool horizontal = (sourceIndex == 2 || sourceIndex == 3);

	// If outside of preview, don't render
	if (!((horizontal && (end.x >= start.x)) || (!horizontal && (end.y >= start.y)))) {
		return;
	}

	float length = vec3_dist(&start, &end);

	float px;

	if (settings) {
		if (horizontal) {
			px = length * (float)canvas_width;
		} else {
			px = length * (float)canvas_height;
		}
	} else {
		obs_source_t *source = obs_scene_get_source(scene);
		if (horizontal) {
			px = length * (float)obs_source_get_width(source);
		} else {
			px = length * (float)obs_source_get_height(source);
		}
	}

	if (px <= 0.0f) {
		return;
	}

	obs_source_t *s = spacerLabel[sourceIndex];
	vec3 labelSize, labelPos;
	vec3_set(&labelSize, (float)obs_source_get_width(s), (float)obs_source_get_height(s), 1.0f);

	vec3_div(&labelSize, &labelSize, &viewport);

	vec3 labelMargin;
	vec3_set(&labelMargin, SPACER_LABEL_MARGIN * pixelRatio, SPACER_LABEL_MARGIN * pixelRatio, 1.0f);
	vec3_div(&labelMargin, &labelMargin, &viewport);

	vec3_set(&labelPos, end.x, end.y, end.z);
	if (horizontal) {
		labelPos.x -= (end.x - start.x) / 2;
		labelPos.x -= labelSize.x / 2;
		labelPos.y -= labelMargin.y + (labelSize.y / 2) + (HANDLE_RADIUS / viewport.y);
	} else {
		labelPos.y -= (end.y - start.y) / 2;
		labelPos.y -= labelSize.y / 2;
		labelPos.x += labelMargin.x;
	}

	DrawSpacingLine(start, end, viewport, pixelRatio);
	SetLabelText(sourceIndex, (int)px);
	DrawLabel(s, labelPos, viewport);
}

void CanvasDock::DrawSpacingLine(vec3 &start, vec3 &end, vec3 &viewport, float pixelRatio)
{
	matrix4 transform;
	matrix4_identity(&transform);
	transform.x.x = viewport.x;
	transform.y.y = viewport.y;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");

	QColor selColor = GetSelectionColor();
	vec4 color;
	vec4_set(&color, selColor.redF(), selColor.greenF(), selColor.blueF(), 1.0f);

	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);

	gs_matrix_push();
	gs_matrix_mul(&transform);

	vec2 scale;
	vec2_set(&scale, viewport.x, viewport.y);

	DrawLine(start.x, start.y, end.x, end.y, pixelRatio * (HANDLE_RADIUS / 2), scale);

	gs_matrix_pop();

	gs_load_vertexbuffer(nullptr);

	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

config_t *CanvasDock::GetUserConfig(void)
{
	return obs_frontend_get_user_config();
}

QColor CanvasDock::GetSelectionColor() const
{
	auto config = GetUserConfig();
	if (config && config_get_bool(config, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(config, "Accessibility", "SelectRed"));
	}
	return QColor::fromRgb(255, 0, 0);
}

QColor CanvasDock::GetCropColor() const
{
	auto config = GetUserConfig();
	if (config && config_get_bool(config, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(config, "Accessibility", "SelectGreen"));
	}
	return QColor::fromRgb(0, 255, 0);
}

QColor CanvasDock::GetHoverColor() const
{
	auto config = GetUserConfig();
	if (config && config_get_bool(config, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(config, "Accessibility", "SelectBlue"));
	}
	return QColor::fromRgb(0, 127, 255);
}

void CanvasDock::SetLabelText(int sourceIndex, int px)
{

	if (px == spacerPx[sourceIndex]) {
		return;
	}

	std::string text = std::to_string(px) + " px";

	obs_source_t *s = spacerLabel[sourceIndex];

	OBSDataAutoRelease settings = obs_source_get_settings(s);
	obs_data_set_string(settings, "text", text.c_str());
	obs_source_update(s, settings);

	spacerPx[sourceIndex] = px;
}

void CanvasDock::DrawLabel(OBSSource source, vec3 &pos, vec3 &viewport)
{
	if (!source) {
		return;
	}

	vec3_mul(&pos, &pos, &viewport);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
	obs_source_video_render(source);
	gs_matrix_pop();
}

void CanvasDock::DrawLine(float x1, float y1, float x2, float y2, float thickness, vec2 scale)
{
	float ySide = (y1 == y2) ? (y1 < 0.5f ? 1.0f : -1.0f) : 0.0f;
	float xSide = (x1 == x2) ? (x1 < 0.5f ? 1.0f : -1.0f) : 0.0f;

	gs_render_start(true);

	gs_vertex2f(x1, y1);
	gs_vertex2f(x1 + (xSide * (thickness / scale.x)), y1 + (ySide * (thickness / scale.y)));
	gs_vertex2f(x2 + (xSide * (thickness / scale.x)), y2 + (ySide * (thickness / scale.y)));
	gs_vertex2f(x2, y2);
	gs_vertex2f(x1, y1);

	gs_vertbuffer_t *line = gs_render_save();

	gs_load_vertexbuffer(line);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_vertexbuffer_destroy(line);
}

void CanvasDock::DrawRotationHandle(gs_vertbuffer_t *circle, float rot, float pixelRatio)
{
	struct vec3 pos;
	vec3_set(&pos, 0.5f, 0.0f, 0.0f);

	struct matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_render_start(true);

	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, 0.5f);
	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, -2.0f);
	gs_vertex2f(0.5f + 0.34f / HANDLE_RADIUS, -2.0f);
	gs_vertex2f(0.5f + 0.34f / HANDLE_RADIUS, 0.5f);
	gs_vertex2f(0.5f - 0.34f / HANDLE_RADIUS, 0.5f);

	gs_vertbuffer_t *line = gs_render_save();

	gs_load_vertexbuffer(line);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);

	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(rot));
	gs_matrix_translate3f(-HANDLE_RADIUS * 1.5f * pixelRatio, -HANDLE_RADIUS * 1.5f * pixelRatio, 0.0f);
	gs_matrix_scale3f(HANDLE_RADIUS * 3 * pixelRatio, HANDLE_RADIUS * 3 * pixelRatio, 1.0f);

	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_translate3f(0.0f, -HANDLE_RADIUS * 2 / 3, 0.0f);

	gs_load_vertexbuffer(circle);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
	gs_vertexbuffer_destroy(line);
}

void CanvasDock::DrawStripedLine(float x1, float y1, float x2, float y2, float thickness, vec2 scale)
{
	float ySide = (y1 == y2) ? (y1 < 0.5f ? 1.0f : -1.0f) : 0.0f;
	float xSide = (x1 == x2) ? (x1 < 0.5f ? 1.0f : -1.0f) : 0.0f;

	float dist = sqrtf(powf((x1 - x2) * scale.x, 2.0f) + powf((y1 - y2) * scale.y, 2.0f));
	if (dist > 1000000.0f) {
		// too many stripes to draw, draw it as a line as fallback
		DrawLine(x1, y1, x2, y2, thickness, scale);
		return;
	}

	float offX = (x2 - x1) / dist;
	float offY = (y2 - y1) / dist;

	for (int i = 0, l = (int)ceil(dist / 15.0); i < l; i++) {
		gs_render_start(true);

		float xx1 = x1 + (float)i * 15.0f * offX;
		float yy1 = y1 + (float)i * 15.0f * offY;

		float dx;
		float dy;

		if (x1 < x2) {
			dx = std::min(xx1 + 7.5f * offX, x2);
		} else {
			dx = std::max(xx1 + 7.5f * offX, x2);
		}

		if (y1 < y2) {
			dy = std::min(yy1 + 7.5f * offY, y2);
		} else {
			dy = std::max(yy1 + 7.5f * offY, y2);
		}

		gs_vertex2f(xx1, yy1);
		gs_vertex2f(xx1 + (xSide * (thickness / scale.x)), yy1 + (ySide * (thickness / scale.y)));
		gs_vertex2f(dx, dy);
		gs_vertex2f(dx + (xSide * (thickness / scale.x)), dy + (ySide * (thickness / scale.y)));

		gs_vertbuffer_t *line = gs_render_save();

		gs_load_vertexbuffer(line);
		gs_draw(GS_TRISTRIP, 0, 0);
		gs_vertexbuffer_destroy(line);
	}
}

void CanvasDock::DrawRect(float thickness, vec2 scale)
{
	if (scale.x <= 0.0f || scale.y <= 0.0f || thickness <= 0.0f) {
		return;
	}
	gs_render_start(true);

	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f + (thickness / scale.x), 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(0.0f + (thickness / scale.x), 1.0f);
	gs_vertex2f(0.0f, 1.0f - (thickness / scale.y));
	gs_vertex2f(1.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f - (thickness / scale.y));
	gs_vertex2f(1.0f - (thickness / scale.x), 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f - (thickness / scale.x), 0.0f);
	gs_vertex2f(1.0f, 0.0f + (thickness / scale.y));
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 0.0f + (thickness / scale.y));

	gs_vertbuffer_t *rect = gs_render_save();

	gs_load_vertexbuffer(rect);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_vertexbuffer_destroy(rect);
}

bool CanvasDock::DrawSelectionBox(float x1, float y1, float x2, float y2, gs_vertbuffer_t *rect_fill)
{
	float pixelRatio = GetDevicePixelRatio();

	x1 = std::round(x1);
	x2 = std::round(x2);
	y1 = std::round(y1);
	y2 = std::round(y2);

	gs_effect_t *eff = gs_get_effect();
	gs_eparam_t *colParam = gs_effect_get_param_by_name(eff, "color");

	vec4 fillColor;
	vec4_set(&fillColor, 0.7f, 0.7f, 0.7f, 0.5f);

	vec4 borderColor;
	vec4_set(&borderColor, 1.0f, 1.0f, 1.0f, 1.0f);

	vec2 scale;
	vec2_set(&scale, std::abs(x2 - x1), std::abs(y2 - y1));

	gs_matrix_push();
	gs_matrix_identity();

	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_matrix_scale3f(x2 - x1, y2 - y1, 1.0f);

	gs_effect_set_vec4(colParam, &fillColor);
	gs_load_vertexbuffer(rect_fill);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_effect_set_vec4(colParam, &borderColor);
	DrawRect(HANDLE_RADIUS * pixelRatio / 2, scale);

	gs_matrix_pop();

	return true;
}

float CanvasDock::GetDevicePixelRatio()
{
	return 1.0f;
}

bool CanvasDock::DrawSelectedItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	if (!SceneItemHasVideo(item)) {
		return true;
	}

	CanvasDock *window = static_cast<CanvasDock *>(param);

	if (obs_sceneitem_is_group(item)) {
		matrix4 mat;
		obs_sceneitem_get_draw_transform(item, &mat);

		window->groupRot = obs_sceneitem_get_rot(item);

		gs_matrix_push();
		gs_matrix_mul(&mat);
		obs_sceneitem_group_enum_items(item, DrawSelectedItem, param);
		gs_matrix_pop();

		window->groupRot = 0.0f;
	}

	float pixelRatio = window->GetDevicePixelRatio();

	bool hovered = false;
	{
		std::lock_guard<std::mutex> lock(window->selectMutex);
		for (size_t i = 0; i < window->hoveredPreviewItems.size(); i++) {
			if (window->hoveredPreviewItems[i] == item) {
				hovered = true;
				break;
			}
		}
	}

	bool selected = obs_sceneitem_selected(item);

	if (!selected && !hovered) {
		return true;
	}

	matrix4 boxTransform;
	matrix4 invBoxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);
	matrix4_inv(&invBoxTransform, &boxTransform);

	vec3 bounds[] = {
		{{{0.f, 0.f, 0.f}}},
		{{{1.f, 0.f, 0.f}}},
		{{{0.f, 1.f, 0.f}}},
		{{{1.f, 1.f, 0.f}}},
	};

	//main->GetCameraIcon();

	QColor selColor = window->GetSelectionColor();
	QColor cropColor = window->GetCropColor();
	QColor hoverColor = window->GetHoverColor();

	vec4 red;
	vec4 green;
	vec4 blue;

	vec4_set(&red, selColor.redF(), selColor.greenF(), selColor.blueF(), 1.0f);
	vec4_set(&green, cropColor.redF(), cropColor.greenF(), cropColor.blueF(), 1.0f);
	vec4_set(&blue, hoverColor.redF(), hoverColor.greenF(), hoverColor.blueF(), 1.0f);

	bool visible = std::all_of(std::begin(bounds), std::end(bounds), [&](const vec3 &b) {
		vec3 pos;
		vec3_transform(&pos, &b, &boxTransform);
		vec3_transform(&pos, &pos, &invBoxTransform);
		return CloseFloat(pos.x, b.x) && CloseFloat(pos.y, b.y);
	});

	if (!visible) {
		return true;
	}

	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "DrawSelectedItem");

	matrix4 curTransform;
	vec2 boxScale;
	gs_matrix_get(&curTransform);
	obs_sceneitem_get_box_scale(item, &boxScale);
	boxScale.x *= curTransform.x.x;
	boxScale.y *= curTransform.y.y;

	gs_matrix_push();
	gs_matrix_mul(&boxTransform);

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	gs_effect_t *eff = gs_get_effect();
	gs_eparam_t *colParam = gs_effect_get_param_by_name(eff, "color");

	gs_effect_set_vec4(colParam, &red);

	if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE && crop_enabled(&crop)) {
#define DRAW_SIDE(side, x1, y1, x2, y2)                                                   \
	if (hovered && !selected) {                                                       \
		gs_effect_set_vec4(colParam, &blue);                                      \
		DrawLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2, boxScale);        \
	} else if (crop.side > 0) {                                                       \
		gs_effect_set_vec4(colParam, &green);                                     \
		DrawStripedLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2, boxScale); \
	} else {                                                                          \
		DrawLine(x1, y1, x2, y2, HANDLE_RADIUS *pixelRatio / 2, boxScale);        \
	}                                                                                 \
	gs_effect_set_vec4(colParam, &red);

		DRAW_SIDE(left, 0.0f, 0.0f, 0.0f, 1.0f);
		DRAW_SIDE(top, 0.0f, 0.0f, 1.0f, 0.0f);
		DRAW_SIDE(right, 1.0f, 0.0f, 1.0f, 1.0f);
		DRAW_SIDE(bottom, 0.0f, 1.0f, 1.0f, 1.0f);
#undef DRAW_SIDE
	} else {
		if (!selected) {
			gs_effect_set_vec4(colParam, &blue);
			DrawRect(HANDLE_RADIUS * pixelRatio / 2, boxScale);
		} else {
			DrawRect(HANDLE_RADIUS * pixelRatio / 2, boxScale);
		}
	}

	gs_load_vertexbuffer(window->box);
	gs_effect_set_vec4(colParam, &red);

	if (selected) {
		DrawSquareAtPos(0.0f, 0.0f, pixelRatio);
		DrawSquareAtPos(0.0f, 1.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 0.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 1.0f, pixelRatio);
		DrawSquareAtPos(0.5f, 0.0f, pixelRatio);
		DrawSquareAtPos(0.0f, 0.5f, pixelRatio);
		DrawSquareAtPos(0.5f, 1.0f, pixelRatio);
		DrawSquareAtPos(1.0f, 0.5f, pixelRatio);

		if (!window->circleFill) {
			gs_render_start(true);

			float angle = 180;
			for (int i = 0, l = 40; i < l; i++) {
				gs_vertex2f(sin(RAD(angle)) / 2 + 0.5f, cos(RAD(angle)) / 2 + 0.5f);
				angle += 360.0f / (float)l;
				gs_vertex2f(sin(RAD(angle)) / 2 + 0.5f, cos(RAD(angle)) / 2 + 0.5f);
				gs_vertex2f(0.5f, 1.0f);
			}

			window->circleFill = gs_render_save();
		}

		DrawRotationHandle(window->circleFill, obs_sceneitem_get_rot(item) + window->groupRot, pixelRatio);
	}

	gs_matrix_pop();

	GS_DEBUG_MARKER_END();

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::SceneItemHasVideo(obs_sceneitem_t *item)
{
	const obs_source_t *source = obs_sceneitem_get_source(item);
	const uint32_t flags = obs_source_get_output_flags(source);
	return (flags & OBS_SOURCE_VIDEO) != 0;
}

bool CanvasDock::CloseFloat(float a, float b, float epsilon)
{
	return std::abs(a - b) <= epsilon;
}

inline bool CanvasDock::crop_enabled(const obs_sceneitem_crop *crop)
{
	return crop->left > 0 || crop->top > 0 || crop->right > 0 || crop->bottom > 0;
}

void CanvasDock::DrawSquareAtPos(float x, float y, float pixelRatio)
{
	struct vec3 pos;
	vec3_set(&pos, x, y, 0.0f);

	struct matrix4 matrix;
	gs_matrix_get(&matrix);
	vec3_transform(&pos, &pos, &matrix);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);

	gs_matrix_translate3f(-HANDLE_RADIUS * pixelRatio, -HANDLE_RADIUS * pixelRatio, 0.0f);
	gs_matrix_scale3f(HANDLE_RADIUS * pixelRatio * 2, HANDLE_RADIUS * pixelRatio * 2, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
}

obs_sceneitem_t *CanvasDock::GetCurrentSceneItem()
{
	return sourceList->Get(GetTopSelectedSourceItem());
}

int CanvasDock::GetTopSelectedSourceItem()
{
	QModelIndexList selectedItems = sourceList->selectionModel()->selectedIndexes();
	return selectedItems.count() ? selectedItems[0].row() : -1;
}

void CanvasDock::ChangeSceneIndex(bool relative, int offset, int invalidIdx)
{
	int idx = sceneList->currentRow();
	if (idx < 0) {
		return;
	}

	auto canvasItem = sceneList->item(idx);
	if (!canvasItem) {
		return;
	}

	if (idx == invalidIdx) {
		return;
	}

	sceneList->blockSignals(true);
	auto item = sceneList->takeItem(idx);
	if (relative) {
		sceneList->insertItem(idx + offset, item);
		sceneList->setCurrentRow(idx + offset);
	} else if (offset == 0) {
		sceneList->insertItem(offset, item);
	} else {
		sceneList->insertItem(sceneList->count(), item);
	}
	item->setSelected(true);
	sceneList->blockSignals(false);
	obs_frontend_save();
}

QWidget *CanvasDock::BorrowScenesPane()
{
	if (!scenesGroup) {
		return nullptr;
	}
	scenesGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	return scenesGroup;
}

void CanvasDock::ReturnScenesPane()
{
	if (!scenesGroup) {
		return;
	}
	if (panel_split && scenesGroup->parentWidget() != panel_split) {
		panel_split->insertWidget(0, scenesGroup);
		panel_split->hide();
	}
}

QWidget *CanvasDock::BorrowTransitionsPane()
{
	if (!transitionsGroup) {
		return nullptr;
	}
	transitionsGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	return transitionsGroup;
}

void CanvasDock::ReturnTransitionsPane()
{
	if (!transitionsGroup) {
		return;
	}
	if (panel_split && transitionsGroup->parentWidget() != panel_split) {
		panel_split->addWidget(transitionsGroup);
		panel_split->hide();
	}
}

void CanvasDock::EditSceneName()
{
	QListWidgetItem *item = sceneList->currentItem();
	if (!item)
		return;
	Qt::ItemFlags flags = item->flags();
	item->setFlags(flags | Qt::ItemIsEditable);
	sceneList->editItem(item);
	item->setFlags(flags);
}

void CanvasDock::AddScene(QString duplicate, bool ask_name)
{
	std::string name;
	if (ask_name) {
		QString format = duplicate.isEmpty()
					 ? QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.DefaultSceneName.Text"))
					 : duplicate + QString::fromUtf8(" %1");
		int i = 2;
		QString placeHolder = format.arg(i);
		for (;;) {
			OBSSourceAutoRelease existing = obs_canvas_get_source_by_name(canvas, placeHolder.toUtf8().constData());
			if (!existing)
				existing = obs_get_source_by_name(placeHolder.toUtf8().constData());
			if (!existing)
				break;
			placeHolder = format.arg(++i);
		}
		for (;;) {
			name.clear();
			if (!NameDialog::AskForName(this,
						    QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.AddSceneDlg.Title")),
						    QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.AddSceneDlg.Text")),
						    name, placeHolder))
				return;
			if (name.empty()) {
				QMessageBox::warning(this, QString::fromUtf8(obs_frontend_get_locale_string("NoNameEntered.Title")),
						     QString::fromUtf8(obs_frontend_get_locale_string("NoNameEntered.Text")));
				continue;
			}
			OBSSourceAutoRelease existing = obs_canvas_get_source_by_name(canvas, name.c_str());
			if (!existing)
				existing = obs_get_source_by_name(name.c_str());
			if (existing) {
				QMessageBox::warning(this, QString::fromUtf8(obs_frontend_get_locale_string("NameExists.Title")),
						     QString::fromUtf8(obs_frontend_get_locale_string("NameExists.Text")));
				continue;
			}
			break;
		}
	} else {
		name = duplicate.isEmpty() ? obs_module_text("Scene") : duplicate.toUtf8().constData();
		obs_source_t *s = obs_canvas_get_source_by_name(canvas, name.c_str());
		int i = 0;
		while (s) {
			obs_source_release(s);
			i++;
			name = obs_module_text("Scene");
			name += " ";
			name += std::to_string(i);
			s = obs_canvas_get_source_by_name(canvas, name.c_str());
		}
		obs_source_release(s);
	}

	obs_source_t *new_scene = nullptr;
		if (!duplicate.isEmpty()) {
			auto origSceneSource = obs_canvas_get_source_by_name(canvas, duplicate.toUtf8().constData());
			if (origSceneSource) {
				auto origScene = obs_scene_from_source(origSceneSource);
				if (origScene) {
					new_scene = obs_scene_get_source(
						obs_scene_duplicate(origScene, name.c_str(), OBS_SCENE_DUP_REFS));
				}
				obs_source_release(origSceneSource);
				if (new_scene) {
					obs_source_save(new_scene);
					obs_source_load(new_scene);
				}
			}
		}
		if (!new_scene) {
			obs_scene_t *ns = obs_canvas_scene_create(canvas, name.c_str());
			new_scene = obs_scene_get_source(ns);
			obs_source_load(new_scene);

			std::string ssn = obs_canvas_get_name(canvas);
			ssn += " ";
			ssn += obs_frontend_get_locale_string("Basic.Hotkeys.SelectScene");
			obs_hotkey_register_source(
				new_scene, "OBSBasic.SelectScene", ssn.c_str(),
				[](void *data, obs_hotkey_id, obs_hotkey_t *key, bool pressed) {
					if (!pressed) {
						return;
					}
					auto p = (CanvasDock *)data;
					auto potential_source = (obs_weak_source_t *)obs_hotkey_get_registerer(key);
					OBSSourceAutoRelease source = obs_weak_source_get_source(potential_source);
					if (source) {
						auto sn = QString::fromUtf8(obs_source_get_name(source));
						QMetaObject::invokeMethod(p, "SwitchScene", Q_ARG(QString, sn), Q_ARG(bool, true));
					}
				},
				this);
			auto sh = obs_source_get_signal_handler(new_scene);
			signal_handler_connect(sh, "rename", source_rename, this);
			signal_handler_connect(sh, "remove", source_remove, this);
		}
		if (vendor) {
			const auto d = obs_data_create();
			obs_data_set_string(d, "canvas", obs_canvas_get_name(canvas));
			obs_data_set_string(d, "name", obs_source_get_name(new_scene));
			obs_data_set_string(d, "uuid", obs_source_get_uuid(new_scene));
			obs_websocket_vendor_emit_event(vendor, "scene_added", d);
			obs_data_release(d);
		}
		auto sn = QString::fromUtf8(obs_source_get_name(new_scene));
		SwitchScene(sn);
		obs_source_release(new_scene);
}

void CanvasDock::RemoveScene(const QString &sceneName)
{
	auto s = obs_canvas_get_source_by_name(canvas, sceneName.toUtf8().constData());
	if (!s) {
		return;
	}
	if (!obs_source_is_scene(s)) {
		obs_source_release(s);
		return;
	}

	if (sceneList && sceneList->count() <= 1) {
		QMessageBox::information(this, QString::fromUtf8(obs_frontend_get_locale_string("FinalScene.Title")),
					 QString::fromUtf8(obs_frontend_get_locale_string("FinalScene.Text")));
		obs_source_release(s);
		return;
	}

	QMessageBox remove_source(this);
	remove_source.setText(QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Text"))
				      .arg(QString::fromUtf8(obs_source_get_name(s))));
	QPushButton *Yes =
		remove_source.addButton(QString::fromUtf8(obs_frontend_get_locale_string("Yes")), QMessageBox::YesRole);
	remove_source.setDefaultButton(Yes);
	remove_source.addButton(QString::fromUtf8(obs_frontend_get_locale_string("No")), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Title")));
	remove_source.exec();
	if (Yes == remove_source.clickedButton()) {
		obs_source_remove(s);
		if (vendor) {
			const auto d = obs_data_create();
			obs_data_set_string(d, "canvas", obs_canvas_get_name(canvas));
			obs_data_set_string(d, "name", obs_source_get_name(s));
			obs_data_set_string(d, "uuid", obs_source_get_uuid(s));
			obs_websocket_vendor_emit_event(vendor, "scene_removed", d);
			obs_data_release(d);
		}
	}

	obs_source_release(s);
}

obs_source_t *CanvasDock::GetEditingSceneSource() const
{
	return obs_source_get_ref(obs_scene_get_source(scene));
}

void CanvasDock::SwitchScene(const QString &scene_name, bool transition)
{
	EnsureOutputTransition();
	auto s = scene_name.isEmpty() ? nullptr : obs_canvas_get_source_by_name(canvas, scene_name.toUtf8().constData());
	if (s == obs_scene_get_source(scene) || (!obs_source_is_scene(s) && !scene_name.isEmpty())) {
		obs_source_release(s);
		return;
	}
	auto oldSource = obs_scene_get_source(scene);
	auto sh = oldSource ? obs_source_get_signal_handler(oldSource) : nullptr;
	if (sh) {
		signal_handler_disconnect(sh, "item_add", SceneItemAdded, this);
		signal_handler_disconnect(sh, "reorder", SceneReordered, this);
		signal_handler_disconnect(sh, "refresh", SceneRefreshed, this);
	}
	if (!source || obs_weak_source_references_source(source, oldSource)) {
		obs_weak_source_release(source);
		source = obs_source_get_weak_source(s);
		if (canvas) {
			obs_canvas_set_channel(canvas, 0, s);
		}
	} else {
		oldSource = obs_weak_source_get_source(source);
		if (oldSource) {
			auto ost = obs_source_get_type(oldSource);
			if (ost == OBS_SOURCE_TYPE_TRANSITION) {
				auto private_settings = s ? obs_source_get_private_settings(s) : nullptr;
				obs_source_t *override_transition =
					GetTransition(obs_data_get_string(private_settings, "transition"));
				if (SwapTransition(override_transition)) {
					obs_source_release(oldSource);
					oldSource = obs_weak_source_get_source(source);
					signal_handler_t *handler = obs_source_get_signal_handler(oldSource);
					signal_handler_connect(handler, "transition_stop", transition_override_stop, this);
				}
				int duration = 0;
				if (override_transition) {
					duration = (int)obs_data_get_int(private_settings, "transition_duration");
				}
				if (duration <= 0) {
					duration = transitionDurationMs > 0 ? transitionDurationMs
									    : obs_frontend_get_transition_duration();
				}
				obs_data_release(private_settings);

				auto sourceA = obs_transition_get_source(oldSource, OBS_TRANSITION_SOURCE_A);
				if (sourceA != obs_scene_get_source(scene)) {
					obs_transition_set(oldSource, obs_scene_get_source(scene));
				}
				obs_source_release(sourceA);
				if (transition) {
					obs_transition_start(oldSource, OBS_TRANSITION_MODE_AUTO, duration, s);
				} else {
					obs_transition_set(oldSource, s);
				}
			} else {
				obs_weak_source_release(source);
				source = obs_source_get_weak_source(s);
				if (canvas) {
					obs_canvas_set_channel(canvas, 0, s);
				}
			}
			obs_source_release(oldSource);
		} else {
			obs_weak_source_release(source);
			source = obs_source_get_weak_source(s);
			if (canvas) {
				obs_canvas_set_channel(canvas, 0, s);
			}
		}
	}
	scene = obs_scene_from_source(s);
	if (scene) {
		sh = obs_source_get_signal_handler(s);
		if (sh) {
			signal_handler_connect(sh, "item_add", SceneItemAdded, this);
			signal_handler_connect(sh, "reorder", SceneReordered, this);
			signal_handler_connect(sh, "refresh", SceneRefreshed, this);
		}
	}
	auto oldName = currentSceneName;
	if (!scene_name.isEmpty()) {
		currentSceneName = scene_name;
	}
	//if (scenesCombo && scenesCombo->currentText() != scene_name) {
	//	scenesCombo->setCurrentText(scene_name);
	//}
	if (!scene_name.isEmpty()) {
		if (sceneList) {
			QListWidgetItem *item = sceneList->currentItem();
			if (!item || item->text() != scene_name) {
				for (int i = 0; i < sceneList->count(); i++) {
					item = sceneList->item(i);
					if (item->text() == scene_name) {
						sceneList->setCurrentRow(i);
						item->setSelected(true);
						break;
					}
				}
			}
		}
		if (sceneCombo) {
			int idx = sceneCombo->findText(scene_name);
			if (idx >= 0 && sceneCombo->currentIndex() != idx) {
				sceneCombo->setCurrentIndex(idx);
			}
		}
	}
	if (sourceList) {
		sourceList->GetStm()->SceneChanged();
	}
	emit editingSceneChanged();

	obs_source_release(s);
	if (vendor && oldName != currentSceneName) {
		const auto d = obs_data_create();
		obs_data_set_int(d, "width", canvas_width);
		obs_data_set_int(d, "height", canvas_height);
		obs_data_set_string(d, "canvas", obs_canvas_get_name(canvas));
		obs_data_set_string(d, "old_scene", oldName.toUtf8().constData());
		obs_data_set_string(d, "new_scene", currentSceneName.toUtf8().constData());
		obs_websocket_vendor_emit_event(vendor, "switch_scene", d);
		obs_data_release(d);
	}
}

void CanvasDock::SceneItemAdded(void *data, calldata_t *params)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "AddSceneItem", Qt::QueuedConnection, Q_ARG(OBSSceneItem, OBSSceneItem(item)),
				  Q_ARG(bool, undoing));
}

void CanvasDock::SceneReordered(void *data, calldata_t *params)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "ReorderSources", Qt::QueuedConnection, Q_ARG(OBSScene, OBSScene(scene)));
}

void CanvasDock::SceneRefreshed(void *data, calldata_t *params)
{
	CanvasDock *window = static_cast<CanvasDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "RefreshSources", Qt::QueuedConnection, Q_ARG(OBSScene, OBSScene(scene)));
}

obs_source_t *CanvasDock::GetTransition(const char *transition_name)
{
	if (!transition_name || !strlen(transition_name)) {
		return nullptr;
	}
	for (auto transition : transitions) {
		if (strcmp(transition_name, obs_source_get_name(transition)) == 0) {
			return transition;
		}
	}
	return nullptr;
}

bool CanvasDock::SwapTransition(obs_source_t *newTransition)
{
	if (!newTransition || obs_weak_source_references_source(source, newTransition)) {
		return false;
	}

	obs_transition_set_size(newTransition, canvas_width, canvas_height);

	obs_source_t *oldTransition = obs_weak_source_get_source(source);
	if (!oldTransition || obs_source_get_type(oldTransition) != OBS_SOURCE_TYPE_TRANSITION) {
		if (oldTransition) {
			obs_transition_set(newTransition, oldTransition);
			obs_source_release(oldTransition);
		}
		obs_weak_source_release(source);
		source = obs_source_get_weak_source(newTransition);
		if (canvas) {
			obs_canvas_set_channel(canvas, 0, newTransition);
		}
		obs_source_inc_showing(newTransition);
		obs_source_inc_active(newTransition);
		RefreshTransition(newTransition);
		return true;
	}
	signal_handler_t *handler = obs_source_get_signal_handler(oldTransition);
	signal_handler_disconnect(handler, "transition_stop", transition_override_stop, this);
	obs_source_inc_showing(newTransition);
	obs_source_inc_active(newTransition);
	obs_transition_swap_begin(newTransition, oldTransition);
	obs_weak_source_release(source);
	source = obs_source_get_weak_source(newTransition);
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, newTransition);
	}
	obs_transition_swap_end(newTransition, oldTransition);
	obs_source_dec_showing(oldTransition);
	obs_source_dec_active(oldTransition);
	obs_source_release(oldTransition);
	RefreshTransition(newTransition);
	return true;
}

void CanvasDock::RefreshTransition(obs_source_t *transitionSource)
{
	if (!transitionSource)
		return;
	/* Properties OK calls obs_source_update. A private transition loaded at
	 * startup never gets that, so stingers and wipes stay inert until the
	 * user opens the dialog. Apply the saved settings once the transition
	 * is active on the canvas. */
	obs_transition_set_size(transitionSource, canvas_width, canvas_height);
	OBSDataAutoRelease transitionSettings = obs_source_get_settings(transitionSource);
	if (transitionSettings)
		obs_source_update(transitionSource, transitionSettings);
	obs_source_load2(transitionSource);
	obs_transition_set_size(transitionSource, canvas_width, canvas_height);
}

void CanvasDock::EnsureOutputTransition()
{
	if (!canvas || !transition)
		return;
	const QByteArray name = transition->currentText().toUtf8();
	obs_source_t *selected = GetTransition(name.constData());
	if (!selected)
		return;

	obs_source_t *current = obs_weak_source_get_source(source);
	const bool already = current && current == selected && obs_source_get_type(current) == OBS_SOURCE_TYPE_TRANSITION;
	obs_source_release(current);
	if (!already)
		SwapTransition(selected);

	obs_source_t *output = obs_weak_source_get_source(source);
	if (output && obs_source_get_type(output) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_canvas_set_channel(canvas, 0, output);
		if (scene) {
			obs_source_t *sceneSource = obs_scene_get_source(scene);
			obs_source_t *active = obs_transition_get_active_source(output);
			if (sceneSource && active != sceneSource)
				obs_transition_set(output, sceneSource);
			obs_source_release(active);
		}
	}
	obs_source_release(output);
}

void CanvasDock::transition_override_stop(void *data, calldata_t *)
{
	auto dock = (CanvasDock *)data;
	QMetaObject::invokeMethod(dock, "SwitchBackToSelectedTransition", Qt::QueuedConnection);
}

QMenu *CanvasDock::CreateAddSourcePopupMenu(QWidget *parent)
{
	const char *unversioned_type;
	const char *type;
	bool foundValues = false;
	bool foundDeprecated = false;
	size_t idx = 0;

	QMenu *popup = new QMenu(QString::fromUtf8(obs_frontend_get_locale_string("Add")), parent);
	QMenu *deprecated = new QMenu(QString::fromUtf8(obs_frontend_get_locale_string("Deprecated")), popup);

	while (obs_enum_input_types2(idx++, &type, &unversioned_type)) {
		const char *name = obs_source_get_display_name(type);
		if (!name) {
			continue;
		}
		uint32_t caps = obs_get_source_output_flags(type);

		if ((caps & OBS_SOURCE_CAP_DISABLED) != 0) {
			continue;
		}

		if ((caps & OBS_SOURCE_DEPRECATED) == 0) {
			AddSourceTypeToMenu(popup, unversioned_type, name);
		} else {
			AddSourceTypeToMenu(deprecated, unversioned_type, name);
			foundDeprecated = true;
		}
		foundValues = true;
	}

	AddSourceTypeToMenu(popup, "scene", obs_frontend_get_locale_string("Basic.Scene"));
	AddSourceTypeToMenu(popup, "group", obs_frontend_get_locale_string("Group"));

	if (!foundDeprecated) {
		delete deprecated;
		deprecated = nullptr;
	}

	if (!foundValues) {
		delete popup;
		popup = nullptr;

	} else if (foundDeprecated) {
		popup->addSeparator();
		popup->addMenu(deprecated);
	}

	return popup;
}

void CanvasDock::check_descendant(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *info = (struct descendant_info *)param;
	if (parent == info->target2 || child == info->target2 || obs_weak_source_references_source(info->target, child) ||
	    obs_weak_source_references_source(info->target, parent)) {
		info->exists = true;
	}
}

bool CanvasDock::add_sources_of_type_to_menu(void *param, obs_source_t *source)
{
	QMenu *menu = static_cast<QMenu *>(param);
	auto parentMenu = qobject_cast<QMenu *>(menu->parent());
	auto a = parentMenu && !parentMenu->menuAction()->data().isNull() ? parentMenu->menuAction() : menu->menuAction();
	QObject *parent = menu->parent();
	auto pm = qobject_cast<QMenu *>(parent);
	while (pm) {
		parent = pm->parent();
		pm = qobject_cast<QMenu *>(parent);
	}
	auto t = a->data().toString();
	auto idUtf8 = t.toUtf8();
	const char *id = idUtf8.constData();
	if (strcmp(obs_source_get_unversioned_id(source), id) == 0) {
		auto name = QString::fromUtf8(obs_source_get_name(source));
		QList<QAction *> actions = menu->actions();
		QAction *after = nullptr;
		for (QAction *menuAction : actions) {
			if (menuAction->text().compare(name, Qt::CaseInsensitive) >= 0) {
				after = menuAction;
				break;
			}
		}
		auto na = new QAction(name, menu);
		QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
		connect(na, &QAction::triggered, parent, [parent, uuid] {
			OBSSourceAutoRelease found = obs_get_source_by_uuid(uuid.toUtf8().constData());
			if (!found) {
				return;
			}
			QMetaObject::invokeMethod(parent, "AddSourceToScene", Qt::QueuedConnection,
						  Q_ARG(OBSSource, OBSSource(found.Get())));
		});
		menu->insertAction(after, na);
		auto cd = qobject_cast<CanvasDock *>(parent);
		if (cd) {
			struct descendant_info info = {false, cd->source, obs_scene_get_source(cd->scene)};
			obs_source_enum_full_tree(source, check_descendant, &info);
			na->setEnabled(!info.exists);
		}
		auto sd = qobject_cast<SourcesDock *>(parent);
		if (sd) {
			auto s = obs_weak_source_get_source(sd->scene);
			struct descendant_info info = {false, sd->scene, s};
			obs_source_enum_full_tree(source, check_descendant, &info);
			na->setEnabled(!info.exists);
			obs_source_release(s);
		}
	}
	return true;
}

void CanvasDock::LoadSourceTypeMenu(QMenu *menu, const char *type)
{
	menu->clear();
	if (obs_get_source_output_flags(type) & OBS_SOURCE_REQUIRES_CANVAS) {
		obs_enum_canvases(
			[](void *param, obs_canvas_t *canvas) {
				QMenu *m = (QMenu *)param;
				auto canvas_name = QString::fromUtf8(obs_canvas_get_name(canvas));
				if (canvas_name == "Components") {
					return true;
				}
				auto cm = new QMenu(canvas_name, m);
				obs_canvas_enum_scenes(canvas, add_sources_of_type_to_menu, cm);
				if (cm->actions().count() == 0) {
					delete cm;
				} else {
					m->addMenu(cm);
				}
				return true;
			},
			menu);
	} else if (strcmp(type, "scene") == 0) {
		obs_enum_scenes(add_sources_of_type_to_menu, menu);
	} else {
		obs_enum_sources(add_sources_of_type_to_menu, menu);

		auto popupItem = new QAction(QString::fromUtf8(obs_frontend_get_locale_string("New")), menu);
		QString typeId = QString::fromUtf8(type);
		popupItem->setData(typeId);
		QObject *parent = menu->parent();
		auto pm = qobject_cast<QMenu *>(parent);
		while (pm) {
			parent = pm->parent();
			pm = qobject_cast<QMenu *>(parent);
		}
		connect(popupItem, &QAction::triggered, parent, [parent, typeId] {
			QMetaObject::invokeMethod(parent, "AddSourceFromAction", Qt::QueuedConnection, Q_ARG(QString, typeId));
		});
		QList<QAction *> actions = menu->actions();
		QAction *first = actions.size() ? actions.first() : nullptr;
		menu->insertAction(first, popupItem);
		menu->insertSeparator(first);
	}
}

void CanvasDock::AddSourceToScene(OBSSource s)
{
	if (!scene || !s) {
		blog(LOG_WARNING, "[Stream Suite] Cannot add existing source: canvas scene is not selected");
		return;
	}
	if (!obs_scene_add(scene, s)) {
		blog(LOG_WARNING, "[Stream Suite] Failed to add existing source '%s' to canvas scene", obs_source_get_name(s));
	}
}

void CanvasDock::AddSourceTypeToMenu(QMenu *popup, const char *source_type, const char *name)
{
	QString qname = QString::fromUtf8(name);
	QAction *popupItem = new QAction(qname, popup);
	if (strcmp(source_type, "scene") == 0) {
		popupItem->setIcon(GetSceneIcon());
	} else if (strcmp(source_type, "group") == 0) {
		popupItem->setIcon(GetGroupIcon());
	} else {
		popupItem->setIcon(GetIconFromType(obs_source_get_icon_type(source_type), source_type));
	}
	popupItem->setData(QString::fromUtf8(source_type));
	QMenu *menu = new QMenu(popup);
	popupItem->setMenu(menu);
	QObject::connect(menu, &QMenu::aboutToShow, [menu, source_type] { LoadSourceTypeMenu(menu, source_type); });
	QList<QAction *> actions = popup->actions();
	QAction *after = nullptr;
	for (QAction *menuAction : actions) {
		if (menuAction->text().compare(name, Qt::CaseInsensitive) >= 0) {
			after = menuAction;
			break;
		}
	}
	popup->insertAction(after, popupItem);
}

bool CanvasDock::selected_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	std::vector<OBSSceneItem> &items = *reinterpret_cast<std::vector<OBSSceneItem> *>(param);

	if (obs_sceneitem_selected(item)) {
		items.emplace_back(item);
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, selected_items, &items);
	}
	return true;
}

void CanvasDock::RemoveSelectedSources(obs_scene_t *scene, QWidget *parent)
{
	if (!scene) {
		return;
	}

	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(scene, selected_items, &items);
	if (items.empty()) {
		return;
	}

	bool confirmed = false;
	if (items.size() > 1) {
		QString text = QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.TextMultiple"))
				       .arg(QString::number(items.size()));
		QMessageBox remove_items(parent);
		remove_items.setText(text);
		QPushButton *Yes = remove_items.addButton(QString::fromUtf8(obs_frontend_get_locale_string("Yes")),
							  QMessageBox::YesRole);
		remove_items.setDefaultButton(Yes);
		remove_items.addButton(QString::fromUtf8(obs_frontend_get_locale_string("No")), QMessageBox::NoRole);
		remove_items.setIcon(QMessageBox::Question);
		remove_items.setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Title")));
		remove_items.exec();
		confirmed = Yes == remove_items.clickedButton();
	} else {
		obs_source_t *source = obs_sceneitem_get_source(items[0]);
		if (source) {
			QString text = QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Text"))
					       .arg(QString::fromUtf8(obs_source_get_name(source)));
			QMessageBox remove_source(parent);
			remove_source.setText(text);
			QPushButton *Yes = remove_source.addButton(QString::fromUtf8(obs_frontend_get_locale_string("Yes")),
								   QMessageBox::YesRole);
			remove_source.setDefaultButton(Yes);
			remove_source.addButton(QString::fromUtf8(obs_frontend_get_locale_string("No")), QMessageBox::NoRole);
			remove_source.setIcon(QMessageBox::Question);
			remove_source.setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string("ConfirmRemove.Title")));
			remove_source.exec();
			confirmed = Yes == remove_source.clickedButton();
		}
	}
	if (!confirmed) {
		return;
	}

	std::string undo_json = backup_scene(scene);
	for (auto &item : items) {
		obs_sceneitem_t *group = obs_sceneitem_get_group(scene, item);
		if (group) {
			obs_sceneitem_group_remove_item(group, item);
		}
		obs_sceneitem_remove(item);
	}
	std::string redo_json = backup_scene(scene);

	QString action_name;
	if (items.size() > 1) {
		action_name = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Sources.Multi"))
				      .arg(QString::number(items.size()));
	} else {
		action_name = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Delete"))
				      .arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(items[0]))));
	}
	obs_frontend_add_undo_redo_action(action_name.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
					  undo_json.c_str(), redo_json.c_str(), false);
}

void CanvasDock::ShowScenesContextMenu(QListWidgetItem *widget_item)
{
	auto menu = QMenu(this);
	QString addLabel = QString::fromUtf8(obs_frontend_get_locale_string("AddScene"));
	addLabel += QString::fromUtf8("...");
	menu.addAction(addLabel, [this] { AddScene(); });
	if (!widget_item) {
		menu.addSeparator();
		menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string(IsGridMode() ? "Basic.Main.ListMode"
											     : "Basic.Main.GridMode")),
			       [this] { SetGridMode(!IsGridMode()); });
		menu.exec(QCursor::pos());
		return;
	}
	static obs_weak_source_t *copied_filters = nullptr;
	OBSSourceAutoRelease sceneSource =
		obs_canvas_get_source_by_name(canvas, widget_item->text().toUtf8().constData());
	menu.addSeparator();
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Duplicate")), [this] {
		auto item = sceneList->currentItem();
		if (!item) {
			return;
		}
		AddScene(item->text());
	});
	auto copyFilters = menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Copy.Filters")), [this] {
		auto item = sceneList->currentItem();
		if (!item) {
			return;
		}
		OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
		if (!source || obs_source_filter_count(source) < 1) {
			return;
		}
		obs_weak_source_release(copied_filters);
		copied_filters = obs_source_get_weak_source(source);
	});
	copyFilters->setEnabled(sceneSource && obs_source_filter_count(sceneSource) > 0);
	auto pasteFilters = menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Paste.Filters")), [this] {
		auto item = sceneList->currentItem();
		if (!item || !copied_filters) {
			return;
		}
		OBSSourceAutoRelease source = obs_weak_source_get_source(copied_filters);
		OBSSourceAutoRelease dst = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
		if (!source || !dst || source == dst) {
			return;
		}
		obs_source_copy_filters(dst, source);
	});
	pasteFilters->setEnabled(copied_filters && !obs_weak_source_expired(copied_filters));
	menu.addSeparator();
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Rename")), this, &CanvasDock::EditSceneName);
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Remove")), [this] {
		auto item = sceneList->currentItem();
		if (!item) {
			return;
		}
		RemoveScene(item->text());
	});
	menu.addSeparator();
	auto orderMenu = menu.addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order")));
	orderMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveUp")),
			     [this] { ChangeSceneIndex(true, -1, 0); });
	orderMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveDown")),
			     [this] { ChangeSceneIndex(true, 1, sceneList->count() - 1); });
	orderMenu->addSeparator();
	orderMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveToTop")),
			     [this] { ChangeSceneIndex(false, 0, 0); });
	orderMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveToBottom")),
			     [this] { ChangeSceneIndex(false, 1, sceneList->count() - 1); });
	menu.addSeparator();

	auto projectorMenu = menu.addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Open.Scene")));
	AddProjectorMenuMonitors(projectorMenu, this, SLOT(OpenSceneProjector()));
	projectorMenu->addSeparator();
	projectorMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Window")), this,
				  SLOT(OpenSceneProjector()));
	menu.addSeparator();

	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Screenshot.Scene")), [this] {
		auto item = sceneList->currentItem();
		if (!item) {
			return;
		}
		auto s = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
		if (s) {
			obs_frontend_take_source_screenshot(s);
			obs_source_release(s);
		}
	});
	menu.addSeparator();
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Filters")), [this] {
		auto item = sceneList->currentItem();
		if (!item) {
			return;
		}
		auto s = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
		if (s) {
			obs_frontend_open_source_filters(s);
			obs_source_release(s);
		}
	});
	menu.addSeparator();

	auto tom = menu.addMenu(QString::fromUtf8(obs_frontend_get_locale_string("TransitionOverride")));
	std::string scene_name = widget_item->text().toUtf8().constData();
	OBSSourceAutoRelease scene_source = obs_canvas_get_source_by_name(canvas, scene_name.c_str());
	OBSDataAutoRelease private_settings = obs_source_get_private_settings(scene_source);
	obs_data_set_default_int(private_settings, "transition_duration", 300);
	const char *curTransition = obs_data_get_string(private_settings, "transition");
	int curDuration = (int)obs_data_get_int(private_settings, "transition_duration");

	QSpinBox *duration = new FocusScrollSpinBox(tom);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	connect(duration, (void (QSpinBox::*)(int))&QSpinBox::valueChanged, [this, scene_name](int dur) {
		OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, scene_name.c_str());
		OBSDataAutoRelease ps = obs_source_get_private_settings(source);

		obs_data_set_int(ps, "transition_duration", dur);
	});

	auto action = tom->addAction(QString::fromUtf8(obs_frontend_get_locale_string("None")));
	action->setCheckable(true);
	action->setChecked(!curTransition || !strlen(curTransition));
	connect(action, &QAction::triggered, [this, scene_name] {
		OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, scene_name.c_str());
		OBSDataAutoRelease ps = obs_source_get_private_settings(source);
		obs_data_set_string(ps, "transition", "");
	});

	for (auto t : transitions) {
		const char *name = obs_source_get_name(t);
		bool match = (name && curTransition && strcmp(name, curTransition) == 0);

		if (!name || !*name) {
			name = obs_frontend_get_locale_string("None");
		}

		auto a2 = tom->addAction(QString::fromUtf8(name));
		a2->setCheckable(true);
		a2->setChecked(match);
		connect(a2, &QAction::triggered, [this, scene_name, a2] {
			OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, scene_name.c_str());
			OBSDataAutoRelease ps = obs_source_get_private_settings(source);
			obs_data_set_string(ps, "transition", a2->text().toUtf8().constData());
		});
	}

	QWidgetAction *durationAction = new QWidgetAction(tom);
	durationAction->setDefaultWidget(duration);

	tom->addSeparator();
	tom->addAction(durationAction);

	auto a = menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("ShowInMultiview")), [this, scene_name](bool checked) {
		OBSSourceAutoRelease source = obs_canvas_get_source_by_name(canvas, scene_name.c_str());
		OBSDataAutoRelease ps = obs_source_get_private_settings(source);
		obs_data_set_bool(ps, "show_in_multiview", checked);
	});
	a->setCheckable(true);
	obs_data_set_default_bool(private_settings, "show_in_multiview", true);
	a->setChecked(obs_data_get_bool(private_settings, "show_in_multiview"));
	menu.addSeparator();
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string(IsGridMode() ? "Basic.Main.ListMode"
										     : "Basic.Main.GridMode")),
		       [this] { SetGridMode(!IsGridMode()); });

	menu.addSeparator();
	auto linkedScenesMenu = menu.addMenu(QString::fromUtf8(obs_module_text("LinkedScenes")));
	connect(linkedScenesMenu, &QMenu::aboutToShow, [linkedScenesMenu, this] {
		linkedScenesMenu->clear();
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; i++) {
			obs_source_t *src = scenes.sources.array[i];
			obs_data_t *settings = obs_source_get_settings(src);

			auto name = QString::fromUtf8(obs_source_get_name(src));
			auto *checkBox = new QCheckBox(name, linkedScenesMenu);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
			connect(checkBox, &QCheckBox::checkStateChanged, [this, src, checkBox] {
#else
				connect(checkBox, &QCheckBox::stateChanged, [this, src, checkBox] {
#endif
				SetLinkedScene(src, checkBox->isChecked() ? sceneList->currentItem()->text() : "");
			});
			auto *checkableAction = new QWidgetAction(linkedScenesMenu);
			checkableAction->setDefaultWidget(checkBox);
			linkedScenesMenu->addAction(checkableAction);

			auto c = obs_data_get_array(settings, "canvas");
			if (c) {
				const auto count = obs_data_array_count(c);

				for (size_t j = 0; j < count; j++) {
					auto item = obs_data_array_item(c, j);
					if (!item)
						continue;
					if (strcmp(obs_data_get_string(item, "name"), obs_canvas_get_name(canvas)) == 0) {
						auto sn = QString::fromUtf8(obs_data_get_string(item, "scene"));
						if (sn == sceneList->currentItem()->text()) {
							checkBox->setChecked(true);
						}
					} else if (strcmp(obs_data_get_string(item, "name"), "") == 0 &&
						   obs_data_get_int(item, "width") == canvas_width &&
						   obs_data_get_int(item, "height") == canvas_height) {
						obs_data_set_string(item, "name", obs_canvas_get_name(canvas));
						auto sn = QString::fromUtf8(obs_data_get_string(item, "scene"));
						if (sn == sceneList->currentItem()->text()) {
							checkBox->setChecked(true);
						}
					}
					obs_data_release(item);
				}

				obs_data_array_release(c);
			}

			obs_data_release(settings);
		}
		obs_frontend_source_list_free(&scenes);
	});

	menu.exec(QCursor::pos());
}

void CanvasDock::SetGridMode(bool checked)
{
	if (auto *tree = qobject_cast<SceneTree *>(sceneList))
		tree->SetGridMode(checked);
}

bool CanvasDock::IsGridMode()
{
	auto *tree = qobject_cast<SceneTree *>(sceneList);
	return tree && tree->GetGridMode();
}

void CanvasDock::SetLinkedScene(obs_source_t *scene_, const QString &linkedScene)
{
	auto ss = obs_source_get_settings(scene_);
	auto c = obs_data_get_array(ss, "canvas");

	auto count = obs_data_array_count(c);
	obs_data_t *found = nullptr;
	for (size_t i = 0; i < count; i++) {
		auto item = obs_data_array_item(c, i);
		if (!item) {
			continue;
		}
		if (strcmp(obs_data_get_string(item, "name"), obs_canvas_get_name(canvas)) == 0) {
			found = item;
			if (linkedScene.isEmpty()) {
				obs_data_array_erase(c, i);
			}
			break;
		} else if (strcmp(obs_data_get_string(item, "name"), "") == 0 && obs_data_get_int(item, "width") == canvas_width &&
			   obs_data_get_int(item, "height") == canvas_height) {
			obs_data_set_string(item, "name", obs_canvas_get_name(canvas));
			found = item;
			if (linkedScene.isEmpty()) {
				obs_data_array_erase(c, i);
			}
			break;
		}
		obs_data_release(item);
	}
	if (!linkedScene.isEmpty()) {
		if (!found) {
			if (!c) {
				c = obs_data_array_create();
				obs_data_set_array(ss, "canvas", c);
			}
			found = obs_data_create();
			obs_data_set_string(found, "name", obs_canvas_get_name(canvas));
			obs_data_set_int(found, "width", canvas_width);
			obs_data_set_int(found, "height", canvas_height);
			obs_data_array_push_back(c, found);
		}
		obs_data_set_string(found, "scene", linkedScene.toUtf8().constData());
	}
	obs_data_release(ss);
	obs_data_release(found);
	obs_data_array_release(c);

	UpdateLinkedScenes();
}

void CanvasDock::ShowSourcesContextMenu(obs_sceneitem_t *item)
{
	auto menu = QMenu(this);
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("AddSource")),
		       [this] { OpenSourceSelectDialog(this, scene); });
	menu.addSeparator();
	if (!item) {
		menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.NewGroup")), sourceList,
			       &SourceTree::AddGroup);
		AddCopyPasteMenuItems(&menu, nullptr, scene);
	} else {
		AddSceneItemMenuItems(&menu, item);
	}
	menu.exec(QCursor::pos());
}

void CanvasDock::AddSceneItemMenuItems(QMenu *popup, OBSSceneItem sceneItem)
{
	QObject *parent = popup->parent();
	auto pm = qobject_cast<QMenu *>(parent);
	while (pm) {
		parent = pm->parent();
		pm = qobject_cast<QMenu *>(parent);
	}
	popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Rename")), [parent] {
		auto *canvasDock = qobject_cast<CanvasDock *>(parent);
		if (canvasDock && canvasDock->sourceList) {
			canvasDock->sourceList->Edit(canvasDock->GetTopSelectedSourceItem());
			return;
		}
		auto *sourcesDock = qobject_cast<SourcesDock *>(parent);
		if (sourcesDock && sourcesDock->sourceList)
			sourcesDock->sourceList->Edit(sourcesDock->GetTopSelectedSourceItem());
	});
	popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Remove")), parent, [sceneItem, parent] {
		obs_scene_t *root = nullptr;
		QWidget *host = qobject_cast<QWidget *>(parent);
		if (auto *cd = qobject_cast<CanvasDock *>(parent)) {
			root = cd->scene;
		} else if (auto *sd = qobject_cast<SourcesDock *>(parent)) {
			OBSSourceAutoRelease ss = obs_weak_source_get_source(sd->scene);
			root = obs_scene_from_source(ss);
		}
		if (!root) {
			root = obs_sceneitem_get_scene(sceneItem);
		}
		if (sceneItem) {
			obs_sceneitem_select(sceneItem, true);
		}
		RemoveSelectedSources(root, host);
	});

	popup->addSeparator();
	auto orderMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order")));
	orderMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveUp")), parent, [sceneItem] {
			if (!sceneItem) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}
			std::string undo_json = backup_scene(scene);
			obs_sceneitem_set_order(sceneItem, OBS_ORDER_MOVE_UP);
			std::string redo_json = backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveUp"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
							  undo_json.c_str(), redo_json.c_str(), false);
		});
	orderMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveDown")), parent, [sceneItem] {
			if (!sceneItem) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}
			std::string undo_json = backup_scene(scene);
			obs_sceneitem_set_order(sceneItem, OBS_ORDER_MOVE_DOWN);
			std::string redo_json = backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveDown"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
							  undo_json.c_str(), redo_json.c_str(), false);
		});
	orderMenu->addSeparator();
	orderMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveToTop")), parent, [sceneItem] {
			if (!sceneItem) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}
			std::string undo_json = backup_scene(scene);
			obs_sceneitem_set_order(sceneItem, OBS_ORDER_MOVE_TOP);
			std::string redo_json = backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveToTop"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
							  undo_json.c_str(), redo_json.c_str(), false);
		});
	orderMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Order.MoveToBottom")), parent, [sceneItem] {
			if (!sceneItem) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}
			std::string undo_json = backup_scene(scene);
			obs_sceneitem_set_order(sceneItem, OBS_ORDER_MOVE_BOTTOM);
			std::string redo_json = backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveToBottom"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
							  undo_json.c_str(), redo_json.c_str(), false);
		});

	auto transformMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform")));
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.EditTransform")), [sceneItem] {
			const auto mainDialog = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!mainDialog) {
				return;
			}
			auto transformDialog = mainDialog->findChild<QDialog *>("OBSBasicTransform");
			if (!transformDialog) {
				// make sure there is an item selected on the main canvas before starting the transform dialog
				const auto currentScene = obs_frontend_preview_program_mode_active()
								  ? obs_frontend_get_current_preview_scene()
								  : obs_frontend_get_current_scene();
				auto selected = GetSelectedItem(obs_scene_from_source(currentScene));
				if (!selected) {
					obs_scene_enum_items(
						obs_scene_from_source(currentScene),
						[](obs_scene_t *, obs_sceneitem_t *item, void *) {
							obs_sceneitem_select(item, true);
							return false;
						},
						nullptr);
				}
				obs_source_release(currentScene);
				QMetaObject::invokeMethod(mainDialog, "on_actionEditTransform_triggered");
				transformDialog = mainDialog->findChild<QDialog *>("OBSBasicTransform");
			}
			if (!transformDialog) {
				return;
			}
			QMetaObject::invokeMethod(transformDialog,
						  obs_get_version() >= MAKE_SEMANTIC_VERSION(32, 1, 0) ? "setItemQt" : "SetItemQt",
						  Q_ARG(OBSSceneItem, OBSSceneItem(sceneItem)));
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.ResetTransform")), parent,
		[sceneItem] {
			if (!sceneItem) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(sceneItem);
			if (!scene) {
				return;
			}
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			obs_sceneitem_defer_update_begin(sceneItem);
			obs_sceneitem_set_alignment(sceneItem, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
			obs_sceneitem_set_bounds_type(sceneItem, OBS_BOUNDS_NONE);
			vec2 scale;
			scale.x = 1.0f;
			scale.y = 1.0f;
			obs_sceneitem_set_scale(sceneItem, &scale);
			vec2 pos;
			pos.x = 0.0f;
			pos.y = 0.0f;
			obs_sceneitem_set_pos(sceneItem, &pos);
			obs_sceneitem_crop crop = {0, 0, 0, 0};
			obs_sceneitem_set_crop(sceneItem, &crop);
			obs_sceneitem_set_rot(sceneItem, 0.0f);
			obs_sceneitem_defer_update_end(sceneItem);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.Reset"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addSeparator();
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.Rotate90CW")), parent, [sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			float rotation = 90.0f;
			obs_scene_enum_items(scene, RotateSelectedSources, &rotation);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.Rotate"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.Rotate90CCW")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			float rotation = -90.0f;
			obs_scene_enum_items(scene, RotateSelectedSources, &rotation);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.Rotate"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.Rotate180")), parent, [sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			float rotation = 180.0f;
			obs_scene_enum_items(scene, RotateSelectedSources, &rotation);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.Rotate"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addSeparator();
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.FlipHorizontal")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			vec2 scale;
			vec2_set(&scale, -1.0f, 1.0f);
			obs_scene_enum_items(scene, MultiplySelectedItemScale, &scale);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.HFlip"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.FlipVertical")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			vec2 scale;
			vec2_set(&scale, 1.0f, -1.0f);
			obs_scene_enum_items(scene, MultiplySelectedItemScale, &scale);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.VFlip"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addSeparator();
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.FitToScreen")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			obs_bounds_type boundsType = OBS_BOUNDS_SCALE_INNER;
			obs_scene_enum_items(scene, CenterAlignSelectedItems, &boundsType);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.FitToScreen"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.StretchToScreen")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			obs_bounds_type boundsType = OBS_BOUNDS_STRETCH;
			obs_scene_enum_items(scene, CenterAlignSelectedItems, &boundsType);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.StretchToScreen"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.CenterToScreen")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			CenterSelectedItems(CenterType::Scene, scene);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.Center"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.VerticalCenter")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			CenterSelectedItems(CenterType::Vertical, scene);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.VCenter"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});
	transformMenu->addAction(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.Transform.HorizontalCenter")), parent,
		[sceneItem] {
			auto scene = obs_sceneitem_get_scene(sceneItem);
			OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
			CenterSelectedItems(CenterType::Horizontal, scene);
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Transform.HCenter"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(
				undoName.toUtf8().constData(), [](const char *data) { obs_scene_load_transform_states(data); },
				[](const char *data) { obs_scene_load_transform_states(data); }, obs_data_get_json(wrapper),
				obs_data_get_json(rwrapper), false);
		});

	auto *canvasDock = qobject_cast<CanvasDock *>(parent);
	auto *sourcesDock = qobject_cast<SourcesDock *>(parent);
	SourceTree *tree = canvasDock ? canvasDock->sourceList : (sourcesDock ? sourcesDock->sourceList : nullptr);
	if (tree)
		AddGroupMenuItems(tree, popup);
	AddCopyPasteMenuItems(popup, sceneItem, obs_sceneitem_get_scene(sceneItem));

	popup->addSeparator();

	obs_scale_type scaleFilter = obs_sceneitem_get_scale_filter(sceneItem);
	auto scaleMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering")));
	auto a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Disable")), parent,
				      [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_DISABLE); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_DISABLE);
	a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering.Point")), parent,
				 [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_POINT); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_POINT);
	a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering.Bilinear")), parent,
				 [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_BILINEAR); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_BILINEAR);
	a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering.Bicubic")), parent,
				 [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_BICUBIC); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_BICUBIC);
	a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering.Lanczos")), parent,
				 [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_LANCZOS); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_LANCZOS);
	a = scaleMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("ScaleFiltering.Area")), parent,
				 [sceneItem] { obs_sceneitem_set_scale_filter(sceneItem, OBS_SCALE_AREA); });
	a->setCheckable(true);
	a->setChecked(scaleFilter == OBS_SCALE_AREA);

	auto blendingMode = obs_sceneitem_get_blending_mode(sceneItem);
	auto blendingMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("BlendingMode")));

	std::map<enum obs_blending_type, const char *> blendingModes = {
		{OBS_BLEND_NORMAL, "BlendingMode.Normal"},     {OBS_BLEND_ADDITIVE, "BlendingMode.Additive"},
		{OBS_BLEND_SUBTRACT, "BlendingMode.Subtract"}, {OBS_BLEND_SCREEN, "BlendingMode.Screen"},
		{OBS_BLEND_MULTIPLY, "BlendingMode.Multiply"}, {OBS_BLEND_LIGHTEN, "BlendingMode.Lighten"},
		{OBS_BLEND_DARKEN, "BlendingMode.Darken"}};

	for (int i = OBS_BLEND_NORMAL; i <= OBS_BLEND_DARKEN; i++) {
		a = blendingMenu->addAction(
			QString::fromUtf8(obs_frontend_get_locale_string(blendingModes[(enum obs_blending_type)i])), parent,
			[sceneItem, i] {
				if (!sceneItem) {
					return;
				}
				auto scene = obs_sceneitem_get_scene(sceneItem);
				if (!scene) {
					return;
				}
				std::string undo_json = backup_scene(scene);
				obs_sceneitem_set_blending_mode(sceneItem, (enum obs_blending_type)i);
				std::string redo_json = backup_scene(scene);
				auto undoName =
					QString::fromUtf8(obs_frontend_get_locale_string("Undo.BlendingMode"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))));
				obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
								  undo_json.c_str(), redo_json.c_str(), false);
			});
		a->setCheckable(true);
		a->setChecked(blendingMode == i);
	}

	auto blendingMethod = obs_sceneitem_get_blending_method(sceneItem);
	blendingMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("BlendingMethod")));

	a = blendingMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("BlendingMethod.Default")), [sceneItem]() {
		if (!sceneItem) {
			return;
		}
		auto scene = obs_sceneitem_get_scene(sceneItem);
		if (!scene) {
			return;
		}
		std::string undo_json = backup_scene(scene);
		obs_sceneitem_set_blending_method(sceneItem, OBS_BLEND_METHOD_DEFAULT);
		std::string redo_json = backup_scene(scene);
		auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.BlendingMethod"))
					.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))));
		obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
						  undo_json.c_str(), redo_json.c_str(), false);
	});
	a->setCheckable(true);
	a->setChecked(blendingMethod == OBS_BLEND_METHOD_DEFAULT);

	a = blendingMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("BlendingMethod.SrgbOff")), [sceneItem]() {
		if (!sceneItem) {
			return;
		}
		auto scene = obs_sceneitem_get_scene(sceneItem);
		if (!scene) {
			return;
		}
		std::string undo_json = backup_scene(scene);
		obs_sceneitem_set_blending_method(sceneItem, OBS_BLEND_METHOD_SRGB_OFF);
		std::string redo_json = backup_scene(scene);
		auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.BlendingMethod"))
					.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(sceneItem))));
		obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), undo_redo_scene, undo_redo_scene,
						  undo_json.c_str(), redo_json.c_str(), false);
	});
	a->setCheckable(true);
	a->setChecked(blendingMethod == OBS_BLEND_METHOD_SRGB_OFF);

	popup->addSeparator();
	popup->addMenu(CreateVisibilityTransitionMenu(true, sceneItem));
	popup->addMenu(CreateVisibilityTransitionMenu(false, sceneItem));

	popup->addSeparator();

	auto projectorMenu = popup->addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Open.Source")));
	AddProjectorMenuMonitors(projectorMenu, parent, SLOT(OpenSourceProjector()));
	projectorMenu->addSeparator();
	a = projectorMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Window")));
	connect(a, &QAction::triggered, parent, [parent] { QMetaObject::invokeMethod(parent, "OpenSourceProjector"); });
	a->setProperty("monitor", -1);

	obs_source_t *s = obs_sceneitem_get_source(sceneItem);
	popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Screenshot.Source")), parent,
			 [s] { obs_frontend_take_source_screenshot(s); });
	popup->addSeparator();
	popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Filters")), parent,
			 [s] { obs_frontend_open_source_filters(s); });
	a = popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Properties")), parent,
			     [s] { obs_frontend_open_source_properties(s); });
	a->setEnabled(obs_source_configurable(s));
}

static bool copy_save_functions = false;
static void (*obs_frontend_copy_sceneitem_func)(obs_sceneitem_t *item) = nullptr;
static bool (*obs_frontend_can_paste_sceneitem_func)(bool duplicate) = nullptr;
static void (*obs_frontend_paste_sceneitem_func)(obs_scene_t *scene, bool duplicate) = nullptr;

void CanvasDock::AddCopyPasteMenuItems(QMenu *popup, OBSSceneItem sceneItem, OBSScene scene)
{
	if (!copy_save_functions) {
		copy_save_functions = true;
		if (obs_get_version() < MAKE_SEMANTIC_VERSION(32, 2, 0)) {
			return;
		}

#ifdef __APPLE__
		auto handle = os_dlopen("obs-frontend-api.dylib");
#else
		auto handle = os_dlopen("obs-frontend-api");
#endif
		obs_frontend_copy_sceneitem_func = (void (*)(obs_sceneitem_t *))os_dlsym(handle, "obs_frontend_copy_sceneitem");
		obs_frontend_can_paste_sceneitem_func =
			(bool (*)(bool duplicate))os_dlsym(handle, "obs_frontend_can_paste_sceneitem");
		obs_frontend_paste_sceneitem_func =
			(void (*)(obs_scene_t *scene, bool duplicate))os_dlsym(handle, "obs_frontend_paste_sceneitem");
		os_dlclose(handle);
	}
	if (obs_frontend_copy_sceneitem_func && sceneItem) {
		auto copyAction = popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Copy")),
						   [sceneItem] { obs_frontend_copy_sceneitem_func(sceneItem); });
		copyAction->setEnabled(true);
	}
	if (obs_frontend_can_paste_sceneitem_func && obs_frontend_paste_sceneitem_func) {
		auto pasteAction = popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("PasteReference")),
						    [scene] { obs_frontend_paste_sceneitem_func(scene, false); });
		pasteAction->setEnabled(obs_frontend_can_paste_sceneitem_func(false));

		pasteAction = popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("PasteDuplicate")),
					       [scene] { obs_frontend_paste_sceneitem_func(scene, true); });
		pasteAction->setEnabled(obs_frontend_can_paste_sceneitem_func(true));
	}
}

void CanvasDock::AddGroupMenuItems(SourceTree *sl, QMenu *popup)
{
	if (sl->MultipleBaseSelected()) {
		popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.GroupItems")), sl,
				 &SourceTree::GroupSelectedItems);
		popup->addSeparator();
	} else if (sl->GroupsSelected()) {
		popup->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.Ungroup")), sl,
				 &SourceTree::UngroupSelectedGroups);
		popup->addSeparator();
	}
}

bool CanvasDock::RotateSelectedSources(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, RotateSelectedSources, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	float rot = *reinterpret_cast<float *>(param);

	vec3 tl = GetItemTL(item);

	rot += obs_sceneitem_get_rot(item);
	if (rot >= 360.0f) {
		rot -= 360.0f;
	} else if (rot <= -360.0f) {
		rot += 360.0f;
	}
	obs_sceneitem_set_rot(item, rot);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	return true;
};

vec3 CanvasDock::GetItemTL(obs_sceneitem_t *item)
{
	vec3 tl, br;
	GetItemBox(item, tl, br);
	return tl;
}

void CanvasDock::SetItemTL(obs_sceneitem_t *item, const vec3 &tl)
{
	vec3 newTL;
	vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	newTL = GetItemTL(item);
	pos.x += tl.x - newTL.x;
	pos.y += tl.y - newTL.y;
	obs_sceneitem_set_pos(item, &pos);
}

void CanvasDock::GetItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&](float x, float y) {
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);
}

bool CanvasDock::MultiplySelectedItemScale(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	vec2 &mul = *reinterpret_cast<vec2 *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, MultiplySelectedItemScale, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	vec3 tl = GetItemTL(item);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	vec2_mul(&scale, &scale, &mul);
	obs_sceneitem_set_scale(item, &scale);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::CenterAlignSelectedItems(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	obs_bounds_type boundsType = *reinterpret_cast<obs_bounds_type *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, CenterAlignSelectedItems, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	obs_source_t *scene_source = obs_scene_get_source(scene);

	obs_transform_info itemInfo;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.rot = 0.0f;

	vec2_set(&itemInfo.bounds, float(obs_source_get_base_width(scene_source)), float(obs_source_get_base_height(scene_source)));
	itemInfo.bounds_type = boundsType;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;
	itemInfo.crop_to_bounds = obs_sceneitem_get_bounds_crop(item);
	obs_sceneitem_set_info2(item, &itemInfo);

	UNUSED_PARAMETER(scene);
	return true;
}

QMenu *CanvasDock::CreateVisibilityTransitionMenu(bool visible, obs_sceneitem_t *si)
{
	QMenu *menu = new QMenu(QString::fromUtf8(obs_frontend_get_locale_string(visible ? "ShowTransition" : "HideTransition")));

	obs_source_t *curTransition = obs_sceneitem_get_transition(si, visible);
	const char *curId = curTransition ? obs_source_get_id(curTransition) : nullptr;
	int curDuration = (int)obs_sceneitem_get_transition_duration(si, visible);

	if (curDuration <= 0) {
		curDuration = obs_frontend_get_transition_duration();
	}

	QSpinBox *duration = new FocusScrollSpinBox(menu);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	auto setTransition = [](QAction *a, bool vis, obs_sceneitem_t *si2) {
		std::string id = a->property("transition_id").toString().toUtf8().constData();
		if (id.empty()) {
			obs_sceneitem_set_transition(si2, vis, nullptr);
		} else {
			obs_source_t *tr = obs_sceneitem_get_transition(si2, vis);

			if (!tr || strcmp(id.c_str(), obs_source_get_id(tr)) != 0) {
				QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(si2)));
				name += " ";
				name += QString::fromUtf8(
					obs_frontend_get_locale_string(vis ? "ShowTransition" : "HideTransition"));
				tr = obs_source_create_private(id.c_str(), name.toUtf8().constData(), nullptr);
				obs_sceneitem_set_transition(si2, vis, tr);
				obs_source_release(tr);

				int dur = (int)obs_sceneitem_get_transition_duration(si2, vis);
				if (dur <= 0) {
					dur = obs_frontend_get_transition_duration();
					obs_sceneitem_set_transition_duration(si2, vis, dur);
				}
			}
			if (obs_source_configurable(tr)) {
				obs_frontend_open_source_properties(tr);
			}
		}
	};
	auto setDuration = [visible, si](int dur) {
		obs_sceneitem_set_transition_duration(si, visible, dur);
	};
	connect(duration, (void (QSpinBox::*)(int))&QSpinBox::valueChanged, setDuration);

	QAction *a = menu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("None")));
	a->setProperty("transition_id", QString::fromUtf8(""));
	a->setCheckable(true);
	a->setChecked(!curId);
	connect(a, &QAction::triggered, std::bind(setTransition, a, visible, si));
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		const char *name = obs_source_get_display_name(id);
		const bool match = id && curId && strcmp(id, curId) == 0;
		a = menu->addAction(QString::fromUtf8(name));
		a->setProperty("transition_id", QString::fromUtf8(id));
		a->setCheckable(true);
		a->setChecked(match);
		connect(a, &QAction::triggered, std::bind(setTransition, a, visible, si));
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	if (curId && obs_is_source_configurable(curId)) {
		menu->addSeparator();
		menu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Properties")), menu,
				[curTransition] { obs_frontend_open_source_properties(curTransition); });
	}

	return menu;
}

void CanvasDock::CenterSelectedItems(CenterType centerType, obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(scene, GetSelectedItemsWithSize, &items);
	if (!items.size()) {
		return;
	}

	// Get center x, y coordinates of items
	vec3 center;

	float top = M_INFINITE;
	float left = M_INFINITE;
	float right = 0.0f;
	float bottom = 0.0f;

	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		left = (std::min)(tl.x, left);
		top = (std::min)(tl.y, top);
		right = (std::max)(br.x, right);
		bottom = (std::max)(br.y, bottom);
	}

	center.x = (right + left) / 2.0f;
	center.y = (top + bottom) / 2.0f;
	center.z = 0.0f;

	// Get coordinates of screen center
	vec3 screenCenter;

	obs_source_t *scene_source = obs_scene_get_source(scene);
	vec3_set(&screenCenter, float(obs_source_get_width(scene_source)), float(obs_source_get_height(scene_source)), 0.0f);

	vec3_mulf(&screenCenter, &screenCenter, 0.5f);

	// Calculate difference between screen center and item center
	vec3 offset;
	vec3_sub(&offset, &screenCenter, &center);

	// Shift items by offset
	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		vec3_add(&tl, &tl, &offset);

		vec3 itemTL = GetItemTL(item);

		if (centerType == CenterType::Vertical) {
			tl.x = itemTL.x;
		} else if (centerType == CenterType::Horizontal) {
			tl.y = itemTL.y;
		}

		SetItemTL(item, tl);
	}
}

bool CanvasDock::GetSelectedItemsWithSize(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto items = static_cast<std::vector<obs_sceneitem_t *> *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, GetSelectedItemsWithSize, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);

	obs_source_t *source = obs_sceneitem_get_source(item);
	const float width = float(obs_source_get_width(source)) * scale.x;
	const float height = float(obs_source_get_height(source)) * scale.y;

	if (width == 0.0f || height == 0.0f) {
		return true;
	}

	items->push_back(item);

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::AddProjectorMenuMonitors(QMenu *parent, QObject *target, const char *slot)
{

	QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *screen = screens[i];
		QRect screenGeometry = screen->geometry();
		qreal ratio = screen->devicePixelRatio();
		QString name = "";
#if defined(_WIN32) && QT_VERSION < QT_VERSION_CHECK(6, 4, 0)
		QTextStream fullname(&name);
		fullname << GetMonitorName(screen->name());
		fullname << " (";
		fullname << (i + 1);
		fullname << ")";
#elif defined(__APPLE__) || defined(_WIN32)
		name = screen->name();
#else
		name = screen->model().simplified();

		if (name.length() > 1 && name.endsWith("-")) {
			name.chop(1);
		}
#endif
		name = name.simplified();

		if (name.length() == 0) {
			name = QString("%1 %2")
				       .arg(QString::fromUtf8(obs_frontend_get_locale_string("Display")))
				       .arg(QString::number(i + 1));
		}
		QString str = QString("%1: %2x%3 @ %4,%5")
				      .arg(name, QString::number(screenGeometry.width() * ratio),
					   QString::number(screenGeometry.height() * ratio), QString::number(screenGeometry.x()),
					   QString::number(screenGeometry.y()));

		QAction *a = parent->addAction(str, target, slot);
		a->setProperty("monitor", i);
	}
}

void CanvasDock::LoadScenes()
{
	/* for (uint32_t i = MAX_CHANNELS - 1; i > 0; i--) {
		auto s = obs_get_output_source(i);
		if (s == nullptr) {
			obs_set_output_source(i, transitionAudioWrapper);
			break;
		}
		obs_source_release(s);
	}*/
	if (sceneList) {
		sceneList->clear();
	}
	if (sceneCombo) {
		sceneCombo->clear();
	}

	obs_canvas_enum_scenes(
		canvas,
		[](void *param, obs_source_t *src) {
			auto t = (CanvasDock *)param;

			std::string ssn = obs_canvas_get_name(t->canvas);
			ssn += " ";
			ssn += obs_frontend_get_locale_string("Basic.Hotkeys.SelectScene");
			obs_hotkey_register_source(
				src, "OBSBasic.SelectScene", ssn.c_str(),
				[](void *data, obs_hotkey_id, obs_hotkey_t *key, bool pressed) {
					if (!pressed) {
						return;
					}
					auto p = (CanvasDock *)data;
					auto potential_source = (obs_weak_source_t *)obs_hotkey_get_registerer(key);
					OBSSourceAutoRelease source = obs_weak_source_get_source(potential_source);
					if (source) {
						auto sn = QString::fromUtf8(obs_source_get_name(source));
						QMetaObject::invokeMethod(p, "SwitchScene", Q_ARG(QString, sn), Q_ARG(bool, true));
					}
				},
				t);
			auto sh = obs_source_get_signal_handler(src);
			signal_handler_connect(sh, "rename", source_rename, t);
			signal_handler_connect(sh, "remove", source_remove, t);
			QString name = QString::fromUtf8(obs_source_get_name(src));
			obs_data_t *settings = obs_source_get_settings(src);
			if (t->sceneList) {
				auto sli = new QListWidgetItem(name, t->sceneList);
				const int order = (int)obs_data_get_int(settings, "order");
				t->sceneList->insertItem(order, sli);
				if ((t->currentSceneName.isEmpty() && obs_data_get_bool(settings, "canvas_active")) ||
				    name == t->currentSceneName) {
					for (int j = 0; j < t->sceneList->count(); j++) {
						auto item = t->sceneList->item(j);
						if (item->text() != name) {
							continue;
						}
						t->sceneList->setCurrentItem(item);
					}
				}
			}
			if (t->sceneCombo) {
				t->sceneCombo->addItem(name);
				if ((t->currentSceneName.isEmpty() && obs_data_get_bool(settings, "canvas_active")) ||
				    name == t->currentSceneName) {
					t->sceneCombo->setCurrentText(name);
				}
			}
			obs_data_release(settings);

			return true;
		},
		this);

	if (sceneList) {
		QListWidgetItem *selectedItem = nullptr;
		sceneList->blockSignals(true);
		for (int idx = 0; idx < sceneList->count();) {
			auto item = sceneList->takeItem(idx);
			auto scene = obs_canvas_get_source_by_name(canvas, item->text().toUtf8().constData());
			auto settings = obs_source_get_settings(scene);
			const int order = (int)obs_data_get_int(settings, "order");
			if (order <= idx) {
				idx++;
			}
			sceneList->insertItem(order, item);
			if (obs_data_get_bool(settings, "canvas_active")) {
				selectedItem = item;
			}
			obs_data_release(settings);
			obs_source_release(scene);
		}
		sceneList->blockSignals(false);
		if (selectedItem) {
			sceneList->setCurrentItem(selectedItem);
		}

		UpdateLinkedScenes();

		if (sceneList->count() == 0) {
			AddScene("", false);
		}

		if (sceneList->currentRow() < 0) {
			sceneList->setCurrentRow(0);
		}
	}
	if (sceneCombo) {
		int selectedIndex = -1;
		for (int idx = 0; idx < sceneCombo->count(); idx++) {
			auto scene = obs_canvas_get_source_by_name(canvas, sceneCombo->itemText(idx).toUtf8().constData());
			auto settings = obs_source_get_settings(scene);
			//const int order = (int)obs_data_get_int(settings, "order");
			//sceneCombo->insertItem(order, sceneCombo->itemText(idx));
			if (obs_data_get_bool(settings, "canvas_active")) {
				selectedIndex = idx;
			}
			obs_data_release(settings);
			obs_source_release(scene);
		}
		if (selectedIndex >= 0) {
			sceneCombo->setCurrentIndex(selectedIndex);
		}

		if (sceneCombo->currentIndex() < 0) {
			sceneCombo->setCurrentIndex(0);
		}
	}
	EnsureOutputTransition();
}

void CanvasDock::LoadTransitions()
{
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		if (obs_is_source_configurable(id)) {
			continue;
		}
		const char *name = obs_source_get_display_name(id);

		OBSSourceAutoRelease tr = obs_source_create_private(id, name, NULL);
		transitions.emplace_back(tr);

		//signals "transition_stop" and "transition_video_stop"
		//        TransitionFullyStopped TransitionStopped
	}

	obs_data_array_t *transition_array = obs_data_get_array(settings, "transitions");
	if (transition_array) {
		size_t c = obs_data_array_count(transition_array);
		for (size_t i = 0; i < c; i++) {
			obs_data_t *td = obs_data_array_item(transition_array, i);
			if (!td) {
				continue;
			}
			OBSSourceAutoRelease loaded = obs_load_private_source(td);
			if (loaded) {
				obs_source_load2(loaded);
				transitions.emplace_back(loaded);
			}

			obs_data_release(td);
		}
		obs_data_array_release(transition_array);
	}

	if (settings) {
		obs_data_set_default_int(settings, "transition_duration", 300);
		transitionDurationMs = (int)obs_data_get_int(settings, "transition_duration");
		if (transitionDurationMs < 50) {
			transitionDurationMs = 300;
		}
		if (transitionDurationBox) {
			transitionDurationBox->blockSignals(true);
			transitionDurationBox->setValue(transitionDurationMs);
			transitionDurationBox->blockSignals(false);
		}
	}

	auto current_transition = GetTransition(obs_data_get_string(settings, "transition"));
	if (!current_transition) {
		current_transition = GetTransition(obs_source_get_display_name("fade_transition"));
	}

	SwapTransition(current_transition);

	for (auto t : transitions) {
		auto name = QString::fromUtf8(obs_source_get_name(t));
		transition->addItem(name);
		if (obs_weak_source_references_source(source, t)) {
			transition->setCurrentText(name);
		}
	}
}

void CanvasDock::UpdateLinkedScenes()
{
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *src = scenes.sources.array[i];
		obs_data_t *settings = obs_source_get_settings(src);
		auto c = obs_data_get_array(settings, "canvas");
		if (c) {
			const auto count = obs_data_array_count(c);
			for (size_t j = 0; j < count; j++) {
				auto item = obs_data_array_item(c, j);
				if (!item) {
					continue;
				}
				if (strcmp(obs_data_get_string(item, "name"), "") == 0 &&
				    obs_data_get_int(item, "width") == canvas_width &&
				    obs_data_get_int(item, "height") == canvas_height) {
					obs_data_set_string(item, "name", obs_canvas_get_name(canvas));
				}
				obs_data_release(item);
			}

			obs_data_array_release(c);
		}

		obs_data_release(settings);
	}
	obs_frontend_source_list_free(&scenes);
}

void CanvasDock::source_rename(void *data, calldata_t *calldata)
{
	const auto d = static_cast<CanvasDock *>(data);
	const auto prev_name = QString::fromUtf8(calldata_string(calldata, "prev_name"));
	const auto new_name = QString::fromUtf8(calldata_string(calldata, "new_name"));
	auto source = (obs_source_t *)calldata_ptr(calldata, "source");
	if (!source || !obs_source_is_scene(source)) {
		return;
	}

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		const obs_source_t *src = scenes.sources.array[i];
		auto ss = obs_source_get_settings(src);
		auto c = obs_data_get_array(ss, "canvas");
		obs_data_release(ss);
		if (!c) {
			continue;
		}
		const auto count = obs_data_array_count(c);
		for (size_t j = 0; j < count; j++) {
			auto item = obs_data_array_item(c, j);
			auto n = QString::fromUtf8(obs_data_get_string(item, "scene"));
			if (n == prev_name) {
				obs_data_set_string(item, "scene", calldata_string(calldata, "new_name"));
			}
			obs_data_release(item);
		}
		obs_data_array_release(c);
	}
	obs_frontend_source_list_free(&scenes);

	if (d->sceneList) {
		for (int i = 0; i < d->sceneList->count(); i++) {
			const auto item = d->sceneList->item(i);
			if (item->text() != prev_name) {
				continue;
			}
			item->setText(new_name);
		}
	}
	if (d->sceneCombo) {
		const auto index = d->sceneCombo->findText(prev_name);
		if (index >= 0) {
			d->sceneCombo->setItemText(index, new_name);
		}
	}
}

void CanvasDock::source_add(void *data, calldata_t *calldata)
{
	const auto d = static_cast<CanvasDock *>(data);
	const auto source = (obs_source_t *)calldata_ptr(calldata, "source");
	if (!obs_source_is_scene(source)) {
		return;
	}
	const auto canvas = obs_source_get_canvas(source);
	obs_canvas_release(canvas);
	if (!canvas || canvas != d->canvas) {
		return;
	}
	const auto name = QString::fromUtf8(obs_source_get_name(source));
	if (name.isEmpty()) {
		return;
	}
	QMetaObject::invokeMethod(d, "SceneAdded", Q_ARG(QString, name));
}

void CanvasDock::SceneAdded(const QString sn)
{
	auto scene = obs_canvas_get_source_by_name(canvas, sn.toUtf8().constData());
	if (!scene) {
		return;
	}
	std::string ssn = obs_canvas_get_name(canvas);
	ssn += " ";
	ssn += obs_frontend_get_locale_string("Basic.Hotkeys.SelectScene");
	obs_hotkey_register_source(
		scene, "OBSBasic.SelectScene", ssn.c_str(),
		[](void *data, obs_hotkey_id, obs_hotkey_t *key, bool pressed) {
			if (!pressed) {
				return;
			}
			auto p = (CanvasDock *)data;
			auto potential_source = (obs_weak_source_t *)obs_hotkey_get_registerer(key);
			OBSSourceAutoRelease source = obs_weak_source_get_source(potential_source);
			if (source) {
				auto sn = QString::fromUtf8(obs_source_get_name(source));
				QMetaObject::invokeMethod(p, "SwitchScene", Q_ARG(QString, sn), Q_ARG(bool, true));
			}
		},
		this);
	auto sh = obs_source_get_signal_handler(scene);
	signal_handler_connect(sh, "rename", source_rename, this);
	signal_handler_connect(sh, "remove", source_remove, this);

	if (sceneList) {
		new QListWidgetItem(sn, sceneList);
	}
	if (sceneCombo) {
		sceneCombo->addItem(sn);
	}
}

void CanvasDock::source_remove(void *data, calldata_t *calldata)
{
	const auto d = static_cast<CanvasDock *>(data);
	const auto source = (obs_source_t *)calldata_ptr(calldata, "source");
	if (!obs_source_is_scene(source)) {
		return;
	}
	const auto canvas = obs_source_get_canvas(source);
	obs_canvas_release(canvas);
	if (!canvas || canvas != d->canvas) {
		return;
	}
	if (obs_weak_source_references_source(d->source, source) || source == obs_scene_get_source(d->scene)) {
		QMetaObject::invokeMethod(d, "SwitchScene", Q_ARG(QString, ""), Q_ARG(bool, false));
	}
	const auto name = QString::fromUtf8(obs_source_get_name(source));
	if (name.isEmpty()) {
		return;
	}
	QMetaObject::invokeMethod(d, "SceneRemoved", Q_ARG(QString, name));
}

void CanvasDock::SceneRemoved(const QString name)
{
	if (sceneList) {
		for (int i = 0; i < sceneList->count(); i++) {
			auto item = sceneList->item(i);
			if (item->text() != name) {
				continue;
			}
			sceneList->takeItem(i);
		}
		auto r = sceneList->currentRow();
		auto c = sceneList->count();
		if ((r < 0 && c > 0) || r >= c) {
			sceneList->setCurrentRow(0);
		}
	}
	if (sceneCombo) {
		auto index = sceneCombo->findText(name);
		if (index >= 0) {
			sceneCombo->removeItem(index);
		}
		if (sceneCombo->currentIndex() < 0 && sceneCombo->count() > 0) {
			sceneCombo->setCurrentIndex(0);
		}
	}
}

void CanvasDock::AddSceneItem(OBSSceneItem item, bool no_select)
{
	obs_scene_t *add_scene = obs_sceneitem_get_scene(item);

	if (scene == add_scene) {
		sourceList->Add(item);
	}

	if (no_select) {
		return;
	}

	obs_scene_enum_items(add_scene, select_one, (obs_sceneitem_t *)item);
}

bool CanvasDock::select_one(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	obs_sceneitem_t *selectedItem = reinterpret_cast<obs_sceneitem_t *>(param);
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, select_one, param);
	}

	obs_sceneitem_select(item, (selectedItem == item));

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::RefreshSources(OBSScene refresh_scene)
{
	if (refresh_scene != scene || sourceList->IgnoreReorder()) {
		return;
	}

	sourceList->RefreshItems();
}

void CanvasDock::ReorderSources(OBSScene order_scene)
{
	if (order_scene != scene || sourceList->IgnoreReorder()) {
		return;
	}

	sourceList->ReorderItems();
}

void CanvasDock::SwitchBackToSelectedTransition()
{
	auto tn = transition->currentText().toUtf8();
	auto transition = GetTransition(tn.constData());
	SwapTransition(transition);
}

OBSEventFilter *CanvasDock::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *obj, QEvent *event) {
		UNUSED_PARAMETER(obj);

		if (!scene) {
			return false;
		}
		switch (event->type()) {
		case QEvent::MouseButtonPress:
			return this->HandleMousePressEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseButtonRelease:
			return this->HandleMouseReleaseEvent(static_cast<QMouseEvent *>(event));
		//case QEvent::MouseButtonDblClick:			return this->HandleMouseClickEvent(				static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));
		//case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseLeaveEvent(static_cast<QMouseEvent *>(event));
		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		//case QEvent::FocusIn:
		//case QEvent::FocusOut:
		case QEvent::KeyPress:
			return this->HandleKeyPressEvent(static_cast<QKeyEvent *>(event));
		case QEvent::KeyRelease:
			return this->HandleKeyReleaseEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

bool CanvasDock::HandleMousePressEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF pos = event->position();
#else
	QPointF pos = event->localPos();
#endif

	if (scrollMode && IsFixedScaling() && event->button() == Qt::LeftButton) {
		setCursor(Qt::ClosedHandCursor);
		scrollingFrom.x = (float)pos.x();
		scrollingFrom.y = (float)pos.y();
		return true;
	}

	if (event->button() == Qt::RightButton) {
		scrollMode = false;
		setCursor(Qt::ArrowCursor);
	}

	if (locked) {
		return false;
	}

	//float pixelRatio = 1.0f;
	//float x = pos.x() - main->previewX / pixelRatio;
	//float y = pos.y() - main->previewY / pixelRatio;
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	bool altDown = (modifiers & Qt::AltModifier);
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	bool ctrlDown = (modifiers & Qt::ControlModifier);

	if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) {
		return false;
	}

	if (event->button() == Qt::LeftButton) {
		mouseDown = true;
	}

	{
		std::lock_guard<std::mutex> lock(selectMutex);
		selectedItems.clear();
	}

	if (altDown) {
		cropping = true;
	}

	if (altDown || shiftDown || ctrlDown) {
		vec2 s;
		SceneFindBoxData sfbd(s, s);

		obs_scene_enum_items(scene, FindSelected, &sfbd);

		std::lock_guard<std::mutex> lock(selectMutex);
		selectedItems = sfbd.sceneItems;
	}
	startPos = GetMouseEventPos(event);

	//vec2_set(&startPos, mouseEvent.x, mouseEvent.y);
	//GetStretchHandleData(startPos, false);

	//vec2_divf(&startPos, &startPos, main->previewScale / pixelRatio);
	startPos.x = std::round(startPos.x);
	startPos.y = std::round(startPos.y);

	mouseOverItems = SelectedAtPos(scene, startPos);
	vec2_zero(&lastMoveOffset);

	mousePos = startPos;

	return true;
}

vec2 CanvasDock::GetMouseEventPos(QMouseEvent *event)
{
	float scale = previewScale > 0.0f ? previewScale : 1.0f;
	float pixelRatio = GetDevicePixelRatio();
	QPoint qtPos = event->pos();

	vec2 pos;
	vec2_set(&pos, ((float)qtPos.x() - (float)previewX / pixelRatio) / scale,
		 ((float)qtPos.y() - (float)previewY / pixelRatio) / scale);

	return pos;
}

bool CanvasDock::SelectedAtPos(obs_scene_t *s, const vec2 &pos)
{
	if (!s) {
		return false;
	}

	SceneFindData sfd(pos, false);
	obs_scene_enum_items(s, CheckItemSelected, &sfd);
	return !!sfd.item;
}

bool CanvasDock::CheckItemSelected(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	SceneFindData *data = reinterpret_cast<SceneFindData *>(param);
	matrix4 transform;
	vec3 transformedPos;
	vec3 pos3;

	if (!SceneItemHasVideo(item)) {
		return true;
	}
	if (obs_sceneitem_is_group(item)) {
		data->group = item;
		obs_sceneitem_group_enum_items(item, CheckItemSelected, param);
		data->group = nullptr;

		if (data->item) {
			return false;
		}
	}

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	if (data->group) {
		matrix4 parent_transform;
		obs_sceneitem_get_draw_transform(data->group, &parent_transform);
		matrix4_mul(&transform, &transform, &parent_transform);
	}

	matrix4_inv(&transform, &transform);
	vec3_transform(&transformedPos, &pos3, &transform);

	if (transformedPos.x >= 0.0f && transformedPos.x <= 1.0f && transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		if (obs_sceneitem_selected(item)) {
			data->item = item;
			return false;
		}
	}

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::HandleMouseReleaseEvent(QMouseEvent *event)
{
	if (scrollMode) {
		setCursor(Qt::OpenHandCursor);
	}

	if (!mouseDown && event->button() == Qt::RightButton) {
		QMenu popup(this);
		QAction *a =
			popup.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.Preview.Disable")), [this] {
				preview_disabled = !preview_disabled;
				obs_display_set_enabled(preview->GetDisplay(), !preview_disabled);
				preview->setVisible(!preview_disabled);
				previewDisabledWidget->setVisible(preview_disabled);
			});
		auto projectorMenu = popup.addMenu(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Open.Preview")));
		AddProjectorMenuMonitors(projectorMenu, this, SLOT(OpenPreviewProjector()));
		projectorMenu->addSeparator();
		a = projectorMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Projector.Window")),
					     [this] { OpenProjector(-1); });

		a = popup.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.MainMenu.Edit.LockPreview")), this,
				    [this] { locked = !locked; });
		a->setCheckable(true);
		a->setChecked(locked);

		popup.addAction(GetIconFromType(OBS_ICON_TYPE_IMAGE),
				QString::fromUtf8(obs_frontend_get_locale_string("Screenshot")), this, [this] {
					auto s = obs_weak_source_get_source(source);
					obs_frontend_take_source_screenshot(s);
					obs_source_release(s);
				});

		popup.addAction(QString::fromUtf8(obs_frontend_get_locale_string("AddSource")),
				[this] { OpenSourceSelectDialog(this, scene); });

		popup.addSeparator();

		OBSSceneItem sceneItem = GetSelectedItem(scene);
		if (sceneItem) {
			AddSceneItemMenuItems(&popup, sceneItem);
		} else {
			AddCopyPasteMenuItems(&popup, nullptr, scene);
		}
		popup.exec(QCursor::pos());
		return true;
	}

	if (locked) {
		return false;
	}

	if (!mouseDown) {
		return false;
	}

	const vec2 pos = GetMouseEventPos(event);

	if (!mouseMoved) {
		ProcessClick(pos);
	}

	if (selectionBox) {
		Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();

		bool altDown = modifiers & Qt::AltModifier;
		bool shiftDown = modifiers & Qt::ShiftModifier;
		bool ctrlDown = modifiers & Qt::ControlModifier;

		std::lock_guard<std::mutex> lock(selectMutex);
		if (altDown || ctrlDown || shiftDown) {
			for (size_t i = 0; i < selectedItems.size(); i++) {
				obs_sceneitem_select(selectedItems[i], true);
			}
		}

		for (size_t i = 0; i < hoveredPreviewItems.size(); i++) {
			bool select = true;
			obs_sceneitem_t *item = hoveredPreviewItems[i];

			if (altDown) {
				select = false;
			} else if (ctrlDown) {
				select = !obs_sceneitem_selected(item);
			}

			obs_sceneitem_select(hoveredPreviewItems[i], select);
		}
	}

	if (stretchGroup) {
		obs_sceneitem_defer_group_resize_end(stretchGroup);
	}

	stretchItem = nullptr;
	stretchGroup = nullptr;
	mouseDown = false;
	mouseMoved = false;
	cropping = false;
	selectionBox = false;
	unsetCursor();

	OBSSceneItem item = GetItemAtPos(pos, true);

	std::lock_guard<std::mutex> lock(selectMutex);
	hoveredPreviewItems.clear();
	hoveredPreviewItems.push_back(item);
	selectedItems.clear();

	return true;
}

void CanvasDock::ProcessClick(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();

	if (modifiers & Qt::ControlModifier) {
		DoCtrlSelect(pos);
	} else {
		DoSelect(pos);
	}
}

void CanvasDock::DoSelect(const vec2 &pos)
{
	OBSSceneItem item = GetItemAtPos(pos, true);
	obs_scene_enum_items(scene, select_one, (obs_sceneitem_t *)item);
	UpdateContextBar();
}

void CanvasDock::DoCtrlSelect(const vec2 &pos)
{
	OBSSceneItem item = GetItemAtPos(pos, false);
	if (!item) {
		return;
	}

	bool selected = obs_sceneitem_selected(item);
	obs_sceneitem_select(item, !selected);
	UpdateContextBar();
}

OBSSceneItem CanvasDock::GetItemAtPos(const vec2 &pos, bool selectBelow)
{
	if (!scene) {
		return OBSSceneItem();
	}

	SceneFindData sfd(pos, selectBelow);
	obs_scene_enum_items(scene, FindItemAtPos, &sfd);
	return sfd.item;
}

bool CanvasDock::FindItemAtPos(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	SceneFindData *data = reinterpret_cast<SceneFindData *>(param);
	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	if (!SceneItemHasVideo(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		if (data->selectBelow && obs_sceneitem_selected(item)) {
			if (data->item) {
				return false;
			} else {
				data->selectBelow = false;
			}
		}

		data->item = item;
	}

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::HandleMouseMoveEvent(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QPointF qtPos = event->position();
#else
	QPointF qtPos = event->localPos();
#endif

	float pixelRatio = GetDevicePixelRatio();

	if (scrollMode && event->buttons() == Qt::LeftButton) {
		scrollingOffset.x += pixelRatio * ((float)qtPos.x() - scrollingFrom.x);
		scrollingOffset.y += pixelRatio * ((float)qtPos.y() - scrollingFrom.y);
		scrollingFrom.x = (float)qtPos.x();
		scrollingFrom.y = (float)qtPos.y();
		UpdatePreviewScrollBars();
		if (preview) {
			preview->update();
		}
		return true;
	}

	if (locked) {
		return true;
	}

	bool updateCursor = false;

	if (mouseDown) {
		vec2 pos = GetMouseEventPos(event);

		if (!mouseMoved && !mouseOverItems && stretchHandle == ItemHandle::None) {
			ProcessClick(startPos);
			mouseOverItems = SelectedAtPos(scene, startPos);
		}

		pos.x = std::round(pos.x);
		pos.y = std::round(pos.y);

		if (stretchHandle != ItemHandle::None) {
			if (obs_sceneitem_locked(stretchItem)) {
				return true;
			}

			selectionBox = false;

			obs_sceneitem_t *group = obs_sceneitem_get_group(scene, stretchItem);
			if (group) {
				vec3 group_pos;
				vec3_set(&group_pos, pos.x, pos.y, 0.0f);
				vec3_transform(&group_pos, &group_pos, &invGroupTransform);
				pos.x = group_pos.x;
				pos.y = group_pos.y;
			}

			if (stretchHandle == ItemHandle::Rot) {
				RotateItem(pos);
				setCursor(Qt::ClosedHandCursor);
			} else if (cropping) {
				CropItem(pos);
			} else {
				StretchItem(pos);
			}

		} else if (mouseOverItems) {
			if (cursor().shape() != Qt::SizeAllCursor) {
				setCursor(Qt::SizeAllCursor);
			}
			selectionBox = false;
			MoveItems(pos);
		} else {
			selectionBox = true;
			if (!mouseMoved) {
				DoSelect(startPos);
			}
			BoxItems(startPos, pos);
		}

		mouseMoved = true;
		mousePos = pos;
	} else {
		vec2 pos = GetMouseEventPos(event);
		OBSSceneItem item = GetItemAtPos(pos, true);

		std::lock_guard<std::mutex> lock(selectMutex);
		hoveredPreviewItems.clear();
		hoveredPreviewItems.push_back(item);

		if (!mouseMoved && hoveredPreviewItems.size() > 0) {
			mousePos = pos;
			//float scale = GetDevicePixelRatio();
			//float x = qtPos.x(); // - main->previewX / scale;
			//float y = qtPos.y(); // - main->previewY / scale;
			vec2_set(&startPos, pos.x, pos.y);
			updateCursor = true;
		}
	}

	if (updateCursor) {
		GetStretchHandleData(startPos, true);
		uint32_t stretchFlags = (uint32_t)stretchHandle;
		UpdateCursor(stretchFlags);
	}
	return true;
}

void CanvasDock::RotateItem(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	bool ctrlDown = (modifiers & Qt::ControlModifier);

	vec2 pos2;
	vec2_copy(&pos2, &pos);

	float angle = atan2(pos2.y - rotatePoint.y, pos2.x - rotatePoint.x) + RAD(90);

#define ROT_SNAP(rot, thresh)                                    \
	if (abs(angle - RAD((float)rot)) < RAD((float)thresh)) { \
		angle = RAD((float)rot);                         \
	}

	if (shiftDown) {
		for (int i = 0; i <= 360 / 15; i++) {
			ROT_SNAP(i * 15 - 90, 7.5);
		}
	} else if (!ctrlDown) {
		ROT_SNAP(rotateAngle, 5)

		ROT_SNAP(-90, 5)
		ROT_SNAP(-45, 5)
		ROT_SNAP(0, 5)
		ROT_SNAP(45, 5)
		ROT_SNAP(90, 5)
		ROT_SNAP(135, 5)
		ROT_SNAP(180, 5)
		ROT_SNAP(225, 5)
		ROT_SNAP(270, 5)
		ROT_SNAP(315, 5)
	}
#undef ROT_SNAP

	vec2 pos3;
	vec2_copy(&pos3, &offsetPoint);
	RotatePos(&pos3, angle);
	pos3.x += rotatePoint.x;
	pos3.y += rotatePoint.y;

	obs_sceneitem_set_rot(stretchItem, DEG(angle));
	obs_sceneitem_set_pos(stretchItem, &pos3);
}

void CanvasDock::RotatePos(vec2 *pos, float rot)
{
	float cosR = cos(rot);
	float sinR = sin(rot);

	vec2 newPos;

	newPos.x = cosR * pos->x - sinR * pos->y;
	newPos.y = sinR * pos->x + cosR * pos->y;

	vec2_copy(pos, &newPos);
}

static float maxfunc(float x, float y)
{
	return x > y ? x : y;
}

static float minfunc(float x, float y)
{
	return x < y ? x : y;
}

void CanvasDock::CropItem(const vec2 &pos)
{
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(stretchItem);
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	uint32_t align = obs_sceneitem_get_alignment(stretchItem);
	vec3 tl, br, pos3;

	vec3_zero(&tl);
	vec3_set(&br, stretchItemSize.x, stretchItemSize.y, 0.0f);

	vec3_set(&pos3, pos.x, pos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &screenToItem);

	obs_sceneitem_crop crop = startCrop;
	vec2 scale;

	obs_sceneitem_get_scale(stretchItem, &scale);

	vec2 max_tl;
	vec2 max_br;

	vec2_set(&max_tl, float(-crop.left) * scale.x, float(-crop.top) * scale.y);
	vec2_set(&max_br, stretchItemSize.x + (float)crop.right * scale.x, stretchItemSize.y + (float)crop.bottom * scale.y);

	typedef std::function<float(float, float)> minmax_func_t;

	minmax_func_t min_x = scale.x < 0.0f ? maxfunc : minfunc;
	minmax_func_t min_y = scale.y < 0.0f ? maxfunc : minfunc;
	minmax_func_t max_x = scale.x < 0.0f ? minfunc : maxfunc;
	minmax_func_t max_y = scale.y < 0.0f ? minfunc : maxfunc;

	pos3.x = min_x(pos3.x, max_br.x);
	pos3.x = max_x(pos3.x, max_tl.x);
	pos3.y = min_y(pos3.y, max_br.y);
	pos3.y = max_y(pos3.y, max_tl.y);

	if (stretchFlags & ITEM_LEFT) {
		float maxX = stretchItemSize.x - (2.0f * scale.x);
		pos3.x = tl.x = min_x(pos3.x, maxX);

	} else if (stretchFlags & ITEM_RIGHT) {
		float minX = (2.0f * scale.x);
		pos3.x = br.x = max_x(pos3.x, minX);
	}

	if (stretchFlags & ITEM_TOP) {
		float maxY = stretchItemSize.y - (2.0f * scale.y);
		pos3.y = tl.y = min_y(pos3.y, maxY);

	} else if (stretchFlags & ITEM_BOTTOM) {
		float minY = (2.0f * scale.y);
		pos3.y = br.y = max_y(pos3.y, minY);
	}

#define ALIGN_X (ITEM_LEFT | ITEM_RIGHT)
#define ALIGN_Y (ITEM_TOP | ITEM_BOTTOM)
	vec3 newPos;
	vec3_zero(&newPos);

	uint32_t align_x = (align & ALIGN_X);
	uint32_t align_y = (align & ALIGN_Y);
	if (align_x == (stretchFlags & ALIGN_X) && align_x != 0) {
		newPos.x = pos3.x;
	} else if (align & ITEM_RIGHT) {
		newPos.x = stretchItemSize.x;
	} else if (!(align & ITEM_LEFT)) {
		newPos.x = stretchItemSize.x * 0.5f;
	}

	if (align_y == (stretchFlags & ALIGN_Y) && align_y != 0) {
		newPos.y = pos3.y;
	} else if (align & ITEM_BOTTOM) {
		newPos.y = stretchItemSize.y;
	} else if (!(align & ITEM_TOP)) {
		newPos.y = stretchItemSize.y * 0.5f;
	}
#undef ALIGN_X
#undef ALIGN_Y

	crop = startCrop;

	if (stretchFlags & ITEM_LEFT) {
		crop.left += int(std::round(tl.x / scale.x));
	} else if (stretchFlags & ITEM_RIGHT) {
		crop.right += int(std::round((stretchItemSize.x - br.x) / scale.x));
	}

	if (stretchFlags & ITEM_TOP) {
		crop.top += int(std::round(tl.y / scale.y));
	} else if (stretchFlags & ITEM_BOTTOM) {
		crop.bottom += int(std::round((stretchItemSize.y - br.y) / scale.y));
	}

	vec3_transform(&newPos, &newPos, &itemToScreen);
	newPos.x = std::round(newPos.x);
	newPos.y = std::round(newPos.y);

	obs_sceneitem_defer_update_begin(stretchItem);
	obs_sceneitem_set_crop(stretchItem, &crop);
	if (boundsType == OBS_BOUNDS_NONE) {
		obs_sceneitem_set_pos(stretchItem, (vec2 *)&newPos);
	}
	obs_sceneitem_defer_update_end(stretchItem);
}

void CanvasDock::StretchItem(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	obs_bounds_type boundsType = obs_sceneitem_get_bounds_type(stretchItem);
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	bool shiftDown = (modifiers & Qt::ShiftModifier);
	vec3 tl, br, pos3;

	vec3_zero(&tl);
	vec3_set(&br, stretchItemSize.x, stretchItemSize.y, 0.0f);

	vec3_set(&pos3, pos.x, pos.y, 0.0f);
	vec3_transform(&pos3, &pos3, &screenToItem);

	if (stretchFlags & ITEM_LEFT) {
		tl.x = pos3.x;
	} else if (stretchFlags & ITEM_RIGHT) {
		br.x = pos3.x;
	}

	if (stretchFlags & ITEM_TOP) {
		tl.y = pos3.y;
	} else if (stretchFlags & ITEM_BOTTOM) {
		br.y = pos3.y;
	}

	if (!(modifiers & Qt::ControlModifier)) {
		SnapStretchingToScreen(tl, br);
	}

	obs_source_t *s = obs_sceneitem_get_source(stretchItem);

	vec2 baseSize;
	vec2_set(&baseSize, float(obs_source_get_width(s)), float(obs_source_get_height(s)));

	vec2 size;
	vec2_set(&size, br.x - tl.x, br.y - tl.y);

	if (boundsType != OBS_BOUNDS_NONE) {
		if (shiftDown) {
			ClampAspect(tl, br, size, baseSize);
		}

		if (tl.x > br.x) {
			std::swap(tl.x, br.x);
		}
		if (tl.y > br.y) {
			std::swap(tl.y, br.y);
		}

		vec2_abs(&size, &size);

		obs_sceneitem_set_bounds(stretchItem, &size);
	} else {
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(stretchItem, &crop);

		baseSize.x -= float(crop.left + crop.right);
		baseSize.y -= float(crop.top + crop.bottom);

		if (baseSize.x > 0.0 && baseSize.y > 0.0) {
			if (!shiftDown) {
				ClampAspect(tl, br, size, baseSize);
			}

			vec2_div(&size, &size, &baseSize);
			obs_sceneitem_set_scale(stretchItem, &size);
		}
	}

	pos3 = CalculateStretchPos(tl, br);
	vec3_transform(&pos3, &pos3, &itemToScreen);

	vec2 newPos;
	vec2_set(&newPos, std::round(pos3.x), std::round(pos3.y));
	obs_sceneitem_set_pos(stretchItem, &newPos);
}

void CanvasDock::MoveItems(const vec2 &pos)
{
	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();

	vec2 offset, moveOffset;
	vec2_sub(&offset, &pos, &startPos);
	vec2_sub(&moveOffset, &offset, &lastMoveOffset);

	if (!(modifiers & Qt::ControlModifier)) {
		SnapItemMovement(moveOffset);
	}

	vec2_add(&lastMoveOffset, &lastMoveOffset, &moveOffset);

	obs_scene_enum_items(scene, move_items, &moveOffset);
}

bool CanvasDock::move_items(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	bool selected = obs_sceneitem_selected(item);
	vec2 *offset = reinterpret_cast<vec2 *>(param);

	if (obs_sceneitem_is_group(item) && !selected) {
		matrix4 transform;
		vec3 new_offset;
		vec3_set(&new_offset, offset->x, offset->y, 0.0f);

		obs_sceneitem_get_draw_transform(item, &transform);
		vec4_set(&transform.t, 0.0f, 0.0f, 0.0f, 1.0f);
		matrix4_inv(&transform, &transform);
		vec3_transform(&new_offset, &new_offset, &transform);
		obs_sceneitem_group_enum_items(item, move_items, &new_offset);
	}

	if (selected) {
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		vec2_add(&pos, &pos, offset);
		obs_sceneitem_set_pos(item, &pos);
	}

	UNUSED_PARAMETER(scene);
	return true;
}

void CanvasDock::SnapItemMovement(vec2 &offset)
{
	SelectedItemBounds sib;
	obs_scene_enum_items(scene, AddItemBounds, &sib);

	sib.tl.x += offset.x;
	sib.tl.y += offset.y;
	sib.br.x += offset.x;
	sib.br.y += offset.y;

	vec3 snapOffset = GetSnapOffset(sib.tl, sib.br);

	auto config = obs_frontend_get_user_config();
	if (!config) {
		return;
	}

	const bool snap = config_get_bool(config, "BasicWindow", "SnappingEnabled");
	const bool sourcesSnap = config_get_bool(config, "BasicWindow", "SourceSnapping");
	if (snap == false) {
		return;
	}
	if (sourcesSnap == false) {
		offset.x += snapOffset.x;
		offset.y += snapOffset.y;
		return;
	}

	const float clampDist = (float)config_get_double(config, "BasicWindow", "SnapDistance") / previewScale;

	OffsetData offsetData;
	offsetData.clampDist = clampDist;
	offsetData.tl = sib.tl;
	offsetData.br = sib.br;
	vec3_copy(&offsetData.offset, &snapOffset);

	obs_scene_enum_items(scene, GetSourceSnapOffset, &offsetData);

	if (fabsf(offsetData.offset.x) > EPSILON || fabsf(offsetData.offset.y) > EPSILON) {
		offset.x += offsetData.offset.x;
		offset.y += offsetData.offset.y;
	} else {
		offset.x += snapOffset.x;
		offset.y += snapOffset.y;
	}
}

void CanvasDock::BoxItems(const vec2 &start_pos, const vec2 &pos)
{
	if (!scene) {
		return;
	}

	if (cursor().shape() != Qt::CrossCursor) {
		setCursor(Qt::CrossCursor);
	}

	SceneFindBoxData sfbd(start_pos, pos);
	obs_scene_enum_items(scene, FindItemsInBox, &sfbd);

	std::lock_guard<std::mutex> lock(selectMutex);
	hoveredPreviewItems = sfbd.sceneItems;
}

bool CanvasDock::FindItemsInBox(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	SceneFindBoxData *data = reinterpret_cast<SceneFindBoxData *>(param);
	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec2 pos_min, pos_max;
	vec2_min(&pos_min, &data->startPos, &data->pos);
	vec2_max(&pos_max, &data->startPos, &data->pos);

	const float x1 = pos_min.x;
	const float x2 = pos_max.x;
	const float y1 = pos_min.y;
	const float y2 = pos_max.y;

	if (!SceneItemHasVideo(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}
	if (!obs_sceneitem_visible(item)) {
		return true;
	}

	vec3_set(&pos3, data->pos.x, data->pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x > x1 && transform.t.x < x2 && transform.t.y > y1 && transform.t.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.x.x > x1 && transform.t.x + transform.x.x < x2 && transform.t.y + transform.x.y > y1 &&
	    transform.t.y + transform.x.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.y.x > x1 && transform.t.x + transform.y.x < x2 && transform.t.y + transform.y.y > y1 &&
	    transform.t.y + transform.y.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + transform.x.x + transform.y.x > x1 && transform.t.x + transform.x.x + transform.y.x < x2 &&
	    transform.t.y + transform.x.y + transform.y.y > y1 && transform.t.y + transform.x.y + transform.y.y < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (transform.t.x + 0.5 * (transform.x.x + transform.y.x) > x1 &&
	    transform.t.x + 0.5 * (transform.x.x + transform.y.x) < x2 &&
	    transform.t.y + 0.5 * (transform.x.y + transform.y.y) > y1 &&
	    transform.t.y + 0.5 * (transform.x.y + transform.y.y) < y2) {

		data->sceneItems.push_back(item);
		return true;
	}

	if (IntersectBox(transform, x1, x2, y1, y2)) {
		data->sceneItems.push_back(item);
		return true;
	}

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::IntersectBox(matrix4 transform, float x1, float x2, float y1, float y2)
{
	float x3, x4, y3, y4;

	x3 = transform.t.x;
	y3 = transform.t.y;
	x4 = x3 + transform.x.x;
	y4 = y3 + transform.x.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4)) {
		return true;
	}

	x4 = x3 + transform.y.x;
	y4 = y3 + transform.y.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4)) {
		return true;
	}

	x3 = transform.t.x + transform.x.x;
	y3 = transform.t.y + transform.x.y;
	x4 = x3 + transform.y.x;
	y4 = y3 + transform.y.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4)) {
		return true;
	}

	x3 = transform.t.x + transform.y.x;
	y3 = transform.t.y + transform.y.y;
	x4 = x3 + transform.x.x;
	y4 = y3 + transform.x.y;

	if (IntersectLine(x1, x1, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y1, y1, y3, y4) ||
	    IntersectLine(x2, x2, x3, x4, y1, y2, y3, y4) || IntersectLine(x1, x2, x3, x4, y2, y2, y3, y4)) {
		return true;
	}

	return false;
}

bool CanvasDock::CounterClockwise(float x1, float x2, float x3, float y1, float y2, float y3)
{
	return (y3 - y1) * (x2 - x1) > (y2 - y1) * (x3 - x1);
}

bool CanvasDock::IntersectLine(float x1, float x2, float x3, float x4, float y1, float y2, float y3, float y4)
{
	bool a = CounterClockwise(x1, x2, x3, y1, y2, y3);
	bool b = CounterClockwise(x1, x2, x4, y1, y2, y4);
	bool c = CounterClockwise(x3, x4, x1, y3, y4, y1);
	bool d = CounterClockwise(x3, x4, x2, y3, y4, y2);

	return (a != b) && (c != d);
}

void CanvasDock::GetStretchHandleData(const vec2 &pos, bool ignoreGroup)
{
	if (!scene) {
		return;
	}

	HandleFindData hfd(pos, previewScale);
	obs_scene_enum_items(scene, FindHandleAtPos, &hfd);

	stretchItem = std::move(hfd.item);
	stretchHandle = hfd.handle;

	rotateAngle = hfd.angle;
	rotatePoint = hfd.rotatePoint;
	offsetPoint = hfd.offsetPoint;

	if (stretchHandle != ItemHandle::None) {
		matrix4 boxTransform;
		vec3 itemUL;
		float itemRot;

		stretchItemSize = GetItemSize(stretchItem);

		obs_sceneitem_get_box_transform(stretchItem, &boxTransform);
		itemRot = obs_sceneitem_get_rot(stretchItem);
		vec3_from_vec4(&itemUL, &boxTransform.t);

		/* build the item space conversion matrices */
		matrix4_identity(&itemToScreen);
		matrix4_rotate_aa4f(&itemToScreen, &itemToScreen, 0.0f, 0.0f, 1.0f, RAD(itemRot));
		matrix4_translate3f(&itemToScreen, &itemToScreen, itemUL.x, itemUL.y, 0.0f);

		matrix4_identity(&screenToItem);
		matrix4_translate3f(&screenToItem, &screenToItem, -itemUL.x, -itemUL.y, 0.0f);
		matrix4_rotate_aa4f(&screenToItem, &screenToItem, 0.0f, 0.0f, 1.0f, RAD(-itemRot));

		obs_sceneitem_get_crop(stretchItem, &startCrop);
		obs_sceneitem_get_pos(stretchItem, &startItemPos);

		obs_source_t *s = obs_sceneitem_get_source(stretchItem);
		cropSize.x = float(obs_source_get_width(s) - startCrop.left - startCrop.right);
		cropSize.y = float(obs_source_get_height(s) - startCrop.top - startCrop.bottom);

		stretchGroup = obs_sceneitem_get_group(scene, stretchItem);
		if (stretchGroup && !ignoreGroup) {
			obs_sceneitem_get_draw_transform(stretchGroup, &invGroupTransform);
			matrix4_inv(&invGroupTransform, &invGroupTransform);
			obs_sceneitem_defer_group_resize_begin(stretchGroup);
		} else {
			stretchGroup = nullptr;
		}
	}
}

void CanvasDock::UpdateCursor(uint32_t &flags)
{
	if (obs_sceneitem_locked(stretchItem)) {
		unsetCursor();
		return;
	}

	if (!flags && (cursor().shape() != Qt::OpenHandCursor || !scrollMode)) {
		unsetCursor();
	}
	if (cursor().shape() != Qt::ArrowCursor) {
		return;
	}

	if ((flags & ITEM_LEFT && flags & ITEM_TOP) || (flags & ITEM_RIGHT && flags & ITEM_BOTTOM)) {
		setCursor(Qt::SizeFDiagCursor);
	} else if ((flags & ITEM_LEFT && flags & ITEM_BOTTOM) || (flags & ITEM_RIGHT && flags & ITEM_TOP)) {
		setCursor(Qt::SizeBDiagCursor);
	} else if (flags & ITEM_LEFT || flags & ITEM_RIGHT) {
		setCursor(Qt::SizeHorCursor);
	} else if (flags & ITEM_TOP || flags & ITEM_BOTTOM) {
		setCursor(Qt::SizeVerCursor);
	} else if (flags & ITEM_ROT) {
		setCursor(Qt::OpenHandCursor);
	}
}

void CanvasDock::SnapStretchingToScreen(vec3 &tl, vec3 &br)
{
	uint32_t stretchFlags = (uint32_t)stretchHandle;
	vec3 newTL = GetTransformedPos(tl.x, tl.y, itemToScreen);
	vec3 newTR = GetTransformedPos(br.x, tl.y, itemToScreen);
	vec3 newBL = GetTransformedPos(tl.x, br.y, itemToScreen);
	vec3 newBR = GetTransformedPos(br.x, br.y, itemToScreen);
	vec3 boundingTL;
	vec3 boundingBR;

	vec3_copy(&boundingTL, &newTL);
	vec3_min(&boundingTL, &boundingTL, &newTR);
	vec3_min(&boundingTL, &boundingTL, &newBL);
	vec3_min(&boundingTL, &boundingTL, &newBR);

	vec3_copy(&boundingBR, &newTL);
	vec3_max(&boundingBR, &boundingBR, &newTR);
	vec3_max(&boundingBR, &boundingBR, &newBL);
	vec3_max(&boundingBR, &boundingBR, &newBR);

	vec3 offset = GetSnapOffset(boundingTL, boundingBR);
	vec3_add(&offset, &offset, &newTL);
	vec3_transform(&offset, &offset, &screenToItem);
	vec3_sub(&offset, &offset, &tl);

	if (stretchFlags & ITEM_LEFT) {
		tl.x += offset.x;
	} else if (stretchFlags & ITEM_RIGHT) {
		br.x += offset.x;
	}

	if (stretchFlags & ITEM_TOP) {
		tl.y += offset.y;
	} else if (stretchFlags & ITEM_BOTTOM) {
		br.y += offset.y;
	}
}

void CanvasDock::ClampAspect(vec3 &tl, vec3 &br, vec2 &size, const vec2 &baseSize)
{
	float baseAspect = baseSize.x / baseSize.y;
	float aspect = size.x / size.y;
	uint32_t stretchFlags = (uint32_t)stretchHandle;

	if (stretchHandle == ItemHandle::TopLeft || stretchHandle == ItemHandle::TopRight ||
	    stretchHandle == ItemHandle::BottomLeft || stretchHandle == ItemHandle::BottomRight) {
		if (aspect < baseAspect) {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.x = size.y * baseAspect;
			} else {
				size.x = size.y * baseAspect * -1.0f;
			}
		} else {
			if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
				size.y = size.x / baseAspect;
			} else {
				size.y = size.x / baseAspect * -1.0f;
			}
		}

	} else if (stretchHandle == ItemHandle::TopCenter || stretchHandle == ItemHandle::BottomCenter) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.x = size.y * baseAspect;
		} else {
			size.x = size.y * baseAspect * -1.0f;
		}

	} else if (stretchHandle == ItemHandle::CenterLeft || stretchHandle == ItemHandle::CenterRight) {
		if ((size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f)) {
			size.y = size.x / baseAspect;
		} else {
			size.y = size.x / baseAspect * -1.0f;
		}
	}

	size.x = std::round(size.x);
	size.y = std::round(size.y);

	if (stretchFlags & ITEM_LEFT) {
		tl.x = br.x - size.x;
	} else if (stretchFlags & ITEM_RIGHT) {
		br.x = tl.x + size.x;
	}

	if (stretchFlags & ITEM_TOP) {
		tl.y = br.y - size.y;
	} else if (stretchFlags & ITEM_BOTTOM) {
		br.y = tl.y + size.y;
	}
}

vec3 CanvasDock::CalculateStretchPos(const vec3 &tl, const vec3 &br)
{
	uint32_t alignment = obs_sceneitem_get_alignment(stretchItem);
	vec3 pos;

	vec3_zero(&pos);

	if (alignment & OBS_ALIGN_LEFT) {
		pos.x = tl.x;
	} else if (alignment & OBS_ALIGN_RIGHT) {
		pos.x = br.x;
	} else {
		pos.x = (br.x - tl.x) * 0.5f + tl.x;
	}

	if (alignment & OBS_ALIGN_TOP) {
		pos.y = tl.y;
	} else if (alignment & OBS_ALIGN_BOTTOM) {
		pos.y = br.y;
	} else {
		pos.y = (br.y - tl.y) * 0.5f + tl.y;
	}

	return pos;
}

bool CanvasDock::AddItemBounds(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	SelectedItemBounds *data = reinterpret_cast<SelectedItemBounds *>(param);
	vec3 t[4];

	auto add_bounds = [data, &t]() {
		for (const vec3 &v : t) {
			if (data->first) {
				vec3_copy(&data->tl, &v);
				vec3_copy(&data->br, &v);
				data->first = false;
			} else {
				vec3_min(&data->tl, &data->tl, &v);
				vec3_max(&data->br, &data->br, &v);
			}
		}
	};

	if (obs_sceneitem_is_group(item)) {
		SelectedItemBounds sib;
		obs_sceneitem_group_enum_items(item, AddItemBounds, &sib);

		if (!sib.first) {
			matrix4 xform;
			obs_sceneitem_get_draw_transform(item, &xform);

			vec3_set(&t[0], sib.tl.x, sib.tl.y, 0.0f);
			vec3_set(&t[1], sib.tl.x, sib.br.y, 0.0f);
			vec3_set(&t[2], sib.br.x, sib.tl.y, 0.0f);
			vec3_set(&t[3], sib.br.x, sib.br.y, 0.0f);
			vec3_transform(&t[0], &t[0], &xform);
			vec3_transform(&t[1], &t[1], &xform);
			vec3_transform(&t[2], &t[2], &xform);
			vec3_transform(&t[3], &t[3], &xform);
			add_bounds();
		}
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	t[0] = GetTransformedPos(0.0f, 0.0f, boxTransform);
	t[1] = GetTransformedPos(1.0f, 0.0f, boxTransform);
	t[2] = GetTransformedPos(0.0f, 1.0f, boxTransform);
	t[3] = GetTransformedPos(1.0f, 1.0f, boxTransform);
	add_bounds();

	UNUSED_PARAMETER(scene);
	return true;
}

vec3 CanvasDock::GetSnapOffset(const vec3 &tl, const vec3 &br)
{
	auto s = obs_weak_source_get_source(source);
	vec2 screenSize;
	screenSize.x = (float)obs_source_get_base_width(s);
	screenSize.y = (float)obs_source_get_base_height(s);
	obs_source_release(s);
	vec3 clampOffset;

	vec3_zero(&clampOffset);

	auto config = obs_frontend_get_user_config();
	if (!config) {
		return clampOffset;
	}

	const bool snap = config_get_bool(config, "BasicWindow", "SnappingEnabled");
	if (snap == false) {
		return clampOffset;
	}

	const bool screenSnap = config_get_bool(config, "BasicWindow", "ScreenSnapping");
	const bool centerSnap = config_get_bool(config, "BasicWindow", "CenterSnapping");

	const float clampDist = (float)config_get_double(config, "BasicWindow", "SnapDistance") / previewScale;
	const float centerX = br.x - (br.x - tl.x) / 2.0f;
	const float centerY = br.y - (br.y - tl.y) / 2.0f;

	// Left screen edge.
	if (screenSnap && fabsf(tl.x) < clampDist) {
		clampOffset.x = -tl.x;
	}
	// Right screen edge.
	if (screenSnap && fabsf(clampOffset.x) < EPSILON && fabsf(screenSize.x - br.x) < clampDist) {
		clampOffset.x = screenSize.x - br.x;
	}
	// Horizontal center.
	if (centerSnap && fabsf(screenSize.x - (br.x - tl.x)) > clampDist && fabsf(screenSize.x / 2.0f - centerX) < clampDist) {
		clampOffset.x = screenSize.x / 2.0f - centerX;
	}

	// Top screen edge.
	if (screenSnap && fabsf(tl.y) < clampDist) {
		clampOffset.y = -tl.y;
	}
	// Bottom screen edge.
	if (screenSnap && fabsf(clampOffset.y) < EPSILON && fabsf(screenSize.y - br.y) < clampDist) {
		clampOffset.y = screenSize.y - br.y;
	}
	// Vertical center.
	if (centerSnap && fabsf(screenSize.y - (br.y - tl.y)) > clampDist && fabsf(screenSize.y / 2.0f - centerY) < clampDist) {
		clampOffset.y = screenSize.y / 2.0f - centerY;
	}

	return clampOffset;
}

bool CanvasDock::GetSourceSnapOffset(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	OffsetData *data = reinterpret_cast<OffsetData *>(param);

	if (obs_sceneitem_selected(item)) {
		return true;
	}

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3 t[4] = {GetTransformedPos(0.0f, 0.0f, boxTransform), GetTransformedPos(1.0f, 0.0f, boxTransform),
		     GetTransformedPos(0.0f, 1.0f, boxTransform), GetTransformedPos(1.0f, 1.0f, boxTransform)};

	bool first = true;
	vec3 tl, br;
	vec3_zero(&tl);
	vec3_zero(&br);
	for (const vec3 &v : t) {
		if (first) {
			vec3_copy(&tl, &v);
			vec3_copy(&br, &v);
			first = false;
		} else {
			vec3_min(&tl, &tl, &v);
			vec3_max(&br, &br, &v);
		}
	}

	// Snap to other source edges
#define EDGE_SNAP(l, r, x, y)                                                                                              \
	do {                                                                                                               \
		double dist = fabsf(l.x - data->r.x);                                                                      \
		if (dist < data->clampDist && fabsf(data->offset.x) < EPSILON && data->tl.y < br.y && data->br.y > tl.y && \
		    (fabsf(data->offset.x) > dist || data->offset.x < EPSILON))                                            \
			data->offset.x = l.x - data->r.x;                                                                  \
	} while (false)

	EDGE_SNAP(tl, br, x, y);
	EDGE_SNAP(tl, br, y, x);
	EDGE_SNAP(br, tl, x, y);
	EDGE_SNAP(br, tl, y, x);
#undef EDGE_SNAP

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::FindHandleAtPos(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	HandleFindData &data = *reinterpret_cast<HandleFindData *>(param);

	if (!obs_sceneitem_selected(item)) {
		if (obs_sceneitem_is_group(item)) {
			HandleFindData newData(data, item);
			newData.angleOffset = obs_sceneitem_get_rot(item);

			obs_sceneitem_group_enum_items(item, FindHandleAtPos, &newData);

			data.item = newData.item;
			data.handle = newData.handle;
			data.angle = newData.angle;
			data.rotatePoint = newData.rotatePoint;
			data.offsetPoint = newData.offsetPoint;
		}

		return true;
	}

	matrix4 transform;
	vec3 pos3;
	float closestHandle = data.radius;

	vec3_set(&pos3, data.pos.x, data.pos.y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	auto TestHandle = [&](float x, float y, ItemHandle handle) {
		vec3 handlePos = GetTransformedPos(x, y, transform);
		vec3_transform(&handlePos, &handlePos, &data.parent_xform);

		float dist = vec3_dist(&handlePos, &pos3);
		if (dist < data.radius) {
			if (dist < closestHandle) {
				closestHandle = dist;
				data.handle = handle;
				data.item = item;
			}
		}
	};

	TestHandle(0.0f, 0.0f, ItemHandle::TopLeft);
	TestHandle(0.5f, 0.0f, ItemHandle::TopCenter);
	TestHandle(1.0f, 0.0f, ItemHandle::TopRight);
	TestHandle(0.0f, 0.5f, ItemHandle::CenterLeft);
	TestHandle(1.0f, 0.5f, ItemHandle::CenterRight);
	TestHandle(0.0f, 1.0f, ItemHandle::BottomLeft);
	TestHandle(0.5f, 1.0f, ItemHandle::BottomCenter);
	TestHandle(1.0f, 1.0f, ItemHandle::BottomRight);

	vec2 rotHandleOffset;
	vec2_set(&rotHandleOffset, 0.0f, HANDLE_RADIUS * data.radius * 1.5f - data.radius);
	RotatePos(&rotHandleOffset, atan2(transform.x.y, transform.x.x));
	RotatePos(&rotHandleOffset, RAD(data.angleOffset));

	vec3 handlePos = GetTransformedPos(0.5f, 0.0f, transform);
	vec3_transform(&handlePos, &handlePos, &data.parent_xform);
	handlePos.x -= rotHandleOffset.x;
	handlePos.y -= rotHandleOffset.y;

	float dist = vec3_dist(&handlePos, &pos3);
	if (dist < data.radius) {
		if (dist < closestHandle) {
			closestHandle = dist;
			data.item = item;
			data.angle = obs_sceneitem_get_rot(item);
			data.handle = ItemHandle::Rot;

			vec2_set(&data.rotatePoint, transform.t.x + transform.x.x / 2 + transform.y.x / 2,
				 transform.t.y + transform.x.y / 2 + transform.y.y / 2);

			obs_sceneitem_get_pos(item, &data.offsetPoint);
			data.offsetPoint.x -= data.rotatePoint.x;
			data.offsetPoint.y -= data.rotatePoint.y;

			RotatePos(&data.offsetPoint, -RAD(obs_sceneitem_get_rot(item)));
		}
	}

	UNUSED_PARAMETER(scene);
	return true;
}

bool CanvasDock::HandleMouseLeaveEvent(QMouseEvent *event)
{
	UNUSED_PARAMETER(event);
	std::lock_guard<std::mutex> lock(selectMutex);
	if (!selectionBox) {
		hoveredPreviewItems.clear();
	}
	return true;
}

bool CanvasDock::HandleMouseWheelEvent(QWheelEvent *event)
{
	if (event->angleDelta().y() > 0) {
		IncreaseScalingLevel();
	} else if (event->angleDelta().y() < 0) {
		DecreaseScalingLevel();
	}
	return true;
}

bool CanvasDock::HandleKeyPressEvent(QKeyEvent *event)
{
	UNUSED_PARAMETER(event);
	return true;
}

bool CanvasDock::HandleKeyReleaseEvent(QKeyEvent *event)
{
	UNUSED_PARAMETER(event);
	return true;
}

void CanvasDock::AddSourceFromAction(QString idText)
{
	QByteArray idUtf8 = idText.toUtf8();
	const char *id = idUtf8.constData();
	if (!id || !*id || !scene) {
		return;
	}

	const char *v_id = obs_get_latest_input_type_id(id);
	if (!v_id || !*v_id) {
		v_id = id;
	}
	QString placeHolderText = QString::fromUtf8(obs_source_get_display_name(v_id));
	if (placeHolderText.isEmpty()) {
		placeHolderText = idText;
	}
	QString text = placeHolderText;
	int i = 2;
	obs_source_t *created_source = nullptr;
	if (strcmp(id, "group") == 0) {
		OBSSourceAutoRelease existing = nullptr;
		while ((existing = obs_canvas_get_source_by_name(canvas, text.toUtf8().constData()))) {
			text = QString("%1 %2").arg(placeHolderText).arg(i++);
		}
		obs_sceneitem_t *group = obs_scene_add_group(scene, text.toUtf8().constData());
		if (!group) {
			blog(LOG_WARNING, "[Stream Suite] Failed to add group '%s' to canvas scene", text.toUtf8().constData());
		}
		return;
	}
	if (obs_get_source_output_flags(id) & OBS_SOURCE_REQUIRES_CANVAS) {
		OBSSourceAutoRelease existing = nullptr;
		while ((existing = obs_canvas_get_source_by_name(canvas, text.toUtf8().constData()))) {
			text = QString("%1 %2").arg(placeHolderText).arg(i++);
		}
		created_source = obs_scene_get_source(obs_canvas_scene_create(canvas, text.toUtf8().constData()));
	} else {
		OBSSourceAutoRelease existing = nullptr;
		while ((existing = obs_get_source_by_name(text.toUtf8().constData()))) {
			text = QString("%1 %2").arg(placeHolderText).arg(i++);
		}
		created_source = obs_source_create(v_id, text.toUtf8().constData(), nullptr, nullptr);
	}
	if (!created_source) {
		blog(LOG_WARNING, "[Stream Suite] Failed to create source '%s'", text.toUtf8().constData());
		return;
	}
	obs_sceneitem_t *item = obs_scene_add(scene, created_source);
	if (!item) {
		blog(LOG_WARNING, "[Stream Suite] Failed to add source '%s' to canvas scene", obs_source_get_name(created_source));
		obs_source_remove(created_source);
		obs_source_release(created_source);
		return;
	}
	if (obs_source_configurable(created_source)) {
		obs_frontend_open_source_properties(created_source);
	}
	obs_source_release(created_source);
}

void CanvasDock::MainSceneChanged()
{
	auto current_scene = obs_frontend_get_current_scene();
	if (!current_scene) {
		//if (linkedButton)
		//	linkedButton->setChecked(false);
		return;
	}

	auto ss = obs_source_get_settings(current_scene);
	obs_source_release(current_scene);
	auto c = obs_data_get_array(ss, "canvas");
	obs_data_release(ss);
	if (!c) {
		//if (linkedButton)
		//	linkedButton->setChecked(false);
		return;
	}
	const auto count = obs_data_array_count(c);
	obs_data_t *found = nullptr;
	for (size_t i = 0; i < count; i++) {
		auto item = obs_data_array_item(c, i);
		if (!item) {
			continue;
		}
		if (strcmp(obs_data_get_string(item, "name"), obs_canvas_get_name(canvas)) == 0) {
			found = item;
			break;
		} else if (strcmp(obs_data_get_string(item, "name"), "") == 0 && obs_data_get_int(item, "width") == canvas_width &&
			   obs_data_get_int(item, "height") == canvas_height) {
			obs_data_set_string(item, "name", obs_canvas_get_name(canvas));
			found = item;
			break;
		}
		obs_data_release(item);
	}
	if (found) {
		auto sn = QString::fromUtf8(obs_data_get_string(found, "scene"));
		SwitchScene(sn);
		//if (linkedButton)
		//	linkedButton->setChecked(true);
	} // else if (linkedButton) {
	//	linkedButton->setChecked(false);
	//}
	obs_data_release(found);
	obs_data_array_release(c);
}

void CanvasDock::save_load(obs_data_t *save_data, bool saving, void *param)
{
	UNUSED_PARAMETER(save_data);
	CanvasDock *window = static_cast<CanvasDock *>(param);
	if (saving) {
		if (window->sceneList) {
			auto c = window->sceneList->count();
			for (int row = 0; row < c; row++) {
				auto scene_name = window->sceneList->item(row)->text();
				auto scene = obs_canvas_get_source_by_name(window->canvas, scene_name.toUtf8().constData());
				if (scene) {
					auto settings = obs_source_get_settings(scene);
					obs_data_set_int(settings, "order", row);
					obs_data_set_bool(settings, "canvas_active", scene_name == window->currentSceneName);
					obs_data_release(settings);
					obs_source_release(scene);
				}
			}
		}
		if (window->sceneCombo) {
			auto c = window->sceneCombo->count();
			for (int row = 0; row < c; row++) {
				auto scene_name = window->sceneCombo->itemText(row);
				auto scene = obs_canvas_get_source_by_name(window->canvas, scene_name.toUtf8().constData());
				if (scene) {
					auto settings = obs_source_get_settings(scene);
					obs_data_set_int(settings, "order", row);
					obs_data_set_bool(settings, "canvas_active", scene_name == window->currentSceneName);
					obs_data_release(settings);
					obs_source_release(scene);
				}
			}
		}
	} else {
		if (window->canvas && obs_canvas_removed(window->canvas)) {
			obs_canvas_release(window->canvas);
			window->canvas = nullptr;
		}
		if (!window->canvas) {
			window->canvas = obs_get_canvas_by_name(window->canvas_name.c_str());
			if (window->canvas) {
				if (window->canvas && obs_canvas_removed(window->canvas)) {
					obs_canvas_release(window->canvas);
					window->canvas = nullptr;
				} else if (!window->settings && window->canvas &&
					   obs_canvas_get_flags(window->canvas) != SCENE_REF) {
					obs_frontend_remove_canvas(window->canvas);
					obs_canvas_remove(window->canvas);
					obs_canvas_release(window->canvas);
					window->canvas = nullptr;
				}
			}
		}
		if (!window->settings) {
			obs_video_info ovi;
			obs_get_video_info(&ovi);
			if (!window->canvas) {
				window->canvas = obs_frontend_add_canvas(window->canvas_name.c_str(), &ovi, SCENE_REF);
				blog(LOG_INFO, "[Aitum Stream Suite] Add frontend canvas '%s'", window->canvas_name.c_str());
			} else if (obs_canvas_reset_video(window->canvas, &ovi)) {
				blog(LOG_INFO, "[Aitum Stream Suite] Reset video on canvas '%s' to %ux%u",
				     obs_canvas_get_name(window->canvas), ovi.base_width, ovi.base_height);
			} else {
				blog(LOG_ERROR, "[Aitum Stream Suite] Failed to reset video on canvas '%s'",
				     obs_canvas_get_name(window->canvas));
			}
			if (window->canvas) {
				auto sh = obs_canvas_get_signal_handler(window->canvas);
				if (sh) {
					signal_handler_disconnect(sh, "source_add", source_add, window);
					signal_handler_connect(sh, "source_add", source_add, window);
				}
			}
			window->LoadMode(QStringLiteral("default"));
			window->LoadScenes();
			window->LogScenes();
		}
	}
}

void CanvasDock::LoadMode(QString mode)
{
	if (panel_split && settings) {
		panel_split->blockSignals(true);
		std::string setting_name = "panel_split_" + mode.toStdString();
		auto state = obs_data_get_string(settings, setting_name.c_str());
		if (state[0] == '\0') {
			std::transform(setting_name.begin(), setting_name.end(), setting_name.begin(),
				       [](unsigned char c) { return std::tolower(c); });
			state = obs_data_get_string(settings, setting_name.c_str());
		}
		setting_name = "panel_split_automatic_" + mode.toStdString();
		obs_data_set_default_bool(settings, setting_name.c_str(), true);
		panel_split->automaticSwitching = obs_data_get_bool(settings, setting_name.c_str());
		setting_name = "panel_split_horizontal_" + mode.toStdString();
		panel_split->setOrientation(obs_data_get_bool(settings, setting_name.c_str()) ? Qt::Horizontal : Qt::Vertical);
		setting_name = "panel_split_order_" + mode.toStdString();
		auto order = obs_data_get_string(settings, setting_name.c_str());
		if (order[0] != '\0') {
			panel_split->restorePanelOrder(QString::fromUtf8(order));
		}
		if (state[0] != '\0') {
			panel_split->restoreState(QByteArray::fromBase64(state));
		}
		panel_split->blockSignals(false);
	}
	if (canvas_split) {
		canvas_split->blockSignals(true);
		auto state = "";
		if (settings) {
			std::string setting_name = "canvas_split_" + mode.toStdString();
			state = obs_data_get_string(settings, setting_name.c_str());
			if (state[0] == '\0') {
				setting_name = "canvas_split_" + mode.toLower().toStdString();
				state = obs_data_get_string(settings, setting_name.c_str());
			}
			setting_name = "canvas_split_automatic_" + mode.toStdString();
			obs_data_set_default_bool(settings, setting_name.c_str(), true);
			canvas_split->automaticSwitching = obs_data_get_bool(settings, setting_name.c_str());
			setting_name = "canvas_split_horizontal_" + mode.toStdString();
			canvas_split->setOrientation(obs_data_get_bool(settings, setting_name.c_str()) ? Qt::Horizontal
												       : Qt::Vertical);
			setting_name = "canvas_split_order_" + mode.toStdString();
			auto order = obs_data_get_string(settings, setting_name.c_str());
			if (order[0] != '\0') {
				canvas_split->restorePanelOrder(QString::fromUtf8(order));
			}
		} else if (current_profile_config) {
			std::string setting_name = canvas_name + "_canvas_split_" + mode.toStdString();
			state = obs_data_get_string(current_profile_config, setting_name.c_str());
			if (state[0] == '\0') {
				setting_name = canvas_name + "_canvas_split_" + mode.toLower().toStdString();
				state = obs_data_get_string(current_profile_config, setting_name.c_str());
			}
			setting_name = canvas_name + "_canvas_split_automatic_" + mode.toStdString();
			obs_data_set_default_bool(current_profile_config, setting_name.c_str(), true);
			canvas_split->automaticSwitching = obs_data_get_bool(current_profile_config, setting_name.c_str());
			setting_name = canvas_name + "_canvas_split_horizontal_" + mode.toStdString();
			canvas_split->setOrientation(
				obs_data_get_bool(current_profile_config, setting_name.c_str()) ? Qt::Horizontal : Qt::Vertical);
			setting_name = canvas_name + "_canvas_split_order_" + mode.toStdString();
			auto order = obs_data_get_string(current_profile_config, setting_name.c_str());
			if (order[0] != '\0') {
				canvas_split->restorePanelOrder(QString::fromUtf8(order));
			}
		}
		if (state[0] != '\0') {
			canvas_split->restoreState(QByteArray::fromBase64(state));
		}
		/* Extra canvas docks are preview-only; never restore a split that
		 * would show scenes/sources/transitions inside the canvas dock. */
		if (panel_split) {
			panel_split->setParent(this);
			panel_split->hide();
		}
		canvas_split->blockSignals(false);
	}
}

void CanvasDock::UpdateSettings(obs_data_t *s)
{
	if (s) {
		obs_data_release(settings);
		settings = s;
	}

	canvas_width = (uint32_t)obs_data_get_int(settings, "width");
	if (canvas_width < 1) {
		canvas_width = 1080;
	}
	canvas_height = (uint32_t)obs_data_get_int(settings, "height");
	if (canvas_height < 1) {
		canvas_height = 1920;
	}

	obs_video_info ovi;
	if (obs_canvas_get_video_info(canvas, &ovi) && (ovi.base_width != canvas_width || ovi.base_height != canvas_height ||
							ovi.output_width != canvas_width || ovi.output_height != canvas_height)) {
		obs_get_video_info(&ovi);
		ovi.base_height = canvas_height;
		ovi.base_width = canvas_width;
		ovi.output_height = canvas_height;
		ovi.output_width = canvas_width;
		if (obs_canvas_reset_video(canvas, &ovi)) {
			blog(LOG_INFO, "[Aitum Stream Suite] Canvas '%s' reset video %ux%u", obs_canvas_get_name(canvas),
			     canvas_width, canvas_height);
		} else {
			blog(LOG_ERROR, "[Aitum Stream Suite] Failed to reset video on canvas '%s'", obs_canvas_get_name(canvas));
		}
	}
}

void CanvasDock::LogScenes()
{
	blog(LOG_INFO, "------------------------------------------------");
	blog(LOG_INFO, "[Aitum Stream Suite] Canvas '%s' scenes:", obs_canvas_get_name(canvas));
	if (sceneList) {
		for (int j = 0; j < sceneList->count(); j++) {
			auto item = sceneList->item(j);
			blog(LOG_INFO, "- scene '%s':", item->text().toUtf8().constData());
			auto scene = obs_canvas_get_scene_by_name(canvas, item->text().toUtf8().constData());
			obs_scene_enum_items(scene, LogSceneItem, (void *)(intptr_t)1);
			obs_source_enum_filters(obs_scene_get_source(scene), LogFilter, (void *)(intptr_t)1);

			obs_scene_release(scene);
		}
	} else if (sceneCombo) {
		for (int j = 0; j < sceneCombo->count(); j++) {
			auto scene_name = sceneCombo->itemText(j);
			blog(LOG_INFO, "- scene '%s':", scene_name.toUtf8().constData());
			auto scene = obs_canvas_get_scene_by_name(canvas, scene_name.toUtf8().constData());
			obs_scene_enum_items(scene, LogSceneItem, (void *)(intptr_t)1);
			obs_source_enum_filters(obs_scene_get_source(scene), LogFilter, (void *)(intptr_t)1);
			obs_scene_release(scene);
		}
	}
	blog(LOG_INFO, "------------------------------------------------");
}

bool CanvasDock::LogSceneItem(obs_scene_t *, obs_sceneitem_t *item, void *v_val)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	const char *name = obs_source_get_name(source);
	const char *id = obs_source_get_id(source);
	int indent_count = (int)(intptr_t)v_val;
	std::string indent;

	for (int i = 0; i < indent_count; i++) {
		indent += "    ";
	}

	blog(LOG_INFO, "%s- source: '%s' (%s)", indent.c_str(), name, id);

	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
		const uint32_t all_mixers = (1 << MAX_AUDIO_MIXES) - 1;
		uint32_t mixers = obs_source_get_audio_mixers(source);
		if (mixers == 0) {
			blog(LOG_INFO, "    %s- audio tracks: none", indent.c_str());
		} else if ((mixers & all_mixers) != all_mixers) {
			std::string tracks;
			for (uint32_t i = 0; i < MAX_AUDIO_MIXES; i++) {
				if (mixers & (1 << i)) {
					tracks += " ";
					tracks += std::to_string(i + 1);
				}
			}
			blog(LOG_INFO, "    %s- audio tracks:%s", indent.c_str(), tracks.c_str());
		}

#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(33, 0, 0)
		bool monitoring = obs_source_get_monitoring_enabled(source);
		if (monitoring) {
			blog(LOG_INFO, "    %s- monitoring: enabled", indent.c_str());
		}
#else
		obs_monitoring_type monitoring_type = obs_source_get_monitoring_type(source);
		if (monitoring_type != OBS_MONITORING_TYPE_NONE) {
			const char *type = (monitoring_type == OBS_MONITORING_TYPE_MONITOR_ONLY) ? "monitor only"
												 : "monitor and output";
			blog(LOG_INFO, "    %s- monitoring: %s", indent.c_str(), type);
		}
#endif
	}
	int child_indent = 1 + indent_count;
	obs_source_enum_filters(source, LogFilter, (void *)(intptr_t)child_indent);

	obs_source_t *show_tn = obs_sceneitem_get_transition(item, true);
	obs_source_t *hide_tn = obs_sceneitem_get_transition(item, false);
	if (show_tn) {
		blog(LOG_INFO, "    %s- show: '%s' (%s)", indent.c_str(), obs_source_get_name(show_tn), obs_source_get_id(show_tn));
	}
	if (hide_tn) {
		blog(LOG_INFO, "    %s- hide: '%s' (%s)", indent.c_str(), obs_source_get_name(hide_tn), obs_source_get_id(hide_tn));
	}

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, LogSceneItem, (void *)(intptr_t)child_indent);
	}
	return true;
}

void CanvasDock::LogFilter(obs_source_t *, obs_source_t *filter, void *v_val)
{
	const char *name = obs_source_get_name(filter);
	const char *id = obs_source_get_id(filter);
	int val = (int)(intptr_t)v_val;
	std::string indent;

	for (int i = 0; i < val; i++) {
		indent += "    ";
	}

	blog(LOG_INFO, "%s- filter: '%s' (%s)", indent.c_str(), name, id);
}

void CanvasDock::OpenSceneProjector()
{
	int monitor = -1;
	if (auto *from = sender()) {
		if (from->property("monitor").isValid()) {
			monitor = from->property("monitor").toInt();
		}
	}
	if (monitor > 9 || (monitor > -1 && monitor > QGuiApplication::screens().size() - 1)) {
		return;
	}
	if (!sceneList || !sceneList->currentItem()) {
		return;
	}
	OBSSourceAutoRelease open_source =
		obs_canvas_get_source_by_name(canvas, sceneList->currentItem()->text().toUtf8().constData());
	if (!open_source) {
		return;
	}
	auto config = obs_frontend_get_user_config();
	if (config && monitor > -1) {
		bool closeProjectors = config_get_bool(config, "BasicWindow", "CloseExistingProjectors");
		if (closeProjectors) {
			for (size_t i = projectors.size(); i > 0; i--) {
				size_t idx = i - 1;
				if (projectors[idx]->GetMonitor() == monitor) {
					DeleteProjector(projectors[idx]);
				}
			}
		}
	}
	obs_source_t *source = open_source;
	OBSProjector *projector = new OBSProjector(nullptr, source, monitor, [this](OBSProjector *p) { DeleteProjector(p); });
	projectors.emplace_back(projector);
}

void CanvasDock::OpenPreviewProjector()
{
	int monitor = sender()->property("monitor").toInt();
	OpenProjector(monitor);
}

void CanvasDock::OpenSourceProjector()
{
	int monitor = sender()->property("monitor").toInt();
	if (monitor > 9 || monitor > QGuiApplication::screens().size() - 1) {
		return;
	}
	OBSSceneItem item = GetSelectedItem(scene);
	if (!item) {
		return;
	}

	obs_source_t *open_source = obs_sceneitem_get_source(item);
	if (!open_source) {
		return;
	}
	if (obs_source_get_output_flags(open_source) & OBS_SOURCE_REQUIRES_CANVAS) {
		auto config = obs_frontend_get_user_config();
		if (config) {
			bool closeProjectors = config_get_bool(config, "BasicWindow", "CloseExistingProjectors");

			if (closeProjectors && monitor > -1) {
				for (size_t i = projectors.size(); i > 0; i--) {
					size_t idx = i - 1;
					if (projectors[idx]->GetMonitor() == monitor) {
						DeleteProjector(projectors[idx]);
					}
				}
			}
		}

		OBSProjector *projector =
			new OBSProjector(nullptr, open_source, monitor, [this](OBSProjector *p) { DeleteProjector(p); });

		projectors.emplace_back(projector);
	} else {
		obs_frontend_open_projector("Source", monitor, nullptr, obs_source_get_name(open_source));
	}
}

void CanvasDock::DeleteProjector(OBSProjector *projector)
{
	for (size_t i = 0; i < projectors.size(); i++) {
		if (projectors[i] == projector) {
			projectors[i]->deleteLater();
			projectors.erase(projectors.begin() + i);
			break;
		}
	}
}

OBSProjector *CanvasDock::OpenProjector(int monitor)
{
	/* seriously?  10 monitors? */
	if (monitor > 9 || monitor > QGuiApplication::screens().size() - 1) {
		return nullptr;
	}
	auto config = obs_frontend_get_user_config();
	if (!config) {
		return nullptr;
	}

	bool closeProjectors = config_get_bool(config, "BasicWindow", "CloseExistingProjectors");

	if (closeProjectors && monitor > -1) {
		for (size_t i = projectors.size(); i > 0; i--) {
			size_t idx = i - 1;
			if (projectors[idx]->GetMonitor() == monitor) {
				DeleteProjector(projectors[idx]);
			}
		}
	}

	OBSProjector *projector = new OBSProjector(canvas, nullptr, monitor, [this](OBSProjector *p) { DeleteProjector(p); });

	projectors.emplace_back(projector);

	return projector;
}

bool CanvasDock::nudge_callback(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	struct vec2 &offset = *reinterpret_cast<struct vec2 *>(param);
	struct vec2 pos;

	if (!obs_sceneitem_selected(item)) {
		if (obs_sceneitem_is_group(item)) {
			struct vec3 offset3;
			vec3_set(&offset3, offset.x, offset.y, 0.0f);

			struct matrix4 matrix;
			obs_sceneitem_get_draw_transform(item, &matrix);
			vec4_set(&matrix.t, 0.0f, 0.0f, 0.0f, 1.0f);
			matrix4_inv(&matrix, &matrix);
			vec3_transform(&offset3, &offset3, &matrix);

			struct vec2 new_offset;
			vec2_set(&new_offset, offset3.x, offset3.y);
			obs_sceneitem_group_enum_items(item, nudge_callback, &new_offset);
		}

		return true;
	}

	obs_sceneitem_get_pos(item, &pos);
	vec2_add(&pos, &pos, &offset);
	obs_sceneitem_set_pos(item, &pos);
	return true;
}

void CanvasDock::Nudge(int dist, MoveDir dir)
{
	if (locked) {
		return;
	}

	struct vec2 offset;
	vec2_set(&offset, 0.0f, 0.0f);

	switch (dir) {
	case MoveDir::Up:
		offset.y = (float)-dist;
		break;
	case MoveDir::Down:
		offset.y = (float)dist;
		break;
	case MoveDir::Left:
		offset.x = (float)-dist;
		break;
	case MoveDir::Right:
		offset.x = (float)dist;
		break;
	}

	obs_scene_enum_items(scene, nudge_callback, &offset);
}

void CanvasDock::SetPanelVisible(const QString &panel_name, bool visible)
{
	/* Canvas docks are preview-only. Scenes/sources/transitions are hosted
	 * by the native OBS docks (canvas tabs), so ignore panel toggles here. */
	UNUSED_PARAMETER(visible);
	if (panel_name == "canvas" && canvas_split) {
		canvas_split->restorePanelOrder(QStringLiteral("canvasPreview"));
	}
}

void CanvasDock::SetSelectedTransition(const QString &transition_name)
{
	auto pos = transition->findText(transition_name);
	if (pos >= 0) {
		transition->setCurrentIndex(pos);
	}
}

void CanvasDock::AddTransition(const char *source_type, const char *name, obs_data_t *settings)
{
	if (!source_type || source_type[0] == '\0') {
		return;
	}
	if (!name || name[0] == '\0') {
		return;
	}
	for (auto tr : transitions) {
		if (strcmp(obs_source_get_name(tr), name) == 0) {
			return;
		}
	}

	OBSSourceAutoRelease source = obs_source_create(source_type, name, settings, nullptr);
	if (source) {
		transitions.emplace_back(source);
		auto n = QString::fromUtf8(name);
		QMetaObject::invokeMethod(this, [this, n]() { transition->addItem(n); });
	}
}

void CanvasDock::RemoveTransition(const char *transition_name)
{
	for (auto it = transitions.begin(); it != transitions.end(); ++it) {
		if (strcmp(transition_name, obs_source_get_name(it->Get())) == 0) {
			if (!obs_is_source_configurable(obs_source_get_id(it->Get()))) {
				return;
			}
			transitions.erase(it);
			break;
		}
	}
	auto name = QString::fromUtf8(transition_name);
	QMetaObject::invokeMethod(this, [this, name]() {
		auto pos = transition->findText(name);
		if (pos >= 0) {
			transition->removeItem(pos);
		}
	});
}

void CanvasDock::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat("application/x-obs-source-uuid")) {
		event->acceptProposedAction();
	}

	// refuse drops of our own widgets
	if (event->source() != nullptr) {
		event->setDropAction(Qt::IgnoreAction);
		return;
	}

	event->acceptProposedAction();
}

void CanvasDock::dragLeaveEvent(QDragLeaveEvent *event)
{
	event->accept();
}

void CanvasDock::dragMoveEvent(QDragMoveEvent *event)
{
	event->acceptProposedAction();
}

#ifdef _WIN32
static QString ReadWindowsURLFile(const QString &file)
{
	QSettings iniFile(file, QSettings::IniFormat);
	QVariant url = iniFile.value("InternetShortcut/URL");
	return url.toString();
}
#endif

static const char *textExtensions[] = {"txt", "log", nullptr};

static const char *imageExtensions[] = {"bmp", "gif", "jpeg", "jpg",
#ifdef _WIN32
					"jxr",
#endif
					"png", "tga", "webp", nullptr};

static const char *htmlExtensions[] = {"htm", "html", nullptr};

static const char *mediaExtensions[] = {
	"3ga",   "669",  "a52",  "aac",  "ac3",  "adt", "adts", "aif", "aifc", "aiff", "amb",  "amr", "aob",  "ape",   "au",
	"awb",   "caf",  "dts",  "flac", "it",   "kar", "m4a",  "m4b", "m4p",  "m5p",  "mid",  "mka", "mlp",  "mod",   "mpa",
	"mp1",   "mp2",  "mp3",  "mpc",  "mpga", "mus", "oga",  "ogg", "oma",  "opus", "qcp",  "ra",  "rmi",  "s3m",   "sid",
	"spx",   "tak",  "thd",  "tta",  "voc",  "vqf", "w64",  "wav", "wma",  "wv",   "xa",   "xm",  "3g2",  "3gp",   "3gp2",
	"3gpp",  "amv",  "asf",  "avi",  "bik",  "crf", "divx", "drc", "dv",   "evo",  "f4v",  "flv", "gvi",  "gxf",   "iso",
	"m1v",   "m2v",  "m2t",  "m2ts", "m4v",  "mkv", "mov",  "mp2", "mp2v", "mp4",  "mp4v", "mpe", "mpeg", "mpeg1", "mpeg2",
	"mpeg4", "mpg",  "mpv2", "mts",  "mtv",  "mxf", "mxg",  "nsv", "nuv",  "ogg",  "ogm",  "ogv", "ogx",  "ps",    "rec",
	"rm",    "rmvb", "rpl",  "thp",  "tod",  "ts",  "tts",  "txd", "vob",  "vro",  "webm", "wm",  "wmv",  "wtv",   nullptr};

void CanvasDock::dropEvent(QDropEvent *event)
{
	const QMimeData *mimeData = event->mimeData();

	if (mimeData->hasUrls()) {
		QList<QUrl> urls = mimeData->urls();

		for (int i = 0; i < urls.size(); i++) {
			QUrl url = urls[i];
			QString file = url.toLocalFile();
			QFileInfo fileInfo(file);

			if (!fileInfo.exists()) {
				ConfirmDropUrl(url.url());
				continue;
			}

#ifdef _WIN32
			if (fileInfo.suffix().compare("url", Qt::CaseInsensitive) == 0) {
				QString urlTarget = ReadWindowsURLFile(file);
				if (!urlTarget.isEmpty()) {
					ConfirmDropUrl(urlTarget);
				}
				continue;
			} else if (fileInfo.isShortcut()) {
				file = fileInfo.symLinkTarget();
				fileInfo = QFileInfo(file);
				if (!fileInfo.exists()) {
					continue;
				}
			}
#endif

			QString suffixQStr = fileInfo.suffix();
			QByteArray suffixArray = suffixQStr.toUtf8();
			const char *suffix = suffixArray.constData();
			bool found = false;

			const char **cmp;
			cmp = textExtensions;
			while (*cmp) {
				if (astrcmpi(*cmp, suffix) == 0) {
					OBSDataAutoRelease s = obs_data_create();
#ifdef _WIN32
					obs_data_set_bool(s, "read_from_file", true);
					obs_data_set_string(s, "file", file.toUtf8().constData());
					AddDropSource(s, "text_gdiplus", QUrl::fromLocalFile(file).fileName());
#else
					obs_data_set_bool(s, "from_file", true);
					obs_data_set_string(s, "text_file", file.toUtf8().constData());
					AddDropSource(s, "text_ft2_source", QUrl::fromLocalFile(file).fileName());
#endif
					found = true;
					break;
				}

				cmp++;
			}

			if (found) {
				continue;
			}

			cmp = htmlExtensions;
			while (*cmp) {
				if (astrcmpi(*cmp, suffix) == 0) {
					OBSDataAutoRelease s = obs_data_create();
					obs_data_set_bool(s, "is_local_file", true);
					obs_data_set_string(s, "local_file", file.toUtf8().constData());
					obs_data_set_int(s, "width", canvas_width);
					obs_data_set_int(s, "height", canvas_height);
					AddDropSource(s, "browser_source", QUrl::fromLocalFile(file).fileName());
					found = true;
					break;
				}

				cmp++;
			}

			if (found) {
				continue;
			}

			cmp = imageExtensions;
			while (*cmp) {
				if (astrcmpi(*cmp, suffix) == 0) {
					OBSDataAutoRelease s = obs_data_create();
					obs_data_set_string(s, "file", file.toUtf8().constData());
					AddDropSource(s, "image_source", QUrl::fromLocalFile(file).fileName());
					found = true;
					break;
				}

				cmp++;
			}

			if (found) {
				continue;
			}

			cmp = mediaExtensions;
			while (*cmp) {
				if (astrcmpi(*cmp, suffix) == 0) {
					OBSDataAutoRelease s = obs_data_create();
					obs_data_set_string(s, "local_file", file.toUtf8().constData());
					AddDropSource(s, "ffmpeg_source", QUrl::fromLocalFile(file).fileName());
					found = true;
					break;
				}

				cmp++;
			}

			if (found) {
				continue;
			}
		}
	} else if (mimeData->hasText()) {
		OBSDataAutoRelease s = obs_data_create();
		obs_data_set_string(settings, "text", mimeData->text().toUtf8().constData());
#ifdef _WIN32
		AddDropSource(s, "text_gdiplus");
#else
		AddDropSource(s, "text_ft2_source");
#endif
	} else if (event->mimeData()->hasFormat("application/x-obs-source-uuid")) {
		QString uuid = QString::fromUtf8(event->mimeData()->data("application/x-obs-source-uuid"));

		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.toStdString().c_str());
		if (source && scene) {
			obs_scene_add(scene, source);
		}
		event->acceptProposedAction();
	}
}

void CanvasDock::ConfirmDropUrl(const QString &url)
{
	if (url.left(7).compare("http://", Qt::CaseInsensitive) != 0 && url.left(8).compare("https://", Qt::CaseInsensitive) != 0) {
		return;
	}

	activateWindow();

	QString msg = QString::fromUtf8(obs_frontend_get_locale_string("AddUrl.Text"));
	msg += "\n\n";
	msg += QString::fromUtf8(obs_frontend_get_locale_string("AddUrl.Text.Url")).arg(url);

	QMessageBox messageBox(this);
	messageBox.setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string("AddUrl.Title")));
	messageBox.setText(msg);

	QPushButton *yesButton =
		messageBox.addButton(QString::fromUtf8(obs_frontend_get_locale_string("Yes")), QMessageBox::YesRole);
	QPushButton *noButton = messageBox.addButton(QString::fromUtf8(obs_frontend_get_locale_string("No")), QMessageBox::NoRole);
	messageBox.setDefaultButton(yesButton);
	messageBox.setEscapeButton(noButton);
	messageBox.setIcon(QMessageBox::Question);
	messageBox.exec();

	if (messageBox.clickedButton() == yesButton) {
		AddDropURL(QUrl(url));
	}
}

void CanvasDock::AddDropSource(obs_data_t *data, const char *type, const QString &name)
{
	type = obs_get_latest_input_type_id(type);
	if (type == nullptr || !obs_source_get_display_name(type)) {
		return;
	}
	std::string baseName = name.toStdString();
	if (baseName.empty()) {
		baseName = obs_source_get_display_name(type);
	}

	std::string sourceName = baseName;
	OBSSourceAutoRelease s = nullptr;
	obs_source_t *created_source = nullptr;
	int i = 2;
	if (obs_get_source_output_flags(type) & OBS_SOURCE_REQUIRES_CANVAS) {
		while ((s = obs_canvas_get_source_by_name(canvas, sourceName.c_str()))) {
			sourceName = baseName + " (" + std::to_string(i++) + ")";
		}
		created_source = obs_scene_get_source(obs_canvas_scene_create(canvas, sourceName.c_str()));
	} else {
		while ((s = obs_get_source_by_name(sourceName.c_str()))) {
			sourceName = baseName + " (" + std::to_string(i++) + ")";
		}
		created_source = obs_source_create(type, sourceName.c_str(), data, nullptr);
	}
	if (scene && created_source) {
		obs_scene_add(scene, created_source);
	}
	obs_source_release(created_source);
}

void CanvasDock::AddDropURL(QUrl url)
{
	OBSDataAutoRelease s = obs_data_create();
	QUrlQuery query = QUrlQuery(url.query(QUrl::FullyEncoded));

	int cx = canvas_width;
	int cy = canvas_height;

	if (query.hasQueryItem("layer-width")) {
		cx = query.queryItemValue("layer-width").toInt();
	}
	if (query.hasQueryItem("layer-height")) {
		cy = query.queryItemValue("layer-height").toInt();
	}
	if (query.hasQueryItem("layer-css")) {
		// QUrl::FullyDecoded does NOT properly decode a
		// application/x-www-form-urlencoded space represented as '+'
		// Thus, this is manually filtered out before QUrl's
		// decoding kicks in again. This is to allow JavaScript's
		// default searchParams.append function to simply append css
		// to the query parameters, which is the intended usecase for this.
		QString fullyEncoded = query.queryItemValue("layer-css", QUrl::FullyEncoded);
		fullyEncoded = fullyEncoded.replace("+", "%20");
		QString decoded = QUrl::fromPercentEncoding(QByteArray::fromStdString(fullyEncoded.toUtf8().constData()));
		obs_data_set_string(s, "css", decoded.toUtf8().constData());
	}

	obs_data_set_int(s, "width", cx);
	obs_data_set_int(s, "height", cy);

	QString name = query.hasQueryItem("layer-name") ? query.queryItemValue("layer-name", QUrl::FullyDecoded) : url.host();

	query.removeQueryItem("layer-width");
	query.removeQueryItem("layer-height");
	query.removeQueryItem("layer-name");
	query.removeQueryItem("layer-css");
	url.setQuery(query);

	obs_data_set_string(s, "url", url.url().toUtf8().constData());
	AddDropSource(s, "browser_source", name);
}
