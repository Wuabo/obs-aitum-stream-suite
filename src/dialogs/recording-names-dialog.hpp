#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QString>
#include <vector>

struct RecordingNameEntry {
	QString id;
	QString label;
	QString suggested;
};

class RecordingNamesDialog : public QDialog {
	Q_OBJECT

public:
	/* Returns false if cancelled. On success, entries[].suggested holds the chosen base name (no extension). */
	static bool Ask(QWidget *parent, std::vector<RecordingNameEntry> &entries);

private:
	explicit RecordingNamesDialog(QWidget *parent, std::vector<RecordingNameEntry> &entries);

	std::vector<RecordingNameEntry> &entries;
	std::vector<QLineEdit *> edits;
};
