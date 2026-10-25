#include "controls-other-options.hpp"
#include "output-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

extern OutputDock *output_dock;

namespace {

QPointer<QWidget> tabHost;
QPointer<QTabBar> tabs;
QPointer<QWidget> otherPage;
QPointer<QWidget> controlsFrame;
QPointer<QPushButton> startStreamsBtn;
QPointer<QPushButton> stopStreamsBtn;
QPointer<QPushButton> startRecordsBtn;
QPointer<QPushButton> stopRecordsBtn;
QPointer<QTimer> refreshTimer;
bool installed = false;

/*
 * Yami.obt styles Controls via:
 *   #controlsFrame { padding: var(--padding_container); }
 *   #controlsFrame QPushButton { margin: var(--spacing_base) var(--spacing_small); }
 *   #streamButton, #recordButton, #replayBufferButton, #broadcastButton {
 *     padding: var(--padding_large);
 *   }
 * So Other Options must use the same objectNames / ancestry as OBSBasicControls.ui.
 */

void ApplyTab(int index)
{
	if (!controlsFrame || !otherPage) {
		return;
	}
	const bool main = index <= 0;
	controlsFrame->setVisible(main);
	otherPage->setVisible(!main);
}

void InvokeOutput(const char *method, bool streamOnly, bool recordOnly)
{
	if (!output_dock) {
		return;
	}
	QMetaObject::invokeMethod(output_dock, method, Q_ARG(bool, streamOnly), Q_ARG(bool, recordOnly));
}

void RefreshActionButtons()
{
	const bool streams = output_dock && output_dock->HasActiveStreams();
	const bool records = output_dock && output_dock->HasActiveRecordings();
	if (startStreamsBtn) {
		startStreamsBtn->setEnabled(!streams);
	}
	if (stopStreamsBtn) {
		stopStreamsBtn->setEnabled(streams);
	}
	if (startRecordsBtn) {
		startRecordsBtn->setEnabled(!records);
	}
	if (stopRecordsBtn) {
		stopRecordsBtn->setEnabled(records);
	}
}

void Polish(QWidget *w)
{
	if (!w || !w->style()) {
		return;
	}
	w->style()->unpolish(w);
	w->style()->polish(w);
}

/*
 * Match streamButton from OBSBasicControls.ui + Yami #streamButton padding.
 * Icons: aitum SVGs are already light; stop uses class icon-media-stop so
 * Yami qproperty-icon swaps in theme:Dark/media/media_stop.svg (same as native).
 */
QPushButton *MakePrimaryControlButton(QWidget *parent, const char *objectName, const char *textKey,
				      const QString &iconPath, const char *iconClass = nullptr)
{
	auto *btn = new QPushButton(QString::fromUtf8(obs_module_text(textKey)), parent);
	btn->setObjectName(QString::fromUtf8(objectName));
	btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	btn->setMinimumWidth(150);
	btn->setAutoDefault(false);
	btn->setDefault(false);
	if (!iconPath.isEmpty()) {
		btn->setIcon(QIcon(iconPath));
	}
	if (iconClass) {
		btn->setProperty("class", QVariant(QString::fromUtf8(iconClass)));
	}
	Polish(btn);
	return btn;
}

void AddStreamLikeRow(QVBoxLayout *layout, QPushButton *btn)
{
	/* Same as OBSBasicControls.ui horizontalLayout_7 around streamButton. */
	auto *row = new QHBoxLayout;
	row->setContentsMargins(0, 0, 0, 0);
	row->addWidget(btn);
	layout->addLayout(row);
}

QWidget *BuildOtherPage(QWidget *parent)
{
	/*
	 * Same shell as OBSBasicControls.ui controlsFrame so Yami rules apply:
	 * objectName must be "controlsFrame".
	 */
	auto *frame = new QFrame(parent);
	frame->setObjectName(QStringLiteral("controlsFrame"));
	auto *layout = new QVBoxLayout(frame);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/* objectNames from Yami primary-control padding list */
	startStreamsBtn = MakePrimaryControlButton(frame, "streamButton", "StartAllStreams",
						   QStringLiteral(":/aitum/media/streaming.svg"));
	stopStreamsBtn = MakePrimaryControlButton(frame, "recordButton", "StopAllStreams",
						  QStringLiteral(":/res/images/media/media_stop.svg"),
						  "icon-media-stop");
	startRecordsBtn = MakePrimaryControlButton(frame, "replayBufferButton", "StartAllRecordings",
						   QStringLiteral(":/aitum/media/recording.svg"));
	stopRecordsBtn = MakePrimaryControlButton(frame, "broadcastButton", "StopAllRecordings",
						  QStringLiteral(":/res/images/media/media_stop.svg"),
						  "icon-media-stop");

	stopStreamsBtn->setEnabled(false);
	stopRecordsBtn->setEnabled(false);

	QObject::connect(startStreamsBtn, &QPushButton::clicked, frame, [] {
		InvokeOutput("StartAll", true, false);
		RefreshActionButtons();
	});
	QObject::connect(stopStreamsBtn, &QPushButton::clicked, frame, [] {
		InvokeOutput("StopAll", true, false);
		RefreshActionButtons();
	});
	QObject::connect(startRecordsBtn, &QPushButton::clicked, frame, [] {
		InvokeOutput("StartAll", false, true);
		RefreshActionButtons();
	});
	QObject::connect(stopRecordsBtn, &QPushButton::clicked, frame, [] {
		InvokeOutput("StopAll", false, true);
		RefreshActionButtons();
	});

	AddStreamLikeRow(layout, startStreamsBtn);
	AddStreamLikeRow(layout, stopStreamsBtn);
	AddStreamLikeRow(layout, startRecordsBtn);
	AddStreamLikeRow(layout, stopRecordsBtn);
	layout->addStretch(1);

	Polish(frame);

	refreshTimer = new QTimer(frame);
	refreshTimer->setInterval(400);
	QObject::connect(refreshTimer, &QTimer::timeout, frame, [] { RefreshActionButtons(); });
	refreshTimer->start();
	RefreshActionButtons();

	frame->hide();
	return frame;
}

} // namespace

