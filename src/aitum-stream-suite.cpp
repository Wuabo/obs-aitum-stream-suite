#include "dialogs/config-dialog.hpp"
#include "dialogs/source-select-dialog.hpp"
#include "docks/canvas-clone-dock.hpp"
#include "docks/native-dock-tabs.hpp"
#include "docks/main-canvas-context-fix.hpp"
#include "docks/canvas-dock.hpp"
#include "docks/filters-dock.hpp"
#include "docks/output-dock.hpp"
#include "docks/properties-dock.hpp"
#include "docks/transform-dock.hpp"
#include "utils/obs-websocket-api.h"
#include "version.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <util/dstr.h>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Aitum");
OBS_MODULE_USE_DEFAULT_LOCALE("aitum-stream-suite", "en-US")

obs_data_t *current_profile_config = nullptr;

OBSBasicSettings *configDialog = nullptr;
OutputDock *output_dock = nullptr;
QPointer<PropertiesDock> properties_dock;
QPointer<FiltersDock> filters_dock;
QPointer<TransformDock> transform_dock;

extern std::list<CanvasDock *> canvas_docks;
extern std::list<CanvasCloneDock *> canvas_clone_docks;
extern obs_websocket_vendor vendor;

static bool finished_loading = false;
static bool scene_collection_changing = false;

static const QString kDefaultMode = QStringLiteral("default");

void transition_start(void *, calldata_t *)
{
	for (const auto &it : canvas_docks) {
		QMetaObject::invokeMethod(it, "MainSceneChanged", Qt::QueuedConnection);
	}
}

/* Persist per-dock widget settings only (canvas panels).
 * OBS owns main-window dock placement — never saveState/restoreState here. */
void save_dock_state(QString mode)
{
	if (!current_profile_config) {
		return;
	}
	if (mode.isEmpty()) {
		mode = kDefaultMode;
	}
	for (const auto &it : canvas_docks) {
		QMetaObject::invokeMethod(it, "SaveSettings", Q_ARG(bool, false), Q_ARG(QString, mode));
	}
	for (const auto &it : canvas_clone_docks) {
		QMetaObject::invokeMethod(it, "SaveSettings", Q_ARG(bool, false), Q_ARG(QString, mode));
	}
}

void load_dock_state(QString mode)
{
	if (!current_profile_config) {
		return;
	}
	scene_collection_changing = false;
	if (mode.isEmpty()) {
		mode = kDefaultMode;
	}

	for (const auto &it : canvas_docks) {
		QMetaObject::invokeMethod(it, "LoadMode", Qt::QueuedConnection, Q_ARG(QString, mode));
	}
	for (const auto &it : canvas_clone_docks) {
		QMetaObject::invokeMethod(it, "LoadMode", Qt::QueuedConnection, Q_ARG(QString, mode));
	}
}

void load_outputs()
{
	QMetaObject::invokeMethod(
		output_dock,
		[] {
			if (output_dock) {
				output_dock->LoadSettings();
			}
		},
		Qt::QueuedConnection);
}

