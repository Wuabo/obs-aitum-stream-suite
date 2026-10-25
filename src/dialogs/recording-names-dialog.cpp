#include "recording-names-dialog.hpp"

#include <obs-module.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QVBoxLayout>

namespace {

QString NormalizeRecordingName(QString name)
{
	name = name.trimmed();
	if (name.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive) ||
	    name.endsWith(QStringLiteral(".mov"), Qt::CaseInsensitive) ||
	    name.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive) ||
	    name.endsWith(QStringLiteral(".flv"), Qt::CaseInsensitive) ||
	    name.endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive) ||
	    name.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
		name = name.left(name.lastIndexOf(QLatin1Char('.')));
	}
#ifdef _WIN32
	name.replace(QRegularExpression(QStringLiteral("[<>:\"|?*\\\\/]")), QString());
#else
	name.replace(QLatin1Char('/'), QString());
#endif
	return name.trimmed();
}

} // namespace

RecordingNamesDialog::RecordingNamesDialog(QWidget *parent, std::vector<RecordingNameEntry> &entries_)
	: QDialog(parent),
	  entries(entries_)
{
	setModal(true);
	setWindowModality(Qt::WindowModal);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setMinimumWidth(480);
	setWindowTitle(QString::fromUtf8(obs_module_text("RecordingFileNames")));

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(new QLabel(QString::fromUtf8(obs_module_text("RecordingFileNames.Help")), this));

	auto *form = new QFormLayout;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	edits.reserve(entries.size());
	for (RecordingNameEntry &entry : entries) {
		auto *edit = new QLineEdit(entry.suggested, this);
		edit->setClearButtonEnabled(true);
		form->addRow(entry.label, edit);
		edits.push_back(edit);
	}
	layout->addLayout(form);

	auto *errorLabel = new QLabel(this);
	errorLabel->setObjectName(QStringLiteral("recordingNamesError"));
	errorLabel->setStyleSheet(QStringLiteral("color: #e74c3c;"));
	errorLabel->setWordWrap(true);
	errorLabel->hide();
	layout->addWidget(errorLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->setCenterButtons(true);
	layout->addWidget(buttons);
	auto *okButton = buttons->button(QDialogButtonBox::Ok);

	auto validate = [this, errorLabel, okButton] {
		QSet<QString> seen;
		bool duplicate = false;
		bool empty = false;
		for (QLineEdit *edit : edits) {
			const QString name = NormalizeRecordingName(edit->text());
			if (name.isEmpty()) {
				empty = true;
				continue;
			}
#ifdef _WIN32
			const QString key = name.toLower();
#else
			const QString key = name;
#endif
			if (seen.contains(key)) {
				duplicate = true;
				break;
			}
			seen.insert(key);
		}
		if (duplicate) {
			errorLabel->setText(QString::fromUtf8(obs_module_text("RecordingFileNames.Duplicate")));
			errorLabel->show();
			okButton->setEnabled(false);
		} else if (empty) {
			errorLabel->setText(QString::fromUtf8(obs_module_text("RecordingFileNames.Empty")));
			errorLabel->show();
			okButton->setEnabled(false);
		} else {
			errorLabel->hide();
			okButton->setEnabled(true);
		}
	};

	for (QLineEdit *edit : edits) {
		connect(edit, &QLineEdit::textChanged, this, validate);
	}
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	validate();

	if (!edits.empty()) {
		edits.front()->setFocus();
		edits.front()->selectAll();
	}
	adjustSize();
}

bool RecordingNamesDialog::Ask(QWidget *parent, std::vector<RecordingNameEntry> &entries)
{
	if (entries.empty()) {
		return true;
	}

	RecordingNamesDialog dialog(parent, entries);
	if (dialog.exec() != QDialog::Accepted) {
		return false;
	}

	for (size_t i = 0; i < entries.size() && i < dialog.edits.size(); i++) {
		QString name = NormalizeRecordingName(dialog.edits[i]->text());
		if (name.isEmpty()) {
			name = entries[i].suggested;
		}
		entries[i].suggested = name;
	}
	return true;
}