void install_controls_other_options_tab()
{
	if (installed && tabHost && tabs && otherPage && controlsFrame) {
		RefreshActionButtons();
		return;
	}

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		return;
	}
	auto *controlsDock = main->findChild<QDockWidget *>(QStringLiteral("controlsDock"));
	if (!controlsDock || !controlsDock->widget()) {
		return;
	}

	QWidget *root = controlsDock->widget();
	auto *layout = qobject_cast<QVBoxLayout *>(root->layout());
	/* Prefer the real native frame (first match), not our Other Options clone. */
	controlsFrame = root->findChild<QWidget *>(QStringLiteral("controlsFrame"));
	if (!layout || !controlsFrame) {
		blog(LOG_WARNING, "[Stream Suite] Controls other-options tab not installed (layout/frame missing)");
		return;
	}

	if (!tabHost) {
		tabHost = new QWidget(root);
		tabHost->setObjectName(QStringLiteral("controlsOtherOptionsHost"));
		tabHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		auto *row = new QHBoxLayout(tabHost);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(0);

		tabs = new QTabBar(tabHost);
		tabs->setObjectName(QStringLiteral("controlsOtherOptionsTabs"));
		tabs->setExpanding(false);
		tabs->setElideMode(Qt::ElideRight);
		tabs->setUsesScrollButtons(true);
		tabs->setDrawBase(false);
		tabs->setDocumentMode(true);
		tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		tabs->setFixedHeight(22);
		tabs->setStyleSheet(QString::fromUtf8(
			"QTabBar { font-size: 8pt; }"
			"QTabBar::tab {"
			"  background: palette(mid);"
			"  color: palette(windowText);"
			"  border: none;"
			"  padding: 2px 6px;"
			"  margin-right: 1px;"
			"  min-width: 2em;"
			"}"
			"QTabBar::tab:selected {"
			"  background: palette(dark);"
			"}"
			"QTabBar::tab:hover:!selected {"
			"  background: palette(button);"
			"}"));

		row->addWidget(tabs);
		layout->insertWidget(0, tabHost);
	}

	if (!otherPage) {
		otherPage = BuildOtherPage(root);
		layout->addWidget(otherPage);
		/* If findChild now returns our clone, pin the native frame again. */
		const auto frames = root->findChildren<QWidget *>(QStringLiteral("controlsFrame"));
		for (QWidget *frame : frames) {
			if (frame != otherPage) {
				controlsFrame = frame;
				break;
			}
		}
	}

	tabs->blockSignals(true);
	while (tabs->count() > 0) {
		tabs->removeTab(0);
	}
	QString controlsTitle = controlsDock->windowTitle();
	if (controlsTitle.isEmpty()) {
		controlsTitle = QString::fromUtf8(obs_frontend_get_locale_string("Basic.Main.Controls"));
	}
	if (controlsTitle.isEmpty()) {
		controlsTitle = QStringLiteral("Controls");
	}
	tabs->addTab(controlsTitle);
	tabs->addTab(QString::fromUtf8(obs_module_text("OtherOptions")));
	tabs->setCurrentIndex(0);
	tabs->blockSignals(false);

	QObject::disconnect(tabs, nullptr, nullptr, nullptr);
	QObject::connect(tabs, &QTabBar::currentChanged, tabs, [](int index) { ApplyTab(index); });
	ApplyTab(0);

	installed = true;
	blog(LOG_INFO, "[Stream Suite] Controls Other Options tab installed");
}

void shutdown_controls_other_options_tab()
{
	if (refreshTimer) {
		refreshTimer->stop();
		refreshTimer = nullptr;
	}
	startStreamsBtn = nullptr;
	stopStreamsBtn = nullptr;
	startRecordsBtn = nullptr;
	stopRecordsBtn = nullptr;
	if (controlsFrame) {
		controlsFrame->setVisible(true);
	}
	if (otherPage) {
		otherPage->hide();
		delete otherPage;
		otherPage = nullptr;
	}
	if (tabHost) {
		delete tabHost;
		tabHost = nullptr;
	}
	tabs = nullptr;
	controlsFrame = nullptr;
	installed = false;
}
