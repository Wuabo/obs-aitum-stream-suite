#include "main-canvas-context-fix.hpp"

#include "../utils/widgets/source-toolbar.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QAbstractItemView>
#include <QChildEvent>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QItemSelectionModel>
#include <QLayout>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

static bool FindSelectedItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_selected(item)) {
		*static_cast<obs_sceneitem_t **>(param) = item;
		return false;
	}
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, FindSelectedItem, param);
		if (*static_cast<obs_sceneitem_t **>(param)) {
			return false;
		}
	}
	return true;
}

static obs_sceneitem_t *GetMainSelectedItem()
{
	obs_source_t *sceneSource = nullptr;
	if (obs_frontend_preview_program_mode_active()) {
		sceneSource = obs_frontend_get_current_preview_scene();
	} else {
		sceneSource = obs_frontend_get_current_scene();
	}
	if (!sceneSource) {
		return nullptr;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_source_release(sceneSource);
	if (!scene) {
		return nullptr;
	}
	obs_sceneitem_t *item = nullptr;
	obs_scene_enum_items(scene, FindSelectedItem, &item);
	return item;
}

class MainCanvasContextBarFix : public QObject {
	Q_OBJECT

	QPointer<QFrame> emptySpace;
	QPointer<QWidget> canvasRoot;
	QPointer<QPushButton> propertiesButton;
	QPointer<QPushButton> filtersButton;
	QPointer<QPushButton> interactButton;
	bool syncQueued = false;

public:
	explicit MainCanvasContextBarFix(QObject *parent = nullptr) : QObject(parent) {}

	void Attach(QWidget *root)
	{
		if (!root) {
			return;
		}
		canvasRoot = root;
		emptySpace = root->findChild<QFrame *>(QStringLiteral("emptySpace"));
		propertiesButton = root->findChild<QPushButton *>(QStringLiteral("sourcePropertiesButton"));
		filtersButton = root->findChild<QPushButton *>(QStringLiteral("sourceFiltersButton"));
		interactButton = root->findChild<QPushButton *>(QStringLiteral("sourceInteractButton"));

		root->installEventFilter(this);
		if (emptySpace) {
			emptySpace->installEventFilter(this);
		}

		auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		if (main) {
			if (auto *sources = main->findChild<QAbstractItemView *>(QStringLiteral("sources"))) {
				if (sources->selectionModel()) {
					connect(sources->selectionModel(), &QItemSelectionModel::selectionChanged, this,
						&MainCanvasContextBarFix::QueueSync, Qt::UniqueConnection);
				}
			}
		}

		QueueSync();
	}

public slots:
	void QueueSync()
	{
		if (syncQueued) {
			return;
		}
		syncQueued = true;
		/* Run after OBSBasic::UpdateContextBar (often QueuedConnection). */
		QTimer::singleShot(0, this, [this] {
			syncQueued = false;
			Sync();
		});
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		switch (event->type()) {
		case QEvent::Resize:
		case QEvent::Show:
		case QEvent::LayoutRequest:
			QueueSync();
			break;
		case QEvent::ChildRemoved:
			if (watched == emptySpace) {
				QueueSync();
			}
			break;
		default:
			break;
		}
		return QObject::eventFilter(watched, event);
	}

private:
	void ClearEmptySpace()
	{
		if (!emptySpace || !emptySpace->layout()) {
			return;
		}
		QLayout *layout = emptySpace->layout();
		while (QLayoutItem *la = layout->takeAt(0)) {
			delete la->widget();
			delete la;
		}
	}

	void ApplyReducedChrome(int width)
	{
		const bool iconOnly = width < 900;
		if (propertiesButton) {
			propertiesButton->setText(iconOnly ? QString()
							   : QString::fromUtf8(obs_frontend_get_locale_string("Properties")));
		}
		if (filtersButton) {
			filtersButton->setText(iconOnly ? QString()
							: QString::fromUtf8(obs_frontend_get_locale_string("Filters")));
		}
		if (interactButton && interactButton->isVisible()) {
			interactButton->setText(iconOnly ? QString()
							 : QString::fromUtf8(obs_frontend_get_locale_string("Interact")));
		}
	}

	void Sync()
	{
		if (!emptySpace || !emptySpace->layout()) {
			if (canvasRoot) {
				emptySpace = canvasRoot->findChild<QFrame *>(QStringLiteral("emptySpace"));
				if (emptySpace) {
					emptySpace->installEventFilter(this);
				}
			}
			if (!emptySpace || !emptySpace->layout()) {
				return;
			}
		}

		const int width = canvasRoot ? canvasRoot->width() : emptySpace->width();
		ApplyReducedChrome(width);

		/* OBS still populates toolbars when width >= 600; leave those alone. */
		if (width >= 600) {
			return;
		}

		obs_sceneitem_t *item = GetMainSelectedItem();
		if (!item) {
			ClearEmptySpace();
			return;
		}

		obs_source_t *source = obs_sceneitem_get_source(item);
		QLayoutItem *la = emptySpace->layout()->itemAt(0);
		if (la && la->widget()) {
			OBSSource current = ContextToolbarGetSource(la->widget());
			if (current == source) {
				return;
			}
		}

		ClearEmptySpace();
		if (QWidget *widget = CreateSourceContextToolbar(emptySpace, source)) {
			emptySpace->layout()->addWidget(widget);
		}
	}
};

static MainCanvasContextBarFix *g_fix = nullptr;

void install_main_canvas_context_bar_fix()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		return;
	}

	QWidget *root = nullptr;
	if (auto *dock = main->findChild<QDockWidget *>(QStringLiteral("AitumStreamSuiteMainCanvas"))) {
		root = dock->widget();
	}
	if (!root) {
		root = main->findChild<QWidget *>(QStringLiteral("centralwidget"));
	}
	if (!root || !root->findChild<QWidget *>(QStringLiteral("canvasEditor"))) {
		return;
	}

	if (!g_fix) {
		g_fix = new MainCanvasContextBarFix(main);
	}
	g_fix->Attach(root);
}

void shutdown_main_canvas_context_bar_fix()
{
	delete g_fix;
	g_fix = nullptr;
}

#include "main-canvas-context-fix.moc"
