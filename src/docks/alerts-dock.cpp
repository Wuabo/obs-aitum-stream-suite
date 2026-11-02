#ifdef _WIN32
#include <winsock2.h>
#endif

#include "alerts-dock.hpp"
#include "chat-dock.hpp"
#include "stream-info-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <src/utils/icon.hpp>
#include <src/utils/oauth-refresh.hpp>
#include <src/utils/profile-secret.hpp>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <cstring>
#include <QVBoxLayout>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <thread>

extern obs_data_t *current_profile_config;
extern StreamInfoDock *stream_info_dock;
extern ChatDock *chat_dock;
void save_current_profile_config(bool save_docks);

AlertsDock *alerts_dock = nullptr;

namespace {

constexpr int kMaxEntries = 200;

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
	UNUSED_PARAMETER(timeoutMs);
	return false;
#endif
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
	if (kind == QLatin1String("redeem"))
		return Tr("AlertRedeem");
	return kind;
}

QColor PlatformAccent(int platform)
{
	if (platform == 1)
		return QColor(QStringLiteral("#FF5555"));
	if (platform == 2)
		return QColor(QStringLiteral("#53FC18"));
	return QColor(QStringLiteral("#9146FF"));
}

QString PlatformIconPath(int platform)
{
	if (platform == 1)
		return QStringLiteral(":/aitum/media/youtube.png");
	if (platform == 2)
		return QStringLiteral(":/aitum/media/kick.png");
	return QStringLiteral(":/aitum/media/twitch.png");
}

QString EventSubtitle(const QString &kind, const QString &detail)
{
	const QString extra = detail.trimmed();
	if (extra.isEmpty())
		return KindLabel(kind);
	if (kind == QLatin1String("sub") || kind == QLatin1String("member") || kind == QLatin1String("follow"))
		return KindLabel(kind) + QStringLiteral(" · ") + extra;
	return extra;
}

QString RelativeAge(qint64 atMs)
{
	if (atMs <= 0)
		return QString();
	const qint64 secs = qMax<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - atMs) / 1000);
	if (secs < 60)
		return QStringLiteral("%1s").arg(qMax<qint64>(1, secs));
	if (secs < 3600)
		return QStringLiteral("%1m").arg(secs / 60);
	if (secs < 86400)
		return QStringLiteral("%1h").arg(secs / 3600);
	return QStringLiteral("%1d").arg(secs / 86400);
}

QWidget *MakeActivityCard(int platform, const QString &kind, const QString &user, const QString &detail,
			  const QString &message, qint64 atMs, QLabel **ageOut)
{
	auto *card = new QFrame;
	card->setObjectName(QStringLiteral("activityCard"));
	card->setAttribute(Qt::WA_StyledBackground, true);
	card->setStyleSheet(QStringLiteral(
		"QFrame#activityCard { background: #1b2230; border-radius: 6px; border: none; }"
		"QFrame#activityCard QLabel { background: transparent; border: none; }"));

	auto *outer = new QVBoxLayout(card);
	outer->setContentsMargins(10, 8, 12, 8);
	outer->setSpacing(0);

	auto *row = new QHBoxLayout;
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(8);

	auto *icon = new QLabel;
	icon->setFixedSize(18, 18);
	icon->setScaledContents(true);
	icon->setPixmap(QIcon(PlatformIconPath(platform)).pixmap(18, 18));
	row->addWidget(icon, 0, Qt::AlignTop);

	auto *textCol = new QVBoxLayout;
	textCol->setContentsMargins(0, 0, 0, 0);
	textCol->setSpacing(1);
	auto *name = new QLabel(user.isEmpty() ? QStringLiteral("—") : user);
	name->setStyleSheet(QStringLiteral("color: %1; font-weight: 700; font-size: 13px;").arg(PlatformAccent(platform).name()));
	auto *subtitle = new QLabel(EventSubtitle(kind, detail));
	subtitle->setStyleSheet(QStringLiteral("color: #9a9a9a; font-size: 12px;"));
	subtitle->setWordWrap(true);
	textCol->addWidget(name);
	textCol->addWidget(subtitle);
	if (!message.trimmed().isEmpty()) {
		auto *msg = new QLabel(message.trimmed());
		msg->setWordWrap(true);
		msg->setStyleSheet(QStringLiteral("color: #e6e6e6; font-size: 13px;"));
		textCol->addWidget(msg);
	}
	row->addLayout(textCol, 1);

	auto *age = new QLabel(RelativeAge(atMs));
	age->setStyleSheet(QStringLiteral("color: #9a9a9a; font-size: 11px;"));
	age->setProperty("atMs", atMs);
	row->addWidget(age, 0, Qt::AlignTop);
	if (ageOut)
		*ageOut = age;

	outer->addLayout(row);

	return card;
}

} // namespace

