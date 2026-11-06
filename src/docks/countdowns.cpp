#include "countdowns.hpp"

#include <src/utils/widgets/hotkey-edit.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QFrame>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern obs_data_t *current_profile_config;
extern void save_current_profile_config(bool save_docks);

namespace {

enum class Phase { Idle, Running, Paused, Finished };

struct Countdown {
	QString id;
	QString name;
	int hours = 0;
	int minutes = 5;
	int seconds = 0;
	QString endText;
	bool showHours = true;
	QString sourceName;
	QString fontFace;
	int fontSize = 72;
	bool bold = true;
	uint32_t color = 0xFFFFFFFFu;
	uint32_t warnColor = 0xFF0000FFu;
	int warnSeconds = 10;
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
	Phase phase = Phase::Idle;
	qint64 endMs = 0;
	qint64 remainingMs = 0;
	bool sourceLive = false;
	QString lastText;
	uint32_t lastColor = 0;
	bool fontDirty = true;
	QPointer<QLabel> timeLabel;
	QPointer<QPushButton> toggleBtn;
};

QVector<Countdown> items;
QPointer<QWidget> page;
QPointer<QVBoxLayout> listLayout;
QPointer<QTimer> tick;
bool signalsConnected = false;
bool pageVisible = false;
bool shuttingDown = false;

qint64 NowMs()
{
	return QDateTime::currentMSecsSinceEpoch();
}

qint64 DurationMs(const Countdown &c)
{
	return (((qint64)c.hours * 60 + c.minutes) * 60 + c.seconds) * 1000;
}

uint32_t PackColor(const QColor &color)
{
	return (uint32_t)color.red() | ((uint32_t)color.green() << 8) | ((uint32_t)color.blue() << 16) | 0xFF000000u;
}

QColor UnpackColor(uint32_t color)
{
	return QColor(int(color & 0xFFu), int((color >> 8) & 0xFFu), int((color >> 16) & 0xFFu));
}

bool IsTextSource(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	return id && (strcmp(id, "text_gdiplus") == 0 || strcmp(id, "text_ft2_source") == 0);
}

Countdown *FindById(const QString &id)
{
	for (auto &item : items) {
		if (item.id == id) {
			return &item;
		}
	}
	return nullptr;
}

Countdown *FindByHotkey(obs_hotkey_id id)
{
	for (auto &item : items) {
		if (item.hotkey == id) {
			return &item;
		}
	}
	return nullptr;
}

QString ClockText(const Countdown &c, qint64 shownMs)
{
	if (c.phase == Phase::Finished) {
		return c.endText;
	}
	if (shownMs < 0) {
		shownMs = 0;
	}
	int total = (int)((shownMs + 999) / 1000);
	if (c.phase == Phase::Running && shownMs <= 0) {
		return c.endText;
	}
	int seconds = total % 60;
	if (c.showHours) {
		int hours = total / 3600;
		int minutes = (total / 60) % 60;
		return QStringLiteral("%1:%2:%3")
			.arg(hours, 2, 10, QChar('0'))
			.arg(minutes, 2, 10, QChar('0'))
			.arg(seconds, 2, 10, QChar('0'));
	}
	int minutes = total / 60;
	return QStringLiteral("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
}

qint64 ShownMs(const Countdown &c)
{
	if (c.phase == Phase::Running) {
		return std::max<qint64>(0, c.endMs - NowMs());
	}
	if (c.phase == Phase::Paused) {
		return std::max<qint64>(0, c.remainingMs);
	}
	if (c.phase == Phase::Finished) {
		return 0;
	}
	return DurationMs(c);
}

uint32_t ColorFor(const Countdown &c, qint64 shownMs)
{
	if ((c.phase == Phase::Running || c.phase == Phase::Paused) && c.warnSeconds > 0) {
		int seconds = (int)((std::max<qint64>(0, shownMs) + 999) / 1000);
		if (seconds <= c.warnSeconds) {
			return c.warnColor;
		}
	}
	return c.color;
}

void WriteSource(Countdown &c, const QString &text, uint32_t color)
{
	if (c.sourceName.isEmpty()) {
		return;
	}
	if (!c.fontDirty && text == c.lastText && color == c.lastColor) {
		return;
	}
	QByteArray name = c.sourceName.toUtf8();
	obs_source_t *source = obs_get_source_by_name(name.constData());
	if (!source) {
		return;
	}
	if (!IsTextSource(source)) {
		obs_source_release(source);
		return;
	}
	obs_data_t *settings = obs_source_get_settings(source);
	QByteArray textUtf8 = text.toUtf8();
	obs_data_set_string(settings, "text", textUtf8.constData());
	obs_data_set_bool(settings, "read_from_file", false);
	obs_data_set_bool(settings, "from_file", false);
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (!font) {
		font = obs_data_create();
	}
	if (!c.fontFace.isEmpty()) {
		QByteArray face = c.fontFace.toUtf8();
		obs_data_set_string(font, "face", face.constData());
	}
	obs_data_set_int(font, "size", c.fontSize > 0 ? c.fontSize : 72);
	obs_data_set_int(font, "flags", c.bold ? 1 : 0);
	obs_data_set_string(font, "style", c.bold ? "Bold" : "Regular");
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
	obs_data_set_int(settings, "color", (long long)color);
	obs_data_set_int(settings, "color1", (long long)color);
	obs_data_set_int(settings, "color2", (long long)color);
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);
	c.lastText = text;
	c.lastColor = color;
	c.fontDirty = false;
}

void PushText(Countdown &c)
{
	qint64 shown = ShownMs(c);
	if (c.phase == Phase::Running && shown <= 0) {
		c.phase = Phase::Finished;
		c.remainingMs = 0;
		shown = 0;
	}
	WriteSource(c, ClockText(c, shown), ColorFor(c, shown));
}

void RefreshLabels()
{
	for (auto &item : items) {
		qint64 shown = ShownMs(item);
		if (item.timeLabel) {
			item.timeLabel->setText(ClockText(item, shown));
		}
		if (item.toggleBtn) {
			const char *key = item.phase == Phase::Running ? "CountdownPause" : "CountdownStart";
			item.toggleBtn->setText(QString::fromUtf8(obs_module_text(key)));
		}
	}
}

void EnsureTick();

void StartFresh(Countdown &c)
{
	c.remainingMs = DurationMs(c);
	c.endMs = NowMs() + c.remainingMs;
	c.phase = c.remainingMs > 0 ? Phase::Running : Phase::Finished;
	PushText(c);
	EnsureTick();
	RefreshLabels();
}

void Resume(Countdown &c)
{
	if (c.phase == Phase::Paused) {
		c.endMs = NowMs() + std::max<qint64>(0, c.remainingMs);
		c.phase = c.remainingMs > 0 ? Phase::Running : Phase::Finished;
	} else if (c.phase != Phase::Running) {
		StartFresh(c);
		return;
	}
	PushText(c);
	EnsureTick();
	RefreshLabels();
}

void Pause(Countdown &c)
{
	if (c.phase != Phase::Running) {
		return;
	}
	c.remainingMs = std::max<qint64>(0, c.endMs - NowMs());
	c.phase = c.remainingMs > 0 ? Phase::Paused : Phase::Finished;
	PushText(c);
	EnsureTick();
	RefreshLabels();
}

void ResetOne(Countdown &c)
{
	c.remainingMs = DurationMs(c);
	if (c.phase == Phase::Running || (c.sourceLive && c.phase != Phase::Paused)) {
		c.endMs = NowMs() + c.remainingMs;
		c.phase = c.remainingMs > 0 ? Phase::Running : Phase::Finished;
	} else if (c.phase == Phase::Paused) {
		c.phase = Phase::Paused;
	} else {
		c.phase = Phase::Idle;
	}
	c.fontDirty = true;
	PushText(c);
	EnsureTick();
	RefreshLabels();
}

void StopBecauseHidden(Countdown &c)
{
	if (c.phase != Phase::Running) {
		EnsureTick();
		return;
	}
	c.remainingMs = std::max<qint64>(0, c.endMs - NowMs());
	c.phase = c.remainingMs > 0 ? Phase::Paused : Phase::Finished;
	EnsureTick();
}

void OnTick()
{
	if (shuttingDown) {
		return;
	}
	bool anyRunning = false;
	for (auto &item : items) {
		if (item.phase != Phase::Running) {
			continue;
		}
		qint64 shown = std::max<qint64>(0, item.endMs - NowMs());
		if (shown <= 0) {
			item.phase = Phase::Finished;
			item.remainingMs = 0;
			PushText(item);
		} else {
			anyRunning = true;
			PushText(item);
		}
	}
	if (pageVisible) {
		RefreshLabels();
	}
	if (!anyRunning && tick) {
		tick->stop();
	}
}

void EnsureTick()
{
	bool anyRunning = false;
	for (const auto &item : items) {
		if (item.phase == Phase::Running) {
			anyRunning = true;
			break;
		}
	}
	if (!tick) {
		return;
	}
	if (anyRunning) {
		if (!tick->isActive()) {
			tick->start();
		}
	} else {
		tick->stop();
	}
}

void Persist()
{
	if (shuttingDown || !current_profile_config) {
		return;
	}
	obs_data_array_t *array = obs_data_array_create();
	for (const auto &item : items) {
		obs_data_t *data = obs_data_create();
		QByteArray id = item.id.toUtf8();
		QByteArray name = item.name.toUtf8();
		QByteArray endText = item.endText.toUtf8();
		QByteArray source = item.sourceName.toUtf8();
		QByteArray face = item.fontFace.toUtf8();
		obs_data_set_string(data, "id", id.constData());
		obs_data_set_string(data, "name", name.constData());
		obs_data_set_int(data, "hours", item.hours);
		obs_data_set_int(data, "minutes", item.minutes);
		obs_data_set_int(data, "seconds", item.seconds);
		obs_data_set_string(data, "end_text", endText.constData());
		obs_data_set_bool(data, "show_hours", item.showHours);
		obs_data_set_string(data, "source", source.constData());
		obs_data_set_string(data, "font", face.constData());
		obs_data_set_int(data, "font_size", item.fontSize);
		obs_data_set_bool(data, "bold", item.bold);
		obs_data_set_int(data, "color", (long long)item.color);
		obs_data_set_int(data, "warn_color", (long long)item.warnColor);
		obs_data_set_int(data, "warn_seconds", item.warnSeconds);
		if (item.hotkey != OBS_INVALID_HOTKEY_ID) {
			obs_data_array_t *hotkey = obs_hotkey_save(item.hotkey);
			obs_data_set_array(data, "reset_hotkey", hotkey);
			obs_data_array_release(hotkey);
		}
		obs_data_array_push_back(array, data);
		obs_data_release(data);
	}
	obs_data_set_array(current_profile_config, "countdowns", array);
	obs_data_array_release(array);
	save_current_profile_config(false);
}

void OnResetHotkey(void *, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !qApp) {
		return;
	}
	QMetaObject::invokeMethod(
		qApp,
		[id] {
			if (Countdown *item = FindByHotkey(id)) {
				ResetOne(*item);
			}
		},
		Qt::QueuedConnection);
}

