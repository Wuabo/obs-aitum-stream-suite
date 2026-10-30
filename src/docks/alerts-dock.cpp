#ifdef _WIN32
#include <winsock2.h>
#endif

#include "alerts-dock.hpp"
#include "stream-info-dock.hpp"

#include <obs-module.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QDateTime>
#include <QIcon>
#include <QList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QVBoxLayout>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <thread>

extern obs_data_t *current_profile_config;
extern StreamInfoDock *stream_info_dock;

AlertsDock *alerts_dock = nullptr;

namespace {

QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void ApplyCurlSsl(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aitum-stream-suite");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef _WIN32
#ifdef CURLSSLOPT_REVOKE_BEST_EFFORT
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
#elif defined(CURLSSLOPT_NO_REVOKE)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
#endif
#endif
}

size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	static_cast<QByteArray *>(userdata)->append(ptr, int(size * nmemb));
	return size * nmemb;
}

int CurlJson(const QByteArray &url, const QByteArray &verb, const QList<QPair<QByteArray, QByteArray>> &headers,
	     const QByteArray &body, QByteArray *response)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return 0;
	ApplyCurlSsl(curl);
	curl_slist *list = nullptr;
	for (const auto &header : headers) {
		const QByteArray line = header.first + ": " + header.second;
		list = curl_slist_append(list, line.constData());
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.constData());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	if (verb != "GET") {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.constData());
		if (!body.isEmpty()) {
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.constData());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
		}
	}
	curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
	return int(code);
}

bool CurlWsWait(CURL *curl, int timeoutMs)
{
	curl_socket_t sock = CURL_SOCKET_BAD;
	if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD)
		return false;
#ifdef _WIN32
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	return select(0, &fds, nullptr, nullptr, &tv) >= 0;
#else
	return false;
#endif
}

QString PlatformName(int platform)
{
	switch (platform) {
	case 1:
		return QStringLiteral("YouTube");
	case 2:
		return QStringLiteral("Kick");
	default:
		return QStringLiteral("Twitch");
	}
}

QIcon PlatformIcon(int platform)
{
	switch (platform) {
	case 1:
		return QIcon(QStringLiteral(":/aitum/media/youtube.png"));
	case 2:
		return QIcon(QStringLiteral(":/aitum/media/kick.png"));
	default:
		return QIcon(QStringLiteral(":/aitum/media/twitch.png"));
	}
}

QString KindLabel(const QString &kind)
{
	if (kind == QLatin1String("follow"))
		return Tr("AlertFollow");
	if (kind == QLatin1String("sub"))
		return Tr("AlertSub");
	if (kind == QLatin1String("gift"))
		return Tr("AlertGift");
	if (kind == QLatin1String("bits"))
		return Tr("AlertBits");
	if (kind == QLatin1String("raid"))
		return Tr("AlertRaid");
	if (kind == QLatin1String("superchat"))
		return Tr("AlertSuperChat");
	if (kind == QLatin1String("member"))
		return Tr("AlertMember");
	return kind;
}

} // namespace

AlertsDock::AlertsDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteAlerts"));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	list = new QListWidget(this);
	list->setObjectName(QStringLiteral("alertsList"));
	list->setSelectionMode(QAbstractItemView::NoSelection);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setStyleSheet(QStringLiteral(
		"QListWidget#alertsList { background: #141414; border: none; color: #eee; outline: none; }"
		"QListWidget#alertsList::item { padding: 6px 8px; }"));
	layout->addWidget(list);
}

AlertsDock::~AlertsDock()
{
	StopEventSub();
}

void AlertsDock::LoadTwitchAccount()
{
	clientId.clear();
	accessToken.clear();
	userId.clear();
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, "twitch") : nullptr;
	if (one) {
		clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
		accessToken = QString::fromUtf8(obs_data_get_string(one, "access_token"));
		userId = QString::fromUtf8(obs_data_get_string(one, "user_id"));
	}
	obs_data_release(one);
	obs_data_release(root);
}

