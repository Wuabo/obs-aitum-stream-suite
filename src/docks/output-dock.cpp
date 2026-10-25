#include "output-dock.hpp"

#include "../version.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QCheckBox>
#include <QDateTime>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QScrollArea>
#include <QToolBar>
#include <QToolButton>
#include <src/utils/color.hpp>
#include <src/utils/icon.hpp>
#include <util/config-file.h>
#include <util/platform.h>

void open_config_dialog(int tab, const char *create_type);

OutputDock::OutputDock(QWidget *parent) : QFrame(parent)
{

	auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	auto t = new QWidget;

	QScrollArea *scrollArea = new QScrollArea;
	scrollArea->setWidget(t);
	scrollArea->setWidgetResizable(true);
	scrollArea->setLineWidth(0);
	scrollArea->setFrameShape(QFrame::NoFrame);
	//scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	auto layout = new QVBoxLayout;
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(scrollArea);

	auto toolbar = new QToolBar();

	toolbar->setObjectName(QStringLiteral("outputsToolbar"));
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);

	auto a = toolbar->addAction(QIcon(QString::fromUtf8(":/settings/images/settings/general.svg")),
				    QString::fromUtf8(obs_frontend_get_locale_string("Settings")),
				    [this] { open_config_dialog(1, nullptr); });
	toolbar->widgetForAction(a)->setProperty("themeID", QVariant(QString::fromUtf8("propertiesIconSmall")));
	toolbar->widgetForAction(a)->setProperty("class", "icon-gear");

	auto addMenu = new QMenu;

	a = toolbar->addAction(QIcon(":/res/images/plus.svg"), QString::fromUtf8(obs_module_text("AddOutput")),
			       [addMenu] { addMenu->exec(QCursor::pos()); });
	toolbar->widgetForAction(a)->setProperty("themeID", QVariant(QString::fromUtf8("addIconSmall")));
	toolbar->widgetForAction(a)->setProperty("class", "icon-plus");

	a = toolbar->addAction(
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart, QIcon(":/res/images/media/media_play.svg")),
#else
		QIcon(":/res/images/media/media_play.svg"),
#endif
		QString::fromUtf8(obs_module_text("StartAll")));
	connect(a, &QAction::triggered, [this] {
		QMenu startMenu;
		auto a2 = startMenu.addAction(
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
			QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart, QIcon(":/res/images/media/media_play.svg")),
#else
			QIcon(":/res/images/media/media_play.svg"),
#endif
			QString::fromUtf8(obs_module_text("StartAllOutputs")), [this]() { StartAll(false, false); });
		a2->setProperty("themeID", QVariant(QString::fromUtf8("playIcon")));
		a2->setProperty("class", "icon-media-play");
		if (mainStreamEnabled || std::find_if(outputWidgets.begin(), outputWidgets.end(),
						      [](OutputWidget *w) { return w->IsStream(); }) != outputWidgets.end()) {
			startMenu.addAction(QIcon(QString::fromUtf8(":/aitum/media/stream.svg")),
					    QString::fromUtf8(obs_module_text("StartAllStreams")),
					    [this]() { StartAll(true, false); });
		}
		if (mainRecordEnabled || std::find_if(outputWidgets.begin(), outputWidgets.end(),
						      [](OutputWidget *w) { return w->IsRecord(); }) != outputWidgets.end()) {
			startMenu.addAction(QIcon(QString::fromUtf8(":/aitum/media/record.svg")),
					    QString::fromUtf8(obs_module_text("StartAllRecordings")),
					    [this]() { StartAll(false, true); });
		}
		startMenu.exec(QCursor::pos());
	});
	toolbar->widgetForAction(a)->setProperty("themeID", QVariant(QString::fromUtf8("playIcon")));
	toolbar->widgetForAction(a)->setProperty("class", "icon-media-play");
	a->setIconText(QString::fromUtf8(obs_module_text("StartAll")));
	((QToolButton *)toolbar->widgetForAction(a))->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	toolbar->widgetForAction(a)->setMinimumWidth(80);

	a = toolbar->addAction(
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
		QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStop, QIcon(":/res/images/media/media_stop.svg")),
#else
		QIcon(":/res/images/media/media_stop.svg"),
