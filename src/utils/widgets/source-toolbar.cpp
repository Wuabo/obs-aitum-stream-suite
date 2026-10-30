#include "source-toolbar.hpp"
#include "media-controls.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QColorDialog>
#include <QFileDialog>
#include <QFontDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QStandardItemModel>

#include "moc_source-toolbar.cpp"

#ifdef _WIN32
#define get_os_module(win, mac, linux) obs_get_module(win)
#define get_os_text(mod, win, mac, linux) obs_module_get_locale_text(mod, win)
#elif __APPLE__
#define get_os_module(win, mac, linux) obs_get_module(mac)
#define get_os_text(mod, win, mac, linux) obs_module_get_locale_text(mod, mac)
#else
#define get_os_module(win, mac, linux) obs_get_module(linux)
#define get_os_text(mod, win, mac, linux) obs_module_get_locale_text(mod, linux)
#endif

static QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff, (val >> 24) & 0xff);
}

static long long color_to_int(QColor color)
{
	auto shift = [](unsigned val, int s) { return ((val & 0xff) << s); };
	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

static void MakeQFont(obs_data_t *font_obj, QFont &font, bool limit = false)
{
	const char *face = obs_data_get_string(font_obj, "face");
	const char *style = obs_data_get_string(font_obj, "style");
	int size = (int)obs_data_get_int(font_obj, "size");
	uint32_t flags = (uint32_t)obs_data_get_int(font_obj, "flags");

	if (face) {
		font.setFamily(face);
		font.setStyleName(style);
	}
	if (size) {
		if (limit) {
			int max_size = font.pointSize();
			if (max_size < 28)
				max_size = 28;
			if (size > max_size)
				size = max_size;
		}
		font.setPointSize(size);
	}
	if (flags & OBS_FONT_BOLD)
		font.setBold(true);
	if (flags & OBS_FONT_ITALIC)
		font.setItalic(true);
	if (flags & OBS_FONT_UNDERLINE)
		font.setUnderline(true);
	if (flags & OBS_FONT_STRIKEOUT)
		font.setStrikeOut(true);
}

void SetComboItemEnabled(QComboBox *c, int idx, bool enabled)
{
	auto *model = dynamic_cast<QStandardItemModel *>(c->model());
	if (!model)
		return;
	QStandardItem *item = model->item(idx);
	if (!item)
		return;
	item->setFlags(enabled ? (item->flags() | Qt::ItemIsEnabled) : (item->flags() & ~Qt::ItemIsEnabled));
}

/* ---- SourceToolbar ---------------------------------------------------- */

SourceToolbar::SourceToolbar(QWidget *parent, OBSSource source)
	: QWidget(parent),
	  weakSource(OBSGetWeakRef(source)),
	  props(obs_source_properties(source))
{
	setMinimumHeight(22);
}

SourceToolbar::~SourceToolbar()
{
	if (props) {
		obs_properties_destroy(props);
		props = nullptr;
	}
}

void SourceToolbar::SaveOldProperties(obs_source_t *source)
{
	oldData = obs_data_create();
	OBSDataAutoRelease oldSettings = obs_source_get_settings(source);
	obs_data_apply(oldData, oldSettings);
	obs_data_set_string(oldData, "undo_suuid", obs_source_get_uuid(source));
}

void SourceToolbar::SetUndoProperties(obs_source_t *source, bool repeatable)
{
	if (!oldData) {
		return;
	}

	OBSDataAutoRelease new_settings = obs_data_create();
	OBSDataAutoRelease curr_settings = obs_source_get_settings(source);
	obs_data_apply(new_settings, curr_settings);
	obs_data_set_string(new_settings, "undo_suuid", obs_source_get_uuid(source));

	std::string undo_data(obs_data_get_json(oldData));
	std::string redo_data(obs_data_get_json(new_settings));

	if (undo_data != redo_data) {
		auto undo_redo = [](const char *data) {
			OBSDataAutoRelease settings = obs_data_create_from_json(data);
			OBSSourceAutoRelease src =
				obs_get_source_by_uuid(obs_data_get_string(settings, "undo_suuid"));
			if (src) {
				obs_source_reset_settings(src, settings);
			}
		};
		QString name = QString::fromUtf8(obs_frontend_get_locale_string("Undo.Properties"))
				       .arg(QString::fromUtf8(obs_source_get_name(source)));
		obs_frontend_add_undo_redo_action(name.toUtf8().constData(), undo_redo, undo_redo, undo_data.c_str(),
						  redo_data.c_str(), repeatable);
	}

	oldData = nullptr;
}

/* ---- ComboSelectToolbar ----------------------------------------------- */

int FillPropertyCombo(QComboBox *c, obs_property_t *p, const std::string &cur_id, bool is_int)
{
	size_t count = obs_property_list_item_count(p);
	int cur_idx = -1;

	for (size_t i = 0; i < count; i++) {
		const char *name = obs_property_list_item_name(p, i);
		std::string id;
		if (is_int) {
			id = std::to_string(obs_property_list_item_int(p, i));
		} else {
			const char *val = obs_property_list_item_string(p, i);
			id = val ? val : "";
		}
		if (cur_id == id) {
			cur_idx = (int)i;
		}
		c->addItem(name, id.c_str());
	}
	return cur_idx;
}

void UpdateSourceComboToolbarProperties(QComboBox *combo, OBSSource source, obs_properties_t *props,
					const char *prop_name, bool is_int)
{
	std::string cur_id;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (is_int) {
		cur_id = std::to_string(obs_data_get_int(settings, prop_name));
	} else {
		cur_id = obs_data_get_string(settings, prop_name);
	}

	combo->blockSignals(true);
	combo->clear();

	obs_property_t *p = obs_properties_get(props, prop_name);
	int cur_idx = FillPropertyCombo(combo, p, cur_id, is_int);

	if (cur_idx == -1 || obs_property_list_item_disabled(p, cur_idx)) {
		if (cur_idx == -1) {
			combo->insertItem(0, QString::fromUtf8(obs_frontend_get_locale_string(
						     "Basic.Settings.Audio.UnknownAudioDevice")));
			cur_idx = 0;
		}
		SetComboItemEnabled(combo, cur_idx, false);
	}

	combo->setCurrentIndex(cur_idx);
	combo->blockSignals(false);
}

void UpdateSourceComboToolbarValue(QComboBox *combo, OBSSource source, int idx, const char *prop_name, bool is_int)
{
	QString id = combo->itemData(idx).toString();
	OBSDataAutoRelease settings = obs_data_create();
	if (is_int) {
		obs_data_set_int(settings, prop_name, id.toInt());
	} else {
		obs_data_set_string(settings, prop_name, id.toUtf8().constData());
	}
	obs_source_update(source, settings);
}

void ComboSelectToolbar::BuildDeviceSelectUi()
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	deviceLabel = new QLabel(QStringLiteral("Device"), this);
	deviceLabel->setMinimumHeight(22);
	deviceLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

	device = new QComboBox(this);
	device->setMinimumHeight(22);
	device->setMaximumWidth(600);
	device->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

	activateButton = new QPushButton(QStringLiteral("Activate"), this);
	activateButton->setMinimumHeight(22);
	activateButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

	layout->addWidget(deviceLabel);
	layout->addWidget(device, 1);
	layout->addWidget(activateButton);
	layout->addStretch(1);

	connect(device, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ComboSelectToolbar::onDeviceChanged);
}

