#ifdef _WIN32
#include <winsock2.h>
#endif

#include "alerts-dock.hpp"
#include "chat-dock.hpp"
#include "stream-info-dock.hpp"

#include <obs-module.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <cmath>
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

QColor EventColor(const QString &kind)
{
	if (kind == QLatin1String("follow"))
		return QColor(QStringLiteral("#a78bfa"));
	if (kind == QLatin1String("sub") || kind == QLatin1String("member"))
		return QColor(QStringLiteral("#fbbf24"));
	if (kind == QLatin1String("gift"))
		return QColor(QStringLiteral("#f472b6"));
	if (kind == QLatin1String("bits"))
		return QColor(QStringLiteral("#c084fc"));
	if (kind == QLatin1String("raid"))
		return QColor(QStringLiteral("#4ade80"));
	if (kind == QLatin1String("superchat"))
		return QColor(QStringLiteral("#f87171"));
	if (kind == QLatin1String("redeem"))
		return QColor(QStringLiteral("#38bdf8"));
	return QColor(QStringLiteral("#d4d4d4"));
}

QString PlatformIconPath(int platform)
{
	if (platform == 1)
		return QStringLiteral(":/aitum/media/youtube.png");
	if (platform == 2)
		return QStringLiteral(":/aitum/media/kick.png");
	return QStringLiteral(":/aitum/media/twitch.png");
}

void PaintEventGlyph(QPainter &p, const QString &kind, const QRectF &box)
{
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::white);
	const QPointF c = box.center();
	if (kind == QLatin1String("sub") || kind == QLatin1String("member")) {
		QPainterPath star;
		const qreal outer = box.width() * 0.48;
		const qreal inner = outer * 0.42;
		for (int i = 0; i < 10; ++i) {
			const qreal ang = -1.5708 + i * 0.6283;
			const qreal rad = (i % 2 == 0) ? outer : inner;
			const QPointF pt(c.x() + std::cos(ang) * rad, c.y() + std::sin(ang) * rad);
			if (i == 0)
				star.moveTo(pt);
			else
				star.lineTo(pt);
		}
		star.closeSubpath();
		p.drawPath(star);
		return;
	}
	if (kind == QLatin1String("gift")) {
		const QRectF body(box.left() + 2, box.top() + box.height() * 0.38, box.width() - 4, box.height() * 0.58);
		p.drawRoundedRect(body, 3, 3);
		p.setBrush(QColor(QStringLiteral("#111111")));
		p.drawRect(QRectF(c.x() - 2, body.top(), 4, body.height()));
		p.drawRect(QRectF(body.left(), body.top() + 4, body.width(), 4));
		p.setBrush(Qt::white);
		p.drawRoundedRect(QRectF(box.left() + 4, box.top() + 2, box.width() * 0.34, box.height() * 0.28), 3, 3);
		p.drawRoundedRect(QRectF(c.x() + 1, box.top() + 2, box.width() * 0.34, box.height() * 0.28), 3, 3);
		return;
	}
	if (kind == QLatin1String("bits")) {
		QPolygonF diamond;
		diamond << QPointF(c.x(), box.top()) << QPointF(box.right(), c.y()) << QPointF(c.x(), box.bottom())
			<< QPointF(box.left(), c.y());
		p.drawPolygon(diamond);
		return;
	}
	if (kind == QLatin1String("raid")) {
		QPainterPath arrow;
		arrow.moveTo(box.left(), box.top() + 4);
		arrow.lineTo(box.right() - 8, c.y());
		arrow.lineTo(box.left(), box.bottom() - 4);
		arrow.lineTo(box.left() + 8, c.y());
		arrow.closeSubpath();
		p.drawPath(arrow);
		return;
	}
	if (kind == QLatin1String("superchat")) {
		p.drawRoundedRect(QRectF(box.left(), box.top(), box.width(), box.height() * 0.72), 6, 6);
		QPainterPath tail;
		tail.moveTo(box.left() + 8, box.top() + box.height() * 0.68);
		tail.lineTo(box.left() + 6, box.bottom());
		tail.lineTo(box.left() + 18, box.top() + box.height() * 0.68);
		p.drawPath(tail);
		return;
	}
	if (kind == QLatin1String("redeem")) {
		p.drawEllipse(box.adjusted(2, 2, -2, -2));
		p.setBrush(QColor(QStringLiteral("#111111")));
		p.drawEllipse(QRectF(c.x() - 4, c.y() - 4, 8, 8));
		return;
	}
	QPainterPath heart;
	heart.moveTo(c.x(), box.bottom() - 1);
	heart.cubicTo(box.left() - 2, c.y(), box.left() + 2, box.top() - 1, c.x(), box.top() + box.height() * 0.32);
	heart.cubicTo(box.right() - 2, box.top() - 1, box.right() + 2, c.y(), c.x(), box.bottom() - 1);
	p.drawPath(heart);
}