#endif
		QString::fromUtf8(obs_module_text("StopAllOutputs")), [this] { StopAll(false, false); });
	toolbar->widgetForAction(a)->setProperty("themeID", QVariant(QString::fromUtf8("stopIcon")));
	toolbar->widgetForAction(a)->setProperty("class", "icon-media-stop");

	a = addMenu->addAction(QIcon(QString::fromUtf8(":/aitum/media/stream.svg")), QString::fromUtf8(obs_module_text("Stream")),
			       [this] { open_config_dialog(1, "stream"); });

	a = addMenu->addAction(QIcon(QString::fromUtf8(":/aitum/media/record.svg")), QString::fromUtf8(obs_module_text("Record")),
			       [this] { open_config_dialog(1, "record"); });

	layout->addWidget(toolbar);

	setLayout(layout);

	mainLayout = new QVBoxLayout;
	mainLayout->setContentsMargins(0, 0, 0, 0);
	t->setLayout(mainLayout);

	mainPlatformIconLabel = new QLabel;
	mainPlatformIconLabel->setFixedSize(outputPlatformIconSize, outputPlatformIconSize);
	mainPlatformIconLabel->setScaledContents(true);
	mainPlatformIconLabel->setPixmap(
		getPlatformIconFromEndpoint(QString::fromUtf8("")).pixmap(outputPlatformIconSize, outputPlatformIconSize));

	auto makeIconSlot = [this](QWidget *icon) {
		auto slot = new QWidget;
		slot->setFixedWidth(outputPlatformIconSize);
		auto sl = new QHBoxLayout(slot);
		sl->setContentsMargins(0, 0, 0, 0);
		sl->setSpacing(0);
		if (icon) {
			sl->addWidget(icon, 0, Qt::AlignVCenter);
		}
		return slot;
	};

	auto styleActionButton = [](QPushButton *btn) {
		btn->setFixedWidth(kActionBtnWidth);
		btn->setMinimumHeight(28);
		btn->setMaximumHeight(32);
		btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	};

	auto l2 = new QHBoxLayout;
	l2->setContentsMargins(8, 4, 8, 4);
	l2->setSpacing(kRowSpacing);

	l2->addWidget(makeIconSlot(mainPlatformIconLabel), 0, Qt::AlignVCenter);
	l2->addWidget(new QLabel(QString::fromUtf8(obs_module_text("BuiltinStream"))), 2, Qt::AlignVCenter);
	mainStreamCanvasLabel = new QLabel(QString::fromUtf8(obs_module_text("MainCanvas")));
	l2->addWidget(mainStreamCanvasLabel, 1, Qt::AlignVCenter);
	mainStreamButton = new QPushButton;
	mainStreamButton->setObjectName(QStringLiteral("canvasStream"));
	styleActionButton(mainStreamButton);
	mainStreamButton->setIcon(create2StateIcon(":/aitum/media/streaming.svg", ":/aitum/media/stream.svg"));
	mainStreamButton->setCheckable(true);
	mainStreamButton->setChecked(false);
	connect(mainStreamButton, &QPushButton::toggled, [this](bool checked) {
		mainStreamButton->setProperty("class", checked ? QStringLiteral("state-active") : QStringLiteral(""));
		mainStreamButton->style()->unpolish(mainStreamButton);
		mainStreamButton->style()->polish(mainStreamButton);
	});

	connect(mainStreamButton, &QPushButton::clicked, [this] {
		const auto config = obs_frontend_get_user_config();
		if (obs_frontend_streaming_active()) {
			bool stop = true;
			bool warnBeforeStreamStop = config_get_bool(config, "BasicWindow", "WarnBeforeStoppingStream");
			if (warnBeforeStreamStop && isVisible()) {
				auto button = QMessageBox::question(
					this, QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Title")),
					QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Text")),
					QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (button == QMessageBox::No) {
					stop = false;
				}
			}
			if (stop) {
				obs_frontend_streaming_stop();
				mainStreamButton->setChecked(false);
			} else {
				mainStreamButton->setChecked(true);
			}
		} else {
			bool warnBeforeStreamStart = config_get_bool(config, "BasicWindow", "WarnBeforeStartingStream");
			if (warnBeforeStreamStart && isVisible()) {
				auto button = QMessageBox::question(
					this, QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Title")),
					QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Text")),
					QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
				if (button == QMessageBox::No) {
					mainStreamButton->setChecked(false);
				} else {
					obs_frontend_streaming_start();
				}
			} else {
				obs_frontend_streaming_start();
			}
			mainStreamButton->setChecked(true);
		}
	});
	mainStreamButton->setToolTip(QString::fromUtf8(obs_module_text("Stream")));
	l2->addWidget(mainStreamButton, 0, Qt::AlignVCenter);

	mainStreamGroup = new QFrame;
	mainStreamGroup->setLayout(l2);

	mainLayout->addWidget(mainStreamGroup);

	l2 = new QHBoxLayout;
	l2->setContentsMargins(8, 4, 8, 4);
	l2->setSpacing(kRowSpacing);
	l2->addWidget(makeIconSlot(nullptr), 0, Qt::AlignVCenter);
	l2->addWidget(new QLabel(QString::fromUtf8(obs_module_text("BuiltinRecord"))), 2, Qt::AlignVCenter);
	mainRecordCanvasLabel = new QLabel(QString::fromUtf8(obs_module_text("MainCanvas")));
	l2->addWidget(mainRecordCanvasLabel, 1, Qt::AlignVCenter);
	mainRecordButton = new QPushButton;
	mainRecordButton->setObjectName(QStringLiteral("canvasRecord"));
	styleActionButton(mainRecordButton);
	mainRecordButton->setIcon(create2StateIcon(":/aitum/media/recording.svg", ":/aitum/media/record.svg"));
	mainRecordButton->setCheckable(true);
	mainRecordButton->setChecked(false);
	connect(mainRecordButton, &QPushButton::toggled, [this](bool checked) {
		mainRecordButton->setProperty("class", checked ? QStringLiteral("state-active") : QStringLiteral(""));
		mainRecordButton->style()->unpolish(mainRecordButton);
		mainRecordButton->style()->polish(mainRecordButton);
	});
	QObject::connect(mainRecordButton, &QPushButton::clicked, [this] { ToggleMainRecord(); });

	l2->addWidget(mainRecordButton, 0, Qt::AlignVCenter);

	mainRecordGroup = new QFrame;
	mainRecordGroup->setLayout(l2);
	mainLayout->addWidget(mainRecordGroup);

	connect(&videoCheckTimer, &QTimer::timeout, [this] {
		if (exiting) {
			return;
		}

		if (mainPlatformIconLabel) {
			auto service = obs_frontend_get_streaming_service();
			auto url = QString::fromUtf8(
				service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : "");
			if (url != mainPlatformUrl) {
				mainPlatformUrl = url;
				mainPlatformIconLabel->setPixmap(
					getPlatformIconFromEndpoint(url).pixmap(outputPlatformIconSize, outputPlatformIconSize));
			}
		}

		if (mainStreamEnabled) {
			auto active = obs_frontend_streaming_active();
			if (mainStreamButton->isChecked() != active) {
				mainStreamButton->setChecked(active);
				if (!active) {
					mainStreamButton->setText("");
				}
			} else if (active) {
				auto t = QTime::fromMSecsSinceStartOfDay(mainStreamStartTime.msecsTo(QDateTime::currentDateTime()));
				mainStreamButton->setText(t.toString(t.hour() ? "hh:mm:ss" : "mm:ss"));
			}
		}

		if (mainRecordEnabled) {
			auto active = CanvasRecordActive() || (!RecordUsesCanvas() && obs_frontend_recording_active());
			if (mainRecordButton->isChecked() != active) {
				mainRecordButton->setChecked(active);
				if (!active) {
					mainRecordButton->setText("");
				}
			} else if (active) {
				auto t = QTime::fromMSecsSinceStartOfDay(mainRecordStartTime.msecsTo(QDateTime::currentDateTime()));
				mainRecordButton->setText(t.toString(t.hour() ? "hh:mm:ss" : "mm:ss"));
			}
		}

		for (auto it = outputWidgets.begin(); it != outputWidgets.end(); it++) {
			(*it)->CheckActive();
		}
	});
	videoCheckTimer.start(500);
	obs_frontend_add_event_callback(frontend_event, this);

	StartStopHotkey = obs_hotkey_pair_register_frontend(
		"AitumStreamSuiteStartAllOutputs", obs_module_text("StartAllOutputs"), "AitumStreamSuiteStopAllOutputs",
		obs_module_text("StopAllOutputs"),
		[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return false;
			}
			auto dock = static_cast<OutputDock *>(data);
			QMetaObject::invokeMethod(dock, [dock] { dock->StartAll(false, false); });
			return true;
		},
		[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return false;
			}
			auto dock = static_cast<OutputDock *>(data);
			QMetaObject::invokeMethod(dock, [dock] { dock->StopAll(false, false); });
			return true;
		},
		this, this);

	StartStreamHotkey = obs_hotkey_register_frontend(
		"AitumStreamSuiteStartAllStreams", obs_module_text("StartAllStreams"),
		[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return;
			}
			auto dock = static_cast<OutputDock *>(data);
			QMetaObject::invokeMethod(dock, [dock] { dock->StartAll(true, false); });
		},
		this);

	StartRecordHotkey = obs_hotkey_register_frontend(
		"AitumStreamSuiteStartAllRecordings", obs_module_text("StartAllRecordings"),
		[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return;
			}
			auto dock = static_cast<OutputDock *>(data);
			QMetaObject::invokeMethod(dock, [dock] { dock->StartAll(false, true); });
		},
		this);
}

