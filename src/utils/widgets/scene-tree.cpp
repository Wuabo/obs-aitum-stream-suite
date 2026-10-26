#include "scene-tree.hpp"

#include <QDragMoveEvent>
#include <QScrollBar>

#include <cmath>

SceneTree::SceneTree(QWidget *parent) : QListWidget(parent)
{
	setDragDropMode(InternalMove);
	setMovement(QListView::Snap);
	setSpacing(0);
}

void SceneTree::SetGridMode(bool grid)
{
	if (parentWidget()) {
		parentWidget()->setProperty("class", grid ? QString::fromUtf8("list-grid") : QString());
		parentWidget()->style()->unpolish(parentWidget());
		parentWidget()->style()->polish(parentWidget());
	}
	gridMode = grid;

	if (gridMode) {
		setResizeMode(QListView::Adjust);
		setViewMode(QListView::IconMode);
		setUniformItemSizes(true);
		setStyleSheet(QString::fromUtf8("*{padding: 0; margin: 0;}"));
	} else {
		setViewMode(QListView::ListMode);
		setResizeMode(QListView::Fixed);
		setStyleSheet(QString());
	}

	QResizeEvent event(size(), size());
	resizeEvent(&event);
}

bool SceneTree::GetGridMode() const
{
	return gridMode;
}

void SceneTree::SetGridItemWidth(int width)
{
	maxWidth = width;
}

void SceneTree::SetGridItemHeight(int height)
{
	itemHeight = height;
}

int SceneTree::GetGridItemWidth() const
{
	return maxWidth;
}

int SceneTree::GetGridItemHeight() const
{
	return itemHeight;
}

void SceneTree::resizeEvent(QResizeEvent *event)
{
	if (gridMode && count() > 0) {
		int scrollWid = verticalScrollBar()->sizeHint().width();
		const QRect lastItem = visualItemRect(item(count() - 1));
		const int h = lastItem.y() + lastItem.height();
		if (h < height()) {
			setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scrollWid = 0;
		} else {
			setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
		}

		int wid = contentsRect().width() - scrollWid - 1;
		int items = (int)std::ceil((float)wid / maxWidth);
		if (items < 1)
			items = 1;
		int itemWidth = wid / items;
		setGridSize(QSize(itemWidth, itemHeight));
		for (int i = 0; i < count(); i++)
			item(i)->setSizeHint(QSize(itemWidth, itemHeight));
	} else if (!gridMode) {
		setGridSize(QSize());
		for (int i = 0; i < count(); i++)
			item(i)->setData(Qt::SizeHintRole, QVariant());
	}

	QListWidget::resizeEvent(event);
}

void SceneTree::dropEvent(QDropEvent *event)
{
	if (event->source() != this) {
		QListWidget::dropEvent(event);
		return;
	}

	if (gridMode && count() > 0 && !selectedIndexes().isEmpty()) {
		int scrollWid = verticalScrollBar()->sizeHint().width();
		const QRect firstItem = visualItemRect(item(0));
		const QRect lastItem = visualItemRect(item(count() - 1));
		const int h = lastItem.y() + lastItem.height();
		const int firstItemY = abs(firstItem.y());
		if (h < height()) {
			setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
			scrollWid = 0;
		} else {
			setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
		}

		float wid = contentsRect().width() - scrollWid - 1;
		QPoint point = event->position().toPoint();
		int columns = (int)std::ceil(wid / maxWidth);
		if (columns < 1)
			columns = 1;
		int x = (float)point.x() / wid * columns;
		int y = (point.y() + firstItemY) / itemHeight;
		int r = x + y * columns;
		QListWidgetItem *moved = takeItem(selectedIndexes()[0].row());
		if (r < 0)
			r = 0;
		if (r > count())
			r = count();
		insertItem(r, moved);
		setCurrentItem(moved);
		resize(size());
	}

	QListWidget::dropEvent(event);
	QResizeEvent resEvent(size(), size());
	resizeEvent(&resEvent);
}

void SceneTree::RepositionGrid(QDragMoveEvent *event)
{
	if (count() <= 0)
		return;
	int scrollWid = verticalScrollBar()->sizeHint().width();
	const QRect firstItem = visualItemRect(item(0));
	const QRect lastItem = visualItemRect(item(count() - 1));
	const int h = lastItem.y() + lastItem.height();
	const int firstItemY = abs(firstItem.y());
	if (h < height()) {
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scrollWid = 0;
	} else {
		setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	}

	float wid = contentsRect().width() - scrollWid - 1;
	int columns = (int)std::ceil(wid / maxWidth);
	if (columns < 1)
		columns = 1;

	if (event && !selectedIndexes().isEmpty()) {
		QPoint point = event->position().toPoint();
		int x = (float)point.x() / wid * columns;
		int y = (point.y() + firstItemY) / itemHeight;
		int r = x + y * columns;
		int orig = selectedIndexes()[0].row();
		for (int i = 0; i < count(); i++) {
			auto *wItem = item(i);
			if (wItem->isSelected())
				continue;
			QModelIndex index = indexFromItem(wItem);
			int off = (i >= r ? 1 : 0) - (i > orig && i > r ? 1 : 0) - (i > orig && i == r ? 2 : 0);
			int xPos = (i + off) % columns;
			int yPos = (i + off) / columns;
			QSize g = gridSize();
			setPositionForIndex(QPoint(xPos * g.width(), yPos * g.height()), index);
		}
	}
}

void SceneTree::dragMoveEvent(QDragMoveEvent *event)
{
	if (gridMode)
		RepositionGrid(event);
	QListWidget::dragMoveEvent(event);
}

void SceneTree::dragLeaveEvent(QDragLeaveEvent *event)
{
	if (gridMode)
		RepositionGrid();
	QListWidget::dragLeaveEvent(event);
}

void SceneTree::rowsInserted(const QModelIndex &parent, int start, int end)
{
	QResizeEvent event(size(), size());
	resizeEvent(&event);
	QListWidget::rowsInserted(parent, start, end);
}