AlertsDock::AlertsDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteActivity"));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	auto *bar = new QFrame(this);
	bar->setObjectName(QStringLiteral("activityFilterBar"));
	filterBar = new QHBoxLayout(bar);
	filterBar->setContentsMargins(8, 6, 8, 6);
	filterBar->setSpacing(6);
	filterGroup = new QButtonGroup(this);
	filterGroup->setExclusive(true);
	if (current_profile_config && obs_data_has_user_value(current_profile_config, "activity_filter")) {
		const int saved = (int)obs_data_get_int(current_profile_config, "activity_filter");
		if (saved >= -1 && saved <= 2)
			platformFilter = saved;
	}
	clearButton = new QPushButton(Tr("ActivityClear"), this);
	clearButton->setFlat(true);
	clearButton->setCursor(Qt::PointingHandCursor);
	clearButton->setMinimumWidth(0);
	clearButton->setStyleSheet(QStringLiteral(
		"QPushButton { color: #b0b0b0; background: transparent; border: 1px solid #3a3a3a;"
		" border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
		"QPushButton:hover { color: #eee; border-color: #666; background: #222; }"
		"QPushButton:pressed { background: #1a1a1a; }"));
	layout->addWidget(bar);
	connect(filterGroup, &QButtonGroup::idClicked, this, [this](int id) {
		platformFilter = id == 3 ? -1 : id;
		if (current_profile_config) {
			obs_data_set_int(current_profile_config, "activity_filter", platformFilter);
			save_current_profile_config(false);
		}
		RebuildVisible();
	});
	connect(clearButton, &QPushButton::clicked, this, &AlertsDock::ClearHistory);

	list = new QListWidget(this);
	list->setObjectName(QStringLiteral("activityList"));
	list->setSelectionMode(QAbstractItemView::NoSelection);
	list->setFocusPolicy(Qt::NoFocus);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list->setSpacing(6);
	list->setStyleSheet(QStringLiteral(
		"QListWidget#activityList { background: #141414; border: none; color: #eee; outline: none; }"
		"QListWidget#activityList::item { padding: 0px; margin: 0px; border: none; background: transparent; }"
		"QListWidget#activityList::item:selected { background: transparent; }"
		"QListWidget#activityList::item:hover { background: transparent; }"));
	layout->addWidget(list, 1);

	ageTimer = new QTimer(this);
	ageTimer->setInterval(30000);
	connect(ageTimer, &QTimer::timeout, this, &AlertsDock::RefreshAges);
	ageTimer->start();

	saveTimer = new QTimer(this);
	saveTimer->setSingleShot(true);
	saveTimer->setInterval(800);
	connect(saveTimer, &QTimer::timeout, this, &AlertsDock::SaveHistory);

	RefreshAvailablePlatforms();
	LoadHistory();
}

AlertsDock::~AlertsDock()
{
	StopEventSub();
	SaveHistory();
}

void AlertsDock::showEvent(QShowEvent *event)
{
	QFrame::showEvent(event);
	RefreshAvailablePlatforms();
}