void AlertsDock::ReloadAccounts()
{
	const QString previousToken = accessToken;
	const QString previousUser = userId;
	LoadTwitchAccount();
	if (accessToken == previousToken && userId == previousUser && (session || accessToken.isEmpty()))
		return;
	StopEventSub();
	askedReconnect = false;
	if (accessToken.isEmpty() || clientId.isEmpty())
		return;
	if (!userId.isEmpty()) {
		StartEventSub(QStringLiteral("wss://eventsub.wss.twitch.tv/ws"), true);
		return;
	}
	const QByteArray token = accessToken.toUtf8();
	const QByteArray cid = clientId.toUtf8();
	QPointer<AlertsDock> alive(this);
	std::thread([alive, token, cid] {
		QByteArray response;
		const int status = CurlJson(QByteArrayLiteral("https://api.twitch.tv/helix/users"), QByteArrayLiteral("GET"),
					     {{QByteArrayLiteral("Client-Id"), cid},
					      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + token}},
					     {}, &response);
		const QString id = QJsonDocument::fromJson(response)
					   .object()
					   .value(QStringLiteral("data"))
					   .toArray()
					   .at(0)
					   .toObject()
					   .value(QStringLiteral("id"))
					   .toString();
		if (!alive)
			return;
		QMetaObject::invokeMethod(
			alive,
			[alive, status, id] {
				if (!alive || status < 200 || status >= 300 || id.isEmpty())
					return;
				alive->userId = id;
				alive->StartEventSub(QStringLiteral("wss://eventsub.wss.twitch.tv/ws"), true);
			},
			Qt::QueuedConnection);
	}).detach();
}

void AlertsDock::StopEventSub()
{
	wsGen++;
	if (session) {
		session->stop = true;
		session.reset();
	}
}

void AlertsDock::StartEventSub(const QString &url, bool subscribeOnWelcome)
{
	StopEventSub();
	if (url.isEmpty() || accessToken.isEmpty())
		return;
	const int gen = wsGen;
	auto held = std::make_shared<WsSession>();
	session = held;
	QPointer<AlertsDock> alive(this);
	const QByteArray urlBytes = url.toUtf8();
	std::thread([alive, held, gen, urlBytes, subscribeOnWelcome] {
		CURL *curl = curl_easy_init();
		if (!curl)
			return;
		ApplyCurlSsl(curl);
		curl_easy_setopt(curl, CURLOPT_URL, urlBytes.constData());
		curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
		if (curl_easy_perform(curl) != CURLE_OK) {
			blog(LOG_WARNING, "[Stream Suite] Twitch EventSub connect failed");
			curl_easy_cleanup(curl);
			return;
		}
		QByteArray message;
		while (!held->stop.load()) {
			char buffer[8192];
			size_t n = 0;
			const curl_ws_frame *meta = nullptr;
			const CURLcode rc = curl_ws_recv(curl, buffer, sizeof(buffer), &n, &meta);
			if (rc == CURLE_AGAIN) {
				CurlWsWait(curl, 200);
				continue;
			}
			if (rc != CURLE_OK || !meta)
				break;
			if (meta->flags & CURLWS_CLOSE)
				break;
			if (meta->flags & CURLWS_PING) {
				size_t sent = 0;
				curl_ws_send(curl, buffer, n, &sent, 0, CURLWS_PONG);
				continue;
			}
			if (!(meta->flags & CURLWS_TEXT) || n == 0)
				continue;
			message.append(buffer, int(n));
			if (meta->bytesleft > 0)
				continue;
			const QByteArray payload = message;
			message.clear();
			if (!alive)
				break;
			QMetaObject::invokeMethod(
				alive,
				[alive, gen, payload, subscribeOnWelcome] {
					if (!alive || alive->wsGen != gen)
						return;
					alive->HandleEventSubMessage(payload, gen, subscribeOnWelcome);
				},
				Qt::QueuedConnection);
		}
		curl_easy_cleanup(curl);
	}).detach();
}

