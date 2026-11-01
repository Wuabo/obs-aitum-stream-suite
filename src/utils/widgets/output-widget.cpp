#include "output-widget.hpp"
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QStyle>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTime>
#include <src/utils/icon.hpp>
#include <src/docks/chat-dock.hpp>
#include <src/utils/obs-websocket-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <cmath>

extern obs_data_t *current_profile_config;
extern bool isTwitchServer(QString outputServer);
extern ChatDock *chat_dock;

static obs_output_t *ActiveFrontendOutput(bool is_record);

OutputWidget::OutputWidget(obs_data_t *output_data, QWidget *parent) : QFrame(parent), settings(output_data)
{
	obs_data_addref(settings);
	auto nameChars = obs_data_get_string(settings, "name");
	auto name = QString::fromUtf8(nameChars);

	setObjectName(name);

	auto outputLayout = new QHBoxLayout;
	outputLayout->setContentsMargins(8, 4, 8, 4);
	outputLayout->setSpacing(kRowSpacing);

	outputButton = new QPushButton;
	outputButton->setFixedWidth(kActionBtnWidth);
	outputButton->setMinimumHeight(28);
	outputButton->setMaximumHeight(32);
	outputButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	outputButton->setObjectName(QStringLiteral("canvasOutput"));

	auto iconSlot = new QWidget;
	iconSlot->setFixedWidth(outputPlatformIconSize);
	auto iconSlotLayout = new QHBoxLayout(iconSlot);
	iconSlotLayout->setContentsMargins(0, 0, 0, 0);
	iconSlotLayout->setSpacing(0);

	auto output_type = obs_data_get_string(settings, "type");
	auto platformIconLabel = new QLabel;
	platformIconLabel->setFixedSize(outputPlatformIconSize, outputPlatformIconSize);
	platformIconLabel->setScaledContents(true);
	if (strcmp(output_type, "record") == 0) {
		platformIconLabel->setPixmap(
			QIcon(QStringLiteral(":/aitum/media/record.svg")).pixmap(outputPlatformIconSize, outputPlatformIconSize));
	} else if (strcmp(output_type, "ffmpeg") == 0) {
		platformIconLabel->setPixmap(QIcon(QStringLiteral(":/aitum/media/ffmpeg_off.svg"))
						     .pixmap(outputPlatformIconSize, outputPlatformIconSize));
	} else {
		auto endpoint = QString::fromUtf8(obs_data_get_string(settings, "stream_server"));
		platformIconLabel->setPixmap(
			getPlatformIconFromEndpoint(endpoint).pixmap(outputPlatformIconSize, outputPlatformIconSize));
	}
	iconSlotLayout->addWidget(platformIconLabel, 0, Qt::AlignVCenter);
	outputLayout->addWidget(iconSlot, 0, Qt::AlignVCenter);
	outputLayout->addWidget(new QLabel(name), 2, Qt::AlignVCenter);

	if (strcmp(output_type, "record") == 0) {
		outputButton->setIcon(create2StateIcon(":/aitum/media/recording.svg", ":/aitum/media/record.svg"));
		outputButton->setObjectName(QStringLiteral("canvasRecord"));
		outputButton->setToolTip(QString::fromUtf8(obs_module_text("Record")));

		std::string splitName = "AitumStreamSuiteSplit";
		splitName += nameChars;
		std::string splitDescription = obs_frontend_get_locale_string("Basic.Main.SplitFile");
		splitDescription = splitDescription + " " + nameChars;

		splitHotkey = obs_hotkey_register_frontend(
			splitName.c_str(), splitDescription.c_str(),
			[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
				UNUSED_PARAMETER(id);
				UNUSED_PARAMETER(hotkey);
				if (!pressed) {
					return;
				}
				auto this_ = (OutputWidget *)data;
				if (!this_->output) {
					return;
				}
				calldata_t cd = {0};
				proc_handler_t *ph = obs_output_get_proc_handler(this_->output);
				proc_handler_call(ph, "split", &cd);
				calldata_free(&cd);
			},
			this);

		auto split_hotkey = obs_data_get_array(settings, "split_hotkey");
		obs_hotkey_load(splitHotkey, split_hotkey);
		obs_data_array_release(split_hotkey);

		std::string chapterName = "AitumStreamSuiteChapter";
		chapterName += nameChars;
		std::string chapterDescription = obs_frontend_get_locale_string("Basic.Main.AddChapterMarker");
		chapterDescription = chapterDescription + " " + nameChars;

		chapterHotkey = obs_hotkey_register_frontend(
			chapterName.c_str(), chapterDescription.c_str(),
			[](void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
				UNUSED_PARAMETER(id);
				UNUSED_PARAMETER(hotkey);
				if (!pressed) {
					return;
				}
				auto this_ = (OutputWidget *)data;
				if (!this_->output) {
					return;
				}
				calldata_t cd = {0};
				proc_handler_t *ph = obs_output_get_proc_handler(this_->output);
				proc_handler_call(ph, "add_chapter", &cd);
				calldata_free(&cd);
			},
			this);

		auto chapter_hotkey = obs_data_get_array(settings, "chapter_hotkey");
		obs_hotkey_load(chapterHotkey, chapter_hotkey);
		obs_data_array_release(chapter_hotkey);

		std::string pauseName = "AitumStreamSuitePause";
		pauseName += nameChars;
		std::string unpauseName = "AitumStreamSuiteUnpause";
		unpauseName += nameChars;

		std::string pauseDescription = obs_frontend_get_locale_string("Basic.Main.PauseRecording");
		pauseDescription = pauseDescription + " " + nameChars;
		std::string unpauseDescription = obs_frontend_get_locale_string("Basic.Main.UnpauseRecording");
		unpauseDescription = unpauseDescription + " " + nameChars;

		PauseHotkey = obs_hotkey_pair_register_frontend(
			pauseName.c_str(), pauseDescription.c_str(), unpauseName.c_str(), unpauseDescription.c_str(),
			[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
				UNUSED_PARAMETER(id);
				UNUSED_PARAMETER(hotkey);
				if (!pressed) {
					return false;
				}
				auto this_ = (OutputWidget *)data;
				if (this_->output && !obs_output_paused(this_->output)) {
					return obs_output_pause(this_->output, true);
				}
				return false;
			},
			[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
				UNUSED_PARAMETER(id);
				UNUSED_PARAMETER(hotkey);
				if (!pressed) {
					return false;
				}
				auto this_ = (OutputWidget *)data;
				if (this_->output && obs_output_paused(this_->output)) {
					return obs_output_pause(this_->output, false);
				}
				return false;
			},
			this, this);

		auto pause_hotkey = obs_data_get_array(settings, "pause_hotkey");
		auto unpause_hotkey = obs_data_get_array(settings, "unpause_hotkey");
		obs_hotkey_pair_load(PauseHotkey, pause_hotkey, unpause_hotkey);
		obs_data_array_release(pause_hotkey);
		obs_data_array_release(unpause_hotkey);

	} else if (strcmp(output_type, "ffmpeg") == 0) {
		outputButton->setIcon(create2StateIcon(":/aitum/media/ffmpeg_on.svg", ":/aitum/media/ffmpeg_off.svg"));
		outputButton->setObjectName(QStringLiteral("canvasStream"));
		outputButton->setToolTip(QString::fromUtf8(obs_module_text("Ffmpeg")));
	} else {
		outputButton->setIcon(create2StateIcon(":/aitum/media/streaming.svg", ":/aitum/media/stream.svg"));
		outputButton->setObjectName(QStringLiteral("canvasStream"));
		outputButton->setToolTip(QString::fromUtf8(obs_module_text("Stream")));
	}
	outputButton->setCheckable(true);
	outputButton->setChecked(false);
	connect(outputButton, &QPushButton::toggled, [this](bool checked) {
		outputButton->setProperty("class", checked ? QStringLiteral("state-active") : QStringLiteral(""));
		outputButton->style()->unpolish(outputButton);
		outputButton->style()->polish(outputButton);
	});

	connect(outputButton, &QPushButton::clicked, [this]() {
		auto output_type = obs_data_get_string(settings, "type");
		if (outputButton->isChecked()) {
			blog(LOG_INFO, "[Aitum Stream Suite] start %s output clicked '%s'", output_type,
			     obs_data_get_string(settings, "name"));
			if (!StartOutput()) {
				outputButton->setChecked(false);
			}
		} else {
			bool stop = true;

			if (output_type[0] == '\0' || strcmp(output_type, "stream") == 0) {
				bool warnBeforeStreamStop =
					config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStoppingStream");
				if (warnBeforeStreamStop && isVisible()) {
					auto button = QMessageBox::question(
						this, QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Title")),
						QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStop.Text")),
						QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
					if (button == QMessageBox::No) {
						stop = false;
					}
				}
			}
			if (stop) {
				blog(LOG_INFO, "[Aitum Stream Suite] stop %s output clicked '%s'", output_type,
				     obs_data_get_string(settings, "name"));
				if ((output_type[0] == '\0' || strcmp(output_type, "stream") == 0) && chat_dock &&
				    DetectStreamPlatformFromEndpoint(GetStreamServer()) == StreamPlatformKind::YouTube)
					chat_dock->EndYouTubeBroadcastNow();

				obs_queue_task(
					OBS_TASK_GRAPHICS, [](void *param) { obs_output_stop((obs_output_t *)param); }, output,
					false);

			} else {
				outputButton->setChecked(true);
			}
		}
	});

	setFrameShape(QFrame::NoFrame);
	setStyleSheet(QString());

	canvasLabel = new QLabel;
	UpdateCanvas();
	outputLayout->addWidget(canvasLabel, 1, Qt::AlignVCenter);

	timerLabel = new QLabel;
	timerLabel->setMinimumWidth(kTimerMinWidth);
	timerLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	timerLabel->setText(QString());
	outputLayout->addWidget(timerLabel, 0, Qt::AlignVCenter);
	outputLayout->addWidget(outputButton, 0, Qt::AlignVCenter);

	auto *column = new QVBoxLayout(this);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(0);
	auto *row = new QWidget(this);
	row->setLayout(outputLayout);
	column->addWidget(row);
	if (IsStream()) {
		statsLabel = new QLabel(this);
		statsLabel->setVisible(false);
		statsLabel->setContentsMargins(40, 0, 12, 4);
		statsLabel->setStyleSheet(QStringLiteral("color: #bdbdbd; font-size: 11px;"));
		column->addWidget(statsLabel);
	}

	std::string startName = "AitumStreamSuiteStartOutput";
	startName += nameChars;
	std::string startDescription = obs_module_text("StartHotkey");
	startDescription = startDescription + " " + nameChars;
	std::string stopName = "AitumStreamSuiteStopOutput";
	stopName += nameChars;
	std::string stopDescription = obs_module_text("StopHotkey");
	stopDescription = stopDescription + " " + nameChars;

	StartStopHotkey = obs_hotkey_pair_register_frontend(
		startName.c_str(), startDescription.c_str(), stopName.c_str(), stopDescription.c_str(),
		[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return false;
			}
			auto this_ = (OutputWidget *)data;
			if (!this_->output || !obs_output_active(this_->output)) {
				QMetaObject::invokeMethod(this_->outputButton, "click");
				return true;
			}
			return false;
		},
		[](void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed) {
			UNUSED_PARAMETER(id);
			UNUSED_PARAMETER(hotkey);
			if (!pressed) {
				return false;
			}
			auto this_ = (OutputWidget *)data;
			if (this_->output && obs_output_active(this_->output)) {
				QMetaObject::invokeMethod(this_->outputButton, "click");
				return true;
			}
			return false;
		},
		this, this);

	auto start_hotkey = obs_data_get_array(output_data, "start_hotkey");
	auto stop_hotkey = obs_data_get_array(output_data, "stop_hotkey");
	obs_hotkey_pair_load(StartStopHotkey, start_hotkey, stop_hotkey);
	obs_data_array_release(start_hotkey);
	obs_data_array_release(stop_hotkey);

	connect(&activeTimer, &QTimer::timeout, this, [this] {
		if (!timerLabel) {
			return;
		}
		const auto t = QTime::fromMSecsSinceStartOfDay(startTime.msecsTo(QDateTime::currentDateTime()));
		timerLabel->setText(t.toString(t.hour() ? QStringLiteral("hh:mm:ss") : QStringLiteral("mm:ss")));
	});
}

