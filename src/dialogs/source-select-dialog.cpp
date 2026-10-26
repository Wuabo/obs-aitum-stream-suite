#include "source-select-dialog.hpp"

#include "../utils/icon.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <graphics/graphics.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QImage>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleOptionButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <mutex>

class FlowLayout : public QLayout {
public:
	explicit FlowLayout(QWidget *parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
	~FlowLayout() override
	{
		QLayoutItem *item;
		while ((item = takeAt(0)))
			delete item;
	}

	void addItem(QLayoutItem *item) override { items.append(item); }
	int count() const override { return items.size(); }
	QLayoutItem *itemAt(int index) const override { return items.value(index); }
	QLayoutItem *takeAt(int index) override
	{
		return (index >= 0 && index < items.size()) ? items.takeAt(index) : nullptr;
	}
	Qt::Orientations expandingDirections() const override { return {}; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return Layout(QRect(0, 0, width, 0), true); }
	void setGeometry(const QRect &rect) override
	{
		QLayout::setGeometry(rect);
		Layout(rect, false);
	}
	QSize sizeHint() const override { return minimumSize(); }
	QSize minimumSize() const override
	{
		QSize size;
		for (const QLayoutItem *item : items)
			size = size.expandedTo(item->minimumSize());
		return size;
	}

private:
	int Layout(const QRect &rect, bool test) const
	{
		int x = rect.x();
		int y = rect.y();
		int lineHeight = 0;
		for (QLayoutItem *item : items) {
			int nextX = x + item->sizeHint().width() + spacing();
			if (nextX - spacing() > rect.right() && lineHeight > 0) {
				x = rect.x();
				y += lineHeight + spacing();
				nextX = x + item->sizeHint().width() + spacing();
				lineHeight = 0;
			}
			if (!test)
				item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
			x = nextX;
			lineHeight = qMax(lineHeight, item->sizeHint().height());
		}
		return y + lineHeight - rect.y();
	}

	QList<QLayoutItem *> items;
};

namespace {

const QString kRecentTypeId = QString::fromUtf8("_recent");

QString Locale(const char *key)
{
	return QString::fromUtf8(obs_frontend_get_locale_string(key));
}

constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 180;

struct PreviewJob {
	OBSWeakSource weak;
	QPointer<SourceSelectButton> button;
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurf = nullptr;
	QImage image;
	int stage = 0;
	int retries = 0;
	int cooldown = 0;
	/* Set when the pointer re-enters while this capture is already rendering. */
	bool again = false;
	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
};

std::mutex previewMutex;
std::deque<PreviewJob *> previewJobs;
PreviewJob *previewActive = nullptr;
bool previewTicking = false;
bool previewShutdown = false;

void DestroyPreviewGpu(PreviewJob *job)
{
	if (!job)
		return;
	if (job->stagesurf)
		gs_stagesurface_destroy(job->stagesurf);
	if (job->texrender)
		gs_texrender_destroy(job->texrender);
	job->stagesurf = nullptr;
	job->texrender = nullptr;
}

enum class PreviewStep { Stay, Retry, Done };

PreviewStep StepPreview(PreviewJob *job)
{
	if (!job->button)
		return PreviewStep::Done;

	OBSSource source = OBSGetStrongRef(job->weak);
	if (!source)
		return PreviewStep::Done;

	if (job->stage == 0) {
		job->sourceWidth = obs_source_get_width(source);
		job->sourceHeight = obs_source_get_height(source);
		if (!job->sourceWidth || !job->sourceHeight) {
			job->retries++;
			job->cooldown = 15;
			return job->retries >= 8 ? PreviewStep::Done : PreviewStep::Retry;
		}

		job->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		job->stagesurf = gs_stagesurface_create(kPreviewWidth, kPreviewHeight, GS_RGBA);
		gs_texrender_reset(job->texrender);
		if (!gs_texrender_begin(job->texrender, kPreviewWidth, kPreviewHeight)) {
			DestroyPreviewGpu(job);
			job->retries++;
			return job->retries >= 8 ? PreviewStep::Done : PreviewStep::Retry;
		}

		vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);

		float scale = 1.0f;
		int x = 0;
		int y = 0;
		double windowAspect = double(kPreviewWidth) / double(kPreviewHeight);
		double sourceAspect = double(job->sourceWidth) / double(job->sourceHeight);
		if (windowAspect > sourceAspect) {
			scale = float(kPreviewHeight) / float(job->sourceHeight);
			x = int((kPreviewWidth - int(float(job->sourceWidth) * scale)) / 2);
		} else {
			scale = float(kPreviewWidth) / float(job->sourceWidth);
			y = int((kPreviewHeight - int(float(job->sourceHeight) * scale)) / 2);
		}
		int scaledWidth = int(scale * float(job->sourceWidth));
		int scaledHeight = int(scale * float(job->sourceHeight));

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, float(job->sourceWidth), 0.0f, float(job->sourceHeight), -100.0f, 100.0f);
		gs_set_viewport(x, y, scaledWidth, scaledHeight);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);
		gs_blend_state_pop();
		gs_projection_pop();
		gs_viewport_pop();
		gs_texrender_end(job->texrender);
		job->stage = 1;
		return PreviewStep::Stay;
	}

	if (job->stage == 1) {
		gs_stage_texture(job->stagesurf, gs_texrender_get_texture(job->texrender));
		job->stage = 2;
		return PreviewStep::Stay;
	}

	uint8_t *videoData = nullptr;
	uint32_t videoLinesize = 0;
	if (gs_stagesurface_map(job->stagesurf, &videoData, &videoLinesize)) {
		job->image = QImage(kPreviewWidth, kPreviewHeight, QImage::Format_RGBX8888);
		const int imageStride = (int)job->image.bytesPerLine();
		const int linesize = imageStride < (int)videoLinesize ? imageStride : (int)videoLinesize;
		for (int y = 0; y < kPreviewHeight; y++)
			memcpy(job->image.scanLine(y), videoData + (y * videoLinesize), linesize);
		gs_stagesurface_unmap(job->stagesurf);
	}
	DestroyPreviewGpu(job);
	return PreviewStep::Done;
}