extern OutputDock *output_dock;
void RemoveWidget(QWidget *widget);

OutputDock::~OutputDock()
{
	StopCanvasRecord();
	if (StartStopHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_hotkey_pair_unregister(StartStopHotkey);
	}
	if (StartStreamHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(StartStreamHotkey);
	}
	if (StartRecordHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(StartRecordHotkey);
	}
	obs_frontend_add_event_callback(frontend_event, this);
	videoCheckTimer.stop();
	for (auto it = outputWidgets.begin(); it != outputWidgets.end(); it++) {
		mainLayout->removeWidget(*it);
		RemoveWidget(*it);
	}
	outputWidgets.clear();
	output_dock = nullptr;
}

extern obs_data_t *current_profile_config;

void OutputDock::LoadSettings()
{
	mainStreamEnabled = obs_data_get_bool(current_profile_config, "main_stream_output_show");
	mainStreamGroup->setVisible(mainStreamEnabled);

	mainRecordEnabled = obs_data_get_bool(current_profile_config, "main_record_output_show");
	mainRecordGroup->setVisible(mainRecordEnabled);
	RefreshRecordCanvasLabel();

	auto outputs2 = obs_data_get_array(current_profile_config, "outputs");
	for (auto it = outputWidgets.begin(); it != outputWidgets.end();) {
		bool found = false;
		auto count = obs_data_array_count(outputs2);
		for (size_t i = 0; i < count; i++) {
			obs_data_t *data2 = obs_data_array_item(outputs2, i);
			auto type = obs_data_get_string(data2, "type");
			if (strcmp(type, "backtrack") == 0 || strcmp(type, "virtual_cam") == 0) {
				obs_data_release(data2);
				continue;
			}
			auto name = QString::fromUtf8(obs_data_get_string(data2, "name"));
			if (name == (*it)->objectName()) {
				(*it)->UpdateSettings(data2);
				auto j = mainLayout->indexOf(*it);
				if (j >= 0 && i + 2 != (size_t)j) {
					mainLayout->takeAt(j);
					mainLayout->insertWidget((int)i + 2, (*it));
				}
				found = true;
				obs_data_release(data2);
				break;
			}
			obs_data_release(data2);
		}
		if (!found) {
			mainLayout->removeWidget(*it);
			RemoveWidget(*it);
			it = outputWidgets.erase(it);
		} else {
			it++;
		}
	}

	auto count = obs_data_array_count(outputs2);
	size_t visibleIdx = 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *data2 = obs_data_array_item(outputs2, i);
		auto type = obs_data_get_string(data2, "type");
		if (strcmp(type, "backtrack") == 0 || strcmp(type, "virtual_cam") == 0) {
			obs_data_release(data2);
			continue;
		}
		auto name = QString::fromUtf8(obs_data_get_string(data2, "name"));
		if (std::find_if(outputWidgets.begin(), outputWidgets.end(),
				 [&name](OutputWidget *ow) { return ow->objectName() == name; }) == outputWidgets.end()) {
			auto outputWidget = new OutputWidget(data2, this);
			outputWidgets.push_back(outputWidget);
			mainLayout->insertWidget((int)visibleIdx + 2, outputWidget);
		}
		visibleIdx++;
		obs_data_release(data2);
	}

	obs_data_array_release(outputs2);

	QWidget *spacer = new QWidget();
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	mainLayout->addWidget(spacer);

	auto start_hotkey = obs_data_get_array(current_profile_config, "start_all_outputs_hotkey");
	auto stop_hotkey = obs_data_get_array(current_profile_config, "stop_all_outputs_hotkey");
	obs_hotkey_pair_load(StartStopHotkey, start_hotkey, stop_hotkey);
	obs_data_array_release(start_hotkey);
	obs_data_array_release(stop_hotkey);

	auto stream_hotkey = obs_data_get_array(current_profile_config, "start_all_streams_hotkey");
	obs_hotkey_load(StartStreamHotkey, stream_hotkey);
	obs_data_array_release(stream_hotkey);

	auto record_hotkey = obs_data_get_array(current_profile_config, "start_all_recordings_hotkey");
	obs_hotkey_load(StartRecordHotkey, record_hotkey);
	obs_data_array_release(record_hotkey);
}