OutputWidget::~OutputWidget()
{
	if (StartStopHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_hotkey_pair_unregister(StartStopHotkey);
	}
	if (PauseHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_hotkey_pair_unregister(PauseHotkey);
	}
	if (splitHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(splitHotkey);
	}
	if (chapterHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(chapterHotkey);
	}
	if (output) {
		signal_handler_t *signal = obs_output_get_signal_handler(output);
		signal_handler_disconnect(signal, "start", output_start, this);
		signal_handler_disconnect(signal, "stop", output_stop, this);
		obs_output_release(output);
		output = nullptr;
	}
	obs_data_release(settings);
}

void OutputWidget::output_start(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto this_ = (OutputWidget *)data;
	if (this_->onStarted) {
		this_->onStarted();
		this_->onStarted = nullptr;
	}
	if (this_->outputButton->isChecked()) {
		return;
	}
	QMetaObject::invokeMethod(this_->outputButton, [this_] { this_->outputButton->setChecked(true); }, Qt::QueuedConnection);
}

extern obs_websocket_vendor vendor;

void OutputWidget::output_stop(void *data, calldata_t *calldata)
{
	auto this_ = (OutputWidget *)data;
	const bool failedStart = this_->onStarted != nullptr;
	if (this_->onStarted) {
		const char *last_error = (const char *)calldata_ptr(calldata, "last_error");
		if (last_error) {
			blog(LOG_WARNING, "[Aitum Stream Suite] failed to start '%s': %s", this_->objectName().toUtf8().constData(),
			     last_error);
		} else {
			blog(LOG_WARNING, "[Aitum Stream Suite] failed to start '%s'", this_->objectName().toUtf8().constData());
		}

		this_->onStarted();
		this_->onStarted = nullptr;
	}
	if (this_->outputButton->isChecked()) {
		QMetaObject::invokeMethod(
			this_->outputButton, [this_] { this_->outputButton->setChecked(false); }, Qt::QueuedConnection);
	}
	if (!failedStart && this_->IsStream() &&
	    DetectStreamPlatformFromEndpoint(this_->GetStreamServer()) == StreamPlatformKind::YouTube) {
		QMetaObject::invokeMethod(
			this_,
			[] {
				if (chat_dock)
					chat_dock->EndYouTubeBroadcastNow();
			},
			Qt::QueuedConnection);
	}

	if (this_->output) {
		const char *error = (const char *)calldata_ptr(calldata, "last_error");
		std::string last_error;
		if (error) {
			last_error = error;
		}
		auto code = calldata_int(calldata, "code");
		QMetaObject::invokeMethod(
			this_->outputButton,
			[this_, last_error, code] {
				obs_output_release(this_->output);
				this_->output = nullptr;
				if (vendor) {
					const auto d = obs_data_create();
					obs_data_set_string(d, "output", obs_output_get_name(this_->output));
					if (!last_error.empty()) {
						obs_data_set_string(d, "last_error", last_error.c_str());
					}
					obs_data_set_int(d, "code", code);

					obs_websocket_vendor_emit_event(vendor, "stop_output", d);
					obs_data_release(d);
				}
			},
			Qt::QueuedConnection);
	}
}