void PreviewTick(void *, float)
{
	PreviewJob *job = nullptr;
	{
		std::lock_guard<std::mutex> lock(previewMutex);
		if (previewShutdown || previewJobs.empty()) {
			obs_remove_tick_callback(PreviewTick, nullptr);
			previewTicking = false;
			return;
		}
		job = previewJobs.front();
		if (job->cooldown > 0) {
			job->cooldown--;
			previewJobs.pop_front();
			previewJobs.push_back(job);
			return;
		}
		previewJobs.pop_front();
		previewActive = job;
	}

	obs_enter_graphics();
	PreviewStep step = StepPreview(job);
	obs_leave_graphics();

	QImage image;
	QPointer<SourceSelectButton> button;
	bool drop = false;
	{
		std::lock_guard<std::mutex> lock(previewMutex);
		previewActive = nullptr;
		if (previewShutdown) {
			drop = true;
		} else if (step == PreviewStep::Retry) {
			previewJobs.push_back(job);
		} else if (step == PreviewStep::Stay) {
			previewJobs.push_front(job);
		} else {
			image = job->image;
			button = job->button;
			drop = true;
			/* Native hover asks for another frame after the one in flight. */
			if (job->again && button) {
				auto *next = new PreviewJob;
				next->weak = job->weak;
				next->button = button;
				previewJobs.push_front(next);
			}
		}
		if (previewJobs.empty()) {
			obs_remove_tick_callback(PreviewTick, nullptr);
			previewTicking = false;
		}
	}
	if (drop) {
		obs_enter_graphics();
		DestroyPreviewGpu(job);
		obs_leave_graphics();
		delete job;
	}

	if (step == PreviewStep::Done && button && !image.isNull()) {
		QMetaObject::invokeMethod(
			button,
			[button, image] {
				if (button)
					button->SetPreview(image);
			},
			Qt::QueuedConnection);
	}
}

void EnsurePreviewTicking()
{
	if (!previewTicking) {
		previewTicking = true;
		obs_add_tick_callback(PreviewTick, nullptr);
	}
}

void QueueSourcePreview(SourceSelectButton *button, obs_source_t *source, bool priority)
{
	if (!button || !source)
		return;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0)
		return;

	std::lock_guard<std::mutex> lock(previewMutex);
	if (previewShutdown)
		return;

	if (previewActive && previewActive->button == button) {
		previewActive->again = true;
		return;
	}

	for (auto it = previewJobs.begin(); it != previewJobs.end(); ++it) {
		PreviewJob *queued = *it;
		if (!queued || queued->button != button)
			continue;
		if (priority && it != previewJobs.begin()) {
			previewJobs.erase(it);
			previewJobs.push_front(queued);
		}
		/* Render already happened; the next hover needs a new frame. */
		if (queued->stage > 0)
			queued->again = true;
		return;
	}

	auto *job = new PreviewJob;
	job->weak = OBSGetWeakRef(source);
	job->button = button;
	if (priority)
		previewJobs.push_front(job);
	else
		previewJobs.push_back(job);
	EnsurePreviewTicking();
}

} // namespace

