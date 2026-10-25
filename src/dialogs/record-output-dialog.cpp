#include "record-output-dialog.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QFileDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <util/config-file.h>
#include "../utils/widgets/focus-scroll-spinbox.hpp"

namespace {

QString ObsRecordingPath()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		return {};
	}
	const char *mode = config_get_string(config, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;
	const char *dir = config_get_string(config, advanced ? "AdvOut" : "SimpleOutput",
					    advanced ? "RecFilePath" : "FilePath");
	return dir ? QString::fromUtf8(dir) : QString();
}

} // namespace

RecordOutputDialog::RecordOutputDialog(QDialog *parent, QStringList _otherNames, bool backtrack_)
	: QDialog(parent),
	  otherNames(_otherNames),
	  backtrack(backtrack_)
{
	setWindowTitle(QString::fromUtf8(obs_module_text(backtrack ? "NewBacktrackOutputWindowTitle" : "NewRecordOutputWindowTitle")));

	auto pageLayout = new QVBoxLayout;
	setModal(true);
	setContentsMargins(0, 0, 0, 0);
	setMinimumSize(520, backtrack ? 280 : 200);

	auto confirmButton =
		new QPushButton(QString::fromUtf8(obs_module_text(backtrack ? "CreateBacktrackOutput" : "CreateRecordOutput")));
	confirmButton->setEnabled(false);

	auto formLayout = new QFormLayout;
	formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	formLayout->setSpacing(12);

	auto nameField = new QLineEdit;
	connect(nameField, &QLineEdit::textEdited, [this, nameField, confirmButton] {
		outputName = nameField->text();
		validateOutputs(confirmButton);
	});
	nameField->setText(QString::fromUtf8(obs_module_text(backtrack ? "BacktrackOutput" : "RecordOutput")));
	outputName = nameField->text();
	formLayout->addRow(QString::fromUtf8(obs_module_text("OutputName")), nameField);

	QLayout *recordPathLayout = new QHBoxLayout();
	auto recordPathEdit = new QLineEdit();
	recordPathEdit->setReadOnly(true);
	recordPath = ObsRecordingPath();
	recordPathEdit->setText(recordPath);

	auto recordPathbutton = new QPushButton(QString::fromUtf8(obs_frontend_get_locale_string("Browse")));
	recordPathbutton->setProperty("themeID", "settingsButtons");
	connect(recordPathbutton, &QPushButton::clicked, [this, recordPathEdit, confirmButton] {
		const QString dir = QFileDialog::getExistingDirectory(
			this, QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.Simple.SavePath")),
			recordPathEdit->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (dir.isEmpty())
			return;
		recordPathEdit->setText(dir);
		recordPath = dir;
		validateOutputs(confirmButton);
	});

	recordPathLayout->addWidget(recordPathEdit);
	recordPathLayout->addWidget(recordPathbutton);

	formLayout->addRow(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.Simple.SavePath")),
			   recordPathLayout);

	/* Format, filename pattern and auto-split follow OBS Settings → Output. */
	filenameFormat.clear();
	fileFormat.clear();
	maxSize = 0;
	maxTime = 0;

	if (backtrack) {
		auto maxTimeSpin = new FocusScrollSpinBox();
		maxTimeSpin->setMinimum(0);
		maxTimeSpin->setMaximum(31536000);
		maxTimeSpin->setSuffix(" s");
		maxTimeSpin->setValue(30);
		maxTime = 30;
		connect(maxTimeSpin, &QSpinBox::valueChanged, [this, confirmButton](int value) {
			this->maxTime = value;
			validateOutputs(confirmButton);
		});

		auto maxSizeSpin = new FocusScrollSpinBox();
		maxSizeSpin->setMinimum(0);
		maxSizeSpin->setMaximum(1073741824);
		maxSizeSpin->setSuffix(" MB");
		connect(maxSizeSpin, &QSpinBox::valueChanged, [this, confirmButton](int value) {
			this->maxSize = value;
			validateOutputs(confirmButton);
		});

		formLayout->addRow(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.ReplayBuffer.SecondsMax")),
			maxTimeSpin);
		formLayout->addRow(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.ReplayBuffer.MegabytesMax")),
			maxSizeSpin);
	}

	auto controlsLayout = new QHBoxLayout;
	controlsLayout->setSpacing(0);
	controlsLayout->setContentsMargins(0, 0, 0, 0);

	connect(confirmButton, &QPushButton::clicked, [this] { accept(); });
	controlsLayout->addWidget(confirmButton, 0, Qt::AlignRight);

	pageLayout->addLayout(formLayout);
	pageLayout->addLayout(controlsLayout);

	setLayout(pageLayout);
	validateOutputs(confirmButton);
}

RecordOutputDialog::RecordOutputDialog(QDialog *parent, QString name, QString path, QString filename, QString format,
				       long long max_size, long long max_time, QStringList _otherNames, bool backtrack_)
	: QDialog(parent),
	  otherNames(_otherNames),
	  backtrack(backtrack_)
{
	outputName = name;
	recordPath = path;
	filenameFormat.clear();
	fileFormat.clear();
	maxSize = backtrack_ ? max_size : 0;
	maxTime = backtrack_ ? max_time : 0;
	UNUSED_PARAMETER(filename);
	UNUSED_PARAMETER(format);

	setWindowTitle(obs_module_text(backtrack ? "EditBacktrackOutputWindowTitle" : "EditRecordOutputWindowTitle"));

	auto pageLayout = new QVBoxLayout;
	setModal(true);
	setContentsMargins(0, 0, 0, 0);
	setMinimumSize(520, backtrack ? 280 : 200);

	auto confirmButton =
		new QPushButton(QString::fromUtf8(obs_module_text(backtrack ? "SaveBacktrackOutput" : "SaveRecordOutput")));
	confirmButton->setEnabled(false);

	auto formLayout = new QFormLayout;
	formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	formLayout->setSpacing(12);

	auto nameField = new QLineEdit;
	nameField->setText(outputName);
	connect(nameField, &QLineEdit::textEdited, [this, nameField, confirmButton] {
		outputName = nameField->text();
		validateOutputs(confirmButton);
	});
	formLayout->addRow(QString::fromUtf8(obs_module_text("OutputName")), nameField);

	QLayout *recordPathLayout = new QHBoxLayout();
	auto recordPathEdit = new QLineEdit();
	recordPathEdit->setReadOnly(true);
	recordPathEdit->setText(recordPath);

	auto recordPathbutton = new QPushButton(QString::fromUtf8(obs_frontend_get_locale_string("Browse")));
	recordPathbutton->setProperty("themeID", "settingsButtons");
	connect(recordPathbutton, &QPushButton::clicked, [this, recordPathEdit, confirmButton] {
		const QString dir = QFileDialog::getExistingDirectory(
			this, QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.Simple.SavePath")),
			recordPathEdit->text(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
		if (dir.isEmpty())
			return;
		recordPathEdit->setText(dir);
		recordPath = dir;
		validateOutputs(confirmButton);
	});

	recordPathLayout->addWidget(recordPathEdit);
	recordPathLayout->addWidget(recordPathbutton);

	formLayout->addRow(QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.Simple.SavePath")),
			   recordPathLayout);

	if (backtrack) {
		auto maxTimeSpin = new FocusScrollSpinBox();
		maxTimeSpin->setMinimum(0);
		maxTimeSpin->setMaximum(31536000);
		maxTimeSpin->setSuffix(" s");
		maxTimeSpin->setValue(maxTime);
		connect(maxTimeSpin, &QSpinBox::valueChanged, [this, confirmButton](int value) {
			this->maxTime = value;
			validateOutputs(confirmButton);
		});

		auto maxSizeSpin = new FocusScrollSpinBox();
		maxSizeSpin->setMinimum(0);
		maxSizeSpin->setMaximum(1073741824);
		maxSizeSpin->setSuffix(" MB");
		maxSizeSpin->setValue(maxSize);
		connect(maxSizeSpin, &QSpinBox::valueChanged, [this, confirmButton](int value) {
			this->maxSize = value;
			validateOutputs(confirmButton);
		});

		formLayout->addRow(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.ReplayBuffer.SecondsMax")),
			maxTimeSpin);
		formLayout->addRow(
			QString::fromUtf8(obs_frontend_get_locale_string("Basic.Settings.Output.ReplayBuffer.MegabytesMax")),
			maxSizeSpin);
	}

	auto controlsLayout = new QHBoxLayout;
	controlsLayout->setSpacing(0);
	controlsLayout->setContentsMargins(0, 0, 0, 0);

	connect(confirmButton, &QPushButton::clicked, [this] { accept(); });
	controlsLayout->addWidget(confirmButton, 0, Qt::AlignRight);

	pageLayout->addLayout(formLayout);
	pageLayout->addLayout(controlsLayout);

	setLayout(pageLayout);
	validateOutputs(confirmButton);
}

void RecordOutputDialog::validateOutputs(QPushButton *confirmButton)
{
	if (outputName.isEmpty() || otherNames.contains(outputName)) {
		confirmButton->setEnabled(false);
	} else if (recordPath.isEmpty()) {
		confirmButton->setEnabled(false);
	} else if (backtrack && maxSize == 0 && maxTime == 0) {
		confirmButton->setEnabled(false);
	} else {
		confirmButton->setEnabled(true);
	}
}