void AlertsDock::RefreshAvailablePlatforms()
{
	QSet<int> next;
	auto add = [&](StreamPlatformKind kind) {
		if (kind == StreamPlatformKind::Twitch)
			next.insert(0);
		else if (kind == StreamPlatformKind::YouTube)
			next.insert(1);
		else if (kind == StreamPlatformKind::Kick)
			next.insert(2);
	};
	if (obs_service_t *service = obs_frontend_get_streaming_service()) {
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		add(DetectStreamPlatformFromEndpoint(QString::fromUtf8(url ? url : "")));
	}
	if (current_profile_config) {
		obs_data_array_t *outputs = obs_data_get_array(current_profile_config, "outputs");
		if (outputs) {
			const size_t count = obs_data_array_count(outputs);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(outputs, i);
				const char *type = obs_data_get_string(item, "type");
				if (type && *type && strcmp(type, "stream") != 0) {
					obs_data_release(item);
					continue;
				}
				QString server = QString::fromUtf8(obs_data_get_string(item, "stream_server"));
				if (server.isEmpty())
					server = QString::fromUtf8(obs_data_get_string(item, "server"));
				obs_data_release(item);
				add(DetectStreamPlatformFromEndpoint(server));
			}
			obs_data_array_release(outputs);
		}
	}
	if (next == availablePlatforms && filterBar && filterBar->count() > 0)
		return;
	availablePlatforms = next;
	RebuildFilters();
	RebuildVisible();
}

void AlertsDock::RebuildFilters()
{
	if (!filterBar || !filterGroup || !clearButton)
		return;
	const auto buttons = filterGroup->buttons();
	for (QAbstractButton *button : buttons)
		filterGroup->removeButton(button);
	while (QLayoutItem *item = filterBar->takeAt(0)) {
		if (QWidget *widget = item->widget()) {
			if (widget != clearButton)
				widget->deleteLater();
		}
		delete item;
	}

	const QString textStyle = QStringLiteral(
		"QPushButton { color: #b0b0b0; background: transparent; border: 1px solid #3a3a3a;"
		" border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
		"QPushButton:hover { color: #eee; border-color: #666; }"
		"QPushButton:checked { color: white; background: #2a2a2a; border-color: #888; }");
	const QString iconStyle = QStringLiteral(
		"QPushButton { color: #b0b0b0; background: transparent; border: 1px solid #3a3a3a;"
		" border-radius: 4px; padding: 4px; }"
		"QPushButton:hover { color: #eee; border-color: #666; }"
		"QPushButton:checked { color: white; background: #2a2a2a; border-color: #888; }");
	const bool several = availablePlatforms.size() >= 2;
	const bool showAll = !(platformFilter >= 0 && availablePlatforms.contains(platformFilter));
	const auto add = [&](int id, const QString &text, const QString &iconPath) {
		auto *button = new QPushButton(iconPath.isEmpty() ? text : QString(), this);
		button->setCheckable(true);
		button->setFlat(true);
		button->setCursor(Qt::PointingHandCursor);
		button->setMinimumWidth(0);
		button->setToolTip(text);
		button->setStyleSheet(iconPath.isEmpty() ? textStyle : iconStyle);
		if (!iconPath.isEmpty()) {
			button->setIcon(QIcon(iconPath));
			button->setIconSize(QSize(16, 16));
			button->setFixedSize(28, 28);
		}
		filterGroup->addButton(button, id);
		filterBar->addWidget(button);
		if (!several)
			button->setChecked(id != 3);
		else if ((id == 3 && showAll) || id == platformFilter)
			button->setChecked(true);
	};
	if (several)
		add(3, Tr("ActivityFilterAll"), QString());
	if (availablePlatforms.contains(0))
		add(0, QStringLiteral("Twitch"), PlatformIconPath(0));
	if (availablePlatforms.contains(1))
		add(1, QStringLiteral("YouTube"), PlatformIconPath(1));
	if (availablePlatforms.contains(2))
		add(2, QStringLiteral("Kick"), PlatformIconPath(2));
	filterBar->addStretch(1);
	filterBar->addWidget(clearButton);
}