void OutputDock::SaveSettings()
{
	for (auto it = outputWidgets.begin(); it != outputWidgets.end(); it++) {
		(*it)->SaveSettings();
	}

	if (StartStopHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_data_array_t *start_hotkey = nullptr;
		obs_data_array_t *stop_hotkey = nullptr;
		obs_hotkey_pair_save(StartStopHotkey, &start_hotkey, &stop_hotkey);
		obs_data_set_array(current_profile_config, "start_all_outputs_hotkey", start_hotkey);
		obs_data_set_array(current_profile_config, "stop_all_outputs_hotkey", stop_hotkey);
		obs_data_array_release(start_hotkey);
		obs_data_array_release(stop_hotkey);
	}
	if (StartStreamHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *stream_hotkey = obs_hotkey_save(StartStreamHotkey);
		obs_data_set_array(current_profile_config, "start_all_streams_hotkey", stream_hotkey);
		obs_data_array_release(stream_hotkey);
	}
	if (StartRecordHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *record_hotkey = obs_hotkey_save(StartRecordHotkey);
		obs_data_set_array(current_profile_config, "start_all_recordings_hotkey", record_hotkey);
		obs_data_array_release(record_hotkey);
	}
}

void OutputDock::UpdateMainStreamStatus(bool active)
{
	mainStreamButton->setChecked(active);
	if (active) {
		mainStreamStartTime = QDateTime::currentDateTime();
		mainStreamButton->setText("00:00");
	} else {
		mainStreamButton->setText("");
		return;
	}
	if (!current_profile_config) {
		return;
	}

	std::map<video_t *, std::string> canvasNames;
	obs_enum_canvases(
		[](void *data, obs_canvas_t *c) {
			auto v = obs_canvas_get_video(c);
			if (!v) {
				return true;
			}
			auto cns = static_cast<std::map<video_t *, std::string> *>(data);
			(*cns)[obs_canvas_get_video(c)] = obs_canvas_get_name(c);
			return true;
		},
		&canvasNames);

	struct obs_video_info ovi = {0};
	obs_get_video_info(&ovi);
	double fps = ovi.fps_den > 0 ? (double)ovi.fps_num / (double)ovi.fps_den : 0.0;
	auto output = obs_frontend_get_streaming_output();
	QStringList canvasNamesList;
	bool found = false;
	for (auto i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		auto encoder = obs_output_get_video_encoder2(output, i);
		QString settingName = QString::fromUtf8("video_encoder_description") + QString::number(i);
		if (encoder) {
			found = true;

			QString mainEncoderDescription;
			auto v = obs_encoder_parent_video(encoder);
			auto f = canvasNames.find(v);
			if (f != canvasNames.end()) {
				mainEncoderDescription = QString::fromUtf8(f->second.c_str());
				if (!canvasNamesList.contains(mainEncoderDescription)) {
					canvasNamesList.push_back(mainEncoderDescription);
				}
			}
			if (!mainEncoderDescription.isEmpty()) {
				mainEncoderDescription += QString::fromUtf8(" ");
			}
			mainEncoderDescription += QString::number(obs_encoder_get_width(encoder)) + "x" + QString::number(obs_encoder_get_height(encoder));
			auto divisor = obs_encoder_get_frame_rate_divisor(encoder);
			if (divisor > 0) {
				mainEncoderDescription +=
					QString::fromUtf8(" ") + QString::number(fps / divisor, 'g', 4) + QString::fromUtf8("fps");
			}

			auto settings = obs_encoder_get_settings(encoder);
			auto bitrate = settings ? obs_data_get_int(settings, "bitrate") : 0;
			if (bitrate > 0) {
				mainEncoderDescription +=
					QString::fromUtf8(" ") + QString::number(bitrate) + QString::fromUtf8("Kbps");
			}
			obs_data_release(settings);

			obs_data_set_string(current_profile_config, settingName.toUtf8().constData(),
					    mainEncoderDescription.toUtf8().constData());

		} else if (found && obs_data_has_user_value(current_profile_config, settingName.toUtf8().constData())) {
			obs_data_unset_user_value(current_profile_config, settingName.toUtf8().constData());
		}
	}
	obs_output_release(output);
	if (canvasNamesList.count() > 1) {
		mainStreamCanvasLabel->setText(canvasNamesList.join(", "));
	} else {
		mainStreamCanvasLabel->setText(QString::fromUtf8(obs_module_text("MainCanvas")));
	}
}