QPixmap EventBadge(int platform, const QString &kind)
{
	constexpr int size = 72;
	QPixmap pm(size, size);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QColor color = EventColor(kind);
	QColor fill = color;
	fill.setAlpha(46);
	p.setPen(QPen(color, 3));
	p.setBrush(fill);
	p.drawRoundedRect(QRectF(2, 2, size - 4, size - 4), 18, 18);
	PaintEventGlyph(p, kind, QRectF(18, 14, 36, 32));

	const QPixmap platformIcon = QIcon(PlatformIconPath(platform)).pixmap(26, 26);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(QStringLiteral("#111111")));
	p.drawEllipse(QRectF(size - 32, size - 32, 30, 30));
	if (!platformIcon.isNull())
		p.drawPixmap(size - 30, size - 30, platformIcon);
	return pm;
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
	const QColor accent = EventColor(kind);
	auto *card = new QFrame;
	card->setObjectName(QStringLiteral("activityCard"));
	card->setStyleSheet(QStringLiteral("QFrame#activityCard {"
					   "  background: #141414;"
					   "  border: 1px solid #2a2a2a;"
					   "  border-left: 3px solid %1;"
					   "  border-radius: 10px;"
					   "}"
					   "QFrame#activityCard QLabel { background: transparent; border: none; }")
				    .arg(accent.name()));

	auto *outer = new QVBoxLayout(card);
	outer->setContentsMargins(10, 8, 12, 8);
	outer->setSpacing(4);

	auto *row = new QHBoxLayout;
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(10);

	auto *icon = new QLabel;
	icon->setFixedSize(36, 36);
	icon->setPixmap(EventBadge(platform, kind));
	icon->setScaledContents(true);
	row->addWidget(icon, 0, Qt::AlignTop);

	auto *textCol = new QVBoxLayout;
	textCol->setContentsMargins(0, 0, 0, 0);
	textCol->setSpacing(1);
	auto *name = new QLabel(user.isEmpty() ? QStringLiteral("—") : user);
	name->setStyleSheet(QStringLiteral("color: #f4f4f5; font-weight: 700; font-size: 13px;"));
	auto *subtitle = new QLabel(EventSubtitle(kind, detail));
	subtitle->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: 600;").arg(accent.name()));
	subtitle->setWordWrap(true);
	textCol->addWidget(name);
	textCol->addWidget(subtitle);
	row->addLayout(textCol, 1);

	auto *age = new QLabel(RelativeAge(atMs));
	age->setStyleSheet(QStringLiteral("color: #737373; font-size: 11px; font-weight: 600;"));
	age->setProperty("atMs", atMs);
	row->addWidget(age, 0, Qt::AlignTop);
	if (ageOut)
		*ageOut = age;

	outer->addLayout(row);

	if (!message.trimmed().isEmpty()) {
		auto *msg = new QLabel(message.trimmed());
		msg->setWordWrap(true);
		msg->setStyleSheet(QStringLiteral("color: #d4d4d4; font-size: 12px; padding-left: 46px;"));
		outer->addWidget(msg);
	}

	return card;
}

} // namespace

AlertsDock::AlertsDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteActivity"));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(0);

	list = new QListWidget(this);
	list->setObjectName(QStringLiteral("activityList"));
	list->setSelectionMode(QAbstractItemView::NoSelection);
	list->setFocusPolicy(Qt::NoFocus);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	list->setSpacing(8);
	list->setStyleSheet(QStringLiteral(
		"QListWidget#activityList { background: #0b0b0b; border: none; outline: none; }"
		"QListWidget#activityList::item { background: transparent; border: none; margin: 0; padding: 0; }"));
	layout->addWidget(list);

	ageTimer = new QTimer(this);
	ageTimer->setInterval(30000);
	connect(ageTimer, &QTimer::timeout, this, &AlertsDock::RefreshAges);
	ageTimer->start();

	saveTimer = new QTimer(this);
	saveTimer->setSingleShot(true);
	saveTimer->setInterval(800);
	connect(saveTimer, &QTimer::timeout, this, &AlertsDock::SaveHistory);

	LoadHistory();
}

AlertsDock::~AlertsDock()
{
	StopEventSub();
	SaveHistory();
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

void AlertsDock::ReloadHistory()
{
	LoadHistory();
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
		bool authFailed = false;
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