void AlertsDock::SubscribeEventSub(const QString &sessionId)
{
	const QByteArray token = accessToken.toUtf8();
	const QByteArray cid = clientId.toUtf8();
	const QByteArray user = userId.toUtf8();
	const QByteArray session = sessionId.toUtf8();
	QPointer<AlertsDock> alive(this);
	std::thread([alive, token, cid, user, session] {
		struct Item {
			const char *type;
			const char *version;
			bool raid;
			bool follow;
		};
		const Item items[] = {
			{"channel.follow", "2", false, true},
			{"channel.subscribe", "1", false, false},
			{"channel.subscription.message", "1", false, false},
			{"channel.subscription.gift", "1", false, false},
			{"channel.cheer", "1", false, false},
			{"channel.raid", "1", true, false},
		};
		bool authFailed = false;
		for (const Item &item : items) {
			QJsonObject condition;
			if (item.raid)
				condition.insert(QStringLiteral("to_broadcaster_user_id"), QString::fromUtf8(user));
			else
				condition.insert(QStringLiteral("broadcaster_user_id"), QString::fromUtf8(user));
			if (item.follow)
				condition.insert(QStringLiteral("moderator_user_id"), QString::fromUtf8(user));
			QJsonObject body;
			body.insert(QStringLiteral("type"), QString::fromUtf8(item.type));
			body.insert(QStringLiteral("version"), QString::fromUtf8(item.version));
			body.insert(QStringLiteral("condition"), condition);
			body.insert(QStringLiteral("transport"),
				    QJsonObject{{QStringLiteral("method"), QStringLiteral("websocket")},
						{QStringLiteral("session_id"), QString::fromUtf8(session)}});
			QByteArray response;
			const int status =
				CurlJson(QByteArrayLiteral("https://api.twitch.tv/helix/eventsub/subscriptions"),
					 QByteArrayLiteral("POST"),
					 {{QByteArrayLiteral("Client-Id"), cid},
					  {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + token},
					  {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
					 QJsonDocument(body).toJson(QJsonDocument::Compact), &response);
			if (status == 401 || status == 403)
				authFailed = true;
			else if (status != 409 && (status < 200 || status >= 300))
				blog(LOG_WARNING, "[Stream Suite] EventSub %s HTTP %d: %s", item.type, status,
				     response.left(180).constData());
		}
		if (!authFailed || !alive)
			return;
		QMetaObject::invokeMethod(
			alive,
			[alive] {
				if (!alive || alive->askedReconnect)
					return;
				alive->askedReconnect = true;
				if (stream_info_dock)
					stream_info_dock->RequestModerationAuth(0);
			},
			Qt::QueuedConnection);
	}).detach();
}

void AlertsDock::HandleEventSubMessage(const QByteArray &payload, int gen, bool subscribeOnWelcome)
{
	if (gen != wsGen)
		return;
	const QJsonObject root = QJsonDocument::fromJson(payload).object();
	const QJsonObject meta = root.value(QStringLiteral("metadata")).toObject();
	const QString messageType = meta.value(QStringLiteral("message_type")).toString();
	const QJsonObject body = root.value(QStringLiteral("payload")).toObject();
	if (messageType == QLatin1String("session_welcome")) {
		if (!subscribeOnWelcome)
			return;
		const QString sessionId = body.value(QStringLiteral("session")).toObject().value(QStringLiteral("id")).toString();
		if (!sessionId.isEmpty())
			SubscribeEventSub(sessionId);
		return;
	}
	if (messageType == QLatin1String("session_reconnect")) {
		const QString url =
			body.value(QStringLiteral("session")).toObject().value(QStringLiteral("reconnect_url")).toString();
		if (!url.isEmpty())
			StartEventSub(url, false);
		return;
	}
	if (messageType != QLatin1String("notification"))
		return;
	const QString subscription = meta.value(QStringLiteral("subscription_type")).toString();
	const QJsonObject event = body.value(QStringLiteral("event")).toObject();
	QString user = event.value(QStringLiteral("user_name")).toString();
	if (user.isEmpty())
		user = event.value(QStringLiteral("from_broadcaster_user_name")).toString();
	QString detail;
	QString kind = QStringLiteral("follow");
	if (subscription == QLatin1String("channel.subscribe") || subscription == QLatin1String("channel.subscription.message")) {
		kind = QStringLiteral("sub");
		const QString tier = event.value(QStringLiteral("tier")).toString();
		if (tier == QLatin1String("2000"))
			detail = QStringLiteral("Tier 2");
		else if (tier == QLatin1String("3000"))
			detail = QStringLiteral("Tier 3");
		else if (!tier.isEmpty())
			detail = QStringLiteral("Tier 1");
		if (event.value(QStringLiteral("is_gift")).toBool())
			detail = Tr("AlertGifted");
		const int months = event.value(QStringLiteral("cumulative_months")).toInt(0);
		if (months > 0)
			detail = Tr("AlertMonths").arg(months);
	} else if (subscription == QLatin1String("channel.subscription.gift")) {
		kind = QStringLiteral("gift");
		detail = Tr("AlertGiftCount").arg(event.value(QStringLiteral("total")).toInt(0));
	} else if (subscription == QLatin1String("channel.cheer")) {
		kind = QStringLiteral("bits");
		detail = Tr("AlertBitsCount").arg(event.value(QStringLiteral("bits")).toInt(0));
	} else if (subscription == QLatin1String("channel.raid")) {
		kind = QStringLiteral("raid");
		detail = Tr("AlertRaidViewers").arg(event.value(QStringLiteral("viewers")).toInt(0));
	}
	Push(0, kind, user, detail);
}

void AlertsDock::Push(int platform, const QString &kind, const QString &user, const QString &detail)
{
	if (!list)
		return;
	QString text = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
	text += QStringLiteral("  ") + PlatformName(platform);
	text += QStringLiteral("  ") + KindLabel(kind);
	if (!user.isEmpty())
		text += QStringLiteral("  ") + user;
	if (!detail.isEmpty())
		text += QStringLiteral("  ") + detail;
	auto *item = new QListWidgetItem(PlatformIcon(platform), text);
	list->insertItem(0, item);
	while (list->count() > 100)
		delete list->takeItem(list->count() - 1);
}