void RegisterHotkey(Countdown &c)
{
	if (c.hotkey != OBS_INVALID_HOTKEY_ID || c.id.isEmpty()) {
		return;
	}
	QByteArray name = QStringLiteral("Aitum.Countdown.Reset.%1").arg(c.id).toUtf8();
	QByteArray desc = QString::fromUtf8(obs_module_text("CountdownResetHotkey")).arg(c.name).toUtf8();
	c.hotkey = obs_hotkey_register_frontend(name.constData(), desc.constData(), OnResetHotkey, nullptr);
}

void UnregisterHotkey(Countdown &c)
{
	if (c.hotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}
	obs_hotkey_unregister(c.hotkey);
	c.hotkey = OBS_INVALID_HOTKEY_ID;
}

std::vector<obs_key_combination_t> CombosFromArray(obs_data_array_t *array)
{
	std::vector<obs_key_combination_t> combos;
	if (!array) {
		return combos;
	}
	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *data = obs_data_array_item(array, i);
		obs_key_combination_t combo = {};
		combo.key = obs_key_from_name(obs_data_get_string(data, "key"));
		if (obs_data_get_bool(data, "shift")) {
			combo.modifiers |= INTERACT_SHIFT_KEY;
		}
		if (obs_data_get_bool(data, "control")) {
			combo.modifiers |= INTERACT_CONTROL_KEY;
		}
		if (obs_data_get_bool(data, "alt")) {
			combo.modifiers |= INTERACT_ALT_KEY;
		}
		if (obs_data_get_bool(data, "command")) {
			combo.modifiers |= INTERACT_COMMAND_KEY;
		}
		combos.push_back(combo);
		obs_data_release(data);
	}
	return combos;
}

