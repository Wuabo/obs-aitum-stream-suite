#include "native-dock-tabs.hpp"
#include "canvas-dock.hpp"
#include "sources-dock.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QSizePolicy>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

extern std::list<CanvasDock *> canvas_docks;

namespace {

struct CanvasPages {
	QPointer<CanvasDock> dock;
	QString uuid;
	QWidget *scenes = nullptr;
	QWidget *sources = nullptr;
	QWidget *transitions = nullptr;
};

QTabBar *scenesTabs = nullptr;
QTabBar *sourcesTabs = nullptr;
QTabBar *transitionsTabs = nullptr;
QWidget *scenesTabHost = nullptr;
QWidget *sourcesTabHost = nullptr;
QWidget *transitionsTabHost = nullptr;
QWidget *nativeScenes = nullptr;
QWidget *nativeScenesToolbar = nullptr;
QWidget *nativeSources = nullptr;
QWidget *nativeSourcesToolbar = nullptr;
QVBoxLayout *scenesLayout = nullptr;
QVBoxLayout *sourcesLayout = nullptr;
QVBoxLayout *transitionsLayout = nullptr;
std::vector<QPointer<QWidget>> nativeTransitionWidgets;
std::vector<CanvasPages> pages;
bool syncing = false;
bool alive = false;

/* Inserting tabs after OBS restoreState makes the dock layout snap back
 * to the list's size hint, so a widened Scenes dock opens narrow again.
 * Remember the restored size and put it back after the tabs are built. */
struct DockSpan {
	QPointer<QDockWidget> dock;
	int w = 0;
	int h = 0;
};

void RestoreNativeDockSizes(QMainWindow *main, const QList<DockSpan> &spans)
{
	if (!main) {
		return;
	}
	QDockWidget *scenes = nullptr;
	QDockWidget *sources = nullptr;
	int scenesW = 0;
	int scenesH = 0;
	int sourcesH = 0;
	for (const DockSpan &span : spans) {
		if (!span.dock) {
			continue;
		}
		if (span.dock->objectName() == QString::fromUtf8("scenesDock")) {
			scenes = span.dock;
			scenesW = span.w;
			scenesH = span.h;
		} else if (span.dock->objectName() == QString::fromUtf8("sourcesDock")) {
			sources = span.dock;
			sourcesH = span.h;
		}
	}
	if (scenes && sources && scenesW > 40) {
		main->resizeDocks({scenes, sources}, {scenesW, scenesW}, Qt::Horizontal);
	} else if (scenes && scenesW > 40) {
		main->resizeDocks({scenes}, {scenesW}, Qt::Horizontal);
	}
	if (scenes && sources && scenesH > 40 && sourcesH > 40) {
		main->resizeDocks({scenes, sources}, {scenesH, sourcesH}, Qt::Vertical);
	}
}

void KeepDockFromShrinking(QDockWidget *dock)
{
	if (!dock) {
		return;
	}
	/* The contents must not pull the dock back to a default width. */
	if (QWidget *contents = dock->widget()) {
		contents->setMinimumSize(0, 0);
		contents->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
	}
}

/* OBSDock QToolBar QToolButton uses margin 1px. A bar built outside the
 * dock keeps the tighter spacing after polish, so the scenes actions sit
 * closer together than the native bar. Keep the themed icon and add only
 * that margin. */
void MatchScenesToolbarSpacing(QWidget *pane)
{
	if (!pane) {
		return;
	}
	auto *toolbar = pane->findChild<QToolBar *>(QString::fromUtf8("scenesToolbar"));
	if (!toolbar) {
		return;
	}
	toolbar->setContentsMargins(0, 0, 0, 0);
	toolbar->setMaximumHeight(32);
	const auto buttons = toolbar->findChildren<QToolButton *>();
	for (QToolButton *button : buttons) {
		const QIcon themed = button->icon();
		const int hint = button->sizeHint().width();
		const int icon = toolbar->iconSize().width();
		int pad = hint > icon ? (hint - icon) / 2 : -1;
		if (pad > 6) {
			pad = -1;
		}
		QString sheet = QString::fromUtf8(
			"QToolButton {"
			"  margin-left: 1px;"
			"  margin-right: 1px;"
			"  background: transparent;"
			"  border: 1px solid transparent;"
			"}"
			"QToolButton:hover { border-color: rgb(97, 97, 97); }"
			"QToolButton:disabled { background: rgb(16, 16, 16); }");
		if (pad >= 0) {
			sheet = QString::fromUtf8(
					"QToolButton {"
					"  margin-left: 1px;"
					"  margin-right: 1px;"
					"  padding: %1px;"
					"  background: transparent;"
					"  border: 1px solid transparent;"
					"}"
					"QToolButton:hover { border-color: rgb(97, 97, 97); }"
					"QToolButton:disabled { background: rgb(16, 16, 16); }")
					.arg(pad);
		}
		button->setStyleSheet(sheet);
		if (!themed.isNull()) {
			button->setIcon(themed);
		}
	}
}

/* Toolbars are built inside the suite dock, so the first polish misses
 * OBSDock QToolBar metrics. Repolish once they live under the native dock. */
void PolishDockWidgets(QWidget *root)
{
	if (!root) {
		return;
	}
	QList<QWidget *> widgets;
	widgets.push_back(root);
	const QList<QWidget *> children = root->findChildren<QWidget *>();
	for (QWidget *child : children) {
		widgets.push_back(child);
	}
	for (QWidget *widget : widgets) {
		if (!widget->style()) {
			continue;
		}
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
	}
}

class CanvasScenesPage : public QWidget {
public:
	CanvasScenesPage(CanvasDock *canvasDock, QWidget *parent) : QWidget(parent), dock(canvasDock)
	{
		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		pane = dock ? dock->BorrowScenesPane() : nullptr;
		if (pane) {
			layout->addWidget(pane);
			PolishDockWidgets(pane);
			MatchScenesToolbarSpacing(pane);
		}
	}