ComboSelectToolbar::ComboSelectToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	BuildDeviceSelectUi();
}

void ComboSelectToolbar::Init()
{
	OBSSource source = GetSource();
	if (!source || !prop_name) {
		return;
	}
	UpdateSourceComboToolbarProperties(device, source, props, prop_name, is_int);
}

void ComboSelectToolbar::onDeviceChanged(int idx)
{
	OBSSource source = GetSource();
	if (idx == -1 || !source || !prop_name) {
		return;
	}
	SaveOldProperties(source);
	UpdateSourceComboToolbarValue(device, source, idx, prop_name, is_int);
	SetUndoProperties(source);
}

WindowCaptureToolbar::WindowCaptureToolbar(QWidget *parent, OBSSource source) : ComboSelectToolbar(parent, source) {}

void WindowCaptureToolbar::Init()
{
	delete activateButton;
	activateButton = nullptr;

	obs_module_t *mod = get_os_module("win-capture", "mac-capture", "linux-capture");
	if (!mod) {
		return;
	}
	deviceLabel->setText(QString::fromUtf8(get_os_text(mod, "WindowCapture.Window", "WindowUtils.Window", "Window")));

#if !defined(_WIN32) && !defined(__APPLE__)
	prop_name = "capture_window";
#else
	prop_name = "window";
#endif
#ifdef __APPLE__
	is_int = true;
#endif
	ComboSelectToolbar::Init();
}

