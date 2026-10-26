#include "native-dock-tabs.hpp"
#include "controls-other-options.hpp"
#include "canvas-clone-dock.hpp"
#include "canvas-dock.hpp"
#include "sources-dock.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QKeySequence>
#include <QMainWindow>
#include <QPointer>
#include <QSizePolicy>
#include <QStyle>
#include <QTabBar>
#include <QToolBar>
#include <QTimer>
#include <QVBoxLayout>

extern std::list<CanvasDock *> canvas_docks;
extern std::list<CanvasCloneDock *> canvas_clone_docks;

static void ensure_preview_docks_independent();
extern bool nativeTabsStopped;

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

/* Same metrics as OBSBasic.ui scenesToolbar. A local stylesheet drops
 * Yami's max icon size and draws the actions larger than the native bar. */
void MatchNativeToolbar(QWidget *pane, const char *name, QWidget *nativeWidget)
{
	if (!pane) {
		return;
	}
	auto *toolbar = pane->findChild<QToolBar *>(QString::fromUtf8(name));
	auto *native = qobject_cast<QToolBar *>(nativeWidget);
	if (!toolbar || !native) {
		return;
	}
	toolbar->setStyleSheet(QString());
	toolbar->setIconSize(native->iconSize());
	toolbar->setFloatable(native->isFloatable());
	toolbar->setMovable(native->isMovable());
	toolbar->setToolButtonStyle(native->toolButtonStyle());
	toolbar->setContentsMargins(native->contentsMargins());
	if (toolbar->layout() && native->layout()) {
		toolbar->layout()->setContentsMargins(native->layout()->contentsMargins());
		toolbar->layout()->setSpacing(native->layout()->spacing());
	}
	toolbar->style()->unpolish(toolbar);
	toolbar->style()->polish(toolbar);
	const auto buttons = toolbar->findChildren<QWidget *>();
	for (QWidget *button : buttons) {
		button->setStyleSheet(QString());
		if (!button->style()) {
			continue;
		}
		button->style()->unpolish(button);
		button->style()->polish(button);
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
			MatchNativeToolbar(pane, "scenesToolbar", nativeScenesToolbar);
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
		MatchNativeToolbar(sources, "sourcesToolbar", nativeSourcesToolbar);

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

void SetNativeRemoveShortcutsEnabled(bool enabled)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		return;
	}
	/* OBSBasic owns actionRemoveSource/Scene with WidgetWithChildrenShortcut
	 * on the main window — Del always hits the Main canvas scene. Disable
	 * them on Youtube/Vertical tabs so our docks handle multi-delete. */
	if (auto *removeSource = main->findChild<QAction *>(QStringLiteral("actionRemoveSource"))) {
		if (enabled) {
			removeSource->setShortcut(QKeySequence(QStringLiteral("Del")));
		} else {
			removeSource->setShortcut(QKeySequence());
		}
	}
	if (auto *removeScene = main->findChild<QAction *>(QStringLiteral("actionRemoveScene"))) {
		if (enabled) {
			removeScene->setShortcut(QKeySequence(QStringLiteral("Del")));
		} else {
			removeScene->setShortcut(QKeySequence());
		}
	}
}

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
	SetNativeRemoveShortcutsEnabled(main);
}

bool syncingSoloPreviewSwap = false;

QList<QDockWidget *> CollectPreviewDocks(QMainWindow *mainWin)
{
	QList<QDockWidget *> previewDocks;
	if (!mainWin) {
		return previewDocks;
	}
	if (auto *mainCanvasDock =
		    mainWin->findChild<QDockWidget *>(QString::fromUtf8("AitumStreamSuiteMainCanvas"))) {
		previewDocks.append(mainCanvasDock);
	}
	for (const auto &it : canvas_docks) {
		if (auto *dock = qobject_cast<QDockWidget *>(it->parentWidget())) {
			if (!previewDocks.contains(dock)) {
				previewDocks.append(dock);
			}
		}
	}
	for (const auto &it : canvas_clone_docks) {
		if (auto *dock = qobject_cast<QDockWidget *>(it->parentWidget())) {
			if (!previewDocks.contains(dock)) {
				previewDocks.append(dock);
			}
		}
	}
	return previewDocks;
}

/* Break preview↔preview tab groups so docks stay independently movable.
 * Does not touch Controls or force Top (that scrambled the layout before). */