bool OutputWidget::StartOutput(bool automated)
{
	if (!settings) {
		return false;
	}

	auto output_type = obs_data_get_string(settings, "type");
	if (!automated && (output_type[0] == '\0' || strcmp(output_type, "stream") == 0)) {

		bool warnBeforeStreamStart =
			config_get_bool(obs_frontend_get_user_config(), "BasicWindow", "WarnBeforeStartingStream");
		if (warnBeforeStreamStart && isVisible()) {
			auto button = QMessageBox::question(this,
							    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Title")),
							    QString::fromUtf8(obs_frontend_get_locale_string("ConfirmStart.Text")),
							    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (button == QMessageBox::No) {
				return false;
			}
		}
	}

	const char *name = obs_data_get_string(settings, "name");
	if (output) {
		auto service = obs_output_get_service(output);
		if (obs_output_active(output)) {
			obs_output_force_stop(output);
		}
		obs_output_release(output);
		obs_service_release(service);
		output = nullptr;
	}

	if (strcmp(output_type, "ffmpeg") == 0) {

		const char *canvas_name = obs_data_get_string(settings, "canvas");
		auto canvas = (!canvas_name || canvas_name[0] == '\0') ? obs_get_main_canvas()
								       : obs_get_canvas_by_name(canvas_name);
		if (!canvas) {
			return false;
		}

		std::string output_name = "Aitum Stream Suite Output ";
		output_name += name;
		output = obs_output_create("ffmpeg_output", output_name.c_str(), nullptr, nullptr);
		if (!output) {
			blog(LOG_WARNING, "[Aitum Stream Suite] failed to create ffmpeg output '%s'", name);
			return false;
		}

		OBSDataAutoRelease s = obs_data_create();
		OBSDataArrayAutoRelease audio_names = obs_data_array_create();
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			std::string cfg_name = "Track";
			cfg_name += std::to_string((int)i + 1);
			cfg_name += "Name";

			const char *audioName = config_get_string(obs_frontend_get_profile_config(), "AdvOut", cfg_name.c_str());
			OBSDataAutoRelease item = obs_data_create();
			if (audioName && audioName[0] != '\0') {
				obs_data_set_string(item, "name", audioName);
			}
			obs_data_array_push_back(audio_names, item);
		}
		obs_data_set_array(s, "audio_names", audio_names);
		if (obs_data_get_bool(settings, "save_file")) {
			std::string path = obs_data_get_string(settings, "directory");
			char lastChar = path.empty() ? '\0' : path.back();
			if (lastChar != '/' && lastChar != '\\') {
				path += "/";
			}
			auto extension = obs_data_get_string(settings, "extension");
			bool spaces = obs_data_get_bool(settings, "allow_spaces");
			auto format = obs_data_get_string(settings, "format");
			obs_data_set_string(s, "directory", path.c_str());
			obs_data_set_string(s, "path", path.c_str());
			obs_data_set_string(s, "format", format);
			obs_data_set_string(s, "extension", extension);
			obs_data_set_bool(s, "allow_spaces", spaces);
			obs_data_set_bool(s, "allow_overwrite", obs_data_get_bool(settings, "allow_overwrite"));
			obs_data_set_bool(s, "split_file", true);
			obs_data_set_int(s, "max_time_sec", obs_data_get_int(settings, "max_time_sec"));
			obs_data_set_int(s, "max_size_mb", obs_data_get_int(settings, "max_size_mb"));

			auto filename = os_generate_formatted_filename(extension, spaces, format);
			path += filename;
			obs_data_set_string(s, "url", path.c_str());
			bfree(filename);
		} else {
			obs_data_set_string(s, "url", obs_data_get_string(settings, "url"));
		}

		obs_data_set_string(s, "format_name", obs_data_get_string(settings, "format_name"));
		obs_data_set_string(s, "format_mime_type", obs_data_get_string(settings, "format_mime_type"));
		obs_data_set_string(s, "muxer_settings", obs_data_get_string(settings, "muxer_settings"));
		obs_data_set_int(s, "gop_size", obs_data_get_int(settings, "gop_size"));
		obs_data_set_int(s, "video_bitrate", obs_data_get_int(settings, "video_bitrate"));
		obs_data_set_string(s, "video_encoder", obs_data_get_string(settings, "video_encoder"));
		obs_data_set_int(s, "video_encoder_id", obs_data_get_int(settings, "video_encoder_id"));
		obs_data_set_string(s, "video_settings", obs_data_get_string(settings, "video_settings"));
		obs_data_set_int(s, "audio_bitrate", obs_data_get_int(settings, "audio_bitrate"));
		obs_data_set_string(s, "audio_encoder", obs_data_get_string(settings, "audio_encoder"));
		obs_data_set_int(s, "audio_encoder_id", obs_data_get_int(settings, "audio_encoder_id"));
		obs_data_set_string(s, "audio_settings", obs_data_get_string(settings, "audio_settings"));
		if (obs_data_get_bool(settings, "rescale")) {
			obs_data_set_int(s, "scale_width", obs_data_get_int(settings, "scale_width"));
			obs_data_set_int(s, "scale_height", obs_data_get_int(settings, "scale_height"));
		}

		size_t aMixes = obs_data_get_int(settings, "audio_mixes");
		obs_output_set_mixers(output, aMixes);
		obs_output_set_media(output, obs_canvas_get_video(canvas), obs_get_audio());
		obs_canvas_release(canvas);

		obs_output_update(output, s);

		signal_handler_t *signal = obs_output_get_signal_handler(output);
		signal_handler_disconnect(signal, "start", output_start, this);
		signal_handler_disconnect(signal, "stop", output_stop, this);
		signal_handler_connect(signal, "start", output_start, this);
		signal_handler_connect(signal, "stop", output_stop, this);

		if (!obs_output_start(output)) {
			obs_output_release(output);
			output = nullptr;
			return false;
		}

		if (vendor) {
			const auto d = obs_data_create();
			obs_data_set_string(d, "output", name);
			obs_websocket_vendor_emit_event(vendor, "start_output", d);
			obs_data_release(d);
		}
		return true;
	}

	std::vector<obs_encoder_t *> vencs;
	std::vector<obs_encoder_t *> aencs;

	bool is_record = (strcmp(output_type, "record") == 0);
	auto advanced = obs_data_get_bool(settings, "advanced");
	auto video_encoders = obs_data_get_array(settings, "video_encoders");
	auto video_encoders_count = obs_data_array_count(video_encoders);
	if (video_encoders_count == 0) {
		auto venc = GetVideoEncoder(settings, advanced, is_record, name, automated);
		if (venc) {
			vencs.push_back(venc);
		}
	} else {
		for (size_t i = 0; i < video_encoders_count; i++) {
			auto item = obs_data_array_item(video_encoders, i);
			if (!item) {
				continue;
			}
			auto venc = GetVideoEncoder(item, advanced, is_record, name, automated);
			if (venc) {
				vencs.push_back(venc);
			}
			obs_data_release(item);
		}
	}
	obs_data_array_release(video_encoders);

	if (advanced) {
		auto aenc_name = obs_data_get_string(settings, "audio_encoder");
		if (!aenc_name || aenc_name[0] == '\0') {
			obs_output_t *main_output = ActiveFrontendOutput(is_record);
			if (!main_output) {
				blog(LOG_WARNING, "[Aitum Stream Suite] failed to start output '%s' because main was not started",
				     name);
				if (!automated) {
					QMessageBox::warning(this, QString::fromUtf8(obs_module_text("MainOutputNotActive")),
							     QString::fromUtf8(obs_module_text("MainOutputNotActive")));
				}
				return false;
			}
			auto aenc = obs_output_get_audio_encoder(main_output, 0);
			if (aenc)
				obs_encoder_get_ref(aenc);
			obs_output_release(main_output);
			if (!aenc) {
				blog(LOG_WARNING,
				     "[Aitum Stream Suite] failed to start output '%s' because audio encoder was not found", name);
				if (!automated) {
					QMessageBox::warning(this,
							     QString::fromUtf8(obs_module_text("MainOutputEncoderIndexNotFound")),
							     QString::fromUtf8(obs_module_text("MainOutputEncoderIndexNotFound")));
				}
				return false;
			}
			aencs.push_back(aenc);
		} else {
			auto streamServer = QString::fromUtf8(obs_data_get_string(settings, "stream_server"));
			if (is_record || streamServer.startsWith("srt://", Qt::CaseInsensitive) ||
			    streamServer.startsWith("rist://", Qt::CaseInsensitive)) {
				auto tracks = obs_data_get_int(settings, "audio_tracks");
				if (!tracks) {
					tracks = 1;
				}

				for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
					if ((tracks & (1ll << i)) == 0) {
						continue;
					}
					obs_data_t *s = nullptr;
					auto aes = obs_data_get_obj(settings, "audio_encoder_settings");
					if (aes) {
						s = obs_data_create();
						obs_data_apply(s, aes);
						obs_data_release(aes);
					}

					std::string audio_encoder_name = "Aitum Stream Suite Audio ";
					audio_encoder_name += name;
					audio_encoder_name += " ";
					audio_encoder_name += std::to_string(i + 1);
					auto aenc = obs_audio_encoder_create(aenc_name, audio_encoder_name.c_str(), s, i, nullptr);
					obs_data_release(s);
					obs_encoder_set_audio(aenc, obs_get_audio());
					aencs.push_back(aenc);
				}
			} else {

				obs_data_t *s = nullptr;
				auto aes = obs_data_get_obj(settings, "audio_encoder_settings");
				if (aes) {
					s = obs_data_create();
					obs_data_apply(s, aes);
				}

				std::string audio_encoder_name = "Aitum Stream Suite Audio ";
				audio_encoder_name += name;
				auto audio_track = obs_data_get_int(settings, "audio_track");
				auto aenc =
					obs_audio_encoder_create(aenc_name, audio_encoder_name.c_str(), s, audio_track, nullptr);
				obs_data_release(s);
				obs_encoder_set_audio(aenc, obs_get_audio());
				aencs.push_back(aenc);

				obs_data_set_default_int(settings, "vod_track", -1);
				auto vod_track = obs_data_get_int(settings, "vod_track");
				if (vod_track >= 0 && vod_track != audio_track &&
				    (config_get_bool(obs_frontend_get_user_config(), "General", "EnableCustomServerVodTrack") ||
				     isTwitchServer(streamServer))) {
					if (aes) {
						s = obs_data_create();
						obs_data_apply(s, aes);
					}
					audio_encoder_name = "Aitum Stream Suite VOD Audio ";
					audio_encoder_name += name;
					aenc = obs_audio_encoder_create(aenc_name, audio_encoder_name.c_str(), s, vod_track,
									nullptr);
					obs_data_release(s);
					obs_encoder_set_audio(aenc, obs_get_audio());
					aencs.push_back(aenc);
				}
				obs_data_release(aes);
			}
		}
	} else {
		if (aencs.empty()) {
			obs_output_t *main_output = nullptr;
			if (is_record) {
				main_output = obs_frontend_get_replay_buffer_output();
				if (main_output && !obs_output_active(main_output)) {
					obs_output_release(main_output);
					main_output = nullptr;
				}
				if (!main_output) {
					main_output = obs_frontend_get_recording_output();
				}
				if (main_output && !obs_output_active(main_output)) {
					obs_output_release(main_output);
					main_output = nullptr;
				}
				if (!main_output) {
					main_output = obs_frontend_get_streaming_output();
				}

				for (size_t idx = 0; idx < MAX_OUTPUT_AUDIO_ENCODERS; idx++) {
					auto aenc = main_output ? obs_output_get_audio_encoder(main_output, idx) : nullptr;
					if (aenc) {
						obs_encoder_get_ref(aenc);
						aencs.push_back(aenc);
					}
				}
			} else {
				main_output = obs_frontend_get_streaming_output();
				auto aenc = main_output ? obs_output_get_audio_encoder(main_output, 0) : nullptr;
				if (aenc) {
					obs_encoder_get_ref(aenc);
					aencs.push_back(aenc);
				}
			}

			obs_output_release(main_output);
		}
		if (!vencs.empty() && aencs.empty()) {
			if (is_record) {
				for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
					std::string audio_encoder_name = "Aitum Stream Suite Audio ";
					audio_encoder_name += name;
					audio_encoder_name += " ";
					audio_encoder_name += std::to_string(i);
					auto aenc = CreateObsDefaultAudioEncoder(audio_encoder_name.c_str(), i, true);
					if (aenc)
						aencs.push_back(aenc);
				}
			} else {
				std::string audio_encoder_name = "Aitum Stream Suite Audio ";
				audio_encoder_name += name;
				auto aenc = CreateObsDefaultAudioEncoder(audio_encoder_name.c_str(), 0, false);
				if (aenc)
					aencs.push_back(aenc);
			}
		}
	}

	if (vencs.empty()) {
		blog(LOG_WARNING, "[Aitum Stream Suite] Failed to start '%s', no video encoder found", name);
		if (!automated) {
			QMessageBox::warning(this, QString::fromUtf8(obs_module_text("NoVideoEncoder")),
					     QString::fromUtf8(obs_module_text("NoVideoEncoder")));
		}
		for (size_t i = 0; i < aencs.size(); i++) {
			obs_encoder_release(aencs[i]);
		}
		return false;
	}
	if (aencs.empty()) {
		blog(LOG_WARNING, "[Aitum Stream Suite] Failed to start '%s', no audio encoder found", name);
		if (!automated) {
			QMessageBox::warning(this, QString::fromUtf8(obs_module_text("NoAudioEncoder")),
					     QString::fromUtf8(obs_module_text("NoAudioEncoder")));
		}
		for (size_t i = 0; i < vencs.size(); i++) {
			obs_encoder_release(vencs[i]);
		}
		return false;
	}

	if (is_record) {
		config_t *profile = obs_frontend_get_profile_config();
		const char *mode = profile ? config_get_string(profile, "Output", "Mode") : nullptr;
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;

		std::string filenameFormat;
		if (!pendingFilenameOverride.isEmpty()) {
			filenameFormat = pendingFilenameOverride.toStdString();
		} else {
			const char *obsFormat =
				profile ? config_get_string(profile, "Output", "FilenameFormatting") : nullptr;
			filenameFormat = (obsFormat && obsFormat[0]) ? obsFormat : "%CCYY-%MM-%DD %hh-%mm-%ss";
			filenameFormat += " ";
			QString safeName = QString::fromUtf8(name);
#ifdef __APPLE__
			safeName.replace(QRegularExpression("[:]"), "");
#elif defined(_WIN32)
			safeName.replace(QRegularExpression("[<>:\"\\|\\?\\*]"), "");
#endif
			filenameFormat += safeName.toStdString();
		}

		const char *format =
			profile ? config_get_string(profile, advanced ? "AdvOut" : "SimpleOutput", "RecFormat2") : nullptr;
		if (!format || !format[0]) {
			format = "hybrid_mp4";
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

		char *filename = os_generate_formatted_filename(ext.c_str(), true, filenameFormat.c_str());

		auto dir = obs_data_get_string(settings, "path");
		char path[512];
		if (dir[0] != '\0') {
			snprintf(path, 512, "%s/%s", dir, filename);
			ensure_directory(path);
		} else {
			snprintf(path, 512, "%s", filename);
		}
		bfree(filename);

		std::string output_name = "Aitum Stream Suite Output ";
		output_name += name;
		const char *output_id = "ffmpeg_muxer";
		if (strcmp(format, "hybrid_mp4") == 0) {
			output_id = "mp4_output";
		} else if (strcmp(format, "hybrid_mov") == 0) {
			output_id = "mov_output";
		} else if (strcmp(format, "flv") == 0) {
			output_id = "flv_output";
		}

		output = obs_output_create(output_id, output_name.c_str(), nullptr, nullptr);

		auto ps = obs_data_create();
		obs_data_set_string(ps, "path", path);
		obs_data_set_string(ps, "directory", dir);
		obs_data_set_string(ps, "format", filenameFormat.c_str());
		obs_data_set_string(ps, "extension", ext.c_str());

		const bool splitFile = advanced ? config_get_bool(profile, "AdvOut", "RecSplitFile") : false;
		obs_data_set_bool(ps, "split_file", splitFile);
		long long max_size = 0;
		long long max_sec = 0;
		if (splitFile && profile) {
			const char *splitType = config_get_string(profile, "AdvOut", "RecSplitFileType");
			if (splitType && strcmp(splitType, "Size") == 0) {
				max_size = config_get_int(profile, "AdvOut", "RecSplitFileSize");
			} else if (splitType && strcmp(splitType, "Time") == 0) {
				max_sec = config_get_int(profile, "AdvOut", "RecSplitFileTime") * 60;
			}
		}
		obs_data_set_int(ps, "max_size_mb", max_size > 0 ? max_size : 0);
		obs_data_set_int(ps, "max_time_sec", max_sec > 0 ? max_sec : 0);
		obs_output_update(output, ps);
		obs_data_release(ps);
	} else {

		auto server = obs_data_get_string(settings, "stream_server");
		if (!server || !strlen(server)) {
			server = obs_data_get_string(settings, "server");
			if (server && strlen(server)) {
				obs_data_set_string(settings, "stream_server", server);
			}
		}
		bool whip = strstr(server, "whip") != nullptr;
		auto s = obs_data_create();
		obs_data_set_string(s, "server", server);
		auto key = obs_data_get_string(settings, "stream_key");
		if (!key || !strlen(key)) {
			key = obs_data_get_string(settings, "key");
			if (key && strlen(key)) {
				obs_data_set_string(settings, "stream_key", key);
			}
		}
		if (whip) {
			obs_data_set_string(s, "bearer_token", key);
		} else {
			obs_data_set_string(s, "key", key);
		}
		std::string service_name = "aitum_stream_suite_service_";
		service_name += name;
		auto service = obs_service_create(whip ? "whip_custom" : "rtmp_custom", service_name.c_str(), s, nullptr);
		obs_data_release(s);

		const char *type = obs_service_get_preferred_output_type(service);
		if (!type) {
			type = "rtmp_output";
			if (strncmp(server, "ftl", 3) == 0) {
				type = "ftl_output";
			} else if (strncmp(server, "rtmp", 4) != 0) {
				type = "ffmpeg_mpegts_muxer";
			}
		}
		std::string output_name = "Aitum Stream Suite Output ";
		output_name += name;
		output = obs_output_create(type, output_name.c_str(), nullptr, nullptr);
		obs_output_set_service(output, service);

		bool customDelay = obs_data_get_bool(settings, "custom_delay");
		if (customDelay) {
			auto delaySec = (uint32_t)obs_data_get_int(settings, "delay_sec");
			bool preserveDelay = obs_data_get_bool(settings, "delay_preserve");
			obs_output_set_delay(output, delaySec, preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);
		}

		config_t *config = obs_frontend_get_profile_config();
		if (config) {
			obs_data_t *output_settings = obs_data_create();
			obs_data_set_string(output_settings, "bind_ip", config_get_string(config, "Output", "BindIP"));
			obs_data_set_string(output_settings, "ip_family", config_get_string(config, "Output", "IPFamily"));
			obs_output_update(output, output_settings);
			obs_data_release(output_settings);

			if (!customDelay) {
				bool useDelay = config_get_bool(config, "Output", "DelayEnable");
				auto delaySec = (uint32_t)config_get_int(config, "Output", "DelaySec");
				bool preserveDelay = config_get_bool(config, "Output", "DelayPreserve");
				obs_output_set_delay(output, useDelay ? delaySec : 0,
						     preserveDelay ? OBS_OUTPUT_DELAY_PRESERVE : 0);
			}
		}
	}

	signal_handler_t *signal = obs_output_get_signal_handler(output);
	signal_handler_disconnect(signal, "start", output_start, this);
	signal_handler_disconnect(signal, "stop", output_stop, this);
	signal_handler_connect(signal, "start", output_start, this);
	signal_handler_connect(signal, "stop", output_stop, this);

	for (size_t i = 0; i < vencs.size(); i++) {
		obs_output_set_video_encoder2(output, vencs[i], i);
		obs_encoder_release(vencs[i]);
	}

	for (size_t i = 0; i < aencs.size(); i++) {
		obs_output_set_audio_encoder(output, aencs[i], i);
		obs_encoder_release(aencs[i]);
	}

	if (!obs_output_start(output)) {
		obs_output_release(output);
		output = nullptr;
		return false;
	}
	if (vendor) {
		const auto d = obs_data_create();
		obs_data_set_string(d, "output", name);
		obs_websocket_vendor_emit_event(vendor, "start_output", d);
		obs_data_release(d);
	}
	return true;
}