void load_canvas(bool check_new_canvas)
{
	UNUSED_PARAMETER(check_new_canvas);
	detach_native_canvas_tabs();
	auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window) {
		return;
	}
	auto canvas = obs_data_get_array(current_profile_config, "canvas");
	auto canvas_count = obs_data_array_count(canvas);
	for (size_t i = 0; i < canvas_count;) {
		obs_data_t *t = obs_data_array_item(canvas, i);
		if (!t) {
			i++;
			continue;
		}
		if (obs_data_get_bool(t, "delete")) {
			const char *canvas_name = obs_data_get_string(t, "name");
			obs_frontend_remove_dock(canvas_name);
			auto uuid = obs_data_get_string(t, "uuid");
			for (const auto &it : canvas_docks) {
				if (strcmp(obs_canvas_get_uuid(it->GetCanvas()), uuid) == 0) {
					obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
					break;
				}
			}
			for (const auto &it : canvas_clone_docks) {
				if (strcmp(obs_canvas_get_uuid(it->GetCanvas()), uuid) == 0) {
					obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
					break;
				}
			}
			obs_canvas_t *c = obs_get_canvas_by_uuid(uuid);
			if (c && obs_canvas_removed(c)) {
				obs_canvas_release(c);
				c = nullptr;
			}
			if (!c) {
				c = obs_get_canvas_by_name(canvas_name);
			}
			if (c && obs_canvas_removed(c)) {
				obs_canvas_release(c);
				c = nullptr;
			}
			if (c) {
				obs_frontend_remove_canvas(c);
				obs_canvas_remove(c);
				obs_canvas_release(c);
			}
			obs_data_array_erase(canvas, i);
			obs_data_release(t);
			canvas_count--;
		} else if (strcmp(obs_data_get_string(t, "type"), "clone") == 0) {
			auto uuid = obs_data_get_string(t, "uuid");
			auto name = obs_data_get_string(t, "name");
			for (const auto &it : canvas_docks) {
				if (strcmp(obs_canvas_get_uuid(it->GetCanvas()), uuid) == 0) {
					obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
					check_new_canvas = false;
					break;
				}
			}
			CanvasCloneDock *ccd = nullptr;
			for (const auto &it : canvas_clone_docks) {
				auto cuuid = obs_canvas_get_uuid(it->GetCanvas());
				if (strcmp(cuuid, uuid) == 0) {
					if (obs_canvas_removed(it->GetCanvas()) ||
					    strcmp(it->parentWidget()->objectName().toUtf8().constData(), name) != 0) {
						// canvas name changed, remove old dock and create a new one
						obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
						check_new_canvas = false;
					} else {
						ccd = it;
					}
					break;
				} else if (strcmp(it->parentWidget()->objectName().toUtf8().constData(), name) == 0) {
					if (obs_canvas_removed(it->GetCanvas())) {
						obs_frontend_remove_dock(name);
						check_new_canvas = false;
						break;
					}
					obs_data_set_string(t, "uuid", cuuid);
					ccd = it;
					break;
				}
			}
			if (ccd) {
				ccd->UpdateSettings(t);
			} else {
				ccd = new CanvasCloneDock(t, main_window);
				std::string title = name;
				if (obs_frontend_add_dock_by_id(name, title.c_str(), ccd)) {
					canvas_clone_docks.push_back(ccd);
					if (!obs_data_get_bool(t, "has_loaded")) {
						ccd->parentWidget()->show();
						obs_data_set_bool(t, "has_loaded", true);
					}
				} else {
					delete ccd;
				}
			}
			i++;
		} else {
			auto uuid = obs_data_get_string(t, "uuid");
			auto name = obs_data_get_string(t, "name");
			for (const auto &it : canvas_clone_docks) {
				if (strcmp(obs_canvas_get_uuid(it->GetCanvas()), uuid) == 0) {
					obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
					check_new_canvas = false;
					break;
				}
			}
			CanvasDock *cd = nullptr;
			for (const auto &it : canvas_docks) {
				auto cuuid = obs_canvas_get_uuid(it->GetCanvas());
				if (strcmp(cuuid, uuid) == 0) {
					if (obs_canvas_removed(it->GetCanvas()) ||
					    strcmp(it->parentWidget()->objectName().toUtf8().constData(), name) != 0) {
						// canvas name changed, remove old dock and create a new one
						obs_frontend_remove_dock(it->parentWidget()->objectName().toUtf8().constData());
						check_new_canvas = false;
					} else {
						cd = it;
					}
					break;
				} else if (strcmp(it->parentWidget()->objectName().toUtf8().constData(), name) == 0) {
					if (obs_canvas_removed(it->GetCanvas())) {
						obs_frontend_remove_dock(name);
						check_new_canvas = false;
						break;
					}
					obs_data_set_string(t, "uuid", cuuid);
					cd = it;
					break;
				}
			}

			if (cd) {
				cd->UpdateSettings(t);
			} else {
				cd = new CanvasDock(t, main_window);
				std::string title = name;
				if (obs_frontend_add_dock_by_id(name, title.c_str(), cd)) {
					canvas_docks.push_back(cd);
					if (!obs_data_get_bool(t, "has_loaded")) {
						cd->parentWidget()->show();
						obs_data_set_bool(t, "has_loaded", true);
					}
				} else {
					delete cd;
				}
			}
			i++;
		}
	}
	obs_data_array_release(canvas);
	for (const auto &it : canvas_clone_docks) {
		it->UpdateSettings(nullptr);
	}
	refresh_native_canvas_tabs();
	/* Canvas docks / preview rebuild land after OBS's first restoreState.
	 * Re-apply DockState on a short schedule so sizes stick. */
	schedule_restore_obs_dock_layout();
}