DisplayCaptureToolbar::DisplayCaptureToolbar(QWidget *parent, OBSSource source) : ComboSelectToolbar(parent, source) {}

void DisplayCaptureToolbar::Init()
{
	delete activateButton;
	activateButton = nullptr;

	obs_module_t *mod = get_os_module("win-capture", "mac-capture", "linux-capture");
	if (!mod) {
		return;
	}
	deviceLabel->setText(QString::fromUtf8(get_os_text(mod, "Monitor", "DisplayCapture.Display", "Screen")));

#ifdef _WIN32
	prop_name = "monitor_id";
#elif __APPLE__
	prop_name = "display_uuid";
#else
	is_int = true;
	prop_name = "screen";
#endif
	ComboSelectToolbar::Init();
}

AudioCaptureToolbar::AudioCaptureToolbar(QWidget *parent, OBSSource source) : ComboSelectToolbar(parent, source) {}

void AudioCaptureToolbar::Init()
{
	delete activateButton;
	activateButton = nullptr;

	obs_module_t *mod = get_os_module("win-wasapi", "mac-capture", "linux-pulseaudio");
	if (!mod) {
		return;
	}
	deviceLabel->setText(QString::fromUtf8(get_os_text(mod, "Device", "CoreAudio.Device", "Device")));
	prop_name = "device_id";
	ComboSelectToolbar::Init();
}

ApplicationAudioCaptureToolbar::ApplicationAudioCaptureToolbar(QWidget *parent, OBSSource source)
	: ComboSelectToolbar(parent, source)
{
}

void ApplicationAudioCaptureToolbar::Init()
{
	delete activateButton;
	activateButton = nullptr;

	obs_module_t *mod = obs_get_module("win-wasapi");
	if (!mod) {
		return;
	}
	deviceLabel->setText(QString::fromUtf8(obs_module_get_locale_text(mod, "Window")));
	prop_name = "window";
	ComboSelectToolbar::Init();
}

/* ---- DeviceCaptureToolbar --------------------------------------------- */

DeviceCaptureToolbar::DeviceCaptureToolbar(QWidget *parent, OBSSource source)
	: QWidget(parent),
	  weakSource(OBSGetWeakRef(source))
{
	setMinimumHeight(22);
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	activateButton = new QPushButton(this);
	activateButton->setMinimumHeight(22);
	layout->addWidget(activateButton);
	layout->addStretch(1);

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	active = obs_data_get_bool(settings, "active");

	obs_module_t *mod = obs_get_module("win-dshow");
	if (mod) {
		activateText = QString::fromUtf8(obs_module_get_locale_text(mod, "Activate"));
		deactivateText = QString::fromUtf8(obs_module_get_locale_text(mod, "Deactivate"));
	} else {
		activateText = QStringLiteral("Activate");
		deactivateText = QStringLiteral("Deactivate");
	}
	activateButton->setText(active ? deactivateText : activateText);
	connect(activateButton, &QPushButton::clicked, this, &DeviceCaptureToolbar::onActivateClicked);
}

