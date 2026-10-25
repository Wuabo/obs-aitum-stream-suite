#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <string>

class NameDialog : public QDialog {
	Q_OBJECT
public:
	static bool AskForName(QWidget *parent, const QString &title, std::string &name);
	static bool AskForName(QWidget *parent, const QString &title, const QString &text, std::string &name,
			       const QString &placeHolder = QString(), int maxSize = 170);

private:
	NameDialog(QWidget *parent);
	QLabel *label;
	QLineEdit *userText;
};