	~CanvasScenesPage() override
	{
		if (pane && dock) {
			dock->ReturnScenesPane();
		}
		pane = nullptr;
	}

private:
	QPointer<CanvasDock> dock;
	QPointer<QWidget> pane;
};

class CanvasSourcesPage : public QWidget {
	Q_OBJECT
public:
	CanvasSourcesPage(CanvasDock *canvasDock, QWidget *parent) : QWidget(parent), dock(canvasDock)
	{
		auto canvas = dock ? dock->GetCanvas() : nullptr;
		weak = canvas ? obs_canvas_get_weak_canvas(canvas) : nullptr;

		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		sources = new SourcesDock(this);
		layout->addWidget(sources);
		PolishDockWidgets(sources);

		if (dock) {
			connect(dock, &CanvasDock::editingSceneChanged, this, [this] { SyncScene(); });
		}
		if (!canvas) {
			SyncScene();
			return;
		}
		auto sh = obs_canvas_get_signal_handler(canvas);
		if (sh) {
			signal_handler_connect(sh, "channel_change", channel_change, this);
		}
		SyncScene();
	}

	~CanvasSourcesPage() override { Disconnect(); }

	void SyncScene()
	{
		if (!sources) {
			return;
		}
		obs_source_t *current = dock ? dock->GetEditingSceneSource() : nullptr;
		if (!current) {
			auto canvas = obs_weak_canvas_get_canvas(weak);
			if (!canvas) {
				sources->SetScene(nullptr);
				return;
			}
			current = obs_canvas_get_channel(canvas, 0);
			obs_canvas_release(canvas);
			obs_source_t *parent = nullptr;
			while (current && obs_source_get_type(current) == OBS_SOURCE_TYPE_TRANSITION) {
				obs_source_release(parent);
				parent = current;
				current = obs_transition_get_active_source(parent);
			}
			obs_source_release(parent);
		}
		sources->SetScene(current);
		obs_source_release(current);
	}

private:
	QPointer<CanvasDock> dock;
	obs_weak_canvas_t *weak = nullptr;
	SourcesDock *sources = nullptr;

