#pragma once

#include <QListWidget>

/* Same class name as OBS so the Yami theme rules for SceneTree apply. */
class SceneTree : public QListWidget {
	Q_OBJECT
	Q_PROPERTY(int gridItemWidth READ GetGridItemWidth WRITE SetGridItemWidth)
	Q_PROPERTY(int gridItemHeight READ GetGridItemHeight WRITE SetGridItemHeight)

	bool gridMode = false;
	int maxWidth = 150;
	int itemHeight = 24;

public:
	explicit SceneTree(QWidget *parent = nullptr);

	void SetGridMode(bool grid);
	bool GetGridMode() const;
	void SetGridItemWidth(int width);
	void SetGridItemHeight(int height);
	int GetGridItemWidth() const;
	int GetGridItemHeight() const;

protected:
	void resizeEvent(QResizeEvent *event) override;
	void dropEvent(QDropEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void rowsInserted(const QModelIndex &parent, int start, int end) override;

private:
	void RepositionGrid(QDragMoveEvent *event = nullptr);
};