void RetitleHotkey(Countdown &c)
{
	obs_data_array_t *saved = nullptr;
	if (c.hotkey != OBS_INVALID_HOTKEY_ID) {
		saved = obs_hotkey_save(c.hotkey);
		obs_hotkey_unregister(c.hotkey);
		c.hotkey = OBS_INVALID_HOTKEY_ID;
	}
	RegisterHotkey(c);
	if (saved) {
		obs_hotkey_load(c.hotkey, saved);
		obs_data_array_release(saved);
	}
}

QString DefaultFace()
{
	const QStringList families = QFontDatabase::families();
	if (families.contains(QStringLiteral("Arial"))) {
		return QStringLiteral("Arial");
	}
	return families.isEmpty() ? QString() : families.front();
}

QString UniqueSourceName(const QString &base)
{
	QString name = base.isEmpty() ? QString::fromUtf8(obs_module_text("CountdownTimers")) : base;
	int suffix = 2;
	while (true) {
		QByteArray utf8 = name.toUtf8();
		obs_source_t *existing = obs_get_source_by_name(utf8.constData());
		if (!existing) {
			return name;
		}
		obs_source_release(existing);
		name = base + QStringLiteral(" ") + QString::number(suffix++);
	}
}

bool CreateTextSource(Countdown &c)
{
	const QString name = UniqueSourceName(c.name);
	QByteArray nameUtf8 = name.toUtf8();
	QByteArray textUtf8 = ClockText(c, DurationMs(c)).toUtf8();
	QByteArray face = c.fontFace.toUtf8();
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "text", textUtf8.constData());
	obs_data_set_bool(settings, "read_from_file", false);
	obs_data_set_bool(settings, "from_file", false);
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", face.constData());
	obs_data_set_int(font, "size", c.fontSize > 0 ? c.fontSize : 72);
	obs_data_set_int(font, "flags", c.bold ? 1 : 0);
	obs_data_set_string(font, "style", c.bold ? "Bold" : "Regular");
	obs_data_set_obj(settings, "font", font);
	obs_data_release(font);
	obs_data_set_int(settings, "color", (long long)c.color);
	obs_data_set_int(settings, "color1", (long long)c.color);
	obs_data_set_int(settings, "color2", (long long)c.color);

	const char *ids[] = {"text_gdiplus_v2", "text_gdiplus", "text_ft2_source"};
	obs_source_t *source = nullptr;
	for (const char *id : ids) {
		source = obs_source_create(id, nameUtf8.constData(), settings, nullptr);
		if (source) {
			break;
		}
	}
	obs_data_release(settings);
	if (!source) {
		return false;
	}
	obs_source_release(source);
	c.sourceName = name;
	c.lastText.clear();
	c.fontDirty = true;
	return true;
}

