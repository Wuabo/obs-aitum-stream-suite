#pragma once

#include <QObject>
#include <QString>
#include <functional>

/* One refresh at a time per platform. Parallel refreshes rotate Twitch's
 * refresh token twice and the second call is "invalid", which used to wipe
 * the saved login and open the device page. */
enum class OauthRefreshResult { Fresh, Refreshed, Failed, Reauth };

void RefreshProfileOauth(const QString &platformKey, bool force, std::function<void(OauthRefreshResult)> done);
