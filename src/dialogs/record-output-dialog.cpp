#include "record-output-dialog.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <util/config-file.h>

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

RecordOutputDialog::RecordOutputDialog(QDialog *parent, QStringList _otherNames)
	: QDialog(parent),
	  otherNames(_otherNames)
{
	setWindowTitle(QString::fromUtf8(obs_module_text("NewRecordOutputWindowTitle")));

	auto pageLayout = new QVBoxLayout;
	setModal(true);
	setContentsMargins(0, 0, 0, 0);
	setMinimumSize(520, 200);

	auto confirmButton = new QPushButton(QString::fromUtf8(obs_module_text("CreateRecordOutput")));
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
	nameField->setText(QString::fromUtf8(obs_module_text("RecordOutput")));
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

RecordOutputDialog::RecordOutputDialog(QDialog *parent, QString name, QString path, QStringList _otherNames)
	: QDialog(parent),
	  otherNames(_otherNames)
{
	outputName = name;
	recordPath = path;

	setWindowTitle(obs_module_text("EditRecordOutputWindowTitle"));

	auto pageLayout = new QVBoxLayout;
	setModal(true);
	setContentsMargins(0, 0, 0, 0);
	setMinimumSize(520, 200);

	auto confirmButton = new QPushButton(QString::fromUtf8(obs_module_text("SaveRecordOutput")));
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
	if (outputName.isEmpty() || otherNames.contains(outputName) || recordPath.isEmpty()) {
		confirmButton->setEnabled(false);
	} else {
		confirmButton->setEnabled(true);
	}
}