static obs_output_t *TakeIfActive(obs_output_t *output)
{
	if (!output)
		return nullptr;
	if (!obs_output_active(output)) {
		obs_output_release(output);
		return nullptr;
	}
	return output;
}

/* Replay buffer, then recording, then the stream output. Caller owns the ref. */
static obs_output_t *ActiveFrontendOutput(bool is_record)
{
	if (is_record) {
		if (obs_output_t *output = TakeIfActive(obs_frontend_get_replay_buffer_output()))
			return output;
		if (obs_output_t *output = TakeIfActive(obs_frontend_get_recording_output()))
			return output;
	}
	return TakeIfActive(obs_frontend_get_streaming_output());
}

static obs_data_t *ReadProfileJson(const char *filename)
{
	char *profile = obs_frontend_get_current_profile_path();
	if (!profile || !filename || !filename[0]) {
		bfree(profile);
		return nullptr;
	}
	std::string path = profile;
	bfree(profile);
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path.push_back('/');
	path += filename;
	char *json = os_quick_read_utf8_file(path.c_str());
	if (!json)
		return nullptr;
	obs_data_t *data = obs_data_create_from_json(json);
	bfree(json);
	return data;
}

static const char *SimpleVideoEncoderId(const char *simple)
{
	if (!simple || !simple[0])
		return "obs_x264";
	if (strcmp(simple, "x264") == 0 || strcmp(simple, "x264_lowcpu") == 0)
		return "obs_x264";
	if (strcmp(simple, "qsv") == 0)
		return "obs_qsv11_v2";
	if (strcmp(simple, "qsv_av1") == 0)
		return "obs_qsv11_av1";
	if (strcmp(simple, "amd") == 0)
		return "h264_texture_amf";
	if (strcmp(simple, "amd_hevc") == 0)
		return "h265_texture_amf";
	if (strcmp(simple, "amd_av1") == 0)
		return "av1_texture_amf";
	if (strcmp(simple, "nvenc") == 0)
		return "obs_nvenc_h264_tex";
	if (strcmp(simple, "nvenc_hevc") == 0)
		return "obs_nvenc_hevc_tex";
	if (strcmp(simple, "nvenc_av1") == 0)
		return "obs_nvenc_av1_tex";
	if (strcmp(simple, "apple_h264") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.avc";
	if (strcmp(simple, "apple_hevc") == 0)
		return "com.apple.videotoolbox.videoencoder.ave.hevc";
	return "obs_x264";
}

static const char *SimplePresetKey(const char *simple)
{
	if (!simple)
		return "Preset";
	if (strcmp(simple, "qsv") == 0 || strcmp(simple, "qsv_av1") == 0)
		return "QSVPreset";
	if (strcmp(simple, "amd") == 0 || strcmp(simple, "amd_hevc") == 0)
		return "AMDPreset";
	if (strcmp(simple, "amd_av1") == 0)
		return "AMDAV1Preset";
	if (strcmp(simple, "nvenc") == 0 || strcmp(simple, "nvenc_hevc") == 0 || strcmp(simple, "nvenc_av1") == 0)
		return "NVENCPreset2";
	return "Preset";
}

static int SimpleRecordingCrf(config_t *config, video_t *video, bool lowCpu)
{
	const char *quality = config ? config_get_string(config, "SimpleOutput", "RecQuality") : nullptr;
	int crf = (quality && strcmp(quality, "HQ") == 0) ? 16 : 23;
	if (lowCpu)
		crf -= 2;
	uint32_t cx = video ? video_output_get_width(video) : 0;
	uint32_t cy = video ? video_output_get_height(video) : 0;
	if ((cx == 0 || cy == 0) && config) {
		cx = (uint32_t)config_get_uint(config, "Video", "OutputCX");
		cy = (uint32_t)config_get_uint(config, "Video", "OutputCY");
	}
	const double cross = std::sqrt(double(cx) * double(cx) + double(cy) * double(cy));
	const double capped = cross < 2000.0 ? cross : 2000.0;
	const double reduction = (1.0 - capped / 2000.0) * 10.0;
	return crf - (int)reduction;
}

static bool EncoderSupportsRateControl(obs_encoder_t *encoder, const char *mode)
{
	if (!encoder || !mode)
		return false;
	obs_properties_t *props = obs_encoder_properties(encoder);
	obs_property_t *property = props ? obs_properties_get(props, "rate_control") : nullptr;
	bool found = false;
	if (property && obs_property_get_type(property) == OBS_PROPERTY_LIST) {
		const size_t count = obs_property_list_item_count(property);
		for (size_t i = 0; i < count; i++) {
			const char *value = obs_property_list_item_string(property, i);
			if (value && strcmp(value, mode) == 0) {
				found = true;
				break;
			}
		}
	}
	obs_properties_destroy(props);
	return found;
}

static void ApplyCanvasVideoFormat(obs_encoder_t *encoder, video_t *video)
{
	if (!encoder || !video)
		return;
	switch (video_output_get_format(video)) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
		break;
	default:
		obs_encoder_set_preferred_video_format(encoder, VIDEO_FORMAT_NV12);
		break;
	}
}