void UntabifyPreviewDocksFromEachOther(QMainWindow *main, const QList<QDockWidget *> &previews)
{
	if (!main) {
		return;
	}
	for (QDockWidget *dock : previews) {
		if (!dock) {
			continue;
		}
		bool withPeer = false;
		for (QDockWidget *peer : main->tabifiedDockWidgets(dock)) {
			if (previews.contains(peer)) {
				withPeer = true;
				break;
			}
		}
		if (!withPeer) {
			continue;
		}

		const bool floating = dock->isFloating();
		Qt::DockWidgetArea area = main->dockWidgetArea(dock);
		if (area == Qt::NoDockWidgetArea) {
			area = Qt::RightDockWidgetArea;
		}
		const bool visible = dock->isVisible();
		const QSize sz = dock->size();
		const QRect geo = dock->geometry();

		main->removeDockWidget(dock);
		main->addDockWidget(area, dock);
		if (floating) {
			dock->setFloating(true);
			dock->setGeometry(geo);
		} else {
			dock->resize(sz);
		}
		dock->setVisible(visible);
	}
}

void EnsurePreviewDocksFreeWhenMulti(QMainWindow *mainWin)
{
	if (syncingSoloPreviewSwap || nativeTabsStopped || !mainWin) {
		return;
	}
	const QList<QDockWidget *> previews = CollectPreviewDocks(mainWin);
	int visibleCount = 0;
	for (QDockWidget *dock : previews) {
		if (dock && dock->isVisible()) {
			visibleCount++;
		}
	}
	if (visibleCount >= 2) {
		UntabifyPreviewDocksFromEachOther(mainWin, previews);
	}
}

void BindPreviewDockVisibilityWatchers(QMainWindow *mainWin)
{
	if (!mainWin) {
		return;
	}
	for (QDockWidget *dock : CollectPreviewDocks(mainWin)) {
		if (!dock || dock->property("aitumPreviewWatch").toBool()) {
			continue;
		}
		dock->setProperty("aitumPreviewWatch", true);
		QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [mainWin](bool) {
			EnsurePreviewDocksFreeWhenMulti(mainWin);
		});
	}
}

/* When exactly one preview dock is visible, align it with the Scenes tab.
 * 0 or ≥2 visible → leave layout alone (and untabify if multi so docks stay free). */
void SyncSoloPreviewDockToTab(int index)
{
	if (nativeTabsStopped) {
		return;
	}
	auto *mainWin = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWin) {
		return;
	}

	BindPreviewDockVisibilityWatchers(mainWin);

	auto *mainCanvasDock =
		mainWin->findChild<QDockWidget *>(QString::fromUtf8("AitumStreamSuiteMainCanvas"));
	const QList<QDockWidget *> previewDocks = CollectPreviewDocks(mainWin);

	QDockWidget *visibleOne = nullptr;
	int visibleCount = 0;
	for (QDockWidget *dock : previewDocks) {
		if (dock && dock->isVisible()) {
			visibleCount++;
			visibleOne = dock;
		}
	}
	if (visibleCount != 1 || !visibleOne) {
		if (visibleCount >= 2) {
			UntabifyPreviewDocksFromEachOther(mainWin, previewDocks);
		}
		return;
	}

	QDockWidget *target = nullptr;
	if (index <= 0) {
		target = mainCanvasDock;
	} else if (index >= 1 && index <= (int)pages.size()) {
		CanvasDock *canvasDock = pages[size_t(index - 1)].dock;
		if (canvasDock) {
			target = qobject_cast<QDockWidget *>(canvasDock->parentWidget());
		}
	}
	if (!target || target == visibleOne) {
		return;
	}

	/* Replace in-place so Controls / other docks keep their side and size.
	 * Do NOT addDockWidget(area) or ensure_preview_docks_independent here —
	 * those relocate to Top and scramble the layout. */
	syncingSoloPreviewSwap = true;
	if (visibleOne->isFloating()) {
		const QRect geo = visibleOne->geometry();
		visibleOne->hide();
		target->setFloating(true);
		target->show();
		target->setGeometry(geo);
		syncingSoloPreviewSwap = false;
		return;
	}

	const QList<QDockWidget *> withVisible = mainWin->tabifiedDockWidgets(visibleOne);
	if (!withVisible.contains(target)) {
		mainWin->tabifyDockWidget(visibleOne, target);
	}
	visibleOne->hide();
	target->show();
	target->raise();
	syncingSoloPreviewSwap = false;
}

