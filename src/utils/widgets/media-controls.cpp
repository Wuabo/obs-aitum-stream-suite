#include "media-controls.hpp"
#include "absolute-slider.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QAction>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QStyle>
#include <QToolTip>

#include "moc_media-controls.cpp"

void ClickableLabel::mousePressEvent(QMouseEvent *event)
{
	emit clicked();
	event->accept();
}

void MediaControls::OBSMediaStopped(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetRestartState);
}

void MediaControls::OBSMediaPlay(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPlayingState);
}

void MediaControls::OBSMediaPause(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPausedState);
}

void MediaControls::OBSMediaStarted(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::SetPlayingState);
}

void MediaControls::OBSMediaNext(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::UpdateSlideCounter);
}

void MediaControls::OBSMediaPrevious(void *data, calldata_t *)
{
	auto *media = static_cast<MediaControls *>(data);
	QMetaObject::invokeMethod(media, &MediaControls::UpdateSlideCounter);
}

static void PolishMediaButton(QPushButton *btn)
{
	btn->style()->unpolish(btn);
	btn->style()->polish(btn);
}

/* Mirror media-controls.ui: minimumSize 22x22 only — Yami #contextContainer
 * QPushButton padding (12px L/R) + QPushButton::flat must be able to grow the
 * button so Pause/Stop get the same blue chrome as Main Canvas. */
static QPushButton *MakeMediaButton(QWidget *parent, const char *objectName, const char *cssClass,
				    const char *tooltipKey)
{
	auto *btn = new QPushButton(parent);
	btn->setObjectName(QString::fromUtf8(objectName));
	btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	btn->setMinimumSize(22, 22);
	btn->setFlat(true);
	btn->setText(QString());
	btn->setIconSize(QSize(20, 20));
	btn->setProperty("class", QString::fromUtf8(cssClass));
	btn->setToolTip(QString::fromUtf8(obs_frontend_get_locale_string(tooltipKey)));
	PolishMediaButton(btn);
	return btn;
}