void CancelSourcePreviews()
{
	std::deque<PreviewJob *> jobs;
	{
		std::lock_guard<std::mutex> lock(previewMutex);
		previewShutdown = true;
		if (previewTicking) {
			obs_remove_tick_callback(PreviewTick, nullptr);
			previewTicking = false;
		}
		jobs.swap(previewJobs);
	}
	if (!jobs.empty()) {
		obs_enter_graphics();
		for (PreviewJob *job : jobs)
			DestroyPreviewGpu(job);
		obs_leave_graphics();
	}
	for (PreviewJob *job : jobs)
		delete job;
}

SourceSelectButton::SourceSelectButton(obs_weak_source_t *weak, QWidget *parent)
	: QAbstractButton(parent), weakSource(weak)
{
	OBSSource source = OBSGetStrongRef(weak);
	if (!source)
		return;

	const char *uuid = obs_source_get_uuid(source);
	sourceUuid = uuid ? uuid : "";
	const char *sourceName = obs_source_get_name(source);

	setCheckable(true);
	setAccessibleName(QString::fromUtf8(sourceName ? sourceName : ""));
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	setFocusPolicy(Qt::StrongFocus);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	image = new QLabel(this);
	image->setObjectName(QString::fromUtf8("thumbnail"));
	image->setAttribute(Qt::WA_TransparentForMouseEvents);
	image->setMinimumSize(160, 90);
	image->setMaximumSize(160, 90);
	image->setAlignment(Qt::AlignCenter);
	QIcon icon;
	if (strcmp(obs_source_get_id(source), "scene") == 0)
		icon = GetSceneIcon();
	else
		icon = GetIconFromType(obs_source_get_icon_type(obs_source_get_id(source)), obs_source_get_id(source));
	image->setPixmap(icon.pixmap(48, 48));

	label = new QLabel(QString::fromUtf8(sourceName ? sourceName : ""), this);
	label->setObjectName(QString::fromUtf8("name"));
	label->setAlignment(Qt::AlignCenter);
	label->setAttribute(Qt::WA_TransparentForMouseEvents);
	label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	layout->addWidget(image);
	layout->addWidget(label);
	QueueSourcePreview(this, source, false);
}

void SourceSelectButton::RequestPreview(bool priority)
{
	if (!previewEnabled && !priority)
		return;
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source)
		return;
	QueueSourcePreview(this, source, priority);
}

void SourceSelectButton::enterEvent(QEnterEvent *)
{
	update();
	RequestPreview(true);
}

void SourceSelectButton::leaveEvent(QEvent *)
{
	update();
}