void ApplyColorButton(QPushButton *button, uint32_t color)
{
	button->setProperty("obsColor", QVariant::fromValue<qulonglong>(color));
	const QColor q = UnpackColor(color);
	button->setText(q.name(QColor::HexRgb));
	button->setStyleSheet(QStringLiteral("background-color:%1; color:%2;")
				       .arg(q.name(QColor::HexRgb), q.lightness() > 140 ? QStringLiteral("black")
											 : QStringLiteral("white")));
}

uint32_t ColorFromButton(QPushButton *button)
{
	return (uint32_t)button->property("obsColor").toULongLong();
}

bool EditCountdown(Countdown &c, QWidget *parent, bool isNew)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(QString::fromUtf8(obs_module_text(isNew ? "CountdownAdd" : "CountdownEdit")));
	auto *form = new QFormLayout(&dialog);

	auto *nameEdit = new QLineEdit(c.name, &dialog);
	auto *hours = new QSpinBox(&dialog);
	auto *minutes = new QSpinBox(&dialog);
	auto *seconds = new QSpinBox(&dialog);
	hours->setRange(0, 99);
	minutes->setRange(0, 59);
	seconds->setRange(0, 59);
	hours->setValue(c.hours);
	minutes->setValue(std::clamp(c.minutes, 0, 59));
	seconds->setValue(std::clamp(c.seconds, 0, 59));
	hours->setToolTip(QString::fromUtf8(obs_module_text("CountdownHours")));
	minutes->setToolTip(QString::fromUtf8(obs_module_text("CountdownMinutes")));
	seconds->setToolTip(QString::fromUtf8(obs_module_text("CountdownSeconds")));
	auto *durationRow = new QWidget(&dialog);
	auto *durationLayout = new QHBoxLayout(durationRow);
	durationLayout->setContentsMargins(0, 0, 0, 0);
	durationLayout->addWidget(hours);
	durationLayout->addWidget(minutes);
	durationLayout->addWidget(seconds);

	auto *endEdit = new QLineEdit(c.endText, &dialog);
	auto *format = new QComboBox(&dialog);
	format->addItem(QString::fromUtf8(obs_module_text("CountdownFormatHours")), true);
	format->addItem(QString::fromUtf8(obs_module_text("CountdownFormatMinutes")), false);
	format->setCurrentIndex(c.showHours ? 0 : 1);

	auto *source = new QComboBox(&dialog);
	source->addItem(QString::fromUtf8(obs_module_text("CountdownCreateSource")), QString());
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (!IsTextSource(src)) {
				return true;
			}
			const char *name = obs_source_get_name(src);
			if (name && *name) {
				static_cast<QStringList *>(param)->append(QString::fromUtf8(name));
			}
			return true;
		},
		&names);
	names.sort(Qt::CaseInsensitive);
	bool sourceListed = false;
	for (const QString &entry : names) {
		source->addItem(entry, entry);
		if (entry == c.sourceName) {
			sourceListed = true;
		}
	}
	if (!c.sourceName.isEmpty() && !sourceListed) {
		source->addItem(c.sourceName, c.sourceName);
	}
	int sourceIndex = c.sourceName.isEmpty() ? 0 : source->findData(c.sourceName);
	source->setCurrentIndex(sourceIndex < 0 ? 0 : sourceIndex);

	auto *font = new QComboBox(&dialog);
	font->setEditable(true);
	const QStringList families = QFontDatabase::families();
	font->addItems(families);
	if (!c.fontFace.isEmpty()) {
		int fontIndex = font->findText(c.fontFace);
		if (fontIndex < 0) {
			font->addItem(c.fontFace);
			fontIndex = font->count() - 1;
		}
		font->setCurrentIndex(fontIndex);
	}
	auto *fontSize = new QSpinBox(&dialog);
	fontSize->setRange(8, 512);
	fontSize->setValue(c.fontSize > 0 ? c.fontSize : 72);
	auto *bold = new QCheckBox(QString::fromUtf8(obs_module_text("CountdownBold")), &dialog);
	bold->setChecked(c.bold);

	auto *colorBtn = new QPushButton(&dialog);
	auto *warnBtn = new QPushButton(&dialog);
	ApplyColorButton(colorBtn, c.color);
	ApplyColorButton(warnBtn, c.warnColor);
	QObject::connect(colorBtn, &QPushButton::clicked, colorBtn, [colorBtn] {
		const QColor next = QColorDialog::getColor(UnpackColor(ColorFromButton(colorBtn)), colorBtn);
		if (next.isValid()) {
			ApplyColorButton(colorBtn, PackColor(next));
		}
	});
	QObject::connect(warnBtn, &QPushButton::clicked, warnBtn, [warnBtn] {
		const QColor next = QColorDialog::getColor(UnpackColor(ColorFromButton(warnBtn)), warnBtn);
		if (next.isValid()) {
			ApplyColorButton(warnBtn, PackColor(next));
		}
	});
	auto *warnSeconds = new QSpinBox(&dialog);
	warnSeconds->setRange(0, 3600);
	warnSeconds->setValue(std::max(0, c.warnSeconds));

	std::vector<obs_key_combination_t> combos;
	if (c.hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *saved = obs_hotkey_save(c.hotkey);
		combos = CombosFromArray(saved);
		obs_data_array_release(saved);
	}
	auto *hotkey = new OBSHotkeyWidget(&dialog, c.hotkey, std::string("Aitum.Countdown.Reset.") + c.id.toStdString(), combos);

	form->addRow(QString::fromUtf8(obs_module_text("CountdownName")), nameEdit);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownDuration")), durationRow);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownEndText")), endEdit);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownFormat")), format);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownSource")), source);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownFont")), font);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownFontSize")), fontSize);
	form->addRow(QString(), bold);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownColor")), colorBtn);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownWarnColor")), warnBtn);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownWarnSeconds")), warnSeconds);
	form->addRow(QString::fromUtf8(obs_module_text("CountdownHotkey")), hotkey);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form->addRow(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
		if (nameEdit->text().trimmed().isEmpty()) {
			nameEdit->setFocus();
			return;
		}
		Countdown next = c;
		next.name = nameEdit->text().trimmed();
		next.hours = hours->value();
		next.minutes = minutes->value();
		next.seconds = seconds->value();
		next.endText = endEdit->text();
		next.showHours = format->currentData().toBool();
		next.fontFace = font->currentText().trimmed();
		next.fontSize = fontSize->value();
		next.bold = bold->isChecked();
		next.color = ColorFromButton(colorBtn);
		next.warnColor = ColorFromButton(warnBtn);
		next.warnSeconds = warnSeconds->value();
		next.fontDirty = true;
		const QString picked = source->currentData().toString();
		if (picked.isEmpty()) {
			if (!CreateTextSource(next)) {
				QMessageBox::warning(&dialog, dialog.windowTitle(),
						     QString::fromUtf8(obs_module_text("CountdownCreateFailed")));
				return;
			}
		} else if (picked != c.sourceName) {
			next.sourceName = picked;
			next.lastText.clear();
		}
		hotkey->Save();
		const QString previousName = c.name;
		c = next;
		if (c.name != previousName) {
			RetitleHotkey(c);
		}
		if (c.phase == Phase::Idle || c.phase == Phase::Finished) {
			c.phase = Phase::Idle;
		}
		PushText(c);
		dialog.accept();
	});

	return dialog.exec() == QDialog::Accepted;
}

