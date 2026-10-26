#include "../dialogs/name-dialog.hpp"
#include "../dialogs/source-select-dialog.hpp"
#include "canvas-dock.hpp"
#include "sources-dock.hpp"
#include <obs-module.h>
#include <QColorDialog>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidgetAction>

extern SourcesDock *sources_dock;

static void StyleDockToolbar(QToolBar *toolbar)
{
	if (!toolbar)
		return;
	/* Same as OBSBasic.ui sourcesToolbar. Leave styling to Yami
	 * "OBSDock QToolBar QToolButton" once this bar lives in the dock. */
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);
	toolbar->setMovable(false);
	toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
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
}

SourcesDock::SourcesDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QString::fromUtf8("AitumStreamSuiteSourcesDock"));
	setFrameShape(QFrame::NoFrame);
	setAutoFillBackground(false);

	auto sourcesGroupLayout = new QVBoxLayout(this);
	sourcesGroupLayout->setContentsMargins(0, 0, 0, 0);
	sourcesGroupLayout->setSpacing(0);
	setLayout(sourcesGroupLayout);

	sourceList = new SourceTree(
		&selectMutex, &hoveredPreviewItems,
		[](void *param) {
			auto s = obs_weak_source_get_source(((SourcesDock *)param)->scene);
			if (!s) {
				return (obs_scene_t *)nullptr;
			}
			auto scene = obs_scene_from_source(s);
			obs_source_release(s);
			return scene;
		},
		this, nullptr, this);

	sourceList->setObjectName(QString::fromUtf8("sources"));
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
	sourceList->setSpacing(0);

	connect(sourceList, &SourceTree::customContextMenuRequested, [this] { ShowSourcesContextMenu(GetCurrentSceneItem()); });

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
	toolbar->setObjectName(QStringLiteral("sourcesToolbar"));
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);
	toolbar->setMovable(false);
	auto a = toolbar->addAction(QIcon(QString::fromUtf8(":/res/images/plus.svg")),
				    QString::fromUtf8(obs_frontend_get_locale_string("Add")), [this] {
					    OBSSourceAutoRelease sceneSource = obs_weak_source_get_source(scene);
					    OpenSourceSelectDialog(this, obs_scene_from_source(sceneSource));
				    });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("AddSource")));
	SetToolbarClass(toolbar, a, "icon-plus");

	a = toolbar->addAction(QIcon(QString::fromUtf8(":/res/images/minus.svg")),
			       QString::fromUtf8(obs_frontend_get_locale_string("Remove")), [this] {
				       OBSSourceAutoRelease scene_source = obs_weak_source_get_source(scene);
				       CanvasDock::RemoveSelectedSources(obs_scene_from_source(scene_source), this);
			       });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("RemoveSource")));
	SetToolbarClass(toolbar, a, "icon-trash");
	removeAction = a;
	a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
#ifdef __APPLE__
	a->setShortcut({Qt::Key_Backspace});
#else
	a->setShortcut({Qt::Key_Delete});