obs_encoder_t *OutputWidget::CreateObsDefaultVideoEncoder(const char *name, video_t *video, bool recording)
{
	if (!name || !name[0] || !video)
		return nullptr;

	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;

	const char *encoderId = "obs_x264";
	obs_data_t *settings = obs_data_create();
	bool simpleQsvRecording = false;
	int simpleCrf = 23;

	if (advanced && config) {
		const char *recordId = recording ? config_get_string(config, "AdvOut", "RecEncoder") : nullptr;
		const bool useStream = !recording || !recordId || !recordId[0] || strcmp(recordId, "none") == 0;
		const char *configured = useStream ? config_get_string(config, "AdvOut", "Encoder") : recordId;
		if (configured && configured[0])
			encoderId = configured;
		obs_data_t *file = ReadProfileJson(useStream ? "streamEncoder.json" : "recordEncoder.json");
		if (file) {
			obs_data_apply(settings, file);
			obs_data_release(file);
		}
	} else if (config) {
		const char *quality = recording ? config_get_string(config, "SimpleOutput", "RecQuality") : nullptr;
		const bool useStream = !recording || !quality || !quality[0] || strcmp(quality, "Stream") == 0 ||
				       strcmp(quality, "Lossless") == 0;
		const char *simple = config_get_string(config, "SimpleOutput", useStream ? "StreamEncoder" : "RecEncoder");
		encoderId = SimpleVideoEncoderId(simple);
		if (useStream) {
			int bitrate = (int)config_get_uint(config, "SimpleOutput", "VBitrate");
			if (bitrate <= 0)
				bitrate = 2500;
			obs_data_set_string(settings, "rate_control", "CBR");
			obs_data_set_int(settings, "bitrate", bitrate);
			const char *presetKey = SimplePresetKey(simple);
			const char *preset = config_get_string(config, "SimpleOutput", presetKey);
			if (preset && preset[0]) {
				if (strncmp(encoderId, "ffmpeg_", 7) == 0 && strcmp(presetKey, "NVENCPreset2") == 0)
					obs_data_set_string(settings, "preset2", preset);
				else
					obs_data_set_string(settings, "preset", preset);
			}
			if (config_get_bool(config, "SimpleOutput", "UseAdvanced")) {
				const char *custom = config_get_string(config, "SimpleOutput", "x264Settings");
				if (custom && custom[0])
					obs_data_set_string(settings, "x264opts", custom);
			}
		} else {
			const bool lowCpu = simple && strcmp(simple, "x264_lowcpu") == 0;
			simpleCrf = SimpleRecordingCrf(config, video, lowCpu);
			simpleQsvRecording = simple && strcmp(simple, "qsv") == 0;
			if (!simple || strcmp(simple, "x264") == 0 || lowCpu) {
				obs_data_set_int(settings, "crf", simpleCrf);
				obs_data_set_bool(settings, "use_bufsize", true);
				obs_data_set_string(settings, "rate_control", "CRF");
				obs_data_set_string(settings, "profile", "high");
				obs_data_set_string(settings, "preset", lowCpu ? "ultrafast" : "veryfast");
			} else if (strcmp(simple, "nvenc_hevc") == 0 || strcmp(simple, "nvenc_av1") == 0) {
				obs_data_set_string(settings, "rate_control", "CQP");
				obs_data_set_string(settings, "profile", "main");
				obs_data_set_int(settings, "cqp", simpleCrf);
			} else if (strcmp(simple, "amd") == 0 || strcmp(simple, "amd_hevc") == 0 ||
				   strcmp(simple, "amd_av1") == 0) {
				obs_data_set_string(settings, "rate_control", "CQP");
				obs_data_set_string(settings, "profile", "high");
				obs_data_set_string(settings, "preset", "quality");
				obs_data_set_int(settings, "cqp", simpleCrf);
			} else {
				obs_data_set_string(settings, "profile", "high");
				obs_data_set_string(settings, "rate_control", "CQP");
				obs_data_set_int(settings, "cqp", simpleCrf);
			}
		}
	} else {
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", 2500);
	}

	if (!EncoderAvailable(encoderId)) {
		if (strcmp(encoderId, "obs_nvenc_h264_tex") == 0 && EncoderAvailable("ffmpeg_nvenc"))
			encoderId = "ffmpeg_nvenc";
		else if (strcmp(encoderId, "obs_nvenc_hevc_tex") == 0 && EncoderAvailable("ffmpeg_hevc_nvenc"))
			encoderId = "ffmpeg_hevc_nvenc";
		else
			encoderId = "obs_x264";
	}

	obs_encoder_t *encoder = obs_video_encoder_create(encoderId, name, settings, nullptr);
	if (!encoder) {
		const int bitrate = (int)obs_data_get_int(settings, "bitrate");
		obs_data_t *fallback = obs_data_create();
		obs_data_set_string(fallback, "rate_control", "CBR");
		obs_data_set_int(fallback, "bitrate", bitrate > 0 ? bitrate : 2500);
		encoder = obs_video_encoder_create("obs_x264", name, fallback, nullptr);
		obs_data_release(fallback);
		encoderId = "obs_x264";
	}
	obs_data_release(settings);
	if (!encoder)
		return nullptr;

	obs_encoder_set_video(encoder, video);
	ApplyCanvasVideoFormat(encoder, video);
	if (simpleQsvRecording && EncoderSupportsRateControl(encoder, "ICQ")) {
		obs_data_t *icq = obs_data_create();
		obs_data_set_string(icq, "rate_control", "ICQ");
		obs_data_set_int(icq, "icq_quality", simpleCrf);
		obs_encoder_update(encoder, icq);
		obs_data_release(icq);
	}
	blog(LOG_INFO, "[Aitum Stream Suite] video encoder %s from OBS settings (%s)", encoderId,
	     recording ? "recording" : "stream");
	return encoder;
}