void RebuildRows();

void OnStartPause(const QString &id)
{
	Countdown *item = FindById(id);
	if (!item) {
		return;
	}
	if (item->phase == Phase::Running) {
		Pause(*item);
	} else {
		Resume(*item);
	}
}

void OnReset(const QString &id)
{
	if (Countdown *item = FindById(id)) {
		ResetOne(*item);
	}
}

void OnEdit(const QString &id)
{
	Countdown *item = FindById(id);
	if (!item || !page) {
		return;
	}
	if (!EditCountdown(*item, page, false)) {
		return;
	}
	Persist();
	RebuildRows();
	RefreshLabels();
}

void OnRemove(const QString &id)
{
	Countdown *item = FindById(id);
	if (!item || !page) {
		return;
	}
	const auto answer = QMessageBox::question(page, QString::fromUtf8(obs_module_text("CountdownRemove")),
						  QString::fromUtf8(obs_module_text("CountdownRemoveConfirm")));
	if (answer != QMessageBox::Yes) {
		return;
	}
	UnregisterHotkey(*item);
	for (int i = 0; i < items.size(); i++) {
		if (items[i].id == id) {
			items.removeAt(i);
			break;
		}
	}
	Persist();
	RebuildRows();
	EnsureTick();
}

void AddCountdown()
{
	Countdown created;
	created.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	created.name = QString::fromUtf8(obs_module_text("CountdownTimers"));
	created.endText = QString::fromUtf8(obs_module_text("CountdownDefaultEnd"));
	created.fontFace = DefaultFace();
	RegisterHotkey(created);
	items.push_back(created);
	Countdown &item = items.last();
	if (!EditCountdown(item, page, true)) {
		UnregisterHotkey(item);
		items.removeLast();
		return;
	}
	Persist();
	RebuildRows();
	RefreshLabels();
	EnsureTick();
}

