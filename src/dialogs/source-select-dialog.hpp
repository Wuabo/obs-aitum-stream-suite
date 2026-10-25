#pragma once

#include <obs.hpp>

#include <QAbstractButton>
#include <QDialog>
#include <QEnterEvent>
#include <QPointer>
#include <QShowEvent>

class QButtonGroup;
class QCheckBox;
class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;

class FlowLayout;

/* Class name matches the OBS theme selector for the native add-source cards. */
class SourceSelectButton : public QAbstractButton {
	Q_OBJECT

public:
	explicit SourceSelectButton(obs_weak_source_t *weak, QWidget *parent = nullptr);

	std::string uuid() const { return sourceUuid; }
	void SetPreview(const QImage &image);
	/* priority: hover, same as ThumbnailView::requestUpdate. */
	void RequestPreview(bool priority);
	void SetPreviewEnabled(bool enabled) { previewEnabled = enabled; }
	bool PreviewEnabled() const { return previewEnabled; }

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	OBSWeakSource weakSource;
	std::string sourceUuid;
	QLabel *image = nullptr;
	QLabel *label = nullptr;
	bool previewEnabled = true;
};

void CancelSourcePreviews();

class QTimer;

class SourceSelectDialog : public QDialog {
	Q_OBJECT

public:
	SourceSelectDialog(QWidget *parent, obs_scene_t *scene);

protected:
	void showEvent(QShowEvent *event) override;

public slots:
	void OnCreateNew();
	void OnAddSelected();
	void OnTypeChanged(QListWidgetItem *current, QListWidgetItem *previous);
	void OnButtonToggled(QAbstractButton *button, bool checked);
	void OnSourceCreated();
	void OnSourceRemoved(QString uuid);

private:
	static constexpr int kUnversionedIdRole = Qt::UserRole + 1;
	static constexpr int kDeprecatedRole = Qt::UserRole + 2;
	static constexpr int kRecentLimit = 16;

	OBSWeakSource sceneWeak;
	QString selectedTypeId;
	std::vector<OBSWeakSource> weakSources;
	std::vector<std::string> selectedItems;
	std::vector<OBSSignal> signalHandlers;
	int lastSelectedIndex = -1;

	QListWidget *sourceTypeList = nullptr;
	QLabel *sourceSelectTitle = nullptr;
	QLabel *deprecatedLabel = nullptr;
	QFrame *deprecatedFrame = nullptr;
	QPushButton *createNewSource = nullptr;
	QLabel *noExistingLabel = nullptr;
	QScrollArea *existingScrollArea = nullptr;
	QWidget *existingListFrame = nullptr;
	FlowLayout *existingFlowLayout = nullptr;
	QPushButton *addExistingButton = nullptr;
	QCheckBox *sourceVisible = nullptr;
	QPointer<QButtonGroup> sourceButtons;
	QTimer *previewRefreshTimer = nullptr;

	static void SourceCreated(void *param, calldata_t *calldata);
	static void SourceRemoved(void *param, calldata_t *calldata);
	static bool EnumSources(void *data, obs_source_t *source);

	void RebuildTypeList();
	void RefreshSources();
	void UpdateExistingSources(int limit);
	void UpdatePreviewVisibility();
	void AddSelectedItem(const std::string &uuid);
	void RemoveSelectedItem(const std::string &uuid);
	void ClearSelectedItems();
	SourceSelectButton *FindButton(const std::string &uuid) const;
	bool AddToScene(obs_source_t *source, bool visible);
	QString DisplayNameForType(const QString &type) const;
	QString UniqueSourceName(const QString &base, bool onCanvas) const;
};

void OpenSourceSelectDialog(QWidget *parent, obs_scene_t *scene);