void SourceSelectButton::SetPreview(const QImage &preview)
{
	if (!image || preview.isNull())
		return;
	const QSize target = image->size().isEmpty() ? QSize(160, 90) : image->size();
	image->setPixmap(QPixmap::fromImage(preview).scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void SourceSelectButton::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	QStyleOptionButton option;
	option.initFrom(this);
	if (isChecked())
		option.state |= QStyle::State_On;
	if (isDown())
		option.state |= QStyle::State_Sunken;
	if (hasFocus())
		option.state |= QStyle::State_HasFocus;
	if (underMouse())
		option.state |= QStyle::State_MouseOver;
	style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
}

void SourceSelectButton::resizeEvent(QResizeEvent *)
{
	QStyleOptionButton option;
	option.initFrom(this);
	QRect content = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
	if (!content.isValid())
		content = rect();
	int left = std::max(0, content.left());
	int top = std::max(0, content.top());
	int right = std::max(0, rect().width() - content.right() - 1);
	int bottom = std::max(0, rect().height() - content.bottom() - 1);
	if (QLayout *box = layout())
		box->setContentsMargins(left, top, right, bottom);
}

SourceSelectDialog::SourceSelectDialog(QWidget *parent, obs_scene_t *scene) : QDialog(parent)
{
	obs_source_t *sceneSource = scene ? obs_scene_get_source(scene) : nullptr;
	if (sceneSource)
		sceneWeak = OBSGetWeakRef(sceneSource);

	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setWindowModality(Qt::NonModal);
	setObjectName(QString::fromUtf8("OBSBasicSourceSelect"));
	setWindowTitle(Locale("Basic.SourceSelect"));
	resize(1040, 620);
	setMaximumHeight(1000);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	auto *inner = new QFrame(this);
	inner->setProperty("class", "dialog-container");
	root->addWidget(inner);
	auto *columns = new QHBoxLayout(inner);
	columns->setContentsMargins(0, 0, 0, 0);
	columns->setSpacing(0);

	auto *typeFrame = new QFrame(inner);
	typeFrame->setProperty("class", "dialog-frame");
	auto *typeLayout = new QVBoxLayout(typeFrame);
	typeLayout->setContentsMargins(0, 0, 0, 0);
	typeLayout->setSpacing(0);
	sourceTypeList = new QListWidget(typeFrame);
	sourceTypeList->setObjectName(QString::fromUtf8("sourceTypeList"));
	sourceTypeList->setAccessibleName(Locale("Basic.SourceSelect.SelectType"));
	sourceTypeList->setFrameShape(QFrame::NoFrame);
	sourceTypeList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sourceTypeList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	sourceTypeList->setMinimumHeight(460);
	sourceTypeList->setIconSize(QSize(16, 16));
	typeLayout->addWidget(sourceTypeList);
	columns->addWidget(typeFrame, 1);

	auto *right = new QFrame(inner);
	auto *rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	rightLayout->setSpacing(0);

	auto *body = new QFrame(right);
	body->setProperty("class", "dialog-container");
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(0, 0, 0, 0);
	bodyLayout->setSpacing(0);

	sourceSelectTitle = new QLabel(Locale("Basic.SourceSelect.Recent"), body);
	sourceSelectTitle->setProperty("class", "text-title");
	bodyLayout->addWidget(sourceSelectTitle);

	auto *description = new QLabel(Locale("Basic.SourceSelect.Description"), body);
	description->setProperty("class", "text-small text-muted margin-y");
	description->setWordWrap(true);
	bodyLayout->addWidget(description);

	deprecatedFrame = new QFrame(body);
	deprecatedFrame->setProperty("class", "frame-notice");
	auto *deprecatedLayout = new QVBoxLayout(deprecatedFrame);
	deprecatedLayout->setContentsMargins(0, 0, 0, 0);
	deprecatedLabel = new QLabel(Locale("Basic.SourceSelect.Deprecated.Create"), deprecatedFrame);
	deprecatedLabel->setWordWrap(true);
	deprecatedLabel->setAlignment(Qt::AlignCenter);
	deprecatedLayout->addWidget(deprecatedLabel);
	deprecatedFrame->setVisible(false);
	bodyLayout->addWidget(deprecatedFrame);

	auto *selectFrame = new QFrame(body);
	selectFrame->setProperty("class", "dialog-container dialog-frame");
	auto *selectLayout = new QVBoxLayout(selectFrame);
	selectLayout->setContentsMargins(0, 0, 0, 0);
	selectLayout->setSpacing(0);

	createNewSource = new QPushButton(selectFrame);
	createNewSource->setProperty("class", "btn-create-new");
	createNewSource->setCursor(Qt::PointingHandCursor);
	createNewSource->setVisible(false);
	selectLayout->addWidget(createNewSource);

	noExistingLabel = new QLabel(selectFrame);
	noExistingLabel->setAlignment(Qt::AlignCenter);
	noExistingLabel->setProperty("class", "text-muted");
	noExistingLabel->setVisible(false);
	selectLayout->addWidget(noExistingLabel, 1);

	existingScrollArea = new QScrollArea(selectFrame);
	existingScrollArea->setFrameShape(QFrame::NoFrame);
	existingScrollArea->setWidgetResizable(true);
	existingScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	existingScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	auto *scrollContents = new QWidget(existingScrollArea);
	scrollContents->setAutoFillBackground(false);
	auto *scrollLayout = new QVBoxLayout(scrollContents);
	scrollLayout->setContentsMargins(0, 0, 0, 0);
	scrollLayout->setSpacing(0);
	existingListFrame = new QWidget(scrollContents);
	existingFlowLayout = new FlowLayout(existingListFrame);
	existingFlowLayout->setSpacing(0);
	scrollLayout->addWidget(existingListFrame);
	scrollLayout->addStretch(1);
	existingScrollArea->setWidget(scrollContents);
	existingScrollArea->viewport()->setAutoFillBackground(false);
	selectLayout->addWidget(existingScrollArea, 1);

	addExistingButton = new QPushButton(Locale("Basic.SourceSelect.NoSelection"), selectFrame);
	addExistingButton->setProperty("class", "button-primary");
	addExistingButton->setEnabled(false);
	auto *addRow = new QHBoxLayout();
	addRow->setContentsMargins(1, 1, 1, 1);
	addRow->addWidget(addExistingButton);
	addRow->addStretch(1);
	selectLayout->addLayout(addRow);

	bodyLayout->addWidget(selectFrame, 1);
	rightLayout->addWidget(body, 1);

	auto *footer = new QFrame(right);
	footer->setProperty("class", "dialog-container");
	auto *footerLayout = new QHBoxLayout(footer);
	footerLayout->setContentsMargins(0, 0, 0, 0);
	sourceVisible = new QCheckBox(Locale("Basic.SourceSelect.AddVisible"), footer);
	sourceVisible->setChecked(true);
	footerLayout->addWidget(sourceVisible);
	footerLayout->addStretch(1);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
	footerLayout->addWidget(buttons);
	rightLayout->addWidget(footer);

	columns->addWidget(right, 4);

	RebuildTypeList();
	RefreshSources();
	UpdateExistingSources(kRecentLimit);

	signalHandlers.emplace_back(obs_get_signal_handler(), "source_create", &SourceSelectDialog::SourceCreated, this);
	signalHandlers.emplace_back(obs_get_signal_handler(), "source_destroy", &SourceSelectDialog::SourceRemoved, this);

	connect(sourceTypeList, &QListWidget::itemDoubleClicked, this, &SourceSelectDialog::OnCreateNew);
	connect(sourceTypeList, &QListWidget::currentItemChanged, this, &SourceSelectDialog::OnTypeChanged);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(createNewSource, &QPushButton::clicked, this, &SourceSelectDialog::OnCreateNew);
	connect(addExistingButton, &QPushButton::clicked, this, &SourceSelectDialog::OnAddSelected);

	/* Same cadence as ThumbnailManager: visible cards refresh every 5s,
	 * and a hover jumps that card to the front of the queue. */
	connect(existingScrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this,
		[this] { UpdatePreviewVisibility(); });
	previewRefreshTimer = new QTimer(this);
	previewRefreshTimer->setInterval(5000);
	connect(previewRefreshTimer, &QTimer::timeout, this, [this] {
		UpdatePreviewVisibility();
		if (!sourceButtons)
			return;
		const auto buttons = sourceButtons->buttons();
		for (QAbstractButton *button : buttons) {
			auto *sourceButton = qobject_cast<SourceSelectButton *>(button);
			if (sourceButton && sourceButton->PreviewEnabled())
				sourceButton->RequestPreview(false);
		}
	});
	previewRefreshTimer->start();
}

void SourceSelectDialog::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	QTimer::singleShot(0, this, [this] { UpdatePreviewVisibility(); });
}

