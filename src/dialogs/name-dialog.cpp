#include "name-dialog.hpp"

#include <QVBoxLayout>

#include "obs-module.h"

NameDialog::NameDialog(QWidget *parent) : QDialog(parent)
{
	setModal(true);
	setWindowModality(Qt::WindowModality::WindowModal);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setFixedWidth(555);
	setMinimumHeight(100);
	QVBoxLayout *layout = new QVBoxLayout;
	setLayout(layout);

	label = new QLabel(this);
	layout->addWidget(label);

	userText = new QLineEdit(this);
	layout->addWidget(userText);
	userText->setFocus();

	QDialogButtonBox *buttonbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget(buttonbox);
	buttonbox->setCenterButtons(true);
	connect(buttonbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttonbox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	adjustSize();
}

static bool IsWhitespace(char ch)
{
	return ch == ' ' || ch == '\t';
}

static void CleanWhitespace(std::string &str)
{
	while (!str.empty() && IsWhitespace(str.back()))
		str.erase(str.end() - 1);
	while (!str.empty() && IsWhitespace(str.front()))
		str.erase(str.begin());
}

bool NameDialog::AskForName(QWidget *parent, const QString &title, const QString &text, std::string &name,
			    const QString &placeHolder, int maxSize)
{
	if (maxSize <= 0 || maxSize > 32767)
		maxSize = 170;

	NameDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.label->setText(text);
	dialog.userText->setMaxLength(maxSize);
	dialog.userText->setText(placeHolder);
	dialog.userText->selectAll();

	if (dialog.exec() != DialogCode::Accepted)
		return false;

	name = dialog.userText->text().toUtf8().constData();
	CleanWhitespace(name);
	return true;
}

bool NameDialog::AskForName(QWidget *parent, const QString &title, std::string &name)
{
	QString current = QString::fromUtf8(name.c_str());
	return AskForName(parent, title, QString(), name, current);
}