void DeviceCaptureToolbar::onActivateClicked()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return;
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	bool now_active = obs_data_get_bool(settings, "active");
	bool desyncedSetting = now_active != active;

	active = !active;
	activateButton->setText(active ? deactivateText : activateText);
	if (desyncedSetting) {
		return;
	}

	calldata_t cd = {};
	calldata_set_bool(&cd, "active", active);
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_call(ph, "activate", &cd);
	calldata_free(&cd);
}

/* ---- BrowserToolbar --------------------------------------------------- */

BrowserToolbar::BrowserToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	auto *refresh = new QPushButton(QString::fromUtf8(obs_frontend_get_locale_string("RefreshBrowser")), this);
	refresh->setMinimumHeight(22);
	refresh->setFlat(true);
	refresh->setIcon(QIcon(QStringLiteral(":/res/images/refresh.svg")));
	refresh->setProperty("class", QStringLiteral("icon-refresh"));
	layout->addWidget(refresh);

	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::VLine);
	layout->addWidget(line);
	layout->addStretch(1);

	connect(refresh, &QPushButton::clicked, this, &BrowserToolbar::onRefreshClicked);
}

void BrowserToolbar::onRefreshClicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}
	obs_property_t *p = obs_properties_get(props, "refreshnocache");
	obs_property_button_clicked(p, source.Get());
}

/* ---- GameCaptureToolbar ----------------------------------------------- */

GameCaptureToolbar::GameCaptureToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	modeLabel = new QLabel(this);
	modeLabel->setMinimumHeight(22);
	mode = new QComboBox(this);
	mode->setMinimumSize(120, 22);
	mode->setMaximumWidth(400);
	mode->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	windowLabel = new QLabel(this);
	windowLabel->setMinimumHeight(22);
	window = new QComboBox(this);
	window->setMinimumSize(120, 22);
	window->setMaximumWidth(600);
	window->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	window->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

	layout->addWidget(modeLabel);
	layout->addWidget(mode, 4);
	layout->addWidget(windowLabel);
	layout->addWidget(window, 5);
	layout->addStretch(0);

	obs_module_t *mod = obs_get_module("win-capture");
	if (mod) {
		modeLabel->setText(QString::fromUtf8(obs_module_get_locale_text(mod, "Mode")));
		windowLabel->setText(QString::fromUtf8(obs_module_get_locale_text(mod, "WindowCapture.Window")));
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	std::string cur_mode = obs_data_get_string(settings, "capture_mode");
	std::string cur_window = obs_data_get_string(settings, "window");

	mode->blockSignals(true);
	obs_property_t *p = obs_properties_get(props, "capture_mode");
	int cur_idx = FillPropertyCombo(mode, p, cur_mode);
	mode->setCurrentIndex(cur_idx);
	mode->blockSignals(false);

	window->blockSignals(true);
	p = obs_properties_get(props, "window");
	cur_idx = FillPropertyCombo(window, p, cur_window);
	window->setCurrentIndex(cur_idx);
	window->blockSignals(false);

	if (cur_idx != -1 && obs_property_list_item_disabled(p, cur_idx)) {
		SetComboItemEnabled(window, cur_idx, false);
	}

	connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GameCaptureToolbar::onModeChanged);
	connect(window, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &GameCaptureToolbar::onWindowChanged);
	UpdateWindowVisibility();
}

void GameCaptureToolbar::UpdateWindowVisibility()
{
	bool is_window = (mode->currentData().toString() == QStringLiteral("window"));
	windowLabel->setVisible(is_window);
	window->setVisible(is_window);
}

void GameCaptureToolbar::onModeChanged(int idx)
{
	OBSSource source = GetSource();
	if (idx == -1 || !source) {
		return;
	}
	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "capture_mode", mode->itemData(idx).toString().toUtf8().constData());
	obs_source_update(source, settings);
	SetUndoProperties(source);
	UpdateWindowVisibility();
}

