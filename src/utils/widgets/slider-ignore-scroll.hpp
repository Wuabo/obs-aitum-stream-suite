#pragma once

#include <QSlider>

class SliderIgnoreScroll : public QSlider {
	Q_OBJECT

public:
	SliderIgnoreScroll(QWidget *parent = nullptr);
	SliderIgnoreScroll(Qt::Orientation orientation, QWidget *parent = nullptr);

protected:
	virtual void wheelEvent(QWheelEvent *event) override;
};
