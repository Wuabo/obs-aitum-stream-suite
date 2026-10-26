#pragma once

#include "slider-ignore-scroll.hpp"

class AbsoluteSlider : public SliderIgnoreScroll {
	Q_OBJECT

public:
	AbsoluteSlider(QWidget *parent = nullptr);
	AbsoluteSlider(Qt::Orientation orientation, QWidget *parent = nullptr);

signals:
	void absoluteSliderHovered(int value);

protected:
	void mouseMoveEvent(QMouseEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	bool eventFilter(QObject *obj, QEvent *event) override;

	int posToRangeValue(QMouseEvent *event);

private:
	bool dragging = false;
};
