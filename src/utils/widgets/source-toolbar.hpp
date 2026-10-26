#pragma once

#include <obs.hpp>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <string>

class SourceToolbar : public QWidget {
	Q_OBJECT

	OBSWeakSource weakSource;

protected:
	obs_properties_t *props = nullptr;
	OBSDataAutoRelease oldData;

	void SaveOldProperties(obs_source_t *source);
	void SetUndoProperties(obs_source_t *source, bool repeatable = false);

public:
	SourceToolbar(QWidget *parent, OBSSource source);
	~SourceToolbar() override;

	OBSSource GetSource() { return OBSGetStrongRef(weakSource); }

public slots:
	virtual void Update() {}
};

class ComboSelectToolbar : public SourceToolbar {
	Q_OBJECT

protected:
	QLabel *deviceLabel = nullptr;
	QComboBox *device = nullptr;
	QPushButton *activateButton = nullptr;
	const char *prop_name = nullptr;
	bool is_int = false;

	void BuildDeviceSelectUi();

public:
	ComboSelectToolbar(QWidget *parent, OBSSource source);
	virtual void Init();

public slots:
	void onDeviceChanged(int idx);
};

class WindowCaptureToolbar : public ComboSelectToolbar {
	Q_OBJECT
public:
	WindowCaptureToolbar(QWidget *parent, OBSSource source);
	void Init() override;
};

class DisplayCaptureToolbar : public ComboSelectToolbar {
	Q_OBJECT
public:
	DisplayCaptureToolbar(QWidget *parent, OBSSource source);
	void Init() override;
};

class AudioCaptureToolbar : public ComboSelectToolbar {
	Q_OBJECT
public:
	AudioCaptureToolbar(QWidget *parent, OBSSource source);
	void Init() override;
};

class ApplicationAudioCaptureToolbar : public ComboSelectToolbar {
	Q_OBJECT
public:
	ApplicationAudioCaptureToolbar(QWidget *parent, OBSSource source);
	void Init() override;
};

class DeviceCaptureToolbar : public QWidget {
	Q_OBJECT

	OBSWeakSource weakSource;
	bool active = false;
	QString activateText;
	QString deactivateText;
	QPushButton *activateButton = nullptr;

public:
	DeviceCaptureToolbar(QWidget *parent, OBSSource source);
	OBSSource GetSource() { return OBSGetStrongRef(weakSource); }

private slots:
	void onActivateClicked();
};

class BrowserToolbar : public SourceToolbar {
	Q_OBJECT
public:
	BrowserToolbar(QWidget *parent, OBSSource source);

private slots:
	void onRefreshClicked();
};

class GameCaptureToolbar : public SourceToolbar {
	Q_OBJECT

	QLabel *modeLabel = nullptr;
	QComboBox *mode = nullptr;
	QLabel *windowLabel = nullptr;
	QComboBox *window = nullptr;

	void UpdateWindowVisibility();

public:
	GameCaptureToolbar(QWidget *parent, OBSSource source);

private slots:
	void onModeChanged(int idx);
	void onWindowChanged(int idx);
};

class ImageSourceToolbar : public SourceToolbar {
	Q_OBJECT

	QLabel *pathLabel = nullptr;
	QLineEdit *path = nullptr;

public:
	ImageSourceToolbar(QWidget *parent, OBSSource source);

private slots:
	void onBrowseClicked();
};

class ColorSourceToolbar : public SourceToolbar {
	Q_OBJECT

	QLabel *colorLabel = nullptr;
	QColor color;

	void UpdateColor();

public:
	ColorSourceToolbar(QWidget *parent, OBSSource source);

private slots:
	void onChooseClicked();
};

class TextSourceToolbar : public SourceToolbar {
	Q_OBJECT

	QLineEdit *text = nullptr;
	QFrame *emptySpace = nullptr;
	QFont font;
	QColor color;

public:
	TextSourceToolbar(QWidget *parent, OBSSource source);

private slots:
	void onSelectFontClicked();
	void onSelectColorClicked();
	void onTextChanged();
};

int FillPropertyCombo(QComboBox *c, obs_property_t *p, const std::string &cur_id, bool is_int = false);
void UpdateSourceComboToolbarProperties(QComboBox *combo, OBSSource source, obs_properties_t *props,
					const char *prop_name, bool is_int);
void UpdateSourceComboToolbarValue(QComboBox *combo, OBSSource source, int idx, const char *prop_name, bool is_int);
void SetComboItemEnabled(QComboBox *c, int idx, bool enabled);
