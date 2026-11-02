#include "channel-dock.hpp"
#include "alerts-dock.hpp"
#include "native-dock-tabs.hpp"
#include "stream-info-dock.hpp"

#include <obs-module.h>
#include <QSizePolicy>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

ChannelDock::ChannelDock(AlertsDock *activity, StreamInfoDock *info, QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteChannel"));
	auto *tabs = new QTabWidget(this);
	tabs->setObjectName(QStringLiteral("channelTabs"));
	tabs->setDocumentMode(true);
	tabs->setTabPosition(QTabWidget::North);
	tabs->setUsesScrollButtons(true);
	style_suite_tab_bar(tabs->tabBar());
	tabs->setStyleSheet(QStringLiteral("QTabWidget#channelTabs::pane { border: none; top: 0px; }"));
	tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	if (activity)
		tabs->addTab(activity, QString::fromUtf8(obs_module_text("AitumStreamSuiteActivity")));
	if (info)
		tabs->addTab(info, QString::fromUtf8(obs_module_text("AitumStreamSuiteStreamInfo")));

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(tabs);
}