void save_current_profile_config(bool save_docks)
{
	if (!current_profile_config) {
		return;
	}
	char *profile_path = obs_frontend_get_current_profile_path();
	if (!profile_path) {
		return;
	}

	struct dstr path;
	dstr_init_copy(&path, profile_path);
	bfree(profile_path);

	if (!dstr_is_empty(&path) && dstr_end(&path) != '/') {
		dstr_cat_ch(&path, '/');
	}
	dstr_cat(&path, "aitum.json");

	if (save_docks) {
		save_dock_state(kDefaultMode);
		obs_data_erase(current_profile_config, "custom_dock_modes");
		obs_data_erase(current_profile_config, "dock_state_mode");
		obs_data_erase(current_profile_config, "dock_mode_manual_save");
		obs_data_erase(current_profile_config, "main_geometry");
		obs_data_erase(current_profile_config, "main_state");
		obs_data_erase(current_profile_config, "empty_docks");
	}
	if (output_dock) {
		output_dock->SaveSettings();
	}

	if (!obs_data_save_json_safe(current_profile_config, path.array, "tmp", "bak")) {
		blog(LOG_WARNING, "[Aitum Stream Suite] Failed to save configuration file");
	} else {
		blog(LOG_INFO, "[Aitum Stream Suite] Saved configuration file");
	}

	dstr_free(&path);
}

void load_current_profile_config()
{
	obs_data_release(current_profile_config);
	current_profile_config = nullptr;

	char *profile_path = obs_frontend_get_current_profile_path();
	if (!profile_path) {
		return;
	}

	struct dstr path;
	dstr_init_copy(&path, profile_path);
	bfree(profile_path);

	if (!dstr_is_empty(&path) && dstr_end(&path) != '/') {
		dstr_cat_ch(&path, '/');
	}
	dstr_cat(&path, "aitum.json");

	current_profile_config = obs_data_create_from_json_file_safe(path.array, "bak");
	dstr_free(&path);
	if (!current_profile_config) {
		current_profile_config = obs_data_create();
		obs_data_set_bool(current_profile_config, "main_stream_output_show", true);
		obs_data_set_bool(current_profile_config, "main_record_output_show", true);
		blog(LOG_WARNING, "[Stream Suite] No configuration file loaded");
		save_current_profile_config(false);
	} else {
		blog(LOG_INFO, "[Stream Suite] Loaded configuration file");
	}
	/* Drop legacy dock-layout keys from older builds. */
	obs_data_erase(current_profile_config, "empty_docks");
	obs_data_erase(current_profile_config, "custom_dock_modes");
	obs_data_erase(current_profile_config, "dock_state_mode");
	obs_data_erase(current_profile_config, "dock_mode_manual_save");
	obs_data_erase(current_profile_config, "main_geometry");
	obs_data_erase(current_profile_config, "main_state");

	bool first_create = false;
	auto canvas = obs_data_get_array(current_profile_config, "canvas");
	if (!canvas) {
		/* Start empty — user adds canvases/outputs in settings. */
		first_create = true;
		canvas = obs_data_array_create();
		obs_data_set_array(current_profile_config, "canvas", canvas);
		auto outputs2 = obs_data_get_array(current_profile_config, "outputs");
		if (!outputs2) {
			outputs2 = obs_data_array_create();
			obs_data_set_array(current_profile_config, "outputs", outputs2);
		}
		obs_data_array_release(outputs2);
	} else {
		obs_data_array_release(canvas);
	}
	load_canvas(first_create);

	load_outputs();

	QMetaObject::invokeMethod(qApp, [] { load_dock_state(kDefaultMode); }, Qt::QueuedConnection);
}


static bool all_string_settings_the_same(obs_data_t *settings_a, obs_data_t *settings_b)
{
	size_t string_count = 0;
	obs_data_item_t *i = obs_data_first(settings_a);
	while (i) {
		const enum obs_data_type t = obs_data_item_gettype(i);
		if (t == OBS_DATA_STRING) {
			string_count++;
			const char *name = obs_data_item_get_name(i);
			const char *value_a = obs_data_item_get_string(i);
			const char *value_b = obs_data_get_string(settings_b, name);
			if (strcmp(value_a, value_b) != 0) {
				return false;
			}
		}
		obs_data_item_next(&i);
	}
	return string_count > 0;
}