obs_encoder_t *OutputWidget::CreateObsDefaultAudioEncoder(const char *name, size_t mix, bool recording)
{
	if (!name || !name[0])
		return nullptr;

	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *encoderId = "ffmpeg_aac";
	int bitrate = 160;

	if (advanced && config) {
		const char *streamId = config_get_string(config, "AdvOut", "AudioEncoder");
		const char *recordId = config_get_string(config, "AdvOut", "RecAudioEncoder");
		const char *chosen = streamId;
		if (recording && recordId && recordId[0] && strcmp(recordId, "none") != 0)
			chosen = recordId;
		if (chosen && chosen[0] && EncoderAvailable(chosen))
			encoderId = chosen;
		static const char *tracks[] = {"Track1Bitrate", "Track2Bitrate", "Track3Bitrate",
						"Track4Bitrate", "Track5Bitrate", "Track6Bitrate"};
		const size_t index = mix < 6 ? mix : 0;
		bitrate = (int)config_get_uint(config, "AdvOut", tracks[index]);
	} else if (config) {
		const char *which = config_get_string(config, "SimpleOutput", recording ? "RecAudioEncoder" : "StreamAudioEncoder");
		if (which && strcmp(which, "opus") == 0 && EncoderAvailable("ffmpeg_opus"))
			encoderId = "ffmpeg_opus";
		else if (EncoderAvailable("CoreAudio_AAC"))
			encoderId = "CoreAudio_AAC";
		else if (EncoderAvailable("libfdk_aac"))
			encoderId = "libfdk_aac";
		if (recording) {
			const char *quality = config_get_string(config, "SimpleOutput", "RecQuality");
			bitrate = (quality && strcmp(quality, "Stream") == 0)
					  ? (int)config_get_uint(config, "SimpleOutput", "ABitrate")
					  : 192;
		} else {
			bitrate = (int)config_get_uint(config, "SimpleOutput", "ABitrate");
		}
	}
	if (bitrate <= 0)
		bitrate = 160;

	auto create = [&](const char *id) {
		obs_data_t *audioSettings = obs_data_create();
		obs_data_set_int(audioSettings, "bitrate", bitrate);
		obs_encoder_t *created = obs_audio_encoder_create(id, name, audioSettings, mix, nullptr);
		obs_data_release(audioSettings);
		return created;
	};

	obs_encoder_t *encoder = create(encoderId);
	if (!encoder && strcmp(encoderId, "ffmpeg_aac") != 0)
		encoder = create("ffmpeg_aac");
	if (encoder)
		obs_encoder_set_audio(encoder, obs_get_audio());
	return encoder;
}