bool OutputDock::RecordUsesCanvas() const
{
	if (!current_profile_config) {
		return false;
	}
	const char *name = obs_data_get_string(current_profile_config, "main_record_canvas");
	if (!name || !name[0]) {
		return false;
	}
	obs_canvas_t *mainCanvas = obs_get_main_canvas();
	const char *mainName = mainCanvas ? obs_canvas_get_name(mainCanvas) : nullptr;
	const bool other = !mainName || strcmp(name, mainName) != 0;
	obs_canvas_release(mainCanvas);
	return other;
}

bool OutputDock::CanvasRecordActive() const
{
	return canvasRecordOutput && obs_output_active(canvasRecordOutput);
}

void OutputDock::RefreshRecordCanvasLabel()
{
	if (!mainRecordCanvasLabel) {
		return;
	}
	QString name;
	if (current_profile_config) {
		name = QString::fromUtf8(obs_data_get_string(current_profile_config, "main_record_canvas"));
	}
	if (name.isEmpty()) {
		name = QString::fromUtf8(obs_module_text("MainCanvas"));
	}
	mainRecordCanvasLabel->setText(name);
}

void OutputDock::StopCanvasRecord()
{
	if (!canvasRecordOutput) {
		return;
	}
	if (obs_output_active(canvasRecordOutput)) {
		obs_output_stop(canvasRecordOutput);
	}
	obs_output_release(canvasRecordOutput);
	canvasRecordOutput = nullptr;
}