static void log_same_sources()
{
	std::list<obs_source_t *> sources;
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto sources = static_cast<std::list<obs_source_t *> *>(data);
			auto id_a = obs_source_get_id(source);
			auto settings_a = obs_source_get_settings(source);
			bool do_not_duplicate = obs_source_get_output_flags(source) & OBS_SOURCE_DO_NOT_DUPLICATE;
			if (settings_a) {
				const char *json_a = obs_data_get_json(settings_a);
				for (auto &it : (*sources)) {
					auto id_b = obs_source_get_id(it);
					if (strcmp(id_a, id_b) == 0) {
						auto settings_b = obs_source_get_settings(it);
						if (settings_b) {
							const char *json_b = obs_data_get_json(settings_b);
							if (strcmp(json_a, json_b) == 0) {
								blog(LOG_WARNING,
								     "[Aitum Stream Suite] Duplicate source found: '%s', '%s'",
								     obs_source_get_name(source), obs_source_get_name(it));
							} else if (do_not_duplicate &&
								   all_string_settings_the_same(settings_a, settings_b)) {
								blog(LOG_WARNING,
								     "[Aitum Stream Suite] Similar source found: '%s', '%s'",
								     obs_source_get_name(source), obs_source_get_name(it));
							}
							obs_data_release(settings_b);
						}
					}
				}
				obs_data_release(settings_a);
			}
			sources->push_back(obs_source_get_ref(source));
			return true;
		},
		&sources);
	for (auto &it : sources) {
		obs_source_release(it);
	}
}

static bool restart = false;