obs_encoder_t *OutputWidget::GetVideoEncoder(obs_data_t *settings, bool advanced, bool is_record, const char *output_name,
					     bool automated)
{
	const char *canvas_name = obs_data_get_string(settings, "canvas");
	auto canvas = (!canvas_name || canvas_name[0] == '\0') ? obs_get_main_canvas() : obs_get_canvas_by_name(canvas_name);
	if (!canvas) {
		blog(LOG_WARNING, "[Aitum Stream Suite] canvas '%s' not found", canvas_name);
		if (!automated) {
			QMessageBox::warning(this, QString::fromUtf8(obs_module_text("CanvasNotFound")),
					     QString::fromUtf8(obs_module_text("CanvasNotFound")));
		}
		return nullptr;
	}
	auto main_canvas = obs_get_main_canvas();
	bool main = canvas == main_canvas;
	obs_canvas_release(main_canvas);
	obs_canvas_release(canvas);

	obs_encoder_t *venc = nullptr;
	if (advanced) {
		auto output_video_encoder_name = obs_data_get_string(settings, "output_video_encoder");
		if (output_video_encoder_name && output_video_encoder_name[0] != '\0' &&
		    (!main || strcmp(output_video_encoder_name, "MainEncoder") != 0)) {
			std::string other_output_name = "Aitum Stream Suite Output ";
			other_output_name += output_video_encoder_name;
			auto other_output = obs_get_output_by_name(other_output_name.c_str());
			if (other_output) {
				auto vei = (int)obs_data_get_int(settings, "video_encoder_index");
				venc = obs_output_get_video_encoder2(other_output, vei);
				if (!venc) {
					venc = obs_output_get_video_encoder(other_output);
				}
				obs_output_release(other_output);
			}
			if (!venc) {
				blog(LOG_WARNING, "[Aitum Stream Suite] failed to start output '%s' because '%s' was not started",
				     output_name, output_video_encoder_name);
				if (!automated) {
					QMessageBox::warning(this, QString::fromUtf8(obs_module_text("OtherOutputNotActive")),
							     QString::fromUtf8(obs_module_text("OtherOutputNotActive")));
				}
				return nullptr;
			} else {
				obs_encoder_get_ref(venc);
			}
		} else {
			auto venc_name = obs_data_get_string(settings, "video_encoder");
			if (!venc_name || venc_name[0] == '\0') {
				if (main) {
					obs_output_t *main_output = ActiveFrontendOutput(is_record);
					if (!main_output) {
						blog(LOG_WARNING,
						     "[Aitum Stream Suite] failed to start output '%s' because main was not started",
						     output_name);
						if (!automated) {
							QMessageBox::warning(
								this, QString::fromUtf8(obs_module_text("MainOutputNotActive")),
								QString::fromUtf8(obs_module_text("MainOutputNotActive")));
						}
						return nullptr;
					}
					auto vei = (int)obs_data_get_int(settings, "video_encoder_index");
					venc = obs_output_get_video_encoder2(main_output, vei);
					if (venc)
						obs_encoder_get_ref(venc);
					obs_output_release(main_output);
					if (!venc) {
						blog(LOG_WARNING,
						     "[Aitum Stream Suite] failed to start output '%s' because encoder index %d was not found",
						     output_name, vei);
						if (!automated) {
							QMessageBox::warning(this,
									     QString::fromUtf8(obs_module_text(
										     "MainOutputEncoderIndexNotFound")),
									     QString::fromUtf8(obs_module_text(
										     "MainOutputEncoderIndexNotFound")));
						}
						return nullptr;
					}
				} else {
					//default encoder
					std::pair<obs_encoder_t **, video_t *> d = {&venc, obs_canvas_get_video(canvas)};
					obs_enum_outputs(
						[](void *param, obs_output_t *output) {
							uint32_t has_flags = OBS_OUTPUT_VIDEO |
									     OBS_OUTPUT_ENCODED; //| OBS_OUTPUT_SERVICE;
							uint32_t flags = obs_output_get_flags(output);
							if ((flags & has_flags) != has_flags) {
								return true;
							}

							std::pair<obs_encoder_t **, video_t *> *d =
								(std::pair<obs_encoder_t **, video_t *> *)param;

							for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
								auto enc = obs_output_get_video_encoder2(output, idx);
								if (enc && obs_encoder_parent_video(enc) == d->second) {
									*d->first = enc;
									return false;
								}
							}
							return true;
						},
						&d);

					if (!venc) {
						std::string venc_name = "Aitum Stream Suite Video ";
						venc_name += output_name;
						venc_name += " ";
						venc_name += obs_canvas_get_name(canvas);
						venc = CreateObsDefaultVideoEncoder(venc_name.c_str(), obs_canvas_get_video(canvas),
										    is_record);
					} else {
						obs_encoder_get_ref(venc);
					}
				}
			} else {
				obs_data_t *s = nullptr;
				auto ves = obs_data_get_obj(settings, "video_encoder_settings");
				if (ves) {
					s = obs_data_create();
					obs_data_apply(s, ves);
					obs_data_release(ves);
				}
				std::string video_encoder_name = "Aitum Stream Suite Video ";
				video_encoder_name += output_name;
				video_encoder_name += " ";
				video_encoder_name += obs_canvas_get_name(canvas);
				venc = obs_video_encoder_create(venc_name, video_encoder_name.c_str(), s, nullptr);
				obs_data_release(s);
				obs_encoder_set_video(venc, obs_canvas_get_video(canvas));
				auto divisor = obs_data_get_int(settings, "frame_rate_divisor");
				if (divisor > 1) {
					obs_encoder_set_frame_rate_divisor(venc, (uint32_t)divisor);
				}

				bool scale = obs_data_get_bool(settings, "scale");
				if (scale) {
					obs_encoder_set_scaled_size(venc, (uint32_t)obs_data_get_int(settings, "width"),
								    (uint32_t)obs_data_get_int(settings, "height"));
					obs_encoder_set_gpu_scale_type(venc,
								       (obs_scale_type)obs_data_get_int(settings, "scale_type"));
				}

				enum video_format video_format = (enum video_format)obs_data_get_int(settings, "color_format");
				obs_encoder_set_preferred_video_format(venc, video_format);

				enum video_colorspace colorspace = (enum video_colorspace)obs_data_get_int(settings, "color_space");
				obs_encoder_set_preferred_color_space(venc, colorspace);

				enum video_range_type range = (enum video_range_type)obs_data_get_int(settings, "color_range");
				obs_encoder_set_preferred_range(venc, range);
			}
		}
	} else {
		std::pair<obs_encoder_t **, video_t *> d = {&venc, obs_canvas_get_video(canvas)};
		obs_enum_outputs(
			[](void *param, obs_output_t *output) {
				uint32_t has_flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED; //| OBS_OUTPUT_SERVICE;
				uint32_t flags = obs_output_get_flags(output);
				if ((flags & has_flags) != has_flags) {
					return true;
				}

				std::pair<obs_encoder_t **, video_t *> *d = (std::pair<obs_encoder_t **, video_t *> *)param;

				for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
					auto enc = obs_output_get_video_encoder2(output, idx);
					if (enc && obs_encoder_video(enc) == d->second) {
						*d->first = obs_encoder_get_ref(enc);
						return false;
					}
				}
				return true;
			},
			&d);

		if (!venc && main) {
			obs_output_t *main_output = ActiveFrontendOutput(is_record);
			venc = main_output ? obs_output_get_video_encoder(main_output) : nullptr;
			if (venc)
				obs_encoder_get_ref(venc);
			obs_output_release(main_output);
			if (!venc) {
				blog(LOG_WARNING, "[Aitum Stream Suite] failed to start output '%s' because main was not started",
				     output_name);
				if (!automated) {
					QMessageBox::warning(this, QString::fromUtf8(obs_module_text("MainOutputNotActive")),
							     QString::fromUtf8(obs_module_text("MainOutputNotActive")));
				}
				return nullptr;
			}
		} else if (!venc) {
			std::string venc_name = "Aitum Stream Suite Video ";
			venc_name += output_name;
			venc_name += " ";
			venc_name += obs_canvas_get_name(canvas);
			venc = CreateObsDefaultVideoEncoder(venc_name.c_str(), obs_canvas_get_video(canvas), is_record);
		}
	}

	return venc;
}