bool AlertsDock::EntryVisible(const Entry &entry) const
{
	if (!availablePlatforms.contains(entry.platform))
		return false;
	if (availablePlatforms.size() < 2)
		return true;
	if (platformFilter >= 0 && availablePlatforms.contains(platformFilter) && entry.platform != platformFilter)
		return false;
	return true;
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
		if (UpgradeProfileSecret(one, "access_token"))
			save_current_profile_config(false);
		accessToken = RevealProfileSecret(obs_data_get_string(one, "access_token"));
		userId = QString::fromUtf8(obs_data_get_string(one, "user_id"));
	}
	obs_data_release(one);
	obs_data_release(root);
}

void AlertsDock::ReloadHistory()
{
	LoadHistory();
}

void AlertsDock::ReloadAccounts()
{
	const QString previousToken = accessToken;
	const QString previousUser = userId;
	LoadTwitchAccount();
	askedReconnect = false;
	tokenRefreshTried = false;
	QPointer<AlertsDock> alive(this);
	RefreshProfileOauth(QStringLiteral("twitch"), false, [alive, previousToken, previousUser](OauthRefreshResult result) {
		if (!alive)
			return;
		if (result == OauthRefreshResult::Refreshed || result == OauthRefreshResult::Reauth)
			alive->LoadTwitchAccount();
		if (result == OauthRefreshResult::Reauth) {
			alive->StopEventSub();
			if (stream_info_dock && !alive->askedReconnect && !alive->clientId.isEmpty()) {
				alive->askedReconnect = true;
				stream_info_dock->RequestModerationAuth(0);
			}
			return;
		}
		if (alive->accessToken.isEmpty() || alive->clientId.isEmpty()) {
			alive->StopEventSub();
			return;
		}
		if (alive->accessToken == previousToken && alive->userId == previousUser && alive->session)
			return;
		if (!alive->userId.isEmpty()) {
			alive->StartEventSub(QStringLiteral("wss://eventsub.wss.twitch.tv/ws"), true);
			return;
		}
		const QByteArray token = alive->accessToken.toUtf8();
		const QByteArray cid = alive->clientId.toUtf8();
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
	});
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
			bool moderator;
		};
		const Item items[] = {
			{"channel.follow", "2", false, true},
			{"channel.subscribe", "1", false, false},
			{"channel.subscription.message", "1", false, false},
			{"channel.subscription.gift", "1", false, false},
			{"channel.cheer", "1", false, false},
			{"channel.raid", "1", true, false},
			{"channel.channel_points_custom_reward_redemption.add", "1", false, false},
			{"channel.ban", "1", false, true},
			{"channel.unban", "1", false, true},
		};
		bool unauthorized = false;
		for (const Item &item : items) {
			QJsonObject condition;
			if (item.raid)
				condition.insert(QStringLiteral("to_broadcaster_user_id"), QString::fromUtf8(user));
			else
				condition.insert(QStringLiteral("broadcaster_user_id"), QString::fromUtf8(user));
			if (item.moderator)
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
			if (status == 401)
				unauthorized = true;
			else if (status == 403 || (status != 409 && (status < 200 || status >= 300)))
				blog(LOG_WARNING, "[Stream Suite] EventSub %s HTTP %d: %s", item.type, status,
				     response.left(180).constData());
		}
		if (!unauthorized || !alive)
			return;
		const QByteArray sessionCopy = session;
		QMetaObject::invokeMethod(
			alive,
			[alive, sessionCopy] {
				if (!alive || alive->tokenRefreshTried)
					return;
				alive->tokenRefreshTried = true;
				RefreshProfileOauth(QStringLiteral("twitch"), true, [alive, sessionCopy](OauthRefreshResult result) {
					if (!alive)
						return;
					if (result == OauthRefreshResult::Refreshed) {
						alive->LoadTwitchAccount();
						alive->SubscribeEventSub(QString::fromUtf8(sessionCopy));
						return;
					}
					if (result != OauthRefreshResult::Reauth || alive->askedReconnect || !stream_info_dock)
						return;
					alive->askedReconnect = true;
					stream_info_dock->RequestModerationAuth(0);
				});
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
	QString message;
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
		message = event.value(QStringLiteral("message")).toObject().value(QStringLiteral("text")).toString();
		if (message.isEmpty())
			message = event.value(QStringLiteral("message")).toString();
	} else if (subscription == QLatin1String("channel.subscription.gift")) {
		kind = QStringLiteral("gift");
		detail = Tr("AlertGiftCount").arg(event.value(QStringLiteral("total")).toInt(0));
	} else if (subscription == QLatin1String("channel.cheer")) {
		kind = QStringLiteral("bits");
		detail = Tr("AlertBitsCount").arg(event.value(QStringLiteral("bits")).toInt(0));
		message = event.value(QStringLiteral("message")).toString();
	} else if (subscription == QLatin1String("channel.raid")) {
		kind = QStringLiteral("raid");
		detail = Tr("AlertRaidViewers").arg(event.value(QStringLiteral("viewers")).toInt(0));
	} else if (subscription == QLatin1String("channel.channel_points_custom_reward_redemption.add")) {
		kind = QStringLiteral("redeem");
		detail = event.value(QStringLiteral("reward")).toObject().value(QStringLiteral("title")).toString();
		message = event.value(QStringLiteral("user_input")).toString();
	} else if (subscription == QLatin1String("channel.ban")) {
		if (chat_dock) {
			const QString id = event.value(QStringLiteral("user_id")).toString();
			const QString login = event.value(QStringLiteral("user_login")).toString();
			const QString name = event.value(QStringLiteral("user_name")).toString(user);
			const QString ends = event.value(QStringLiteral("ends_at")).toString();
			int timeoutSecs = 0;
			if (!ends.isEmpty()) {
				const QDateTime endAt = QDateTime::fromString(ends, Qt::ISODate);
				if (endAt.isValid())
					timeoutSecs = qMax(1, int(QDateTime::currentDateTimeUtc().secsTo(endAt.toUTC())));
			}
			chat_dock->ApplyExternalModeration(ChatDock::Twitch, id, login, name, timeoutSecs);
		}
		return;
	} else if (subscription == QLatin1String("channel.unban")) {
		if (chat_dock) {
			chat_dock->ApplyExternalUnban(ChatDock::Twitch, event.value(QStringLiteral("user_id")).toString(),
						      event.value(QStringLiteral("user_login")).toString(),
						      event.value(QStringLiteral("user_name")).toString(user));
		}
		return;
	}
	Push(0, kind, user, detail, message);
}