#endif
	sourceList->addAction(a);
	toolbar->addSeparator();
	a = toolbar->addAction(QIcon(QString::fromUtf8(":/settings/images/settings/general.svg")),
			       QString::fromUtf8(obs_frontend_get_locale_string("Properties")), [this] {
				       auto item = GetCurrentSceneItem();
				       auto source = obs_sceneitem_get_source(item);
				       if (source) {
					       obs_frontend_open_source_properties(source);
				       }
			       });
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("SourceProperties")));
	SetToolbarClass(toolbar, a, "icon-gear");
	propertiesAction = a;

	toolbar->addSeparator();
	a = toolbar->addAction(
		QIcon(QString::fromUtf8(":/res/images/up.svg")), QString::fromUtf8(obs_frontend_get_locale_string("MoveUp")), [this] {
			auto item = GetCurrentSceneItem();
			if (!item) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(item);
			if (!scene) {
				return;
			}
			std::string undo_json = CanvasDock::backup_scene(scene);
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
			std::string redo_json = CanvasDock::backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveUp"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), CanvasDock::undo_redo_scene,
							  CanvasDock::undo_redo_scene, undo_json.c_str(), redo_json.c_str(), false);
		});
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSourceUp")));
	SetToolbarClass(toolbar, a, "icon-up");
	moveUpAction = a;
	a = toolbar->addAction(
		QIcon(QString::fromUtf8(":/res/images/down.svg")), QString::fromUtf8(obs_frontend_get_locale_string("MoveDown")), [this] {
			auto item = GetCurrentSceneItem();
			if (!item) {
				return;
			}
			auto scene = obs_sceneitem_get_scene(item);
			if (!scene) {
				return;
			}
			std::string undo_json = CanvasDock::backup_scene(scene);
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
			std::string redo_json = CanvasDock::backup_scene(scene);
			auto undoName = QString::fromUtf8(obs_frontend_get_locale_string("Undo.MoveDown"))
						.arg(QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item))),
						     QString::fromUtf8(obs_source_get_name(obs_scene_get_source(scene))));
			obs_frontend_add_undo_redo_action(undoName.toUtf8().constData(), CanvasDock::undo_redo_scene,
							  CanvasDock::undo_redo_scene, undo_json.c_str(), redo_json.c_str(), false);
		});
	a->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string("MoveSourceDown")));
	SetToolbarClass(toolbar, a, "icon-down");
	moveDownAction = a;

	StyleDockToolbar(toolbar);
	sourceToolbar = toolbar;
	sourcesGroupLayout->addWidget(toolbar);
	connect(sourceList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
		[this](const QItemSelection &, const QItemSelection &) { UpdateSourceToolbar(); });
	/* No selection yet: remove, properties and move match the native
	 * disabled look (softer icon, grey7 button). */
	UpdateSourceToolbar();
}

SourcesDock::~SourcesDock()
{
	auto old_scene = obs_weak_source_get_source(scene);
	if (old_scene) {
		auto sh = obs_source_get_signal_handler(old_scene);
		if (sh) {
			signal_handler_disconnect(sh, "item_add", SceneItemAdded, this);
			signal_handler_disconnect(sh, "reorder", SceneReordered, this);
			signal_handler_disconnect(sh, "refresh", SceneRefreshed, this);
		}
		obs_source_release(old_scene);
	}
	obs_weak_source_release(scene);
	scene = nullptr;
	if (sources_dock == this) {
		sources_dock = nullptr;
	}
}

obs_sceneitem_t *SourcesDock::GetCurrentSceneItem()
{
	return sourceList->Get(GetTopSelectedSourceItem());
}

void SourcesDock::UpdateSourceToolbar()
{
	obs_sceneitem_t *item = GetCurrentSceneItem();
	bool enable = item != nullptr;
	bool props = enable;
	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (!source || !obs_source_configurable(source)) {
			props = false;
		}
	}
	if (removeAction) {
		removeAction->setEnabled(enable);
	}
	if (propertiesAction) {
		propertiesAction->setEnabled(props);
	}
	if (moveUpAction) {
		moveUpAction->setEnabled(enable);
	}
	if (moveDownAction) {
		moveDownAction->setEnabled(enable);
	}
	/* Same as OBS RefreshToolBarStyling: the :disabled dock rule
	 * (grey7 button, faded icon) is applied at polish time. */
	if (!sourceToolbar || !sourceToolbar->style()) {
		return;
	}
	sourceToolbar->style()->unpolish(sourceToolbar);
	sourceToolbar->style()->polish(sourceToolbar);
	const auto buttons = sourceToolbar->findChildren<QWidget *>();
	for (QWidget *button : buttons) {
		if (!button->style()) {
			continue;
		}
		button->style()->unpolish(button);
		button->style()->polish(button);
	}
}

int SourcesDock::GetTopSelectedSourceItem()
{
	QModelIndexList selectedItems = sourceList->selectionModel()->selectedIndexes();
	return selectedItems.isEmpty() ? -1 : selectedItems[0].row();
}