	void Disconnect()
	{
		auto canvas = obs_weak_canvas_get_canvas(weak);
		if (canvas) {
			auto sh = obs_canvas_get_signal_handler(canvas);
			if (sh) {
				signal_handler_disconnect(sh, "channel_change", channel_change, this);
			}
			obs_canvas_release(canvas);
		}
		obs_weak_canvas_release(weak);
		weak = nullptr;
		if (sources) {
			sources->Shutdown();
		}
	}

	static void channel_change(void *data, calldata_t *cd)
	{
		if (calldata_int(cd, "channel") != 0) {
			return;
		}
		auto self = static_cast<CanvasSourcesPage *>(data);
		QMetaObject::invokeMethod(self, [self] { self->SyncScene(); }, Qt::QueuedConnection);
	}
};

class CanvasTransitionsPage : public QWidget {
public:
	CanvasTransitionsPage(CanvasDock *canvasDock, QWidget *parent) : QWidget(parent), dock(canvasDock)
	{
		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		pane = dock ? dock->BorrowTransitionsPane() : nullptr;
		if (pane) {
			layout->addWidget(pane, 0, Qt::AlignTop);
			PolishDockWidgets(pane);
		}
		layout->addStretch(1);
	}

	~CanvasTransitionsPage() override
	{
		if (pane && dock) {
			dock->ReturnTransitionsPane();
		}
		pane = nullptr;
	}

private:
	QPointer<CanvasDock> dock;
	QPointer<QWidget> pane;
};

void ShowMain(bool main)
{
	if (nativeScenes) {
		nativeScenes->setVisible(main);
	}
	if (nativeScenesToolbar) {
		nativeScenesToolbar->setVisible(main);
	}
	if (nativeSources) {
		nativeSources->setVisible(main);
	}
	if (nativeSourcesToolbar) {
		nativeSourcesToolbar->setVisible(main);
	}
	for (auto widget : nativeTransitionWidgets) {
		if (widget) {
			widget->setVisible(main);
		}
	}
	for (size_t i = 0; i < pages.size(); i++) {
		bool on = !main && scenesTabs && scenesTabs->currentIndex() == int(i) + 1;
		if (pages[i].scenes) {
			pages[i].scenes->setVisible(on);
		}
		if (pages[i].sources) {
			pages[i].sources->setVisible(on);
		}
		if (pages[i].transitions) {
			pages[i].transitions->setVisible(on);
		}
	}
}

void ApplyIndex(int index)
{
	if (index < 0) {
		index = 0;
	}
	ShowMain(index == 0);
}

void OnScenesTab(int index)
{
	if (syncing || !alive) {
		return;
	}
	syncing = true;
	if (sourcesTabs && sourcesTabs->currentIndex() != index) {
		sourcesTabs->setCurrentIndex(index);
	}
	if (transitionsTabs && transitionsTabs->currentIndex() != index) {
		transitionsTabs->setCurrentIndex(index);
	}
	ApplyIndex(index);
	syncing = false;
}

void OnSourcesTab(int index)
{
	if (syncing || !alive) {
		return;
	}
	syncing = true;
	if (scenesTabs && scenesTabs->currentIndex() != index) {
		scenesTabs->setCurrentIndex(index);
	}
	if (transitionsTabs && transitionsTabs->currentIndex() != index) {
		transitionsTabs->setCurrentIndex(index);
	}
	ApplyIndex(index);
	syncing = false;
}

void OnTransitionsTab(int index)
{
	if (syncing || !alive) {
		return;
	}
	syncing = true;
	if (scenesTabs && scenesTabs->currentIndex() != index) {
		scenesTabs->setCurrentIndex(index);
	}
	if (sourcesTabs && sourcesTabs->currentIndex() != index) {
		sourcesTabs->setCurrentIndex(index);
	}
	ApplyIndex(index);
	syncing = false;
}

QTabBar *EnsureBar(QWidget *frame, QVBoxLayout *layout, QTabBar *&bar, QWidget *&host, const char *name)
{
	if (!frame || !layout) {
		return nullptr;
	}
	if (!bar) {
		host = new QWidget(frame);
		host->setObjectName(QString::fromUtf8(name) + QString::fromUtf8("Host"));
		host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		auto *row = new QHBoxLayout(host);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(0);
		bar = new QTabBar(host);
		bar->setObjectName(QString::fromUtf8(name));
		bar->setExpanding(false);
		bar->setElideMode(Qt::ElideRight);
		bar->setUsesScrollButtons(true);
		bar->setDrawBase(false);
		bar->setDocumentMode(true);
		bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		bar->setFixedHeight(22);
		/* Tighter than Yami's default tab padding so Scenes | Youtube
		 * stays a single short row inside the dock. */
		bar->setStyleSheet(QString::fromUtf8(
			"QTabBar { font-size: 8pt; }"
			"QTabBar::tab {"
			"  background: palette(mid);"
			"  color: palette(window-text);"
			"  border: 1px solid transparent;"
			"  border-radius: 3px;"
			"  padding: 1px 8px;"
			"  min-width: 0px;"
			"  min-height: 0px;"
			"  margin: 0px;"
			"  margin-right: 2px;"
			"}"
			"QTabBar::tab:hover { background: palette(light); color: palette(window-text); }"
			"QTabBar::tab:selected { background: palette(highlight); color: palette(highlighted-text); }"));
		row->addWidget(bar);
		host->setMaximumHeight(24);
		layout->insertWidget(0, host);
	}
	return bar;
}

void HideSuiteDocks()
{
	auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		return;
	}
	for (const char *id : {"AitumStreamSuiteScenes", "AitumStreamSuiteSources", "AitumStreamSuiteTransitions"}) {
		auto dock = main->findChild<QDockWidget *>(QString::fromUtf8(id));
		if (!dock) {
			continue;
		}
		/* Same menu labels as the OBS docks ("Scenes" / "Sources").
		 * Keep them out of the Docks menu so OBS can remember the real ones. */
		dock->hide();
		if (auto *action = dock->toggleViewAction()) {
			action->setVisible(false);
		}
	}
}

