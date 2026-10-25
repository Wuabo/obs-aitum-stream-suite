#pragma once

#include <obs.h>
#include <QDateTime>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <functional>

class OutputWidget : public QFrame {
	Q_OBJECT
private:
	obs_data_t *settings = nullptr;
	int outputPlatformIconSize = 24;
	static constexpr int kRowSpacing = 8;
	static constexpr int kActionBtnWidth = 40;
	static constexpr int kTimerMinWidth = 52;

	QPushButton *outputButton = nullptr;
	QLabel *canvasLabel = nullptr;
	QLabel *timerLabel = nullptr;
	obs_output_t *output = nullptr;

	std::function<void()> onStarted = nullptr;
	QString pendingFilenameOverride;

	QTimer activeTimer;
	QDateTime startTime;

	obs_hotkey_pair_id StartStopHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	obs_hotkey_pair_id PauseHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	obs_hotkey_id splitHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id chapterHotkey = OBS_INVALID_HOTKEY_ID;

	bool StartOutput(bool automated = false);
	void UpdateCanvas();
	obs_encoder_t *GetVideoEncoder(obs_data_t *settings, bool advanced, bool is_record, const char *output_name,
				       bool automated);

	static void output_stop(void *data, calldata_t *calldata);
	static void output_start(void *data, calldata_t *calldata);
	static bool EncoderAvailable(const char *encoder);
	static void ensure_directory(char *path);

public:
	OutputWidget(obs_data_t *output_data, QWidget *parent = nullptr);
	~OutputWidget();

	obs_output_t *GetOutput() const { return output; }

	void CheckActive();
	void SaveSettings();
	void UpdateSettings(obs_data_t *data);
	bool AddChapter(const char *chapter_name);
	bool StartOutput(std::function<void()> onStarted, const QString &filenameOverride = QString());
	void StopOutput();
	bool IsStream() const;
	bool IsRecord() const;
	bool IsFileRecord() const;
	const char* GetOutputType() const;
	QString GetDisplayName() const;
};