MediaControls::MediaControls(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("MediaControls"));
	setMinimumHeight(22);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	setFocusPolicy(Qt::StrongFocus);

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	playPauseButton = MakeMediaButton(this, "playPauseButton", "icon-media-restart",
					  "ContextBar.MediaControls.RestartMedia");
	previousButton = MakeMediaButton(this, "previousButton", "icon-media-prev",
					 "ContextBar.MediaControls.PlaylistPrevious");
	previousButton->setShortcut(QKeySequence(QStringLiteral("P")));
	stopButton = MakeMediaButton(this, "stopButton", "icon-media-stop", "ContextBar.MediaControls.StopMedia");
	stopButton->setShortcut(QKeySequence(QStringLiteral("S")));
	nextButton = MakeMediaButton(this, "nextButton", "icon-media-next", "ContextBar.MediaControls.PlaylistNext");
	nextButton->setShortcut(QKeySequence(QStringLiteral("N")));

	slider = new AbsoluteSlider(Qt::Horizontal, this);
	slider->setObjectName(QStringLiteral("slider"));
	slider->setMinimumHeight(22);
	slider->setMaximum(1024);
	slider->setTracking(false);
	slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	slider->setAccessibleName(
		QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.MediaControls.BlindSeek")));

	timerLabel = new QLabel(QStringLiteral("--:--:--"), this);
	timerLabel->setObjectName(QStringLiteral("timerLabel"));
	timerLabel->setMinimumHeight(22);
	timerLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	auto *slash = new QLabel(QStringLiteral("/"), this);
	slash->setObjectName(QStringLiteral("label"));
	slash->setMinimumHeight(22);
	slash->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	durationLabel = new ClickableLabel(this);
	durationLabel->setObjectName(QStringLiteral("durationLabel"));
	durationLabel->setMinimumHeight(22);
	durationLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	durationLabel->setText(QStringLiteral("--:--:--"));

	emptySpaceAgain = new QFrame(this);
	emptySpaceAgain->setObjectName(QStringLiteral("emptySpaceAgain"));
	emptySpaceAgain->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	emptySpaceAgain->hide();

	layout->addWidget(playPauseButton);
	layout->addWidget(previousButton);
	layout->addWidget(stopButton);
	layout->addWidget(nextButton);
	layout->addWidget(slider);
	layout->addWidget(timerLabel);
	layout->addWidget(slash);
	layout->addWidget(durationLabel);
	layout->addWidget(emptySpaceAgain);

	connect(&mediaTimer, &QTimer::timeout, this, &MediaControls::SetSliderPosition);
	connect(&seekTimer, &QTimer::timeout, this, &MediaControls::SeekTimerCallback);
	connect(slider, &AbsoluteSlider::sliderPressed, this, &MediaControls::AbsoluteSliderClicked);
	connect(slider, &AbsoluteSlider::absoluteSliderHovered, this, &MediaControls::AbsoluteSliderHovered);
	connect(slider, &AbsoluteSlider::sliderReleased, this, &MediaControls::AbsoluteSliderReleased);
	connect(slider, &AbsoluteSlider::sliderMoved, this, &MediaControls::AbsoluteSliderMoved);
	connect(playPauseButton, &QPushButton::clicked, this, &MediaControls::onPlayPauseClicked);
	connect(stopButton, &QPushButton::clicked, this, &MediaControls::onStopClicked);
	connect(nextButton, &QPushButton::clicked, this, &MediaControls::onNextClicked);
	connect(previousButton, &QPushButton::clicked, this, &MediaControls::onPreviousClicked);
	connect(durationLabel, &ClickableLabel::clicked, this, &MediaControls::onDurationClicked);

	if (config_t *cfg = obs_frontend_get_user_config()) {
		countDownTimer = config_get_bool(cfg, "BasicWindow", "MediaControlsCountdownTimer");
	}

	auto *restartAction = new QAction(this);
	restartAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	restartAction->setShortcut({Qt::Key_R});
	connect(restartAction, &QAction::triggered, this, &MediaControls::RestartMedia);
	addAction(restartAction);

	auto *sliderForward = new QAction(this);
	sliderForward->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	sliderForward->setShortcut({Qt::Key_Right});
	connect(sliderForward, &QAction::triggered, this, &MediaControls::MoveSliderForward);
	addAction(sliderForward);

	auto *sliderBack = new QAction(this);
	sliderBack->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	sliderBack->setShortcut({Qt::Key_Left});
	connect(sliderBack, &QAction::triggered, this, &MediaControls::MoveSliderBackwards);
	addAction(sliderBack);

	auto *playPause = new QAction(this);
	playPause->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	playPause->setShortcut({Qt::Key_Space});
	connect(playPause, &QAction::triggered, this, &MediaControls::onPlayPauseClicked);
	addAction(playPause);
}

bool MediaControls::MediaPaused()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return false;
	}
	return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED;
}

int64_t MediaControls::GetSliderTime(int val)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return 0;
	}
	float percent = (float)val / (float)slider->maximum();
	float duration = (float)obs_source_media_get_duration(source);
	return (int64_t)(percent * duration);
}

void MediaControls::AbsoluteSliderClicked()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	obs_media_state state = obs_source_media_get_state(source);
	if (state == OBS_MEDIA_STATE_PAUSED) {
		prevPaused = true;
	} else if (state == OBS_MEDIA_STATE_PLAYING) {
		prevPaused = false;
		PauseMedia();
		StopMediaTimer();
	}

	seek = slider->value();
	seekTimer.start(100);
}

void MediaControls::AbsoluteSliderReleased()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	if (seekTimer.isActive()) {
		seekTimer.stop();
		if (lastSeek != seek) {
			obs_source_media_set_time(source, GetSliderTime(seek));
		}
		UpdateLabels(seek);
		seek = lastSeek = -1;
	}

	if (!prevPaused) {
		PlayMedia();
		StartMediaTimer();
	}
}

void MediaControls::AbsoluteSliderHovered(int val)
{
	float seconds = ((float)GetSliderTime(val) / 1000.0f);
	QToolTip::showText(QCursor::pos(), FormatSeconds((int)seconds), this);
}

