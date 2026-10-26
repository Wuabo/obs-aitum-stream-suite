#include "native-dock-tabs.hpp"
#include "controls-other-options.hpp"
#include "canvas-clone-dock.hpp"
#include "canvas-dock.hpp"
#include "sources-dock.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QHBoxLayout>
#include <QDockWidget>
#include <QKeySequence>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
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

/* Same path rule as OBSBasicStatusBar::GetIcon. Do not use availableSizes() —
 * SVG theme icons report empty sizes and we would load black :/res glyphs. */
static QIcon ThemeFileIcon(const char *fileName)
{
	if (obs_frontend_is_theme_dark()) {
		return QIcon(QStringLiteral("theme:Dark/") + QString::fromUtf8(fileName));
	}
	return QIcon(QStringLiteral(":/res/images/") + QString::fromUtf8(fileName));
}

static const char *IconFileForClass(const QString &cls)
{
	if (cls.contains(QString::fromUtf8("icon-trash"))) {
		return "trash.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-plus"))) {
		return "plus.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-minus"))) {
		return "minus.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-filter"))) {
		return "filter.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-up"))) {
		return "up.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-down"))) {
		return "down.svg";
	}
	if (cls.contains(QString::fromUtf8("icon-gear"))) {
		return "settings/general.svg";
	}
	return nullptr;
}

/* Keep every toolbar button on the same themed SVG set as native OBS. */
void RefreshToolbarThemeIcons(QToolBar *toolbar)
{
	if (!toolbar) {
		return;
	}
	for (QAction *action : toolbar->actions()) {
		if (!action) {
			continue;
		}
		const QString cls = action->property("class").toString();
		if (const char *file = IconFileForClass(cls)) {
			action->setIcon(ThemeFileIcon(file));
		}
		QWidget *button = toolbar->widgetForAction(action);
		if (!button || !button->style()) {
			continue;
		}
		for (const QByteArray &prop : action->dynamicPropertyNames()) {
			button->setProperty(prop.constData(), action->property(prop.constData()));
		}
		if (const char *file = IconFileForClass(cls)) {
			if (auto *toolBtn = qobject_cast<QAbstractButton *>(button)) {
				toolBtn->setIcon(ThemeFileIcon(file));
			}
		}
		button->style()->unpolish(button);
		button->style()->polish(button);
	}
}

/* Copy metrics from the OBS scenes/sources toolbar. Never setStyleSheet here —
 * an empty widget stylesheet blocks Yami (.icon-trash → themed trash icon). */
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
	RefreshToolbarThemeIcons(toolbar);
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
			MatchNativeToolbar(pane, "AitumCanvasScenesToolbar", nativeScenesToolbar);
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
		MatchNativeToolbar(sources, "AitumSourcesToolbar", nativeSourcesToolbar);

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

