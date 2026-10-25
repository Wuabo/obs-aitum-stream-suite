#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QMainWindow>
#include <obs-frontend-api.h>
#include <QGroupBox>
#include <QListWidget>
#include <QSpinBox>
#include <QFormLayout>
#include <QRadioButton>
#include <QLabel>
#include <QString>
#include <QToolButton>
#include <src/utils/widgets/hotkey-edit.hpp>

class OBSBasicSettings : public QDialog {
	Q_OBJECT
private:
	QListWidget *listWidget;

	void AddCanvas(QFormLayout *canvasLayout, obs_data_t *settings, obs_data_array_t *canvas);
	void AddUnmanagedCanvas(const std::string name);
	void AddOutput(QFormLayout *outputsLayout, obs_data_t *settings, obs_data_array_t *outputs, bool isNew = false);
	void AddVideoEncoderPage(QTabWidget *tabWidget, obs_data_t *output_settings, obs_data_array_t *outputs,
				 QComboBox *canvasCombo, int idx);
	void AddProperty(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings, QFormLayout *layout);
	void RefreshProperties(obs_properties_t *properties, QFormLayout *layout);
	void AddCanvas();

	void LoadOutputLayout(obs_data_t *settings, QFormLayout *outputLayout, obs_data_array_t *outputs,
			      QLabel *titleLabel, QCheckBox *expandToggle, bool isNew);
	bool UpdateVideoEncoderIndexCombo(QComboBox *videoEncoderIndex, obs_data_t *settings, obs_data_array_t *outputs);

	obs_data_t *main_settings = nullptr;
	obs_data_array_t *extra_outputs = nullptr;

	std::map<obs_property_t *, QWidget *> encoder_property_widgets;
	std::map<QWidget *, obs_properties_t *> video_encoder_properties;
	std::map<QWidget *, obs_properties_t *> audio_encoder_properties;

	std::vector<std::pair<QComboBox *, QString>> sourceCombos;

	QFormLayout *canvasLayout;
	QFormLayout *outputsLayout;

	QCheckBox *mainStream;
	QCheckBox *mainRecord;
	QComboBox *mainRecordCanvas;

	std::vector<OBSHotkeyWidget *> hotkeys;

	void AddStream();
	void AddRecord();
	void LoadSourceCombos();
	void StyleSettingsCard(QGroupBox *box);
	QCheckBox *MakeExpandIndicator(bool expanded);

	obs_hotkey_t *GetHotkeyByName(const char *name);
	std::vector<obs_key_combination_t> GetCombosForHotkey(obs_hotkey_id hotkey);

public:
	OBSBasicSettings(QMainWindow *parent = nullptr);
	~OBSBasicSettings();

	void LoadSettings(obs_data_t *settings);
	void ShowTab(int i);
	void SetCreateType(const char *create_type);
	void SaveHotkeys();
public slots:
};
