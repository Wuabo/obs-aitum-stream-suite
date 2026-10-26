#pragma once
#include <QIcon>
#include <QString>
#include <obs.h>

enum class StreamPlatformKind { Unknown = 0, Twitch, YouTube, Kick };

QIcon GetSceneIcon();
QIcon GetGroupIcon();
QIcon GetIconFromType(enum obs_icon_type icon_type, const char *id = nullptr);
QIcon create2StateIcon(QString fileOn, QString fileOff);
QIcon getPlatformIconFromEndpoint(QString endpoint);
StreamPlatformKind DetectStreamPlatformFromEndpoint(const QString &endpoint);