void SourcesDock::SetScene(obs_source_t *s)
{
	if (obs_weak_source_references_source(scene, s)) {
		return;
	}
	auto old_scene = obs_weak_source_get_source(scene);
	if (old_scene) {
		auto sh = obs_source_get_signal_handler(old_scene);
		if (sh) {
			signal_handler_disconnect(sh, "item_add", SceneItemAdded, this);
			signal_handler_disconnect(sh, "reorder", SceneReordered, this);
			signal_handler_disconnect(sh, "refresh", SceneRefreshed, this);
		}
		obs_source_release(old_scene);
	}
	obs_weak_source_release(scene);
	scene = obs_source_get_weak_source(s);
	auto sh = s ? obs_source_get_signal_handler(s) : nullptr;
	if (sh) {
		signal_handler_connect(sh, "item_add", SceneItemAdded, this);
		signal_handler_connect(sh, "reorder", SceneReordered, this);
		signal_handler_connect(sh, "refresh", SceneRefreshed, this);
	}
	sourceList->GetStm()->SceneChanged();
	UpdateSourceToolbar();
}

void SourcesDock::SceneItemAdded(void *data, calldata_t *params)
{
	SourcesDock *window = static_cast<SourcesDock *>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, "AddSceneItem", Qt::QueuedConnection, Q_ARG(OBSSceneItem, OBSSceneItem(item)));
}

void SourcesDock::SceneReordered(void *data, calldata_t *params)
{
	SourcesDock *window = static_cast<SourcesDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "ReorderSources", Qt::QueuedConnection, Q_ARG(OBSScene, OBSScene(scene)));
}

void SourcesDock::SceneRefreshed(void *data, calldata_t *params)
{
	SourcesDock *window = static_cast<SourcesDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, "RefreshSources", Qt::QueuedConnection, Q_ARG(OBSScene, OBSScene(scene)));
}

void SourcesDock::AddSceneItem(OBSSceneItem item, bool no_select)
{
	obs_scene_t *add_scene = obs_sceneitem_get_scene(item);

	if (obs_weak_source_references_source(scene, obs_scene_get_source(add_scene))) {
		sourceList->Add(item);
	}

	if (no_select) {
		return;
	}

	obs_scene_enum_items(add_scene, CanvasDock::select_one, (obs_sceneitem_t *)item);
}

void SourcesDock::RefreshSources(OBSScene refresh_scene)
{
	if (!obs_weak_source_references_source(scene, obs_scene_get_source(refresh_scene)) || sourceList->IgnoreReorder()) {
		return;
	}

	sourceList->RefreshItems();
}

void SourcesDock::ReorderSources(OBSScene order_scene)
{
	if (!obs_weak_source_references_source(scene, obs_scene_get_source(order_scene)) || sourceList->IgnoreReorder()) {
		return;
	}

	sourceList->ReorderItems();
}

void SourcesDock::ShowSourcesContextMenu(obs_sceneitem_t *item)
{
	QMenu menu(this);
	menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("AddSource")), [this] {
		OBSSourceAutoRelease sceneSource = obs_weak_source_get_source(scene);
		OpenSourceSelectDialog(this, obs_scene_from_source(sceneSource));
	});
	menu.addSeparator();
	auto sceneSource = obs_weak_source_get_source(scene);
	auto itemScene = obs_scene_from_source(sceneSource);
	if (!item) {
		menu.addAction(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.NewGroup")), sourceList,
			       &SourceTree::AddGroup);
		CanvasDock::AddCopyPasteMenuItems(&menu, nullptr, itemScene);
		obs_source_release(sceneSource);
		menu.exec(QCursor::pos());
		return;
	}
		auto colorMenu = menu.addMenu(QString::fromUtf8(obs_frontend_get_locale_string("ChangeBG")));
		colorMenu->setStyleSheet(QString("*[bgColor=\"1\"]{background-color:rgba(255,68,68,33%);}"
						 "*[bgColor=\"2\"]{background-color:rgba(255,255,68,33%);}"
						 "*[bgColor=\"3\"]{background-color:rgba(68,255,68,33%);}"
						 "*[bgColor=\"4\"]{background-color:rgba(68,255,255,33%);}"
						 "*[bgColor=\"5\"]{background-color:rgba(68,68,255,33%);}"
						 "*[bgColor=\"6\"]{background-color:rgba(255,68,255,33%);}"
						 "*[bgColor=\"7\"]{background-color:rgba(68,68,68,33%);}"
						 "*[bgColor=\"8\"]{background-color:rgba(255,255,255,33%);}"));
		obs_data_t *privData = obs_sceneitem_get_private_settings(item);
		obs_data_set_default_int(privData, "color-preset", 0);
		int preset = obs_data_get_int(privData, "color-preset");
		obs_data_release(privData);

		auto action = colorMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Clear")), this,
						   &SourcesDock::ColorChange);
		action->setCheckable(true);
		action->setProperty("bgColor", 0);
		action->setChecked(preset == 0);

		action = colorMenu->addAction(QString::fromUtf8(obs_frontend_get_locale_string("CustomColor")), this,
					      &SourcesDock::ColorChange);
		action->setCheckable(true);
		action->setProperty("bgColor", 1);
		action->setChecked(preset == 1);

		colorMenu->addSeparator();

		auto colorWidgetAction = new QWidgetAction(colorMenu);
		auto colorSelect = new QWidget(colorMenu);
		auto colorGridLayout = new QGridLayout(colorSelect);
		colorSelect->setLayout(colorGridLayout);

		colorWidgetAction->setDefaultWidget(colorSelect);

		for (int i = 1; i < 9; i++) {
			QString button = "preset";
			button += QString::number(i);
			auto colorButton = new QPushButton();
			colorButton->setObjectName(button);
			colorButton->setMinimumSize(QSize(22, 22));
			colorButton->setMaximumSize(QSize(22, 22));
			if (preset == i + 1) {
				colorButton->setStyleSheet("border: 2px solid black");
			}
			colorButton->setProperty("bgColor", i);
			colorGridLayout->addWidget(colorButton, (i - 1) / 4, (i - 1) % 4, 1, 1);
			connect(colorButton, &QPushButton::released, this, &SourcesDock::ColorChange);
		}

		colorMenu->addAction(colorWidgetAction);

		CanvasDock::AddSceneItemMenuItems(&menu, item);
	obs_source_release(sceneSource);
	menu.exec(QCursor::pos());
}