void GameCaptureToolbar::onWindowChanged(int idx)
{
	OBSSource source = GetSource();
	if (idx == -1 || !source) {
		return;
	}
	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "window", window->itemData(idx).toString().toUtf8().constData());
	obs_source_update(source, settings);
	SetUndoProperties(source);
}

/* ---- ImageSourceToolbar ----------------------------------------------- */

ImageSourceToolbar::ImageSourceToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	pathLabel = new QLabel(this);
	pathLabel->setMinimumHeight(22);
	path = new QLineEdit(this);
	path->setMinimumSize(100, 22);
	path->setMaximumWidth(400);
	path->setReadOnly(true);

	auto *browse = new QPushButton(QString::fromUtf8(obs_frontend_get_locale_string("Browse")), this);
	browse->setMinimumHeight(22);

	layout->addWidget(pathLabel);
	layout->addWidget(path, 1);
	layout->addWidget(browse);
	layout->addStretch(1);

	obs_module_t *mod = obs_get_module("image-source");
	if (mod) {
		pathLabel->setText(QString::fromUtf8(obs_module_get_locale_text(mod, "File")));
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	path->setText(QString::fromUtf8(obs_data_get_string(settings, "file")));
	connect(browse, &QPushButton::clicked, this, &ImageSourceToolbar::onBrowseClicked);
}

void ImageSourceToolbar::onBrowseClicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}

	obs_property_t *p = obs_properties_get(props, "file");
	const char *desc = obs_property_description(p);
	const char *filter = obs_property_path_filter(p);
	const char *default_path = obs_property_path_default_path(p);

	QString startDir = path->text();
	if (startDir.isEmpty()) {
		startDir = QString::fromUtf8(default_path ? default_path : "");
	}

	QString selected = QFileDialog::getOpenFileName(this, QString::fromUtf8(desc ? desc : ""), startDir,
							QString::fromUtf8(filter ? filter : ""));
	if (selected.isEmpty()) {
		return;
	}

	path->setText(selected);
	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "file", selected.toUtf8().constData());
	obs_source_update(source, settings);
	SetUndoProperties(source);
}

/* ---- ColorSourceToolbar ----------------------------------------------- */

ColorSourceToolbar::ColorSourceToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	colorLabel = new QLabel(this);
	colorLabel->setMinimumSize(80, 22);
	colorLabel->setAlignment(Qt::AlignCenter);

	auto *choose = new QPushButton(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow.SelectColor")), this);
	choose->setMinimumHeight(22);

	layout->addWidget(colorLabel);
	layout->addWidget(choose);
	layout->addStretch(1);

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	color = color_from_int(obs_data_get_int(settings, "color"));
	UpdateColor();
	connect(choose, &QPushButton::clicked, this, &ColorSourceToolbar::onChooseClicked);
}

void ColorSourceToolbar::UpdateColor()
{
	QPalette palette = QPalette(color);
	colorLabel->setFrameStyle(QFrame::Sunken | QFrame::Panel);
	colorLabel->setText(color.name(QColor::HexRgb));
	colorLabel->setPalette(palette);
	colorLabel->setStyleSheet(QStringLiteral("background-color :%1; color: %2;")
					  .arg(palette.color(QPalette::Window).name(QColor::HexRgb),
					       palette.color(QPalette::WindowText).name(QColor::HexRgb)));
	colorLabel->setAutoFillBackground(true);
	colorLabel->setAlignment(Qt::AlignCenter);
}

void ColorSourceToolbar::onChooseClicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}

	obs_property_t *p = obs_properties_get(props, "color");
	const char *desc = obs_property_description(p);

	QColorDialog::ColorDialogOptions options = QColorDialog::ShowAlphaChannel;
	QColor newColor = QColorDialog::getColor(color, this, QString::fromUtf8(desc ? desc : ""), options);
	if (!newColor.isValid()) {
		return;
	}

	color = newColor;
	UpdateColor();
	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "color", color_to_int(color));
	obs_source_update(source, settings);
	SetUndoProperties(source);
}

