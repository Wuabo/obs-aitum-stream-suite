#pragma once

#include <QFrame>

class AlertsDock;
class StreamInfoDock;

/* Activity Feed and Stream Info share one dock. Each page keeps its own behavior. */
class ChannelDock : public QFrame {
	Q_OBJECT

public:
	explicit ChannelDock(AlertsDock *activity, StreamInfoDock *info, QWidget *parent = nullptr);
	QSize sizeHint() const override { return QSize(300, 480); }
	QSize minimumSizeHint() const override { return QSize(200, 160); }
};
