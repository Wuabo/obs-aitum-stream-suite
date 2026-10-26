#pragma once

#include <obs.hpp>
#include <QFrame>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <vector>

class AbsoluteSlider;

class ClickableLabel : public QLabel {
	Q_OBJECT
public:
	explicit ClickableLabel(QWidget *parent = nullptr) : QLabel(parent) {}

signals:
	void clicked();

protected:
	void mousePressEvent(QMouseEvent *event) override;
};

class MediaControls : public QWidget {
	Q_OBJECT

public:
	explicit MediaControls(QWidget *parent = nullptr);

	OBSSource GetSource();
	void SetSource(OBSSource newSource);

public slots:
	void PlayMedia();
	void PauseMedia();

private slots:
	void onPlayPauseClicked();
	void onStopClicked();
	void onNextClicked();
	void onPreviousClicked();
	void onDurationClicked();

	void AbsoluteSliderClicked();
	void AbsoluteSliderReleased();
	void AbsoluteSliderHovered(int val);
	void AbsoluteSliderMoved(int val);
	void SetSliderPosition();
	void SetPlayingState();
	void SetPausedState();
	void SetRestartState();
	void RestartMedia();
	void StopMedia();
	void PlaylistNext();
	void PlaylistPrevious();
	void SeekTimerCallback();
	void MoveSliderForward(int seconds = 5);
	void MoveSliderBackwards(int seconds = 5);
	void UpdateSlideCounter();
	void UpdateLabels(int val);

private:
	QString FormatSeconds(int totalSeconds);
	void StartMediaTimer();
	void StopMediaTimer();
	void RefreshControls();
	int64_t GetSliderTime(int val);

	static void OBSMediaStopped(void *data, calldata_t *calldata);
	static void OBSMediaPlay(void *data, calldata_t *calldata);
	static void OBSMediaPause(void *data, calldata_t *calldata);
	static void OBSMediaStarted(void *data, calldata_t *calldata);
	static void OBSMediaNext(void *data, calldata_t *calldata);
	static void OBSMediaPrevious(void *data, calldata_t *calldata);

	std::vector<OBSSignal> sigs;
	OBSWeakSource weakSource = nullptr;
	QTimer mediaTimer;
	QTimer seekTimer;
	int seek = -1;
	int lastSeek = -1;
	bool prevPaused = false;
	bool countDownTimer = false;
	bool isSlideshow = false;

	QPushButton *playPauseButton = nullptr;
	QPushButton *previousButton = nullptr;
	QPushButton *stopButton = nullptr;
	QPushButton *nextButton = nullptr;
	AbsoluteSlider *slider = nullptr;
	QLabel *timerLabel = nullptr;
	ClickableLabel *durationLabel = nullptr;
	QFrame *emptySpaceAgain = nullptr;
};