void SourceSelectDialog::UpdatePreviewVisibility()
{
	if (!existingScrollArea || !sourceButtons)
		return;
	const auto buttons = sourceButtons->buttons();
	if (buttons.isEmpty())
		return;

	QRect visible(QPoint(0, 0), existingScrollArea->viewport()->size());
	const int slack = buttons.first()->height();
	visible.setTop(visible.top() - slack);
	visible.setBottom(visible.bottom() + slack);

	for (QAbstractButton *button : buttons) {
		auto *sourceButton = qobject_cast<SourceSelectButton *>(button);
		if (!sourceButton)
			continue;
		QRect rect = button->rect();
		rect.moveTo(button->mapTo(existingScrollArea->viewport(), rect.topLeft()));
		sourceButton->SetPreviewEnabled(visible.intersects(rect));
	}
}

void SourceSelectDialog::SourceCreated(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<SourceSelectDialog *>(data), "OnSourceCreated", Qt::QueuedConnection);
}

void SourceSelectDialog::SourceRemoved(void *data, calldata_t *params)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(params, "source"));
	const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
	if (!uuid)
		return;
	QMetaObject::invokeMethod(static_cast<SourceSelectDialog *>(data), "OnSourceRemoved", Qt::QueuedConnection,
				  Q_ARG(QString, QString::fromUtf8(uuid)));
}

bool SourceSelectDialog::EnumSources(void *data, obs_source_t *source)
{
	auto *self = static_cast<SourceSelectDialog *>(data);
	self->weakSources.push_back(OBSGetWeakRef(source));
	return true;
}

QString SourceSelectDialog::DisplayNameForType(const QString &type) const
{
	if (type == QString::fromUtf8("scene"))
		return Locale("Basic.Scene");
	QByteArray utf8 = type.toUtf8();
	const char *latest = obs_get_latest_input_type_id(utf8.constData());
	const char *name = obs_source_get_display_name(latest ? latest : utf8.constData());
	return name ? QString::fromUtf8(name) : type;
}

QString SourceSelectDialog::UniqueSourceName(const QString &base, bool onCanvas) const
{
	OBSSource sceneSource = OBSGetStrongRef(sceneWeak.Get());
	OBSCanvasAutoRelease canvas = sceneSource ? obs_source_get_canvas(sceneSource) : nullptr;
	QString name = base;
	int suffix = 1;
	for (;;) {
		OBSSourceAutoRelease existing = nullptr;
		if (onCanvas && canvas) {
			/* Scenes/groups live per-canvas; same name on Main is OK. */
			existing = obs_canvas_get_source_by_name(canvas, name.toUtf8().constData());
		} else {
			existing = obs_get_source_by_name(name.toUtf8().constData());
		}
		if (!existing)
			return name;
		name = base + QString::fromUtf8(" %1").arg(++suffix);
	}
}