void SourcesDock::AddSourceFromAction(QString idText)
{
	QByteArray idUtf8 = idText.toUtf8();
	const char *id = idUtf8.constData();
	OBSSourceAutoRelease canvas_scene = obs_weak_source_get_source(scene);
	obs_scene_t *item_scene = obs_scene_from_source(canvas_scene);
	if (!id || !*id || !item_scene) {
		blog(LOG_WARNING, "[Stream Suite] Cannot add source: canvas scene is not selected");
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
	obs_canvas_t *canvas = obs_source_get_canvas(canvas_scene);
	if (strcmp(id, "group") == 0) {
		OBSSourceAutoRelease existing = nullptr;
		while (canvas && (existing = obs_canvas_get_source_by_name(canvas, text.toUtf8().constData()))) {
			text = QString("%1 %2").arg(placeHolderText).arg(i++);
		}
		if (!canvas) {
			OBSSourceAutoRelease global = nullptr;
			while ((global = obs_get_source_by_name(text.toUtf8().constData()))) {
				text = QString("%1 %2").arg(placeHolderText).arg(i++);
			}
		}
		obs_sceneitem_t *group = obs_scene_add_group(item_scene, text.toUtf8().constData());
		obs_canvas_release(canvas);
		if (!group) {
			blog(LOG_WARNING, "[Stream Suite] Failed to add group '%s' to canvas scene", text.toUtf8().constData());
		}
		return;
	}
	if ((obs_get_source_output_flags(id) & OBS_SOURCE_REQUIRES_CANVAS) && canvas) {
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
	obs_canvas_release(canvas);
	if (!created_source) {
		blog(LOG_WARNING, "[Stream Suite] Failed to create source '%s'", text.toUtf8().constData());
		return;
	}
	obs_sceneitem_t *item = obs_scene_add(item_scene, created_source);
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

void SourcesDock::AddSourceToScene(OBSSource source)
{
	OBSSourceAutoRelease s = obs_weak_source_get_source(scene);
	obs_scene_t *item_scene = obs_scene_from_source(s);
	if (!item_scene || !source) {
		blog(LOG_WARNING, "[Stream Suite] Cannot add existing source: canvas scene is not selected");
		return;
	}
	if (!obs_scene_add(item_scene, source)) {
		blog(LOG_WARNING, "[Stream Suite] Failed to add existing source '%s' to canvas scene", obs_source_get_name(source));
	}
}

void SourcesDock::OpenSourceProjector()
{
	int monitor = sender()->property("monitor").toInt();
	if (monitor > 9 || monitor > QGuiApplication::screens().size() - 1) {
		return;
	}
	OBSSceneItem item = GetCurrentSceneItem();
	if (!item) {
		return;
	}

	obs_source_t *open_source = obs_sceneitem_get_source(item);
	if (!open_source) {
		return;
	}
	obs_frontend_open_projector("Source", monitor, nullptr, obs_source_get_name(open_source));
}

void SourcesDock::ColorChange()
{
	auto row = GetTopSelectedSourceItem();
	if (row < 0) {
		return;
	}
	obs_sceneitem_t *sceneItem = sourceList->Get(row);
	if (!sceneItem) {
		return;
	}
	QAction *action = qobject_cast<QAction *>(sender());
	QPushButton *colorButton = qobject_cast<QPushButton *>(sender());

	if (colorButton) {
		int preset = colorButton->property("bgColor").value<int>();
		SourceTreeItem *treeItem = sourceList->GetItemWidget(row);
		treeItem->setStyleSheet("");
		treeItem->setProperty("bgColor", preset);
		treeItem->style()->unpolish(treeItem);
		treeItem->style()->polish(treeItem);

		OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
		obs_data_set_int(privData, "color-preset", preset + 1);
		obs_data_set_string(privData, "color", "");

		for (int i = 1; i < 9; i++) {
			QString button = "preset";
			button += QString::number(i);
			QPushButton *cButton = colorButton->parentWidget()->findChild<QPushButton *>(button);
			cButton->setStyleSheet("border: 1px solid black");
		}

		colorButton->setStyleSheet("border: 2px solid black");
	} else if (action) {
		int preset = action->property("bgColor").value<int>();

		if (preset == 1) {
			OBSDataAutoRelease curPrivData = obs_sceneitem_get_private_settings(sceneItem);
			SourceTreeItem *treeItem = sourceList->GetItemWidget(row);
			int oldPreset = obs_data_get_int(curPrivData, "color-preset");
			const QString oldSheet = treeItem->styleSheet();

			auto liveChangeColor = [=](const QColor &color) {
				if (!color.isValid()) {
					return;
				}
				treeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
			};

			auto changedColor = [=](const QColor &color) {
				if (!color.isValid()) {
					return;
				}
				treeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
				treeItem->style()->unpolish(treeItem);
				treeItem->style()->polish(treeItem);

				OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
				obs_data_set_int(privData, "color-preset", 1);
				obs_data_set_string(privData, "color", color.name(QColor::HexArgb).toUtf8().constData());
			};

			auto rejected = [=]() {
				if (oldPreset == 1) {
					treeItem->setStyleSheet(oldSheet);
					treeItem->setProperty("bgColor", 0);
				} else if (oldPreset == 0) {
					treeItem->setStyleSheet("background: none");
					treeItem->setProperty("bgColor", 0);
				} else {
					treeItem->setStyleSheet("");
					treeItem->setProperty("bgColor", oldPreset - 1);
				}

				treeItem->style()->unpolish(treeItem);
				treeItem->style()->polish(treeItem);
			};

			QColorDialog::ColorDialogOptions options = QColorDialog::ShowAlphaChannel;

			const char *oldColor = obs_data_get_string(curPrivData, "color");
			const char *customColor = *oldColor != 0 ? oldColor : "#55FF0000";
#ifdef __linux__
			// TODO: Revisit hang on Ubuntu with native dialog
			options |= QColorDialog::DontUseNativeDialog;
#endif

			QColorDialog *colorDialog = new QColorDialog(this);
			colorDialog->setOptions(options);
			colorDialog->setCurrentColor(QColor(customColor));
			connect(colorDialog, &QColorDialog::currentColorChanged, this, liveChangeColor);
			connect(colorDialog, &QColorDialog::colorSelected, this, changedColor);
			connect(colorDialog, &QColorDialog::rejected, this, rejected);
			colorDialog->open();
		} else {

			SourceTreeItem *treeItem = sourceList->GetItemWidget(row);
			treeItem->setStyleSheet("background: none");
			treeItem->setProperty("bgColor", preset);
			treeItem->style()->unpolish(treeItem);
			treeItem->style()->polish(treeItem);

			OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
			obs_data_set_int(privData, "color-preset", preset);
			obs_data_set_string(privData, "color", "");
		}
	}
}