void MediaControls::AbsoluteSliderMoved(int val)
{
	if (seekTimer.isActive()) {
		seek = val;
		UpdateLabels(seek);
	}
}

void MediaControls::SeekTimerCallback()
{
	if (lastSeek != seek) {
		OBSSource source = OBSGetStrongRef(weakSource);
		if (source) {
			obs_source_media_set_time(source, GetSliderTime(seek));
		}
		lastSeek = seek;
	}
}

void MediaControls::StartMediaTimer()
{
	if (isSlideshow) {
		return;
	}
	if (!mediaTimer.isActive()) {
		mediaTimer.start(16);
	}
}

void MediaControls::StopMediaTimer()
{
	if (mediaTimer.isActive()) {
		mediaTimer.stop();
	}
}

void MediaControls::SetPlayingState()
{
	slider->setEnabled(true);
	playPauseButton->setProperty("class", "icon-media-pause");
	PolishMediaButton(playPauseButton);
	playPauseButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.MediaControls.PauseMedia")));
	prevPaused = false;
	UpdateSlideCounter();
	StartMediaTimer();
}

void MediaControls::SetPausedState()
{
	playPauseButton->setProperty("class", "icon-media-play");
	PolishMediaButton(playPauseButton);
	playPauseButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.MediaControls.PlayMedia")));
	StopMediaTimer();
}

void MediaControls::SetRestartState()
{
	playPauseButton->setProperty("class", "icon-media-restart");
	PolishMediaButton(playPauseButton);
	playPauseButton->setToolTip(
		QString::fromUtf8(obs_frontend_get_locale_string("ContextBar.MediaControls.RestartMedia")));

	slider->setValue(0);
	if (!isSlideshow) {
		timerLabel->setText(QStringLiteral("--:--:--"));
		durationLabel->setText(QStringLiteral("--:--:--"));
	} else {
		timerLabel->setText(QStringLiteral("-"));
		durationLabel->setText(QStringLiteral("-"));
	}
	slider->setEnabled(false);
	StopMediaTimer();
}

void MediaControls::RefreshControls()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	uint32_t flags = 0;
	const char *id = nullptr;

	if (source) {
		flags = obs_source_get_output_flags(source);
		id = obs_source_get_unversioned_id(source);
	}

	if (!source || !(flags & OBS_SOURCE_CONTROLLABLE_MEDIA)) {
		SetRestartState();
		setEnabled(false);
		hide();
		return;
	}

	setEnabled(true);
	show();

	bool has_playlist = strcmp(id, "ffmpeg_source") != 0;
	previousButton->setVisible(has_playlist);
	nextButton->setVisible(has_playlist);

	isSlideshow = strcmp(id, "slideshow") == 0;
	slider->setVisible(!isSlideshow);
	emptySpaceAgain->setVisible(isSlideshow);

	switch (obs_source_media_get_state(source)) {
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ENDED:
	case OBS_MEDIA_STATE_NONE:
		SetRestartState();
		break;
	case OBS_MEDIA_STATE_PLAYING:
		SetPlayingState();
		break;
	case OBS_MEDIA_STATE_PAUSED:
		SetPausedState();
		break;
	default:
		break;
	}

	if (isSlideshow) {
		UpdateSlideCounter();
	} else {
		SetSliderPosition();
	}
}

OBSSource MediaControls::GetSource()
{
	return OBSGetStrongRef(weakSource);
}

void MediaControls::SetSource(OBSSource source)
{
	sigs.clear();

	if (source) {
		weakSource = OBSGetWeakRef(source);
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		sigs.emplace_back(sh, "media_play", OBSMediaPlay, this);
		sigs.emplace_back(sh, "media_pause", OBSMediaPause, this);
		sigs.emplace_back(sh, "media_restart", OBSMediaPlay, this);
		sigs.emplace_back(sh, "media_stopped", OBSMediaStopped, this);
		sigs.emplace_back(sh, "media_started", OBSMediaStarted, this);
		sigs.emplace_back(sh, "media_ended", OBSMediaStopped, this);
		sigs.emplace_back(sh, "media_next", OBSMediaNext, this);
		sigs.emplace_back(sh, "media_previous", OBSMediaPrevious, this);
	} else {
		weakSource = nullptr;
	}

	RefreshControls();
}