void SyncVisibleToolbarIcons()
{
	RefreshToolbarThemeIcons(qobject_cast<QToolBar *>(nativeScenesToolbar));
	RefreshToolbarThemeIcons(qobject_cast<QToolBar *>(nativeSourcesToolbar));
	for (auto &page : pages) {
		if (page.scenes && page.scenes->isVisible()) {
			if (auto *tb = page.scenes->findChild<QToolBar *>(QString::fromUtf8("AitumCanvasScenesToolbar"))) {
				RefreshToolbarThemeIcons(tb);
			}
		}
		if (page.sources && page.sources->isVisible()) {
			if (auto *tb = page.sources->findChild<QToolBar *>(QString::fromUtf8("AitumSourcesToolbar"))) {
				RefreshToolbarThemeIcons(tb);
			}
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
	/* After show/hide + first reparent into OBSDock, force the same
	 * theme:Dark (or :/res) glyph set so colors don't drift vs native. */
	SyncVisibleToolbarIcons();
	QTimer::singleShot(0, &SyncVisibleToolbarIcons);
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

static bool PreviewTabbedWithPeer(QMainWindow *main, QDockWidget *dock, const QList<QDockWidget *> &previews)
{
	if (!main || !dock) {
		return false;
	}
	for (QDockWidget *peer : main->tabifiedDockWidgets(dock)) {
		if (previews.contains(peer)) {
			return true;
		}
	}
	return false;
}

static void RestoreControlsArea(QMainWindow *main, QDockWidget *controls, Qt::DockWidgetArea area, bool visible)
{
	if (!main || !controls || area == Qt::NoDockWidgetArea) {
		return;
	}
	if (main->dockWidgetArea(controls) == area) {
		return;
	}
	const bool floating = controls->isFloating();
	main->addDockWidget(area, controls);
	if (floating) {
		controls->setFloating(true);
	}
	controls->setVisible(visible);
}

/* Break preview↔preview tabs without removeDockWidget (that shoved Controls). */
void UntabifyPreviewDocksFromEachOther(QMainWindow *main, const QList<QDockWidget *> &previews, bool showAllPeers)
{
	if (!main) {
		return;
	}
	auto *controls = main->findChild<QDockWidget *>(QString::fromUtf8("controlsDock"));
	const Qt::DockWidgetArea controlsArea = controls ? main->dockWidgetArea(controls) : Qt::NoDockWidgetArea;
	const bool controlsVis = controls && controls->isVisible();

	QSet<QDockWidget *> processed;
	for (QDockWidget *dock : previews) {
		if (!dock || processed.contains(dock)) {
			continue;
		}
		QList<QDockWidget *> peers;
		for (QDockWidget *peer : main->tabifiedDockWidgets(dock)) {
			if (previews.contains(peer)) {
				peers.append(peer);
			}
		}
		if (peers.isEmpty()) {
			continue;
		}

		processed.insert(dock);
		Qt::DockWidgetArea area = main->dockWidgetArea(dock);
		if (area == Qt::NoDockWidgetArea) {
			area = Qt::RightDockWidgetArea;
		}

		for (QDockWidget *peer : peers) {
			processed.insert(peer);
			const bool peerVis = showAllPeers ? true : peer->isVisible();
			const bool anchorVis = showAllPeers ? true : dock->isVisible();
			peer->setFloating(true);
			peer->show();
			main->addDockWidget(area, peer);
			peer->setFloating(false);
			main->splitDockWidget(dock, peer, Qt::Horizontal);
			dock->setVisible(anchorVis);
			peer->setVisible(peerVis);
			dock->setProperty("aitumSoloActive", false);
			peer->setProperty("aitumSoloActive", false);
		}
	}

	RestoreControlsArea(main, controls, controlsArea, controlsVis);
}

void HandlePreviewVisibilityChanged(QMainWindow *mainWin, QDockWidget *changed, bool visible)
{
	if (syncingSoloPreviewSwap || nativeTabsStopped || !mainWin || !changed) {
		return;
	}
	const QList<QDockWidget *> previews = CollectPreviewDocks(mainWin);
	if (!previews.contains(changed)) {
		return;
	}

	int visibleCount = 0;
	for (QDockWidget *dock : previews) {
		if (dock && dock->isVisible()) {
			visibleCount++;
		}
	}

	const bool tabbed = PreviewTabbedWithPeer(mainWin, changed, previews);

	/* Two+ raised previews must never stay tabbed. */
	if (visibleCount >= 2 && tabbed) {
		UntabifyPreviewDocksFromEachOther(mainWin, previews, false);
		return;
	}

	/* Docks menu raised a hidden peer from a solo tab group → show both
	 * side-by-side. Skip if this dock is the intentional solo active one. */
	if (visible && tabbed && visibleCount == 1 && !changed->property("aitumSoloActive").toBool()) {
		UntabifyPreviewDocksFromEachOther(mainWin, previews, true);
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
		QObject::connect(dock, &QDockWidget::visibilityChanged, dock, [mainWin, dock](bool visible) {
			HandlePreviewVisibilityChanged(mainWin, dock, visible);
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
			UntabifyPreviewDocksFromEachOther(mainWin, previewDocks, false);
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
		visibleOne->setProperty("aitumSoloActive", true);
		return;
	}

	/* Replace in-place so Controls / other docks keep their side and size. */
	syncingSoloPreviewSwap = true;
	for (QDockWidget *dock : previewDocks) {
		if (dock) {
			dock->setProperty("aitumSoloActive", false);
		}
	}
	if (visibleOne->isFloating()) {
		const QRect geo = visibleOne->geometry();
		visibleOne->hide();
		target->setProperty("aitumSoloActive", true);
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
	target->setProperty("aitumSoloActive", true);
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
		nativeScenes = scenesFrame->findChild<QWidget *>(QString::fromUtf8("scenes"), Qt::FindDirectChildrenOnly);
		nativeScenesToolbar =
			scenesFrame->findChild<QWidget *>(QString::fromUtf8("scenesToolbar"), Qt::FindDirectChildrenOnly);
	}
	if (sourcesFrame) {
		nativeSources = sourcesFrame->findChild<QWidget *>(QString::fromUtf8("sources"), Qt::FindDirectChildrenOnly);
		nativeSourcesToolbar =
			sourcesFrame->findChild<QWidget *>(QString::fromUtf8("sourcesToolbar"), Qt::FindDirectChildrenOnly);
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
	RefreshToolbarThemeIcons(qobject_cast<QToolBar *>(nativeScenesToolbar));
	RefreshToolbarThemeIcons(qobject_cast<QToolBar *>(nativeSourcesToolbar));
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
	const Qt::DockWidgetArea controlsArea = main->dockWidgetArea(controls);
	const bool controlsVis = controls->isVisible();
	/* Split beside Controls in the same area — never force Top / removeDock
	 * (that shoved Controls to the left on first launch). */
	dock->setFloating(true);
	dock->show();
	Qt::DockWidgetArea area = controlsArea != Qt::NoDockWidgetArea ? controlsArea : Qt::RightDockWidgetArea;
	main->addDockWidget(area, dock);
	dock->setFloating(false);
	main->splitDockWidget(controls, dock, Qt::Horizontal);
	dock->setVisible(visible);
	if (controlsArea != Qt::NoDockWidgetArea && main->dockWidgetArea(controls) != controlsArea) {
		const bool floating = controls->isFloating();
		main->addDockWidget(controlsArea, controls);
		if (floating) {
			controls->setFloating(true);
		}
		controls->setVisible(controlsVis);
	}
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
