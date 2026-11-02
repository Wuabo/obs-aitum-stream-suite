#include "oauth-refresh.hpp"
#include "profile-secret.hpp"

#include <obs-module.h>
#include <util/base.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QUrl>
#include <QUrlQuery>

#include <curl/curl.h>

#include <thread>

extern obs_data_t *current_profile_config;
void save_current_profile_config(bool save_docks);

namespace {

struct HttpResult {
	int status = 0;
	QByteArray body;
	QString error;
};

struct Job {
	bool running = false;
	QList<std::function<void(OauthRefreshResult)>> waiters;
};

QMutex mutex;
QHash<QString, Job> jobs;

size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *out = static_cast<QByteArray *>(userdata);
	const size_t total = size * nmemb;
	out->append(ptr, static_cast<qsizetype>(total));
	return total;
}

HttpResult PostForm(const QByteArray &url, const QByteArray &body)
{
	HttpResult result;
	CURL *curl = curl_easy_init();
	if (!curl) {
		result.error = QStringLiteral("curl_easy_init failed");
		return result;
	}
	curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
	char error[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_URL, url.constData());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	result.status = static_cast<int>(status);
	if (code != CURLE_OK)
		result.error = QString::fromUtf8(error[0] ? error : curl_easy_strerror(code));
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return result;
}

void Deliver(const QString &key, OauthRefreshResult result)
{
	QList<std::function<void(OauthRefreshResult)>> waiters;
	{
		QMutexLocker lock(&mutex);
		Job &job = jobs[key];
		waiters.swap(job.waiters);
		job.running = false;
	}
	for (const auto &waiter : waiters)
		waiter(result);
}

bool GrantRejected(const QJsonObject &json)
{
	const QString error = json.value(QStringLiteral("error")).toString();
	const QString message = json.value(QStringLiteral("message")).toString();
	if (error == QLatin1String("invalid_grant") || error == QLatin1String("invalid_token"))
		return true;
	return message.contains(QStringLiteral("Invalid refresh token"), Qt::CaseInsensitive);
}

void StoreTokens(const QString &key, const QString &access, const QString &refresh, qint64 expiresAt, bool clear)
{
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, key.toUtf8().constData()) : nullptr;
	if (!one) {
		obs_data_release(root);
		return;
	}
	if (clear) {
		obs_data_set_string(one, "access_token", "");
		obs_data_set_string(one, "refresh_token", "");
		obs_data_set_int(one, "expires_at", 0);
	} else {
		StoreProfileSecret(one, "access_token", access);
		StoreProfileSecret(one, "refresh_token", refresh);
		obs_data_set_int(one, "expires_at", expiresAt);
	}
	obs_data_set_obj(root, key.toUtf8().constData(), one);
	obs_data_set_obj(current_profile_config, "stream_accounts", root);
	obs_data_release(one);
	obs_data_release(root);
	save_current_profile_config(false);
}

void Finish(const QString &key, OauthRefreshResult result, const QString &access, const QString &refresh, qint64 expiresAt,
	    bool wipe)
{
	if (result == OauthRefreshResult::Refreshed)
		StoreTokens(key, access, refresh, expiresAt, false);
	else if (wipe)
		StoreTokens(key, {}, {}, 0, true);
	Deliver(key, result);
}

} // namespace

void RefreshProfileOauth(const QString &platformKey, bool force, std::function<void(OauthRefreshResult)> done)
{
	if (platformKey.isEmpty() || !done)
		return;
	{
		QMutexLocker lock(&mutex);
		Job &job = jobs[platformKey];
		job.waiters.append(std::move(done));
		if (job.running)
			return;
		job.running = true;
	}

	QString access;
	QString refresh;
	QString clientId;
	QString clientSecret;
	qint64 expiresAt = 0;
	if (current_profile_config) {
		obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
		obs_data_t *one = root ? obs_data_get_obj(root, platformKey.toUtf8().constData()) : nullptr;
		if (one) {
			clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
			clientSecret = RevealProfileSecret(obs_data_get_string(one, "client_secret"));
			access = RevealProfileSecret(obs_data_get_string(one, "access_token"));
			refresh = RevealProfileSecret(obs_data_get_string(one, "refresh_token"));
			expiresAt = obs_data_get_int(one, "expires_at");
		}
		obs_data_release(one);
		obs_data_release(root);
	}

	const qint64 now = QDateTime::currentSecsSinceEpoch();
	/* expiresAt <= 0 used to mean "never refresh". Twitch tokens die in a few
	 * hours, so a missing expiry is refreshed whenever a refresh token exists. */
	const bool fresh = !access.isEmpty() && expiresAt > now + 120;
	if (!force && fresh) {
		Finish(platformKey, OauthRefreshResult::Fresh, {}, {}, 0, false);
		return;
	}
	if (refresh.isEmpty() || clientId.isEmpty()) {
		/* Keep the saved access token. A missing refresh token is not proof it was revoked. */
		Finish(platformKey, access.isEmpty() ? OauthRefreshResult::Failed : OauthRefreshResult::Reauth, {}, {}, 0, false);
		return;
	}

	QUrl url;
	if (platformKey == QLatin1String("youtube"))
		url = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
	else if (platformKey == QLatin1String("kick"))
		url = QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
	else
		url = QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
	query.addQueryItem(QStringLiteral("refresh_token"), refresh);
	query.addQueryItem(QStringLiteral("client_id"), clientId);
	if (platformKey != QLatin1String("twitch"))
		query.addQueryItem(QStringLiteral("client_secret"), clientSecret);
	const QByteArray urlBytes = url.toString(QUrl::FullyEncoded).toUtf8();
	const QByteArray body = query.query(QUrl::FullyEncoded).toUtf8();
	const QString key = platformKey;
	const QString refreshCopy = refresh;

	std::thread([key, urlBytes, body, refreshCopy] {
		const HttpResult http = PostForm(urlBytes, body);
		QMetaObject::invokeMethod(
			qApp, [key, http, refreshCopy] {
				const QJsonObject json = QJsonDocument::fromJson(http.body).object();
				if (http.status < 200 || http.status >= 300 || !json.contains(QStringLiteral("access_token"))) {
					if (GrantRejected(json)) {
						blog(LOG_WARNING, "[Stream Suite] %s refresh token rejected", key.toUtf8().constData());
						Finish(key, OauthRefreshResult::Reauth, {}, {}, 0, true);
					} else {
						blog(LOG_WARNING, "[Stream Suite] %s token refresh failed (%d)", key.toUtf8().constData(),
						     http.status);
						Finish(key, OauthRefreshResult::Failed, {}, {}, 0, false);
					}
					return;
				}
				const QString access = json.value(QStringLiteral("access_token")).toString();
				const QString refresh = json.contains(QStringLiteral("refresh_token"))
								? json.value(QStringLiteral("refresh_token")).toString()
								: refreshCopy;
				const int expiresIn = json.value(QStringLiteral("expires_in")).toInt(0);
				const qint64 expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
				Finish(key, OauthRefreshResult::Refreshed, access, refresh, expiresAt, false);
			},
			Qt::QueuedConnection);
	}).detach();
}