void ClearPages()
{
	for (auto &page : pages) {
		delete page.scenes;
		delete page.sources;
		delete page.transitions;
	}
	pages.clear();
	if (scenesTabs) {
		scenesTabs->blockSignals(true);
		while (scenesTabs->count() > 0) {
			scenesTabs->removeTab(0);
		}
		scenesTabs->blockSignals(false);
	}
	if (sourcesTabs) {
		sourcesTabs->blockSignals(true);
		while (sourcesTabs->count() > 0) {
			sourcesTabs->removeTab(0);
		}
		sourcesTabs->blockSignals(false);
	}
	if (transitionsTabs) {
		transitionsTabs->blockSignals(true);
		while (transitionsTabs->count() > 0) {
			transitionsTabs->removeTab(0);
		}
		transitionsTabs->blockSignals(false);
	}
	ShowMain(true);
}

void BindNative()
{
	auto main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		return;
	}
	/* fromUtf8, not QStringLiteral: the plugin is built against Qt 6.8 and
	 * OBS 32 runs Qt 6.11. Literals can miss objectName comparisons. */
	auto scenesDock = main->findChild<QDockWidget *>(QString::fromUtf8("scenesDock"));
	auto sourcesDock = main->findChild<QDockWidget *>(QString::fromUtf8("sourcesDock"));
	KeepDockFromShrinking(scenesDock);
	KeepDockFromShrinking(sourcesDock);
	auto scenesFrame = scenesDock ? scenesDock->findChild<QWidget *>(QString::fromUtf8("scenesFrame")) : nullptr;
	auto sourcesFrame = sourcesDock ? sourcesDock->findChild<QWidget *>(QString::fromUtf8("sourcesFrame")) : nullptr;
	if (!scenesFrame) {
		scenesFrame = main->findChild<QWidget *>(QString::fromUtf8("scenesFrame"));
	}
	if (!sourcesFrame) {
		sourcesFrame = main->findChild<QWidget *>(QString::fromUtf8("sourcesFrame"));
	}
	scenesLayout = scenesFrame ? qobject_cast<QVBoxLayout *>(scenesFrame->layout()) : nullptr;
	sourcesLayout = sourcesFrame ? qobject_cast<QVBoxLayout *>(sourcesFrame->layout()) : nullptr;
	if (scenesFrame && !scenesLayout && scenesFrame->layout()) {
		blog(LOG_WARNING, "[Stream Suite] scenesFrame layout is '%s', expected QVBoxLayout",
		     scenesFrame->layout()->metaObject()->className());
	}
	if (sourcesFrame && !sourcesLayout && sourcesFrame->layout()) {
		blog(LOG_WARNING, "[Stream Suite] sourcesFrame layout is '%s', expected QVBoxLayout",
		     sourcesFrame->layout()->metaObject()->className());
	}
	if (scenesFrame) {
		nativeScenes = scenesFrame->findChild<QWidget *>(QString::fromUtf8("scenes"));
		nativeScenesToolbar = scenesFrame->findChild<QWidget *>(QString::fromUtf8("scenesToolbar"));
	}
	if (sourcesFrame) {
		nativeSources = sourcesFrame->findChild<QWidget *>(QString::fromUtf8("sources"));
		nativeSourcesToolbar = sourcesFrame->findChild<QWidget *>(QString::fromUtf8("sourcesToolbar"));
	}
	auto transitionsDock = main->findChild<QDockWidget *>(QString::fromUtf8("transitionsDock"));
	auto transitionsFrame =
		transitionsDock ? transitionsDock->findChild<QWidget *>(QString::fromUtf8("transitionsFrame")) : nullptr;
	if (!transitionsFrame) {
		transitionsFrame = main->findChild<QWidget *>(QString::fromUtf8("transitionsFrame"));
	}
	transitionsLayout = transitionsFrame ? qobject_cast<QVBoxLayout *>(transitionsFrame->layout()) : nullptr;
	nativeTransitionWidgets.clear();
	if (transitionsFrame) {
		for (const char *name : {"transitions", "transitionDurationLabel", "transitionDuration", "transitionAdd",
					 "transitionRemove", "transitionProps"}) {
			if (auto widget = transitionsFrame->findChild<QWidget *>(QString::fromUtf8(name))) {
				nativeTransitionWidgets.push_back(widget);
			}
		}
	}
	EnsureBar(scenesFrame, scenesLayout, scenesTabs, scenesTabHost, "StreamSuiteScenesTabs");
	EnsureBar(sourcesFrame, sourcesLayout, sourcesTabs, sourcesTabHost, "StreamSuiteSourcesTabs");
	EnsureBar(transitionsFrame, transitionsLayout, transitionsTabs, transitionsTabHost, "StreamSuiteTransitionsTabs");
	static bool scenesConnected = false;
	static bool sourcesConnected = false;
	static bool transitionsConnected = false;
	if (scenesTabs && !scenesConnected) {
		QObject::connect(scenesTabs, &QTabBar::currentChanged, &OnScenesTab);
		scenesConnected = true;
	}
	if (sourcesTabs && !sourcesConnected) {
		QObject::connect(sourcesTabs, &QTabBar::currentChanged, &OnSourcesTab);
		sourcesConnected = true;
	}
	if (transitionsTabs && !transitionsConnected) {
		QObject::connect(transitionsTabs, &QTabBar::currentChanged, &OnTransitionsTab);
		transitionsConnected = true;
	}
}

} // namespace