void ResetActive()
{
	for (auto &item : items) {
		if (item.phase == Phase::Running) {
			ResetOne(item);
		}
	}
}

void RebuildRows()
{
	if (!listLayout) {
		return;
	}
	while (QLayoutItem *child = listLayout->takeAt(0)) {
		if (QWidget *widget = child->widget()) {
			delete widget;
		}
		delete child;
	}
	for (auto &item : items) {
		item.timeLabel = nullptr;
		item.toggleBtn = nullptr;
		auto *row = new QWidget;
		auto *column = new QVBoxLayout(row);
		column->setContentsMargins(0, 4, 0, 4);
		column->setSpacing(2);
		auto *title = new QLabel(item.name, row);
		title->setWordWrap(true);
		auto *time = new QLabel(row);
		time->setAlignment(Qt::AlignLeft);
		auto *buttons = new QHBoxLayout;
		buttons->setContentsMargins(0, 0, 0, 0);
		buttons->setSpacing(4);
		auto *toggle = new QPushButton(row);
		auto *reset = new QPushButton(QString::fromUtf8(obs_module_text("CountdownReset")), row);
		auto *edit = new QPushButton(QString::fromUtf8(obs_module_text("CountdownEdit")), row);
		auto *remove = new QPushButton(QString::fromUtf8(obs_module_text("CountdownRemove")), row);
		const QString id = item.id;
		QObject::connect(toggle, &QPushButton::clicked, row, [id] { OnStartPause(id); });
		QObject::connect(reset, &QPushButton::clicked, row, [id] { OnReset(id); });
		QObject::connect(edit, &QPushButton::clicked, row, [id] { OnEdit(id); });
		QObject::connect(remove, &QPushButton::clicked, row, [id] { OnRemove(id); });
		for (QPushButton *button : {toggle, reset, edit, remove}) {
			button->setMinimumWidth(0);
			button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			buttons->addWidget(button);
		}
		column->addWidget(title);
		column->addWidget(time);
		column->addLayout(buttons);
		listLayout->addWidget(row);
		item.timeLabel = time;
		item.toggleBtn = toggle;
	}
	listLayout->addStretch(1);
	RefreshLabels();
}

void OnSourceVisibility(const QString &name)
{
	if (shuttingDown) {
		return;
	}
	QByteArray utf8 = name.toUtf8();
	obs_source_t *source = obs_get_source_by_name(utf8.constData());
	const bool live = source && (obs_source_active(source) || obs_source_showing(source));
	if (source) {
		obs_source_release(source);
	}
	for (auto &item : items) {
		if (item.sourceName != name) {
			continue;
		}
		const bool wasLive = item.sourceLive;
		item.sourceLive = live;
		if (live && !wasLive) {
			StartFresh(item);
		} else if (!live && wasLive) {
			StopBecauseHidden(item);
			RefreshLabels();
		}
	}
}