bool OutputDock::StartCanvasRecord()
{
	if (CanvasRecordActive()) {
		return true;
	}
	StopCanvasRecord();
	if (!current_profile_config) {
		return false;
	}
	const char *canvasName = obs_data_get_string(current_profile_config, "main_record_canvas");
	obs_canvas_t *canvas = (canvasName && canvasName[0]) ? obs_get_canvas_by_name(canvasName) : nullptr;
	if (!canvas) {
		QMessageBox::warning(this, QString::fromUtf8(obs_module_text("CanvasNotFound")),
				     QString::fromUtf8(obs_module_text("CanvasNotFound")));
		return false;
	}
	video_t *video = obs_canvas_get_video(canvas);
	if (!video) {
		obs_canvas_release(canvas);
		return false;
	}

	const char *encoderIds[] = {"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc", "obs_qsv11_v2", "h264_texture_amf",
				    "obs_x264"};
	const char *vencId = "obs_x264";
	for (const char *id : encoderIds) {
		const char *val = nullptr;
		for (int i = 0; obs_enum_encoder_types(i, &val); i++) {
			if (val && strcmp(val, id) == 0) {
				vencId = id;
				break;
			}
		}
		if (strcmp(vencId, id) == 0) {
			break;
		}
	}

	std::string vencName = "Aitum Stream Suite Builtin Record ";
	vencName += canvasName;
	obs_encoder_t *venc = obs_video_encoder_create(vencId, vencName.c_str(), nullptr, nullptr);
	if (!venc) {
		obs_canvas_release(canvas);
		return false;
	}
	obs_encoder_set_video(venc, video);
	obs_data_t *videoSettings = obs_data_create();
	obs_data_set_string(videoSettings, "rate_control", "CBR");
	obs_data_set_int(videoSettings, "bitrate", 6000);
	obs_encoder_update(venc, videoSettings);
	obs_data_release(videoSettings);

	std::string aencName = vencName + " Audio";
	obs_encoder_t *aenc = obs_audio_encoder_create("ffmpeg_aac", aencName.c_str(), nullptr, 0, nullptr);
	if (!aenc) {
		obs_encoder_release(venc);
		obs_canvas_release(canvas);
		return false;
	}
	obs_encoder_set_audio(aenc, obs_get_audio());

	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *dir = config ? config_get_string(config, advanced ? "AdvOut" : "SimpleOutput",
						     advanced ? "RecFilePath" : "FilePath")
				 : nullptr;
	const char *format = config ? config_get_string(config, advanced ? "AdvOut" : "SimpleOutput", "RecFormat2") : nullptr;
	const char *filenameFormat = config ? config_get_string(config, "Output", "FilenameFormatting") : nullptr;
	if (!format || !format[0]) {
		format = "hybrid_mp4";
	}
	std::string pattern = (filenameFormat && filenameFormat[0]) ? filenameFormat : "%CCYY-%MM-%DD %hh-%mm-%ss";
	pattern += " ";
	for (const char *c = canvasName; *c; c++) {
		const char ch = *c;
		if (ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' ||
		    ch == '*') {
			continue;
		}
		pattern.push_back(ch);
	}
	std::string ext = format;
	if (ext == "fragmented_mp4" || ext == "hybrid_mp4") {
		ext = "mp4";
	} else if (ext == "fragmented_mov" || ext == "hybrid_mov") {
		ext = "mov";
	} else if (ext == "hls") {
		ext = "m3u8";
	} else if (ext == "mpegts") {
		ext = "ts";
	}
	char *filename = os_generate_formatted_filename(ext.c_str(), true, pattern.c_str());
	char path[512];
	if (dir && dir[0]) {
		snprintf(path, sizeof(path), "%s/%s", dir, filename ? filename : "recording.mp4");
		size_t cut = 0;
		bool foundSep = false;
		for (size_t i = 0; path[i]; i++) {
			if (path[i] == '/' || path[i] == '\\') {
				cut = i;
				foundSep = true;
			}
		}
		if (foundSep) {
			const char saved = path[cut];
			path[cut] = 0;
			os_mkdirs(path);
			path[cut] = saved;
		}
	} else {
		snprintf(path, sizeof(path), "%s", filename ? filename : "recording.mp4");
	}
	bfree(filename);

	const char *outputId = "ffmpeg_muxer";
	if (strcmp(format, "hybrid_mp4") == 0) {
		outputId = "mp4_output";
	} else if (strcmp(format, "hybrid_mov") == 0) {
		outputId = "mov_output";
	} else if (strcmp(format, "flv") == 0) {
		outputId = "flv_output";
	}
	obs_output_t *output = obs_output_create(outputId, "Aitum Stream Suite Builtin Record", nullptr, nullptr);
	if (!output) {
		obs_encoder_release(venc);
		obs_encoder_release(aenc);
		obs_canvas_release(canvas);
		return false;
	}
	obs_data_t *outputSettings = obs_data_create();
	obs_data_set_string(outputSettings, "path", path);
	if (dir) {
		obs_data_set_string(outputSettings, "directory", dir);
	}
	obs_data_set_string(outputSettings, "format", pattern.c_str());
	obs_data_set_string(outputSettings, "extension", ext.c_str());
	obs_output_update(output, outputSettings);
	obs_data_release(outputSettings);
	obs_output_set_video_encoder(output, venc);
	obs_output_set_audio_encoder(output, aenc, 0);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_canvas_release(canvas);

	if (!obs_output_start(output)) {
		const char *error = obs_output_get_last_error(output);
		blog(LOG_WARNING, "[Aitum Stream Suite] canvas recording failed to start%s%s", error ? ": " : "",
		     error ? error : "");
		obs_output_release(output);
		QMessageBox::warning(this, QString::fromUtf8(obs_frontend_get_locale_string("Output.StartRecordingFailed")),
				     error && error[0] ? QString::fromUtf8(error)
						       : QString::fromUtf8(obs_frontend_get_locale_string("Output.StartFailedGeneric")));
		return false;
	}
	canvasRecordOutput = output;
	mainRecordStartTime = QDateTime::currentDateTime();
	blog(LOG_INFO, "[Aitum Stream Suite] recording canvas '%s' to %s", canvasName, path);
	return true;
}