void AlertsDock::InsertCard(const Entry &entry, bool atTop)
{
	if (!list)
		return;
	if (!EntryVisible(entry))
		return;
	QLabel *age = nullptr;
	QWidget *card = MakeActivityCard(entry.platform, entry.kind, entry.user, entry.detail, entry.message, entry.atMs, &age);
	auto *item = new QListWidgetItem;
	item->setFlags(Qt::NoItemFlags);
	item->setSizeHint(card->sizeHint());
	item->setData(Qt::UserRole, entry.atMs);
	if (atTop)
		list->insertItem(0, item);
	else
		list->addItem(item);
	list->setItemWidget(item, card);
	/* SizeHint before show can be short — force after layout. */
	card->adjustSize();
	item->setSizeHint(card->sizeHint() + QSize(0, 4));
}

void AlertsDock::Push(int platform, const QString &kind, const QString &user, const QString &detail, const QString &message,
		      qint64 atMs)
{
	if (!list)
		return;
	Entry entry;
	entry.platform = platform;
	entry.kind = kind;
	entry.user = user;
	entry.detail = detail;
	entry.message = message;
	entry.atMs = atMs > 0 ? atMs : QDateTime::currentMSecsSinceEpoch();

	if (!loadingHistory) {
		entries.insert(entries.begin(), entry);
		while (static_cast<int>(entries.size()) > kMaxEntries)
			entries.pop_back();
		InsertCard(entry, true);
		while (list->count() > kMaxEntries) {
			QListWidgetItem *old = list->takeItem(list->count() - 1);
			delete old;
		}
		ScheduleSave();
	} else {
		entries.push_back(entry);
		InsertCard(entry, false);
	}
}

