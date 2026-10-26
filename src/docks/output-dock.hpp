#pragma once

#include <obs.h>
#include <obs-frontend-api.h>
#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <src/utils/widgets/output-widget.hpp>

class OutputDock : public QFrame {
	Q_OBJECT

private:
	int outputPlatformIconSize = 24;
	static constexpr int kRowSpacing = 8;
	static constexpr int kActionBtnWidth = 40;
	static constexpr int kTimerMinWidth = 52;
	QVBoxLayout *mainLayout = nullptr;
	QLabel *mainPlatformIconLabel = nullptr;
	QLabel *mainStreamCanvasLabel = nullptr;
	QLabel *mainRecordCanvasLabel = nullptr;
	QLabel *mainStreamTimerLabel = nullptr;
	QLabel *mainRecordTimerLabel = nullptr;
	QPushButton *mainStreamButton = nullptr;
	QPushButton *mainRecordButton = nullptr;
	obs_output_t *canvasRecordOutput = nullptr;
	QFrame *mainStreamGroup = nullptr;
	QFrame *mainRecordGroup = nullptr;
	QString mainPlatformUrl;
	bool exiting = false;
	bool mainStreamEnabled = true;
	bool mainRecordEnabled = true;
	QDateTime mainStreamStartTime;
	QDateTime mainRecordStartTime;

	QTimer videoCheckTimer;

	static QLabel *MakeTimerLabel(QWidget *parent);
	static QString FormatElapsed(const QDateTime &start);
	void SetTimerText(QLabel *label, bool active, QDateTime &startTime);

	std::vector<OutputWidget *> outputWidgets;

	std::list<std::function<bool(std::function<void()>)>> outputsToStart;
	size_t outputStarting = 0;

	obs_hotkey_pair_id StartStopHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	obs_hotkey_id StartStreamHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id StartRecordHotkey = OBS_INVALID_HOTKEY_ID;

	std::function<void()> mainStreamOnStarted;
	std::function<void()> mainRecordOnStarted;

	bool RecordUsesCanvas() const;
	bool CanvasRecordActive() const;
	bool StartCanvasRecord(const QString &filenameOverride = QString());
	void StopCanvasRecord();
	void ToggleMainRecord();
	void RefreshRecordCanvasLabel();
	void ReorderOutputRows();
	QString SuggestRecordingBasename(const QString &suffix) const;
	bool PromptAndStartRecordings(bool includeBuiltin, bool includeExtraOutputs);

	static void frontend_event(enum obs_frontend_event event, void *private_data);

private slots:
	void StartAll(bool streamOnly, bool recordOnly);
	void StopAll(bool streamOnly, bool recordOnly);
	void StartNextOutput();

public:
	OutputDock(QWidget *parent = nullptr);
	~OutputDock();

	obs_data_array_t *GetOutputsArray();

	void Exiting() { exiting = true; }
	void LoadSettings();
	void SaveSettings();
	bool AddChapterToOutput(const char *output_name, const char *chapter_name);
	bool HasActiveStreams() const;
	bool HasActiveRecordings() const;
	bool HasActiveStreamMatching(const std::function<bool(const QString &server)> &match) const;

public slots:
	void UpdateMainStreamStatus(bool active);
	void UpdateMainRecordingStatus(bool active);
};