void ApplyIndex(int index)
{
	if (index < 0) {
		index = 0;
	}
	ShowMain(index == 0);
	SyncSoloPreviewDockToTab(index);
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
	shutdown_controls_other_options_tab();
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
	install_controls_other_options_tab();
	alive = true;
	QString keep;
	if (scenesTabs && scenesTabs->currentIndex() > 0 && scenesTabs->currentIndex() - 1 < int(pages.size())) {
		keep = pages[scenesTabs->currentIndex() - 1].uuid;
	}
	ClearPages();
	BindNative();
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
	/* Do not force-show docks here: OBS DockState owns visibility and size. */
	ApplyIndex(restore);
	ensure_preview_docks_independent();
	blog(LOG_INFO, "[Stream Suite] Native canvas tabs: %d extra canvas(es), host %dx%d dock %s", (int)pages.size(),
	     scenesTabHost ? scenesTabHost->width() : -1, scenesTabHost ? scenesTabHost->height() : -1,
	     scenesDock && scenesDock->isVisible() ? "shown" : "hidden");
}

static QByteArray ReadObsDockState()
{
	config_t *userConfig = obs_frontend_get_user_config();
	if (!userConfig) {
		return {};
	}
	const char *dockStateStr = config_get_string(userConfig, "BasicWindow", "DockState");
	if (!dockStateStr || !*dockStateStr) {
		return {};
	}
	return QByteArray::fromBase64(QByteArray(dockStateStr));
}

static void UntabifyFromControls(QMainWindow *main, QDockWidget *dock, QDockWidget *controls)
{
	if (!main || !dock || !controls || dock == controls) {
		return;
	}
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			  QDockWidget::DockWidgetFloatable);

	const QList<QDockWidget *> withControls = main->tabifiedDockWidgets(controls);
	if (!withControls.contains(dock)) {
		return;
	}

	const bool visible = dock->isVisible();
	const bool floating = dock->isFloating();
	/* Drop out of the Controls tab group and place as its own dock
	 * (Top is away from the usual bottom Controls strip). */
	main->removeDockWidget(dock);
	main->addDockWidget(Qt::TopDockWidgetArea, dock);
	if (floating) {
		dock->setFloating(true);
	}
	dock->setVisible(visible);
	blog(LOG_INFO, "[Stream Suite] Untabified '%s' from Controls", dock->objectName().toUtf8().constData());
}

static void ensure_preview_docks_independent()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main || nativeTabsStopped) {
		return;
	}
	auto *controls = main->findChild<QDockWidget *>(QString::fromUtf8("controlsDock"));
	if (!controls) {
		return;
	}

	static const char *kPreviewDockIds[] = {"AitumStreamSuiteMainCanvas"};
	for (const char *id : kPreviewDockIds) {
		if (auto *dock = main->findChild<QDockWidget *>(QString::fromUtf8(id))) {
			UntabifyFromControls(main, dock, controls);
		}
	}
	for (const auto &it : canvas_docks) {
		if (auto *dock = qobject_cast<QDockWidget *>(it->parentWidget())) {
			UntabifyFromControls(main, dock, controls);
		}
	}
	for (const auto &it : canvas_clone_docks) {
		if (auto *dock = qobject_cast<QDockWidget *>(it->parentWidget())) {
			UntabifyFromControls(main, dock, controls);
		}
	}
}

static void restore_obs_dock_layout()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main || nativeTabsStopped || !main->isVisible()) {
		return;
	}
	static bool restoring = false;
	if (restoring) {
		return;
	}
	const QByteArray dockState = ReadObsDockState();
	if (dockState.isEmpty()) {
		ensure_preview_docks_independent();
		return;
	}
	restoring = true;
	const bool ok = main->restoreState(dockState);
	ensure_preview_docks_independent();
	restoring = false;
	if (!ok) {
		blog(LOG_WARNING, "[Stream Suite] restoreState(DockState) failed");
	}
}

void schedule_restore_obs_dock_layout()
{
	if (nativeTabsStopped) {
		return;
	}
	/* One delayed pass after canvas docks + preview rebuild finish. */
	static int generation = 0;
	const int gen = ++generation;
	QTimer::singleShot(250, qApp, [gen] {
		if (nativeTabsStopped || gen != generation) {
			return;
		}
		restore_obs_dock_layout();
	});
}

#include "native-dock-tabs.moc"