bool OutputWidget::EncoderAvailable(const char *encoder)
{
	const char *val;
	int i = 0;

	while (obs_enum_encoder_types(i++, &val)) {
		if (strcmp(val, encoder) == 0) {
			return true;
		}
	}

	return false;
}

void OutputWidget::CheckActive()
{
	bool active = obs_output_active(output);
	if (outputButton->isChecked() != active) {
		outputButton->setChecked(active);
	}
	if (activeTimer.isActive() != active) {
		if (active) {
			startTime = QDateTime::currentDateTime();
			if (timerLabel) {
				timerLabel->setText(QStringLiteral("00:00"));
			}
			activeTimer.start(500);
		} else {
			activeTimer.stop();
			if (timerLabel) {
				timerLabel->setText(QString());
			}
		}
	}
	UpdateStats();
}

void OutputWidget::UpdateStats()
{
	if (!statsLabel)
		return;
	const bool live = output && (obs_output_active(output) || obs_output_reconnecting(output));
	if (!live) {
		statsLabel->clear();
		statsLabel->setVisible(false);
		statsHaveSample = false;
		return;
	}
	if (obs_output_reconnecting(output)) {
		statsLabel->setText(QString::fromUtf8(obs_module_text("OutputReconnecting")));
		statsLabel->setVisible(true);
		statsHaveSample = false;
		return;
	}
	const uint64_t now = os_gettime_ns();
	const uint64_t bytes = obs_output_get_total_bytes(output);
	if (!statsHaveSample) {
		statsLastBytes = bytes;
		statsLastNs = now;
		statsHaveSample = true;
		return;
	}
	if (now < statsLastNs || now - statsLastNs < 1000000000ULL)
		return;
	const double seconds = double(now - statsLastNs) / 1000000000.0;
	const double kbps = seconds > 0.0 ? (double(bytes - statsLastBytes) * 8.0 / seconds) / 1000.0 : 0.0;
	statsLastBytes = bytes;
	statsLastNs = now;
	const long long dropped = (long long)obs_output_get_frames_dropped(output);
	statsLabel->setText(QString::fromUtf8(obs_module_text("OutputStats")).arg(qRound(kbps)).arg(dropped));
	statsLabel->setVisible(true);
}

void OutputWidget::SaveSettings()
{
	if (StartStopHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_data_array_t *start_hotkey = nullptr;
		obs_data_array_t *stop_hotkey = nullptr;
		obs_hotkey_pair_save(StartStopHotkey, &start_hotkey, &stop_hotkey);
		obs_data_set_array(settings, "start_hotkey", start_hotkey);
		obs_data_set_array(settings, "stop_hotkey", stop_hotkey);
		obs_data_array_release(start_hotkey);
		obs_data_array_release(stop_hotkey);
	}
	if (PauseHotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_data_array_t *pause_hotkey = nullptr;
		obs_data_array_t *unpause_hotkey = nullptr;
		obs_hotkey_pair_save(PauseHotkey, &pause_hotkey, &unpause_hotkey);
		obs_data_set_array(settings, "pause_hotkey", pause_hotkey);
		obs_data_set_array(settings, "unpause_hotkey", unpause_hotkey);
		obs_data_array_release(pause_hotkey);
		obs_data_array_release(unpause_hotkey);
	}
	if (splitHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *split_hotkey = obs_hotkey_save(splitHotkey);
		obs_data_set_array(settings, "split_hotkey", split_hotkey);
		obs_data_array_release(split_hotkey);
	}
	if (chapterHotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *chapter_hotkey = obs_hotkey_save(chapterHotkey);
		obs_data_set_array(settings, "chapter_hotkey", chapter_hotkey);
		obs_data_array_release(chapter_hotkey);
	}
}

void OutputWidget::UpdateSettings(obs_data_t *data)
{
	obs_data_release(settings);
	settings = data;
	obs_data_addref(settings);
	UpdateCanvas();
}

void OutputWidget::UpdateCanvas()
{
	auto canvas_name = obs_data_get_string(settings, "canvas");
	auto video_encoders = obs_data_get_array(settings, "video_encoders");
	auto count = obs_data_array_count(video_encoders);
	QSet<QString> canvases;
	for (size_t i = 0; i < count; i++) {
		auto item = obs_data_array_item(video_encoders, i);
		if (!item) {
			continue;
		}
		auto cn = obs_data_get_string(item, "canvas");
		canvases.insert(QString::fromUtf8(cn[0] == '\0' ? obs_module_text("MainCanvas") : cn));
		obs_data_release(item);
	}
	canvasLabel->setText(canvases.isEmpty()
				     ? QString::fromUtf8(canvas_name[0] == '\0' ? obs_module_text("MainCanvas") : canvas_name)
				     : canvases.values().join(", "));
	obs_data_array_release(video_encoders);
}

void OutputWidget::ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash) {
		*backslash = '/';
	}
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash) {
		*backslash = '\\';
	}
#endif
}

bool OutputWidget::AddChapter(const char *chapter_name)
{
	if (!output) {
		return false;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(output);
	calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "chapter_name", chapter_name);
	bool result = proc_handler_call(ph, "add_chapter", &cd);
	calldata_free(&cd);
	return result;
}

bool OutputWidget::StartOutput(std::function<void()> onStarted, const QString &filenameOverride)
{
	if (output && obs_output_active(output)) {
		onStarted();
		return true;
	}
	pendingFilenameOverride = filenameOverride;
	this->onStarted = onStarted;
	auto starting = StartOutput(true);
	pendingFilenameOverride.clear();
	if (!starting) {
		this->onStarted = nullptr;
	}
	return starting;
}

void OutputWidget::StopForShutdown()
{
	if (!output) {
		return;
	}
	signal_handler_t *signal = obs_output_get_signal_handler(output);
	if (signal) {
		signal_handler_disconnect(signal, "start", output_start, this);
		signal_handler_disconnect(signal, "stop", output_stop, this);
	}
	if (obs_output_active(output) || obs_output_reconnecting(output)) {
		obs_output_force_stop(output);
	}
	obs_output_release(output);
	output = nullptr;
}

void OutputWidget::StopOutput()
{
	if (!output || !obs_output_active(output)) {
		return;
	}

	if (IsStream() && chat_dock &&
	    DetectStreamPlatformFromEndpoint(GetStreamServer()) == StreamPlatformKind::YouTube)
		chat_dock->EndYouTubeBroadcastNow();

	if (obs_output_get_active_delay(output) > 0) {
		obs_output_stop(output);
	} else {
		obs_output_force_stop(output);
	}
}

bool OutputWidget::IsStream() const
{
	const auto output_type = obs_data_get_string(settings, "type");
	if (strcmp(output_type, "ffmpeg") == 0) {
		return !obs_data_get_bool(settings, "save_file");
	}
	return (output_type[0] == '\0' || strcmp(output_type, "stream") == 0);
}

QString OutputWidget::GetStreamServer() const
{
	if (!settings)
		return {};
	QString server = QString::fromUtf8(obs_data_get_string(settings, "stream_server"));
	if (server.isEmpty())
		server = QString::fromUtf8(obs_data_get_string(settings, "server"));
	return server;
}

bool OutputWidget::IsRecord() const
{
	const auto output_type = obs_data_get_string(settings, "type");
	if (strcmp(output_type, "ffmpeg") == 0) {
		return obs_data_get_bool(settings, "save_file");
	}
	return strcmp(output_type, "record") == 0;
}

bool OutputWidget::IsFileRecord() const
{
	const auto output_type = obs_data_get_string(settings, "type");
	return strcmp(output_type, "record") == 0;
}

QString OutputWidget::GetDisplayName() const
{
	const char *name = obs_data_get_string(settings, "name");
	if (name && name[0]) {
		return QString::fromUtf8(name);
	}
	return objectName();
}

const char *OutputWidget::GetOutputType() const
{
	const auto output_type = obs_data_get_string(settings, "type");
	if (output_type[0] == '\0') {
		return "stream";
	} else {
		return output_type;
	}
}