void QueueVisibility(calldata_t *data)
{
	if (!qApp) {
		return;
	}
	obs_source_t *source = (obs_source_t *)calldata_ptr(data, "source");
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!name || !*name) {
		return;
	}
	bool interested = false;
	const QString qname = QString::fromUtf8(name);
	for (const auto &item : items) {
		if (item.sourceName == qname) {
			interested = true;
			break;
		}
	}
	if (!interested) {
		return;
	}
	QMetaObject::invokeMethod(qApp, [qname] { OnSourceVisibility(qname); }, Qt::QueuedConnection);
}

void OnActivate(void *, calldata_t *data)
{
	QueueVisibility(data);
}
void OnDeactivate(void *, calldata_t *data)
{
	QueueVisibility(data);
}
void OnShow(void *, calldata_t *data)
{
	QueueVisibility(data);
}
void OnHide(void *, calldata_t *data)
{
	QueueVisibility(data);
}

void OnRename(void *, calldata_t *data)
{
	if (!qApp) {
		return;
	}
	const char *prev = calldata_string(data, "prev_name");
	const char *next = calldata_string(data, "new_name");
	if (!prev || !next) {
		return;
	}
	const QString previous = QString::fromUtf8(prev);
	const QString renamed = QString::fromUtf8(next);
	QMetaObject::invokeMethod(
		qApp,
		[previous, renamed] {
			if (shuttingDown) {
				return;
			}
			bool changed = false;
			for (auto &item : items) {
				if (item.sourceName == previous) {
					item.sourceName = renamed;
					changed = true;
				}
			}
			if (changed) {
				Persist();
				RebuildRows();
			}
		},
		Qt::QueuedConnection);
}

void ConnectSignals()
{
	if (signalsConnected) {
		return;
	}
	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_connect(handler, "source_activate", OnActivate, nullptr);
	signal_handler_connect(handler, "source_deactivate", OnDeactivate, nullptr);
	signal_handler_connect(handler, "source_show", OnShow, nullptr);
	signal_handler_connect(handler, "source_hide", OnHide, nullptr);
	signal_handler_connect(handler, "source_rename", OnRename, nullptr);
	signalsConnected = true;
}

void DisconnectSignals()
{
	if (!signalsConnected) {
		return;
	}
	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_disconnect(handler, "source_activate", OnActivate, nullptr);
	signal_handler_disconnect(handler, "source_deactivate", OnDeactivate, nullptr);
	signal_handler_disconnect(handler, "source_show", OnShow, nullptr);
	signal_handler_disconnect(handler, "source_hide", OnHide, nullptr);
	signal_handler_disconnect(handler, "source_rename", OnRename, nullptr);
	signalsConnected = false;
}

int ClampInt(long long value, int minValue, int maxValue)
{
	if (value < minValue) {
		return minValue;
	}
	if (value > maxValue) {
		return maxValue;
	}
	return (int)value;
}

} // namespace

QWidget *countdowns_create_page(QWidget *parent)
{
	shuttingDown = false;
	auto *root = new QWidget(parent);
	root->setObjectName(QStringLiteral("countdownTimersPage"));
	auto *layout = new QVBoxLayout(root);
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);

	auto *actions = new QHBoxLayout;
	auto *add = new QPushButton(QString::fromUtf8(obs_module_text("CountdownAdd")), root);
	auto *resetActive = new QPushButton(QString::fromUtf8(obs_module_text("CountdownResetActive")), root);
	QObject::connect(add, &QPushButton::clicked, root, [] { AddCountdown(); });
	QObject::connect(resetActive, &QPushButton::clicked, root, [] { ResetActive(); });
	actions->addWidget(add);
	actions->addWidget(resetActive);
	layout->addLayout(actions);

	auto *scroll = new QScrollArea(root);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *listHost = new QWidget(scroll);
	listLayout = new QVBoxLayout(listHost);
	listLayout->setContentsMargins(0, 0, 0, 0);
	listLayout->setSpacing(0);
	scroll->setWidget(listHost);
	layout->addWidget(scroll, 1);

	page = root;
	if (!tick) {
		tick = new QTimer(qApp);
		tick->setInterval(200);
		tick->setTimerType(Qt::PreciseTimer);
		QObject::connect(tick, &QTimer::timeout, [] { OnTick(); });
	}
	ConnectSignals();
	root->hide();
	countdowns_reload();
	return root;
}