/* ---- TextSourceToolbar ------------------------------------------------ */

TextSourceToolbar::TextSourceToolbar(QWidget *parent, OBSSource source) : SourceToolbar(parent, source)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(6);

	auto *selectFont = new QPushButton(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow.SelectFont")), this);
	selectFont->setMinimumHeight(22);
	auto *selectColor = new QPushButton(
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow.SelectColor")), this);
	selectColor->setMinimumHeight(22);

	emptySpace = new QFrame(this);
	emptySpace->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	text = new QLineEdit(this);
	text->setMinimumHeight(22);

	layout->addWidget(selectFont);
	layout->addWidget(selectColor);
	layout->addWidget(emptySpace);
	layout->addWidget(text);

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	const char *id = obs_source_get_unversioned_id(source);
	bool ft2 = strcmp(id, "text_ft2_source") == 0;
	bool read_from_file = obs_data_get_bool(settings, ft2 ? "from_file" : "read_from_file");

	OBSDataAutoRelease font_obj = obs_data_get_obj(settings, "font");
	MakeQFont(font_obj, font);

	unsigned int val = (unsigned int)obs_data_get_int(settings, ft2 ? "color1" : "color");
	color = color_from_int(val);

	const char *txt = obs_data_get_string(settings, "text");
	bool single_line = !read_from_file && (!txt || strchr(txt, '\n') == nullptr);
	emptySpace->setVisible(!single_line);
	text->setVisible(single_line);
	if (single_line) {
		text->setText(QString::fromUtf8(txt));
	}

	connect(selectFont, &QPushButton::clicked, this, &TextSourceToolbar::onSelectFontClicked);
	connect(selectColor, &QPushButton::clicked, this, &TextSourceToolbar::onSelectColorClicked);
	connect(text, &QLineEdit::textChanged, this, &TextSourceToolbar::onTextChanged);
}

void TextSourceToolbar::onSelectFontClicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}

	bool success = false;
	QFont next = QFontDialog::getFont(
		&success, font, this,
		QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow.SelectFont.WindowTitle")));
	if (!success) {
		return;
	}
	font = next;

	OBSDataAutoRelease font_obj = obs_data_create();
	obs_data_set_string(font_obj, "face", font.family().toUtf8().constData());
	obs_data_set_string(font_obj, "style", font.styleName().toUtf8().constData());
	obs_data_set_int(font_obj, "size", font.pointSize());
	uint32_t flags = font.bold() ? OBS_FONT_BOLD : 0;
	flags |= font.italic() ? OBS_FONT_ITALIC : 0;
	flags |= font.underline() ? OBS_FONT_UNDERLINE : 0;
	flags |= font.strikeOut() ? OBS_FONT_STRIKEOUT : 0;
	obs_data_set_int(font_obj, "flags", flags);

	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_obj(settings, "font", font_obj);
	obs_source_update(source, settings);
	SetUndoProperties(source);
}

void TextSourceToolbar::onSelectColorClicked()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}

	bool freetype = strncmp(obs_source_get_id(source), "text_ft2_source", 15) == 0;
	obs_property_t *p = obs_properties_get(props, freetype ? "color1" : "color");
	const char *desc = obs_property_description(p);

	QColor newColor = QColorDialog::getColor(color, this, QString::fromUtf8(desc ? desc : ""),
						 QColorDialog::ShowAlphaChannel);
	if (!newColor.isValid()) {
		return;
	}
	color = newColor;

	SaveOldProperties(source);
	OBSDataAutoRelease settings = obs_data_create();
	if (freetype) {
		obs_data_set_int(settings, "color1", color_to_int(color));
		obs_data_set_int(settings, "color2", color_to_int(color));
	} else {
		obs_data_set_int(settings, "color", color_to_int(color));
	}
	obs_source_update(source, settings);
	SetUndoProperties(source);
}