void OutputDock::ToggleMainRecord()
{
	const bool canvas = RecordUsesCanvas();
	const bool active = canvas ? CanvasRecordActive() : obs_frontend_recording_active();
	if (active) {
		bool warn = config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStoppingRecord");
		if (canvas && warn && isVisible()) {
			auto button = QMessageBox::question(
				this, QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStopRecord.Title")),
				QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStopRecord.Text")),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (button == QMessageBox::No) {
				mainRecordButton->setChecked(true);
				return;
			}
		}
		if (canvas) {
			StopCanvasRecord();
		} else {
			obs_frontend_recording_stop();
		}
		mainRecordButton->setChecked(false);
		mainRecordButton->setText("");
		return;
	}
	if (canvas) {
		const bool started = StartCanvasRecord();
		mainRecordButton->setChecked(started);
		if (!started) {
			mainRecordButton->setText("");
		}
		return;
	}
	obs_frontend_recording_start();
	mainRecordButton->setChecked(true);
}

void OutputDock::UpdateMainRecordingStatus(bool active)
{
	mainRecordButton->setChecked(active);
	if (active) {
		mainRecordStartTime = QDateTime::currentDateTime();
		mainRecordButton->setText("00:00");
	} else {
		mainRecordButton->setText("");
	}
}

obs_data_array_t *OutputDock::GetOutputsArray()
{
	auto outputs2 = obs_data_array_create();
	for (auto it = outputWidgets.begin(); it != outputWidgets.end(); it++) {
		auto data = obs_data_create();
		obs_data_set_string(data, "name", (*it)->objectName().toUtf8().constData());
		obs_data_set_string(data, "type", (*it)->GetOutputType());
		obs_data_set_bool(data, "active", obs_output_active((*it)->GetOutput()));
		obs_data_array_push_back(outputs2, data);
		obs_data_release(data);
	}
	return outputs2;
}

bool OutputDock::AddChapterToOutput(const char *output_name, const char *chapter_name)
{
	auto on = QString::fromUtf8(output_name);
	for (auto it = outputWidgets.begin(); it != outputWidgets.end(); it++) {
		if ((*it)->objectName() == on) {
			return (*it)->AddChapter(chapter_name);
		}
	}
	return false;
}

void OutputDock::StartNextOutput()
{
	if (outputsToStart.empty()) {
		return;
	}
	if (outputStarting >= outputsToStart.size()) {
		outputStarting = 0;
	}
	auto startedAt = outputStarting;
	while (true) {
		auto action = std::next(outputsToStart.begin(), outputStarting);
		if ((*action)([this] { QMetaObject::invokeMethod(this, [this] { StartNextOutput(); }, Qt::QueuedConnection); })) {
			outputsToStart.erase(action);
			break;
		}
		outputStarting++;
		if (outputStarting >= outputsToStart.size()) {
			outputStarting = 0;
		}
		if (outputStarting == startedAt) {
			blog(LOG_WARNING, "[Aitum Stream Suite] went through all outputs and %zu could not be started",
			     outputsToStart.size());
			outputsToStart.clear();
			break;
		}
	}
}