void SourceSelectDialog::RebuildTypeList()
{
	sourceTypeList->clear();
	const char *unversioned = nullptr;
	const char *type = nullptr;
	size_t idx = 0;
	while (obs_enum_input_types2(++idx, &type, &unversioned)) {
		const char *name = obs_source_get_display_name(type);
		uint32_t caps = obs_get_source_output_flags(type);
		if ((caps & OBS_SOURCE_CAP_DISABLED) != 0)
			continue;
		auto *item = new QListWidgetItem(sourceTypeList);
		item->setText(name ? QString::fromUtf8(name) : QString::fromUtf8(type));
		item->setData(kUnversionedIdRole, QString::fromUtf8(unversioned ? unversioned : type));
		bool deprecated = (caps & OBS_SOURCE_DEPRECATED) != 0;
		item->setData(kDeprecatedRole, deprecated);
		if (!deprecated)
			item->setIcon(GetIconFromType(obs_source_get_icon_type(type), type));
	}

	auto *sceneItem = new QListWidgetItem(sourceTypeList);
	sceneItem->setText(Locale("Basic.Scene"));
	sceneItem->setData(kUnversionedIdRole, QString::fromUtf8("scene"));
	sceneItem->setData(kDeprecatedRole, false);
	sceneItem->setIcon(GetSceneIcon());
	sourceTypeList->sortItems();

	auto *deprecatedTitle = new QListWidgetItem(Locale("Deprecated"));
	deprecatedTitle->setFlags(deprecatedTitle->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable);
	sourceTypeList->addItem(deprecatedTitle);

	QList<QListWidgetItem *> deprecatedItems;
	for (int i = sourceTypeList->count() - 1; i >= 0; --i) {
		QListWidgetItem *item = sourceTypeList->item(i);
		if (item && item->data(kDeprecatedRole).toBool())
			deprecatedItems.append(sourceTypeList->takeItem(i));
	}
	std::reverse(deprecatedItems.begin(), deprecatedItems.end());
	for (QListWidgetItem *item : deprecatedItems)
		sourceTypeList->addItem(item);

	auto *recent = new QListWidgetItem();
	recent->setText(Locale("Basic.SourceSelect.Recent"));
	recent->setData(kUnversionedIdRole, kRecentTypeId);
	recent->setData(kDeprecatedRole, false);
	sourceTypeList->insertItem(0, recent);
	sourceTypeList->setCurrentItem(recent);
	selectedTypeId = kRecentTypeId;
}

void SourceSelectDialog::RefreshSources()
{
	weakSources.clear();
	obs_enum_sources(EnumSources, this);

	struct obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++)
		weakSources.push_back(OBSGetWeakRef(list.sources.array[i]));
	obs_frontend_source_list_free(&list);
}

void SourceSelectDialog::UpdateExistingSources(int limit)
{
	while (QLayoutItem *child = existingFlowLayout->takeAt(0)) {
		if (child->widget())
			child->widget()->deleteLater();
		delete child;
	}
	if (sourceButtons)
		sourceButtons->deleteLater();
	sourceButtons = new QButtonGroup(this);
	sourceButtons->setExclusive(false);

	std::vector<OBSWeakSource> matching;
	for (const OBSWeakSource &weak : weakSources) {
		OBSSource source = OBSGetStrongRef(weak.Get());
		if (!source || obs_source_removed(source))
			continue;
		const char *id = obs_source_get_unversioned_id(source);
		QString typeId = QString::fromUtf8(id ? id : "");
		if (typeId == QString::fromUtf8("group"))
			continue;
		if (selectedTypeId == kRecentTypeId) {
			if (typeId == QString::fromUtf8("scene"))
				continue;
			matching.push_back(weak);
			continue;
		}
		if (typeId == selectedTypeId)
			matching.push_back(weak);
	}

	const bool reverse = selectedTypeId == kRecentTypeId;
	const size_t count = limit > 0 ? std::min(static_cast<size_t>(limit), matching.size()) : matching.size();
	QWidget *previous = sourceTypeList;
	auto addButton = [&](const OBSWeakSource &weak) {
		OBSSource source = OBSGetStrongRef(weak.Get());
		if (!source)
			return;
		auto *button = new SourceSelectButton(weak.Get(), existingListFrame);
		existingFlowLayout->addWidget(button);
		sourceButtons->addButton(button);
		const char *uuidPointer = obs_source_get_uuid(source);
		std::string uuid = uuidPointer ? uuidPointer : "";
		bool selected = std::find(selectedItems.begin(), selectedItems.end(), uuid) != selectedItems.end();
		button->setChecked(selected);
		setTabOrder(previous, button);
		previous = button;
	};
	if (reverse) {
		for (size_t i = 0; i < count; i++)
			addButton(matching[matching.size() - 1 - i]);
	} else {
		for (size_t i = 0; i < count; i++)
			addButton(matching[i]);
	}

	connect(sourceButtons, &QButtonGroup::buttonToggled, this, &SourceSelectDialog::OnButtonToggled);
	existingListFrame->adjustSize();
}