void MediaControls::SetSliderPosition()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	float time = (float)obs_source_media_get_time(source);
	float duration = (float)obs_source_media_get_duration(source);
	float sliderPosition = duration ? (time / duration) * (float)slider->maximum() : 0.0f;
	slider->setValue((int)sliderPosition);
	UpdateLabels((int)sliderPosition);
}

QString MediaControls::FormatSeconds(int totalSeconds)
{
	int seconds = totalSeconds % 60;
	int totalMinutes = totalSeconds / 60;
	int minutes = totalMinutes % 60;
	int hours = totalMinutes / 60;
	return QString::asprintf("%02d:%02d:%02d", hours, minutes, seconds);
}

void MediaControls::onPlayPauseClicked()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	switch (obs_source_media_get_state(source)) {
	case OBS_MEDIA_STATE_STOPPED:
	case OBS_MEDIA_STATE_ENDED:
		RestartMedia();
		break;
	case OBS_MEDIA_STATE_PLAYING:
		PauseMedia();
		break;
	case OBS_MEDIA_STATE_PAUSED:
		PlayMedia();
		break;
	default:
		break;
	}
}

void MediaControls::RestartMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_restart(source);
	}
}

void MediaControls::PlayMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_play_pause(source, false);
	}
}

void MediaControls::PauseMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_play_pause(source, true);
	}
}

void MediaControls::StopMedia()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_stop(source);
	}
}

void MediaControls::PlaylistNext()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_next(source);
	}
}

void MediaControls::PlaylistPrevious()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (source) {
		obs_source_media_previous(source);
	}
}

void MediaControls::onStopClicked()
{
	StopMedia();
}

void MediaControls::onNextClicked()
{
	PlaylistNext();
}

void MediaControls::onPreviousClicked()
{
	PlaylistPrevious();
}

void MediaControls::onDurationClicked()
{
	countDownTimer = !countDownTimer;
	if (config_t *cfg = obs_frontend_get_user_config()) {
		config_set_bool(cfg, "BasicWindow", "MediaControlsCountdownTimer", countDownTimer);
	}
	SetSliderPosition();
}

void MediaControls::MoveSliderForward(int seconds)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}
	obs_source_media_set_time(source, obs_source_media_get_time(source) + seconds * 1000);
	SetSliderPosition();
}

void MediaControls::MoveSliderBackwards(int seconds)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}
	obs_source_media_set_time(source, obs_source_media_get_time(source) - seconds * 1000);
	SetSliderPosition();
}

void MediaControls::UpdateSlideCounter()
{
	if (!isSlideshow) {
		return;
	}

	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	calldata_t cd = {};
	proc_handler_call(ph, "current_index", &cd);
	int slide = (int)calldata_int(&cd, "current_index");
	proc_handler_call(ph, "total_files", &cd);
	int total = (int)calldata_int(&cd, "total_files");
	calldata_free(&cd);

	if (total > 0) {
		timerLabel->setText(QString::number(slide + 1));
		durationLabel->setText(QString::number(total));
	} else {
		timerLabel->setText(QStringLiteral("-"));
		durationLabel->setText(QStringLiteral("-"));
	}
}

void MediaControls::UpdateLabels(int val)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	/* Re-read so multiple docks stay in sync with the Main Canvas toggle. */
	if (config_t *cfg = obs_frontend_get_user_config()) {
		countDownTimer = config_get_bool(cfg, "BasicWindow", "MediaControlsCountdownTimer");
	}

	float duration = (float)obs_source_media_get_duration(source);
	float percent = (float)val / (float)slider->maximum();
	float time = percent * duration;

	timerLabel->setText(FormatSeconds((int)(time / 1000.0f)));
	if (!countDownTimer) {
		durationLabel->setText(FormatSeconds((int)(duration / 1000.0f)));
	} else {
		durationLabel->setText(QStringLiteral("-") + FormatSeconds((int)((duration - time) / 1000.0f)));
	}
}