void OutputDock::frontend_event(enum obs_frontend_event event, void *private_data)
{
	auto dock = static_cast<OutputDock *>(private_data);
	if (!dock) {
		return;
	}
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (dock->mainStreamOnStarted) {
			blog(LOG_INFO, "[Aitum Stream Suite] Main stream started, starting next output");
			dock->mainStreamOnStarted();
			dock->mainStreamOnStarted = nullptr;
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (dock->mainStreamOnStarted) {
			blog(LOG_INFO, "[Aitum Stream Suite] Main stream stopped, starting next output");
			dock->mainStreamOnStarted();
			dock->mainStreamOnStarted = nullptr;
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (dock->mainRecordOnStarted) {
			blog(LOG_INFO, "[Aitum Stream Suite] Main recording started, starting next output");
			dock->mainRecordOnStarted();
			dock->mainRecordOnStarted = nullptr;
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (dock->mainRecordOnStarted) {
			blog(LOG_INFO, "[Aitum Stream Suite] Main recording stopped, starting next output");
			dock->mainRecordOnStarted();
			dock->mainRecordOnStarted = nullptr;
		}
		break;
	default:
		break;
	}
}

void OutputDock::StartAll(bool streamOnly, bool recordOnly)
{
	outputsToStart.clear();
	if (mainStreamEnabled && !recordOnly) {
		bool warnBeforeStreamStart =
			config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStartingStream");
		if (warnBeforeStreamStart && isVisible()) {
			auto button = QMessageBox::question(this,
							    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Title")),
							    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Text")),
							    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (button == QMessageBox::No) {
				return;
			}
		}

		outputsToStart.push_back([this](std::function<void()> onStarted) {
			if (obs_frontend_streaming_active()) {
				blog(LOG_INFO, "[Aitum Stream Suite] Skipped starting main stream, already active");
				onStarted();
				return true;
			}
			this->mainStreamOnStarted = onStarted;
			obs_frontend_streaming_start();
			return true;
		});
	}
	if (mainRecordEnabled && !streamOnly) {
		outputsToStart.push_back([this](std::function<void()> onStarted) {
			if (RecordUsesCanvas()) {
				if (!CanvasRecordActive() && !StartCanvasRecord()) {
					blog(LOG_INFO, "[Aitum Stream Suite] Canvas recording did not start");
				}
				onStarted();
				return true;
			}
			if (obs_frontend_recording_active()) {
				blog(LOG_INFO, "[Aitum Stream Suite] Skipped starting main recording, already active");
				onStarted();
				return true;
			}
			this->mainRecordOnStarted = onStarted;
			obs_frontend_recording_start();
			auto output = obs_frontend_get_recording_output();
			if (!output) {
				blog(LOG_INFO, "[Aitum Stream Suite] No main recording output found");
				this->mainRecordOnStarted = nullptr;
				onStarted();
			}
			obs_output_release(output);
			return true;
		});
	}
	for (auto &ow : outputWidgets) {
		if (streamOnly && !ow->IsStream()) {
			continue;
		}
		if (recordOnly && !ow->IsRecord()) {
			continue;
		}
		outputsToStart.push_back([this, ow](std::function<void()> onStarted) { return ow->StartOutput(onStarted); });
	}
	if (outputsToStart.empty()) {
		return;
	}
	blog(LOG_INFO, "[Aitum Stream Suite] Starting %zu outputs", outputsToStart.size());
	StartNextOutput();
}

void OutputDock::StopAll(bool streamOnly, bool recordOnly)
{
	outputsToStart.clear();
	bool warnStream = config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStoppingStream");
	bool warnRecord = config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStoppingRecord");
	if (warnStream && mainStreamEnabled && !recordOnly && obs_frontend_streaming_active() && isVisible()) {
		auto button = QMessageBox::question(this, QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Title")),
						    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Text")),
						    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (button == QMessageBox::No) {
			return;
		}
	} else if (warnRecord && mainRecordEnabled && !streamOnly &&
		   (obs_frontend_recording_active() || CanvasRecordActive()) && isVisible()) {
		auto button = QMessageBox::question(this,
						    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStopRecord.Title")),
						    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStopRecord.Text")),
						    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (button == QMessageBox::No) {
			return;
		}
	}

	if (mainStreamEnabled && !recordOnly && obs_frontend_streaming_active()) {
		obs_frontend_streaming_stop();
	}
	if (mainRecordEnabled && !streamOnly) {
		if (CanvasRecordActive()) {
			StopCanvasRecord();
		}
		if (obs_frontend_recording_active()) {
			obs_frontend_recording_stop();
		}
	}
	for (auto &ow : outputWidgets) {
		if (streamOnly && !ow->IsStream()) {
			continue;
		}
		if (recordOnly && !ow->IsRecord()) {
			continue;
		}
		ow->StopOutput();
	}
}