void TextSourceToolbar::onTextChanged()
{
	OBSSource source = GetSource();
	if (!source) {
		return;
	}
	std::string newText = text->text().toStdString();
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (newText == obs_data_get_string(settings, "text")) {
		return;
	}
	SaveOldProperties(source);
	obs_data_set_string(settings, "text", newText.c_str());
	obs_source_update(source, nullptr);
	SetUndoProperties(source, true);
}

static bool is_network_media_source(obs_source_t *source, const char *id)
{
	if (strcmp(id, "ffmpeg_source") != 0) {
		return false;
	}
	OBSDataAutoRelease s = obs_source_get_settings(source);
	return !obs_data_get_bool(s, "is_local_file");
}

QWidget *CreateSourceContextToolbar(QWidget *parent, obs_source_t *source)
{
	if (!parent || !source) {
		return nullptr;
	}

	const uint32_t flags = obs_source_get_output_flags(source);
	const char *id = obs_source_get_unversioned_id(source);
	OBSSource obsSource(source);

	if ((flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && !is_network_media_source(source, id)) {
		auto *media = new MediaControls(parent);
		media->SetSource(obsSource);
		return media;
	}
	if (strcmp(id, "browser_source") == 0) {
		return new BrowserToolbar(parent, obsSource);
	}
	if (strcmp(id, "wasapi_input_capture") == 0 || strcmp(id, "wasapi_output_capture") == 0 ||
	    strcmp(id, "coreaudio_input_capture") == 0 || strcmp(id, "coreaudio_output_capture") == 0 ||
	    strcmp(id, "pulse_input_capture") == 0 || strcmp(id, "pulse_output_capture") == 0 ||
	    strcmp(id, "alsa_input_capture") == 0) {
		auto *tb = new AudioCaptureToolbar(parent, obsSource);
		tb->Init();
		return tb;
	}
	if (strcmp(id, "wasapi_process_output_capture") == 0) {
		auto *tb = new ApplicationAudioCaptureToolbar(parent, obsSource);
		tb->Init();
		return tb;
	}
	if (strcmp(id, "window_capture") == 0 || strcmp(id, "xcomposite_input") == 0) {
		auto *tb = new WindowCaptureToolbar(parent, obsSource);
		tb->Init();
		return tb;
	}
	if (strcmp(id, "monitor_capture") == 0 || strcmp(id, "display_capture") == 0 || strcmp(id, "xshm_input") == 0) {
		auto *tb = new DisplayCaptureToolbar(parent, obsSource);
		tb->Init();
		return tb;
	}
	if (strcmp(id, "dshow_input") == 0) {
		return new DeviceCaptureToolbar(parent, obsSource);
	}
	if (strcmp(id, "game_capture") == 0) {
		return new GameCaptureToolbar(parent, obsSource);
	}
	if (strcmp(id, "image_source") == 0) {
		return new ImageSourceToolbar(parent, obsSource);
	}
	if (strcmp(id, "color_source") == 0) {
		return new ColorSourceToolbar(parent, obsSource);
	}
	if (strcmp(id, "text_ft2_source") == 0 || strcmp(id, "text_gdiplus") == 0) {
		return new TextSourceToolbar(parent, obsSource);
	}

	return nullptr;
}

OBSSource ContextToolbarGetSource(QWidget *widget)
{
	if (!widget) {
		return OBSSource();
	}
	if (auto *tb = dynamic_cast<SourceToolbar *>(widget)) {
		return tb->GetSource();
	}
	if (auto *tb = dynamic_cast<MediaControls *>(widget)) {
		return tb->GetSource();
	}
	if (auto *tb = dynamic_cast<DeviceCaptureToolbar *>(widget)) {
		return tb->GetSource();
	}
	return OBSSource();
}