void countdowns_reload()
{
	if (shuttingDown) {
		return;
	}
	ConnectSignals();
	QVector<Countdown> previous = items;
	QVector<Countdown> loaded;
	obs_data_array_t *array = current_profile_config ? obs_data_get_array(current_profile_config, "countdowns") : nullptr;
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *data = obs_data_array_item(array, i);
		Countdown item;
		item.id = QString::fromUtf8(obs_data_get_string(data, "id"));
		if (item.id.isEmpty()) {
			item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
		}
		item.name = QString::fromUtf8(obs_data_get_string(data, "name"));
		if (item.name.isEmpty()) {
			item.name = QString::fromUtf8(obs_module_text("CountdownTimers"));
		}
		item.hours = ClampInt(obs_data_get_int(data, "hours"), 0, 99);
		item.minutes = ClampInt(obs_data_get_int(data, "minutes"), 0, 59);
		item.seconds = ClampInt(obs_data_get_int(data, "seconds"), 0, 59);
		item.endText = QString::fromUtf8(obs_data_get_string(data, "end_text"));
		item.showHours = !obs_data_has_user_value(data, "show_hours") || obs_data_get_bool(data, "show_hours");
		item.sourceName = QString::fromUtf8(obs_data_get_string(data, "source"));
		item.fontFace = QString::fromUtf8(obs_data_get_string(data, "font"));
		item.fontSize = ClampInt(obs_data_get_int(data, "font_size"), 8, 512);
		if (!obs_data_has_user_value(data, "font_size") && item.fontSize == 8) {
			item.fontSize = 72;
		}
		item.bold = obs_data_get_bool(data, "bold") || !obs_data_has_user_value(data, "bold");
		item.color = obs_data_has_user_value(data, "color") ? (uint32_t)obs_data_get_int(data, "color") : 0xFFFFFFFFu;
		item.warnColor =
			obs_data_has_user_value(data, "warn_color") ? (uint32_t)obs_data_get_int(data, "warn_color") : 0xFF0000FFu;
		item.warnSeconds = obs_data_has_user_value(data, "warn_seconds")
					   ? ClampInt(obs_data_get_int(data, "warn_seconds"), 0, 3600)
					   : 10;
		for (const auto &old : previous) {
			if (old.id != item.id) {
				continue;
			}
			item.phase = old.phase;
			item.endMs = old.endMs;
			item.remainingMs = old.remainingMs;
			item.sourceLive = old.sourceLive;
			item.lastText = old.lastText;
			item.lastColor = old.lastColor;
			item.hotkey = old.hotkey;
			item.fontDirty = old.fontFace != item.fontFace || old.fontSize != item.fontSize || old.bold != item.bold ||
					 old.color != item.color;
			break;
		}
		if (item.hotkey == OBS_INVALID_HOTKEY_ID) {
			RegisterHotkey(item);
			obs_data_array_t *hotkey = obs_data_get_array(data, "reset_hotkey");
			if (hotkey) {
				obs_hotkey_load(item.hotkey, hotkey);
				obs_data_array_release(hotkey);
			}
		}
		loaded.push_back(item);
		obs_data_release(data);
	}
	if (array) {
		obs_data_array_release(array);
	}
	for (auto &old : previous) {
		bool kept = false;
		for (const auto &item : loaded) {
			if (item.id == old.id && item.hotkey == old.hotkey) {
				kept = true;
				break;
			}
		}
		if (!kept) {
			UnregisterHotkey(old);
		}
	}
	items = loaded;
	for (auto &item : items) {
		bool live = false;
		if (!item.sourceName.isEmpty()) {
			QByteArray sourceName = item.sourceName.toUtf8();
			obs_source_t *source = obs_get_source_by_name(sourceName.constData());
			live = source && (obs_source_active(source) || obs_source_showing(source));
			if (source) {
				obs_source_release(source);
			}
		}
		if (live && item.phase == Phase::Idle && !item.sourceLive) {
			item.sourceLive = true;
			StartFresh(item);
		} else {
			item.sourceLive = live;
			if (item.phase == Phase::Idle || item.phase == Phase::Finished) {
				item.fontDirty = true;
				PushText(item);
			}
		}
	}
	RebuildRows();
	EnsureTick();
}

void countdowns_shutdown()
{
	shuttingDown = true;
	pageVisible = false;
	if (tick) {
		tick->stop();
		delete tick;
		tick = nullptr;
	}
	DisconnectSignals();
	for (auto &item : items) {
		UnregisterHotkey(item);
	}
	items.clear();
	listLayout = nullptr;
	page = nullptr;
}

void countdowns_set_visible(bool visible)
{
	pageVisible = visible;
	if (visible) {
		RefreshLabels();
	}
	EnsureTick();
}