void SourceSelectDialog::OnButtonToggled(QAbstractButton *button, bool checked)
{
	auto *sourceButton = dynamic_cast<SourceSelectButton *>(button);
	if (!sourceButton) {
		ClearSelectedItems();
		return;
	}

	Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
	bool ctrl = modifiers & Qt::ControlModifier;
	bool shift = modifiers & Qt::ShiftModifier;
	int toggledIndex = existingFlowLayout->indexOf(sourceButton);
	std::string toggledUuid(sourceButton->uuid());

	if (shift && lastSelectedIndex >= 0) {
		if (!ctrl)
			ClearSelectedItems();
		QSignalBlocker block(sourceButtons);
		int start = std::min(toggledIndex, lastSelectedIndex);
		int end = std::max(toggledIndex, lastSelectedIndex);
		for (int i = start; i <= end; i++) {
			QLayoutItem *item = existingFlowLayout->itemAt(i);
			auto *entry = item ? dynamic_cast<SourceSelectButton *>(item->widget()) : nullptr;
			if (!entry)
				continue;
			QSignalBlocker entryBlock(entry);
			entry->setChecked(true);
			AddSelectedItem(std::string(entry->uuid()));
		}
		return;
	}

	lastSelectedIndex = toggledIndex;
	if (ctrl) {
		if (checked)
			AddSelectedItem(toggledUuid);
		else
			RemoveSelectedItem(toggledUuid);
		return;
	}

	bool reselect = selectedItems.size() > 1;
	ClearSelectedItems();
	if (checked || reselect) {
		QSignalBlocker block(button);
		button->setChecked(true);
		AddSelectedItem(toggledUuid);
	}
}

void SourceSelectDialog::AddSelectedItem(const std::string &uuid)
{
	if (std::find(selectedItems.begin(), selectedItems.end(), uuid) == selectedItems.end())
		selectedItems.push_back(uuid);
	if (SourceSelectButton *button = FindButton(uuid))
		lastSelectedIndex = existingFlowLayout->indexOf(button);
	addExistingButton->setEnabled(!selectedItems.empty());
	if (selectedItems.empty())
		addExistingButton->setText(Locale("Basic.SourceSelect.NoSelection"));
	else
		addExistingButton->setText(Locale("Basic.SourceSelect.AddExisting").arg(static_cast<int>(selectedItems.size())));
}

void SourceSelectDialog::RemoveSelectedItem(const std::string &uuid)
{
	auto it = std::find(selectedItems.begin(), selectedItems.end(), uuid);
	if (it != selectedItems.end())
		selectedItems.erase(it);
	if (SourceSelectButton *button = FindButton(uuid))
		lastSelectedIndex = existingFlowLayout->indexOf(button);
	addExistingButton->setEnabled(!selectedItems.empty());
	if (selectedItems.empty())
		addExistingButton->setText(Locale("Basic.SourceSelect.NoSelection"));
	else
		addExistingButton->setText(Locale("Basic.SourceSelect.AddExisting").arg(static_cast<int>(selectedItems.size())));
}

void SourceSelectDialog::ClearSelectedItems()
{
	if (selectedItems.empty())
		return;
	if (sourceButtons)
		sourceButtons->blockSignals(true);
	for (const std::string &uuid : selectedItems) {
		if (SourceSelectButton *button = FindButton(uuid)) {
			QSignalBlocker block(button);
			button->setChecked(false);
		}
	}
	if (sourceButtons)
		sourceButtons->blockSignals(false);
	selectedItems.clear();
	addExistingButton->setEnabled(false);
	addExistingButton->setText(Locale("Basic.SourceSelect.NoSelection"));
}

SourceSelectButton *SourceSelectDialog::FindButton(const std::string &uuid) const
{
	if (!existingFlowLayout)
		return nullptr;
	for (int i = 0; i < existingFlowLayout->count(); i++) {
		QLayoutItem *item = existingFlowLayout->itemAt(i);
		auto *button = item ? dynamic_cast<SourceSelectButton *>(item->widget()) : nullptr;
		if (button && button->uuid() == uuid)
			return button;
	}
	return nullptr;
}

bool SourceSelectDialog::AddToScene(obs_source_t *source, bool visible)
{
	OBSSource sceneSource = OBSGetStrongRef(sceneWeak.Get());
	obs_scene_t *target = obs_scene_from_source(sceneSource);
	if (!target || !source) {
		blog(LOG_WARNING, "[Stream Suite] Cannot add source: canvas scene is not selected");
		return false;
	}
	obs_sceneitem_t *item = obs_scene_add(target, source);
	if (!item) {
		blog(LOG_WARNING, "[Stream Suite] Failed to add source '%s' to canvas scene", obs_source_get_name(source));
		return false;
	}
	obs_sceneitem_set_visible(item, visible);
	return true;
}