bool nativeTabsStopped = false;

void detach_native_canvas_tabs()
{
	ClearPages();
	if (scenesTabHost) {
		scenesTabHost->hide();
	}
	if (sourcesTabHost) {
		sourcesTabHost->hide();
	}
	if (transitionsTabHost) {
		transitionsTabHost->hide();
	}
}

void shutdown_native_canvas_tabs()
{
	nativeTabsStopped = true;
	alive = false;
	ClearPages();
	delete scenesTabHost;
	delete sourcesTabHost;
	delete transitionsTabHost;
	scenesTabHost = nullptr;
	sourcesTabHost = nullptr;
	transitionsTabHost = nullptr;
	scenesTabs = nullptr;
	sourcesTabs = nullptr;
	transitionsTabs = nullptr;
	nativeScenes = nullptr;
	nativeScenesToolbar = nullptr;
	nativeSources = nullptr;
	nativeSourcesToolbar = nullptr;
	nativeTransitionWidgets.clear();
	scenesLayout = nullptr;
	sourcesLayout = nullptr;
	transitionsLayout = nullptr;
	ShowMain(true);
}

void refresh_native_canvas_tabs()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (nativeTabsStopped || !main) {
		return;
	}
	alive = true;
	QList<DockSpan> spans;
	for (const char *id : {"scenesDock", "sourcesDock"}) {
		auto *dock = main->findChild<QDockWidget *>(QString::fromUtf8(id));
		if (!dock) {
			continue;
		}
		spans.append({dock, dock->width(), dock->height()});
	}
	QString keep;
	if (scenesTabs && scenesTabs->currentIndex() > 0 && scenesTabs->currentIndex() - 1 < int(pages.size())) {
		keep = pages[scenesTabs->currentIndex() - 1].uuid;
	}
	ClearPages();
	BindNative();
	HideSuiteDocks();
	if (!scenesTabs || !sourcesTabs || !scenesLayout || !sourcesLayout) {
		blog(LOG_WARNING,
		     "[Stream Suite] Native canvas tabs not installed (tabs=%d/%d layouts=%d/%d canvases=%d)",
		     scenesTabs ? 1 : 0, sourcesTabs ? 1 : 0, scenesLayout ? 1 : 0, sourcesLayout ? 1 : 0,
		     (int)canvas_docks.size());
		return;
	}

	auto scenesDock = static_cast<QMainWindow *>(obs_frontend_get_main_window())
				  ->findChild<QDockWidget *>(QString::fromUtf8("scenesDock"));
	auto sourcesDock = static_cast<QMainWindow *>(obs_frontend_get_main_window())
				   ->findChild<QDockWidget *>(QString::fromUtf8("sourcesDock"));
	auto transitionsDock = static_cast<QMainWindow *>(obs_frontend_get_main_window())
				       ->findChild<QDockWidget *>(QString::fromUtf8("transitionsDock"));
	QString scenesTitle = scenesDock ? scenesDock->windowTitle() : QStringLiteral("Scenes");
	QString sourcesTitle = sourcesDock ? sourcesDock->windowTitle() : QStringLiteral("Sources");
	QString transitionsTitle = transitionsDock ? transitionsDock->windowTitle() : QStringLiteral("Scene Transitions");
	QString transitionWord = QString::fromUtf8(obs_frontend_get_locale_string("Transition"));
	if (transitionWord.isEmpty()) {
		transitionWord = QString::fromUtf8("Transition");
	}

	auto mainCanvas = obs_get_main_canvas();
	scenesTabs->blockSignals(true);
	sourcesTabs->blockSignals(true);
	if (transitionsTabs) {
		transitionsTabs->blockSignals(true);
	}
	scenesTabs->addTab(scenesTitle);
	sourcesTabs->addTab(sourcesTitle);
	if (transitionsTabs) {
		transitionsTabs->addTab(transitionsTitle);
	}

	auto scenesFrame = scenesLayout->parentWidget();
	auto sourcesFrame = sourcesLayout->parentWidget();
	auto transitionsFrame = transitionsLayout ? transitionsLayout->parentWidget() : nullptr;
	int restore = 0;
	int index = 1;
	for (auto *dock : canvas_docks) {
		if (!dock || !dock->GetCanvas()) {
			continue;
		}
		if (mainCanvas && dock->GetCanvas() == mainCanvas) {
			blog(LOG_INFO, "[Stream Suite] Native tabs skip main canvas '%s'",
			     obs_canvas_get_name(dock->GetCanvas()));
			continue;
		}
		auto name = QString::fromUtf8(obs_canvas_get_name(dock->GetCanvas()));
		auto uuid = QString::fromUtf8(obs_canvas_get_uuid(dock->GetCanvas()));
		auto scenesPage = new CanvasScenesPage(dock, scenesFrame);
		auto sourcesPage = new CanvasSourcesPage(dock, sourcesFrame);
		QWidget *transitionsPage = nullptr;
		scenesPage->hide();
		sourcesPage->hide();
		scenesLayout->insertWidget(index, scenesPage);
		sourcesLayout->insertWidget(index, sourcesPage);
		scenesTabs->addTab(name);
		sourcesTabs->addTab(name);
		if (transitionsTabs && transitionsLayout && transitionsFrame) {
			transitionsPage = new CanvasTransitionsPage(dock, transitionsFrame);
			transitionsPage->hide();
			transitionsLayout->insertWidget(index, transitionsPage);
			transitionsTabs->addTab(transitionWord + QString::fromUtf8(" ") + name);
		}
		if (!keep.isEmpty() && keep == uuid) {
			restore = index;
		}
		pages.push_back({dock, uuid, scenesPage, sourcesPage, transitionsPage});
		index++;
	}
	obs_canvas_release(mainCanvas);

	bool show = pages.size() > 0;
	if (scenesTabHost) {
		scenesTabHost->setVisible(show);
	}
	if (sourcesTabHost) {
		sourcesTabHost->setVisible(show);
	}
	if (transitionsTabHost) {
		transitionsTabHost->setVisible(show);
	}
	scenesTabs->setVisible(show);
	sourcesTabs->setVisible(show);
	if (transitionsTabs) {
		transitionsTabs->setVisible(show);
		transitionsTabs->setCurrentIndex(restore);
		transitionsTabs->blockSignals(false);
	}
	scenesTabs->setCurrentIndex(restore);
	sourcesTabs->setCurrentIndex(restore);
	scenesTabs->blockSignals(false);
	sourcesTabs->blockSignals(false);
	if (show) {
		if (scenesDock && !scenesDock->isVisible()) {
			scenesDock->show();
		}
		if (sourcesDock && !sourcesDock->isVisible()) {
			sourcesDock->show();
		}
		if (transitionsDock && !transitionsDock->isVisible()) {
			transitionsDock->show();
		}
		if (scenesTabHost && scenesTabHost->parentWidget() && scenesTabHost->parentWidget()->layout()) {
			scenesTabHost->parentWidget()->layout()->activate();
		}
		if (sourcesTabHost && sourcesTabHost->parentWidget() && sourcesTabHost->parentWidget()->layout()) {
			sourcesTabHost->parentWidget()->layout()->activate();
		}
		if (transitionsTabHost && transitionsTabHost->parentWidget() && transitionsTabHost->parentWidget()->layout()) {
			transitionsTabHost->parentWidget()->layout()->activate();
		}
	}
	ApplyIndex(restore);
	RestoreNativeDockSizes(main, spans);
	QTimer::singleShot(0, main, [main, spans] { RestoreNativeDockSizes(main, spans); });
	blog(LOG_INFO, "[Stream Suite] Native canvas tabs: %d extra canvas(es), host %dx%d dock %s", (int)pages.size(),
	     scenesTabHost ? scenesTabHost->width() : -1, scenesTabHost ? scenesTabHost->height() : -1,
	     scenesDock && scenesDock->isVisible() ? "shown" : "hidden");
}

#include "native-dock-tabs.moc"