static void frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		finished_loading = true;
		if (restart) {
			const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!main_window) {
				return;
			}
			QTimer::singleShot(2000, main_window, [main_window] {
				auto dialogs = main_window->findChildren<QDialog *>();
				for (auto dialog : dialogs) {
					dialog->close();
				}
				QMetaObject::invokeMethod(main_window, "close", Qt::QueuedConnection);
			});
			return;
		}
		log_same_sources();
		struct obs_frontend_source_list transitions = {};
		obs_frontend_get_transitions(&transitions);
		for (size_t i = 0; i < transitions.sources.num; i++) {
			auto sh = obs_source_get_signal_handler(transitions.sources.array[i]);
			signal_handler_connect(sh, "transition_start", transition_start, nullptr);
		}
		obs_frontend_source_list_free(&transitions);

		load_current_profile_config();
		install_main_canvas_context_bar_fix();
		auto scene = obs_frontend_get_current_scene();
		if (scene) {
			if (properties_dock) {
				QMetaObject::invokeMethod(properties_dock, "SceneChanged", Qt::QueuedConnection,
							  Q_ARG(OBSSource, OBSSource(scene)));
			}
			obs_source_release(scene);
		}
	} else if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		load_current_profile_config();
	} else if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGING) {
		save_current_profile_config(true);
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		CancelSourcePreviews();
		if (current_profile_config) {
			obs_data_release(current_profile_config);
			current_profile_config = nullptr;
		}
		if (output_dock) {
			output_dock->Exiting();
		}
		if (properties_dock) {
			properties_dock->Exiting();
		}
		/* Disconnect libobs signal handlers before ClearSceneData
		 * (SCRIPTING_SHUTDOWN runs first) so callbacks never hit
		 * destroyed docks during scene teardown. */
		shutdown_native_canvas_tabs();
		shutdown_main_canvas_context_bar_fix();

		/* Tear down while the UI is still alive. Registered docks are
		 * owned by OBS after obs_frontend_add_dock_by_id — remove only. */
		obs_frontend_remove_dock("AitumStreamSuiteOutput");
		output_dock = nullptr;

		/* Hidden helpers are children of the main window; delete them
		 * here so obs_module_unload does not touch dangling pointers. */
		delete properties_dock;
		properties_dock = nullptr;
		delete filters_dock;
		filters_dock = nullptr;
		delete transform_dock;
		transform_dock = nullptr;
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		if (output_dock) {
			output_dock->UpdateMainStreamStatus(true);
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		if (output_dock) {
			output_dock->UpdateMainStreamStatus(false);
		}
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
		if (output_dock) {
			output_dock->UpdateMainRecordingStatus(true);
		}
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPING || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		if (output_dock) {
			output_dock->UpdateMainRecordingStatus(false);
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) {
		for (const auto &it : canvas_docks) {
			QMetaObject::invokeMethod(it, "MainSceneChanged", Qt::QueuedConnection);
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) {
		if (finished_loading) {
			log_same_sources();
			struct obs_frontend_source_list transitions = {};
			obs_frontend_get_transitions(&transitions);
			for (size_t i = 0; i < transitions.sources.num; i++) {
				auto sh = obs_source_get_signal_handler(transitions.sources.array[i]);
				signal_handler_connect(sh, "transition_start", transition_start, nullptr);
			}
			obs_frontend_source_list_free(&transitions);
			load_current_profile_config();

			auto scene = obs_frontend_get_current_scene();
			if (scene) {
				if (properties_dock) {
					QMetaObject::invokeMethod(properties_dock, "SceneChanged", Qt::QueuedConnection,
								  Q_ARG(OBSSource, OBSSource(scene)));
				}
				obs_source_release(scene);
			}
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		scene_collection_changing = true;
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		for (auto i = canvas_clone_docks.size(); i > 0; i--) {
			auto it = canvas_clone_docks.begin();
			std::advance(it, i - 1);
			auto dock = (*it)->parentWidget();
			if (dock) {
				obs_frontend_remove_dock(dock->objectName().toUtf8().constData());
			}
		}

		for (auto i = canvas_docks.size(); i > 0; i--) {
			auto it = canvas_docks.begin();
			std::advance(it, i - 1);
			auto dock = (*it)->parentWidget();
			if (dock) {
				obs_frontend_remove_dock(dock->objectName().toUtf8().constData());
			}
		}
	}
}

bool obs_data_array_equal(obs_data_array_t *a, obs_data_array_t *b)
{
	size_t a_count = obs_data_array_count(a);
	size_t b_count = obs_data_array_count(b);
	if (a_count != b_count) {
		return false;
	}
	for (size_t i = 0; i < a_count; i++) {
		obs_data_t *a_item = obs_data_array_item(a, i);
		obs_data_t *b_item = obs_data_array_item(b, i);
		const char *a_json = obs_data_get_json(a_item);
		const char *b_json = obs_data_get_json(b_item);
		bool equal = (strcmp(a_json, b_json) == 0);
		obs_data_release(a_item);
		obs_data_release(b_item);
		if (!equal) {
			return false;
		}
	}
	return true;
}

void open_config_dialog(int tab, const char *create_type)
{
	if (!configDialog) {
		configDialog = new OBSBasicSettings((QMainWindow *)obs_frontend_get_main_window());
		QObject::connect(configDialog, &OBSBasicSettings::accepted, [] {

		});
	}
	auto settings = obs_data_create();
	if (current_profile_config) {
		const char *geom = obs_data_get_string(current_profile_config, "config_geometry");
		if (geom && strlen(geom)) {
			QByteArray ba = QByteArray::fromBase64(QByteArray(geom));
			configDialog->restoreGeometry(ba);
		}
		obs_data_apply(settings, current_profile_config);
	}

	configDialog->LoadSettings(settings);
	if (tab > 0) {
		configDialog->ShowTab(tab);
	}
	configDialog->show();
	configDialog->SetCreateType(create_type);

	if (configDialog->exec() == QDialog::Accepted) {
		bool canvas_changed = false;
		bool outputs_changed = false;
		bool check_new_canvas = false;
		if (current_profile_config) {
			auto show = obs_data_get_bool(settings, "main_stream_output_show");
			if (show != obs_data_get_bool(current_profile_config, "main_stream_output_show")) {
				obs_data_set_bool(current_profile_config, "main_stream_output_show", show);
				outputs_changed = true;
			}
			show = obs_data_get_bool(settings, "main_record_output_show");
			if (show != obs_data_get_bool(current_profile_config, "main_record_output_show")) {
				obs_data_set_bool(current_profile_config, "main_record_output_show", show);
				outputs_changed = true;
			}
			const char *recordCanvas = obs_data_get_string(settings, "main_record_canvas");
			const char *savedCanvas = obs_data_get_string(current_profile_config, "main_record_canvas");
			if (strcmp(recordCanvas ? recordCanvas : "", savedCanvas ? savedCanvas : "") != 0) {
				obs_data_set_string(current_profile_config, "main_record_canvas", recordCanvas ? recordCanvas : "");
				outputs_changed = true;
			}
			obs_data_array_t *a = obs_data_get_array(current_profile_config, "canvas");
			obs_data_array_t *b = obs_data_get_array(settings, "canvas");
			if (!obs_data_array_equal(a, b)) {
				canvas_changed = true;
				obs_data_set_array(current_profile_config, "canvas", b);
			}
			obs_data_array_release(a);
			obs_data_array_release(b);
			a = obs_data_get_array(current_profile_config, "outputs");
			b = obs_data_get_array(settings, "outputs");
			if (!obs_data_array_equal(a, b)) {
				outputs_changed = true;
				check_new_canvas = true;
				obs_data_set_array(current_profile_config, "outputs", b);
			}
			obs_data_array_release(a);
			obs_data_array_release(b);
			obs_data_release(settings);
		} else {
			canvas_changed = true;
			outputs_changed = true;
			current_profile_config = settings;
		}
		obs_data_set_string(current_profile_config, "config_geometry", configDialog->saveGeometry().toBase64().constData());
		configDialog->SaveHotkeys();

		save_current_profile_config(true);
		if (canvas_changed) {
			load_canvas(check_new_canvas);
			if (vendor) {
				obs_websocket_vendor_emit_event(vendor, "canvas_changed", nullptr);
			}
		}

		if (outputs_changed) {
			load_outputs();
			if (vendor) {
				obs_websocket_vendor_emit_event(vendor, "outputs_changed", nullptr);
			}
		}
	} else {
		obs_data_release(settings);
	}
}
extern "C" const struct obs_source_info component_info;

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Stream Suite] loaded version %s", PROJECT_VERSION);

	obs_register_source(&component_info);

	obs_frontend_add_event_callback(frontend_event, nullptr);

	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	/* Keep the user's OBS theme (Yami/Acri, etc.). Do not force Aitum theme. */

	/* OBS owns dock placement. Settings stay in the Stream Suite dialog and in OBS Settings. */
	auto cw = main_window->centralWidget();
	if (cw && cw->objectName() == "centralwidget" && cw->findChild<QWidget *>("canvasEditor") != nullptr) {
		obs_frontend_add_dock_by_id("AitumStreamSuiteMainCanvas", obs_module_text("AitumStreamSuiteMainCanvas"), cw);
		auto l = new QLabel(QString::fromUtf8(obs_module_text("ResizeDocks")), main_window);
		l->setAlignment(Qt::AlignCenter);
		l->setContentsMargins(0, 0, 0, 0);
		l->setMinimumSize(0, 0);
		l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
		main_window->setCentralWidget(l);
		/* Dock is narrow → OBS Minimized clears typed toolbars; reinject. */
		QTimer::singleShot(0, main_window, [] { install_main_canvas_context_bar_fix(); });
	}

	output_dock = new OutputDock(main_window);
	obs_frontend_add_dock_by_id("AitumStreamSuiteOutput", obs_module_text("AitumStreamSuiteOutput"), output_dock);
	/* Keep Properties/Filters/Transform as hidden helpers — use OBS native dialogs in the UI. */
	properties_dock = new PropertiesDock(main_window);
	properties_dock->hide();
	filters_dock = new FiltersDock(main_window);
	filters_dock->hide();
	transform_dock = new TransformDock(main_window);
	transform_dock->hide();
	/* Scenes/Sources/Transitions live in OBS native docks (canvas tabs). */

	return true;
}

static void save_load(obs_data_t *save_data, bool saving, void *private_data)
{
	UNUSED_PARAMETER(save_data);
	UNUSED_PARAMETER(private_data);
	if (saving) {
		save_current_profile_config(!scene_collection_changing);
	}
}

void load_obs_websocket();

void obs_module_post_load()
{
	obs_frontend_add_save_callback(save_load, nullptr);
	load_obs_websocket();
}

void unload_obs_websocket();

void obs_module_unload()
{
	unload_obs_websocket();
	obs_frontend_remove_save_callback(save_load, nullptr);
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	if (current_profile_config) {
		obs_data_release(current_profile_config);
		current_profile_config = nullptr;
	}

	/* Prefer teardown on EXIT. If unload runs without EXIT (e.g. module
	 * reload), remove registered docks but never delete their widgets —
	 * obs_frontend_remove_dock already destroys them. */
	if (output_dock) {
		obs_frontend_remove_dock("AitumStreamSuiteOutput");
		output_dock = nullptr;
	}

	if (properties_dock) {
		properties_dock->setParent(nullptr);
		delete properties_dock;
		properties_dock = nullptr;
	}
	if (filters_dock) {
		filters_dock->setParent(nullptr);
		delete filters_dock;
		filters_dock = nullptr;
	}
	if (transform_dock) {
		transform_dock->setParent(nullptr);
		delete transform_dock;
		transform_dock = nullptr;
	}

}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("AitumStreamSuite");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("AitumStreamSuite");
}