void SourceSelectDialog::OnCreateNew()
{
	if (selectedTypeId == kRecentTypeId || selectedTypeId == QString::fromUtf8("scene"))
		return;

	QByteArray typeUtf8 = selectedTypeId.toUtf8();
	const char *id = typeUtf8.constData();
	const char *latest = obs_get_latest_input_type_id(id);
	if (!latest || !*latest)
		latest = id;

	bool onCanvas = (obs_get_source_output_flags(id) & OBS_SOURCE_REQUIRES_CANVAS) != 0;
	QString placeholder = DisplayNameForType(selectedTypeId);
	if (placeholder.isEmpty())
		placeholder = selectedTypeId;
	QString name = UniqueSourceName(placeholder, onCanvas);

	OBSSource sceneSource = OBSGetStrongRef(sceneWeak.Get());
	OBSCanvasAutoRelease canvas = sceneSource ? obs_source_get_canvas(sceneSource) : nullptr;
	OBSSourceAutoRelease existing = nullptr;
	if (onCanvas && canvas) {
		existing = obs_canvas_get_source_by_name(canvas, name.toUtf8().constData());
	} else {
		existing = obs_get_source_by_name(name.toUtf8().constData());
	}
	if (existing) {
		QMessageBox::information(this, Locale("NameExists.Title"), Locale("NameExists.Text"));
		return;
	}

	obs_source_t *created = nullptr;
	if (onCanvas) {
		if (!canvas)
			return;
		created = obs_scene_get_source(obs_canvas_scene_create(canvas, name.toUtf8().constData()));
	} else {
		created = obs_source_create(latest, name.toUtf8().constData(), nullptr, nullptr);
	}
	if (!created) {
		blog(LOG_WARNING, "[Stream Suite] Failed to create source '%s'", name.toUtf8().constData());
		return;
	}

	OBSSource owned(created);
	obs_source_release(created);
	if (!AddToScene(owned, sourceVisible->isChecked())) {
		obs_source_remove(owned);
		return;
	}
	obs_frontend_open_source_properties(owned);
	close();
}

void SourceSelectDialog::OnAddSelected()
{
	if (selectedItems.empty())
		return;
	bool visible = sourceVisible->isChecked();
	for (const std::string &uuid : selectedItems) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source)
			AddToScene(source, visible);
	}
	close();
}

void SourceSelectDialog::OnSourceCreated()
{
	RefreshSources();
	UpdateExistingSources(selectedTypeId == kRecentTypeId ? kRecentLimit : 0);
}

void SourceSelectDialog::OnSourceRemoved(QString uuid)
{
	RefreshSources();
	RemoveSelectedItem(uuid.toStdString());
	UpdateExistingSources(selectedTypeId == kRecentTypeId ? kRecentLimit : 0);
}

void SourceSelectDialog::OnTypeChanged(QListWidgetItem *current, QListWidgetItem *)
{
	if (!current)
		return;
	ClearSelectedItems();
	bool deprecated = current->data(kDeprecatedRole).toBool();
	deprecatedFrame->setVisible(deprecated);

	QString type = current->data(kUnversionedIdRole).toString();
	if (type == kRecentTypeId) {
		sourceSelectTitle->setText(Locale("Basic.SourceSelect.Recent"));
		selectedTypeId = kRecentTypeId;
		noExistingLabel->setVisible(false);
		existingScrollArea->setVisible(true);
		createNewSource->setVisible(false);
		UpdateExistingSources(kRecentLimit);
		return;
	}
	if (type == selectedTypeId)
		return;

	selectedTypeId = type;
	QString display = DisplayNameForType(type);
	createNewSource->setVisible(type != QString::fromUtf8("scene"));
	createNewSource->setText(Locale("Basic.SourceSelect.NewSource").arg(display));
	sourceSelectTitle->setText(display);
	UpdateExistingSources(0);
	noExistingLabel->setText(Locale("Basic.SourceSelect.NoExisting").arg(display));
	bool empty = existingFlowLayout->count() == 0;
	existingScrollArea->setVisible(!empty);
	noExistingLabel->setVisible(empty);
}

void OpenSourceSelectDialog(QWidget *parent, obs_scene_t *scene)
{
	if (!scene)
		return;
	static QPointer<SourceSelectDialog> window;
	if (window)
		window->close();
	QWidget *owner = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!owner)
		owner = parent;
	window = new SourceSelectDialog(owner, scene);
	window->setAttribute(Qt::WA_DeleteOnClose, true);
	window->show();
	window->raise();
	window->activateWindow();
}