void AlertsDock::RebuildVisible()
{
	if (!list)
		return;
	list->clear();
	for (const Entry &entry : entries)
		InsertCard(entry, false);
	RefreshAges();
}

void AlertsDock::ClearHistory()
{
	entries.clear();
	if (list)
		list->clear();
	SaveHistory();
}

void AlertsDock::RefreshAges()
{
	if (!list)
		return;
	for (int i = 0; i < list->count(); ++i) {
		QListWidgetItem *item = list->item(i);
		QWidget *w = list->itemWidget(item);
		if (!w)
			continue;
		const auto ages = w->findChildren<QLabel *>();
		for (QLabel *lab : ages) {
			if (!lab->property("atMs").isValid())
				continue;
			const qint64 atMs = lab->property("atMs").toLongLong();
			lab->setText(RelativeAge(atMs));
		}
	}
}

void AlertsDock::ScheduleSave()
{
	if (loadingHistory || !saveTimer)
		return;
	saveTimer->start();
}

void AlertsDock::SaveHistory()
{
	if (!current_profile_config || loadingHistory)
		return;
	obs_data_array_t *arr = obs_data_array_create();
	for (const Entry &entry : entries) {
		obs_data_t *one = obs_data_create();
		obs_data_set_int(one, "platform", entry.platform);
		obs_data_set_string(one, "kind", entry.kind.toUtf8().constData());
		obs_data_set_string(one, "user", entry.user.toUtf8().constData());
		obs_data_set_string(one, "detail", entry.detail.toUtf8().constData());
		obs_data_set_string(one, "message", entry.message.toUtf8().constData());
		obs_data_set_int(one, "at", entry.atMs);
		obs_data_array_push_back(arr, one);
		obs_data_release(one);
	}
	obs_data_set_array(current_profile_config, "activity_feed", arr);
	obs_data_array_release(arr);
	save_current_profile_config(false);
}

void AlertsDock::LoadHistory()
{
	if (!list)
		return;
	loadingHistory = true;
	list->clear();
	entries.clear();
	if (current_profile_config) {
		obs_data_array_t *arr = obs_data_get_array(current_profile_config, "activity_feed");
		const size_t count = arr ? obs_data_array_count(arr) : 0;
		for (size_t i = 0; i < count && static_cast<int>(entries.size()) < kMaxEntries; ++i) {
			obs_data_t *one = obs_data_array_item(arr, i);
			if (!one)
				continue;
			Entry entry;
			entry.platform = int(obs_data_get_int(one, "platform"));
			entry.kind = QString::fromUtf8(obs_data_get_string(one, "kind"));
			entry.user = QString::fromUtf8(obs_data_get_string(one, "user"));
			entry.detail = QString::fromUtf8(obs_data_get_string(one, "detail"));
			entry.message = QString::fromUtf8(obs_data_get_string(one, "message"));
			entry.atMs = obs_data_get_int(one, "at");
			obs_data_release(one);
			if (entry.kind.isEmpty())
				continue;
			Push(entry.platform, entry.kind, entry.user, entry.detail, entry.message, entry.atMs);
		}
		obs_data_array_release(arr);
	}
	loadingHistory = false;
	RefreshAges();
}
