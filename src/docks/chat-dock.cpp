#include "chat-dock.hpp"
#include "stream-info-dock.hpp"
#include <src/utils/icon.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QColor>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSslSocket>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <curl/curl.h>
#include <thread>
#include <QtEndian>

extern obs_data_t *current_profile_config;
extern StreamInfoDock *stream_info_dock;

namespace {

struct HttpResult {
	int status = 0;
	QByteArray body;
	QString error;
};

size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *out = static_cast<QByteArray *>(userdata);
	out->append(ptr, static_cast<qsizetype>(size * nmemb));
	return size * nmemb;
}

HttpResult CurlPerform(CURL *curl)
{
	HttpResult result;
	char error[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aitum-stream-suite");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
#ifdef CURLSSLOPT_REVOKE_BEST_EFFORT
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
#elif defined(CURLSSLOPT_NO_REVOKE)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
#endif
#endif
	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	result.status = static_cast<int>(status);
	if (code != CURLE_OK)
		result.error = QString::fromUtf8(error[0] ? error : curl_easy_strerror(code));
	return result;
}

void RunAsync(QObject *context, std::function<HttpResult()> work,
	      std::function<void(int, const QByteArray &, const QString &)> done)
{
	QPointer<QObject> alive(context);
	std::thread([alive, work = std::move(work), done = std::move(done)] {
		const HttpResult result = work();
		if (!alive)
			return;
		QMetaObject::invokeMethod(
			alive.data(),
			[alive, done, result] {
				if (alive)
					done(result.status, result.body, result.error);
			},
			Qt::QueuedConnection);
	}).detach();
}

QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString EscapeHtml(const QString &text)
{
	QString out = text;
	out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	return out;
}

QString Linkify(const QString &escaped)
{
	QString out;
	out.reserve(escaped.size() + 32);
	int i = 0;
	while (i < escaped.size()) {
		const int http = escaped.indexOf(QStringLiteral("http"), i);
		if (http < 0) {
			out += escaped.mid(i);
			break;
		}
		out += escaped.mid(i, http - i);
		int end = http;
		while (end < escaped.size() && !escaped.at(end).isSpace() && escaped.at(end) != QLatin1Char('<'))
			++end;
		const QString url = escaped.mid(http, end - http);
		out += QStringLiteral("<a href=\"%1\" style=\"color:#6cb6ff;\">%1</a>").arg(url);
		i = end;
	}
	return out;
}

QJsonObject FirstDataObject(const QByteArray &body)
{
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (doc.isArray() && !doc.array().isEmpty())
		return doc.array().at(0).toObject();
	const QJsonObject root = doc.object();
	const QJsonValue data = root.value("data");
	if (data.isArray() && !data.toArray().isEmpty())
		return data.toArray().at(0).toObject();
	if (data.isObject())
		return data.toObject();
	return root;
}

QString JsonId(const QJsonValue &value)
{
	if (value.isDouble())
		return QString::number(value.toInteger());
	return value.toString();
}

} // namespace

const char *ChatDock::PlatformKey(Platform platform)
{
	switch (platform) {
	case YouTube:
		return "youtube";
	case Kick:
		return "kick";
	case Twitch:
	default:
		return "twitch";
	}
}

QString ChatDock::PlatformLabel(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QStringLiteral("YouTube");
	case Kick:
		return QStringLiteral("Kick");
	case Twitch:
	default:
		return QStringLiteral("Twitch");
	}
}

QIcon ChatDock::PlatformIcon(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QIcon(QStringLiteral(":/aitum/media/youtube.png"));
	case Kick:
		return QIcon(QStringLiteral(":/aitum/media/kick.png"));
	case Twitch:
	default:
		return QIcon(QStringLiteral(":/aitum/media/twitch.png"));
	}
}

QColor ChatDock::PlatformAccent(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QColor(QStringLiteral("#FF0000"));
	case Kick:
		return QColor(QStringLiteral("#53FC18"));
	case Twitch:
	default:
		return QColor(QStringLiteral("#9146FF"));
	}
}

ChatDock::ChatDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteChat"));
	curl_global_init(CURL_GLOBAL_DEFAULT);

	viewerTimer = new QTimer(this);
	viewerTimer->setInterval(15000);
	connect(viewerTimer, &QTimer::timeout, this, &ChatDock::RefreshViewers);

	youtubeTimer = new QTimer(this);
	youtubeTimer->setSingleShot(true);
	connect(youtubeTimer, &QTimer::timeout, this, &ChatDock::PollYouTubeChat);

	kickReconnectTimer = new QTimer(this);
	kickReconnectTimer->setSingleShot(true);
	connect(kickReconnectTimer, &QTimer::timeout, this, &ChatDock::ConnectKickChat);

	BuildUi();
	RefreshAvailablePlatforms();
}

ChatDock::~ChatDock()
{
	StopConnections();
}

void ChatDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	viewerBar = new QFrame(this);
	viewerBar->setObjectName(QStringLiteral("chatViewerBar"));
	auto *barLayout = new QHBoxLayout(viewerBar);
	barLayout->setContentsMargins(8, 6, 8, 6);
	barLayout->setSpacing(6);

	for (int i = 0; i < 3; ++i) {
		viewerChips[i] = new QFrame(viewerBar);
		viewerChips[i]->setObjectName(QStringLiteral("chatViewerChip"));
		auto *chipLayout = new QHBoxLayout(viewerChips[i]);
		chipLayout->setContentsMargins(8, 4, 10, 4);
		chipLayout->setSpacing(6);
		auto *icon = new QLabel(viewerChips[i]);
		icon->setFixedSize(16, 16);
		icon->setScaledContents(true);
		icon->setPixmap(PlatformIcon(Platform(i)).pixmap(16, 16));
		viewerLabels[i] = new QLabel(QStringLiteral("—"), viewerChips[i]);
		viewerLabels[i]->setStyleSheet(QStringLiteral("color: white; font-weight: 600;"));
		chipLayout->addWidget(icon);
		chipLayout->addWidget(viewerLabels[i]);
		const QColor accent = PlatformAccent(Platform(i));
		viewerChips[i]->setStyleSheet(
			QStringLiteral("QFrame#chatViewerChip { background: %1; border-radius: 4px; }")
				.arg(accent.name()));
		viewerChips[i]->setVisible(false);
		barLayout->addWidget(viewerChips[i]);
	}
	barLayout->addStretch(1);

	settingsButton = new QPushButton(viewerBar);
	settingsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
	settingsButton->setFlat(true);
	settingsButton->setFixedSize(28, 28);
	settingsButton->setToolTip(Tr("ChatDockSettings"));
	connect(settingsButton, &QPushButton::clicked, this, [this] {
		if (stream_info_dock)
			stream_info_dock->OpenSettings();
	});
	accountsButton = new QPushButton(viewerBar);
	accountsButton->setIcon(style()->standardIcon(QStyle::SP_DialogYesButton));
	accountsButton->setFlat(true);
	accountsButton->setFixedSize(28, 28);
	accountsButton->setToolTip(Tr("ChatDockAccounts"));
	connect(accountsButton, &QPushButton::clicked, this, &ChatDock::ReloadAccounts);
	barLayout->addWidget(settingsButton);
	barLayout->addWidget(accountsButton);
	root->addWidget(viewerBar);

	messageList = new QListWidget(this);
	messageList->setObjectName(QStringLiteral("chatMessageList"));
	messageList->setSelectionMode(QAbstractItemView::NoSelection);
	messageList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	messageList->setSpacing(2);
	messageList->setStyleSheet(QStringLiteral(
		"QListWidget#chatMessageList { background: #141414; border: none; color: #eee; }"
		"QListWidget#chatMessageList::item { padding: 2px 6px; }"));
	root->addWidget(messageList, 1);

	auto *inputRow = new QFrame(this);
	auto *inputLayout = new QHBoxLayout(inputRow);
	inputLayout->setContentsMargins(8, 6, 8, 2);
	inputLayout->setSpacing(6);
	sendPlatformIcon = new QLabel(inputRow);
	sendPlatformIcon->setFixedSize(22, 22);
	sendPlatformIcon->setScaledContents(true);
	sendPlatformIcon->setCursor(Qt::PointingHandCursor);
	sendPlatformIcon->installEventFilter(this);
	connect(sendPlatformIcon, &QLabel::linkActivated, this, [this](const QString &) { CycleSendPlatform(); });
	messageEdit = new QLineEdit(inputRow);
	messageEdit->setPlaceholderText(Tr("ChatDockMessagePlaceholder"));
	connect(messageEdit, &QLineEdit::returnPressed, this, &ChatDock::SendChatMessage);
	inputLayout->addWidget(sendPlatformIcon);
	inputLayout->addWidget(messageEdit, 1);
	root->addWidget(inputRow);

	sendStatus = new QLabel(this);
	sendStatus->setStyleSheet(QStringLiteral("color: #9a9a9a; padding: 0 10px 8px 10px;"));
	root->addWidget(sendStatus);
	UpdateSendStatus();
}

bool ChatDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == sendPlatformIcon && event->type() == QEvent::MouseButtonRelease) {
		CycleSendPlatform();
		return true;
	}
	return QFrame::eventFilter(watched, event);
}

void ChatDock::showEvent(QShowEvent *event)
{
	QFrame::showEvent(event);
	RefreshAvailablePlatforms();
	StartConnections();
}

void ChatDock::ReloadAccounts()
{
	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));
	StartConnections();
	RefreshViewers();
}

void ChatDock::RefreshAvailablePlatforms()
{
	availablePlatforms.clear();
	if (obs_service_t *service = obs_frontend_get_streaming_service()) {
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(QString::fromUtf8(url ? url : ""));
		if (kind == StreamPlatformKind::Twitch)
			availablePlatforms.insert(int(Twitch));
		else if (kind == StreamPlatformKind::YouTube)
			availablePlatforms.insert(int(YouTube));
		else if (kind == StreamPlatformKind::Kick)
			availablePlatforms.insert(int(Kick));
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
				const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(server);
				if (kind == StreamPlatformKind::Twitch)
					availablePlatforms.insert(int(Twitch));
				else if (kind == StreamPlatformKind::YouTube)
					availablePlatforms.insert(int(YouTube));
				else if (kind == StreamPlatformKind::Kick)
					availablePlatforms.insert(int(Kick));
			}
			obs_data_array_release(outputs);
		}
	}

	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));

	if (!availablePlatforms.contains(sendPlatform) && !availablePlatforms.isEmpty())
		sendPlatform = *availablePlatforms.constBegin();
	RebuildViewerBar();
	UpdateSendStatus();
}

void ChatDock::RebuildViewerBar()
{
	for (int i = 0; i < 3; ++i) {
		const bool show = availablePlatforms.contains(i);
		viewerChips[i]->setVisible(show);
		if (!show)
			viewerLabels[i]->setText(QStringLiteral("—"));
	}
}

void ChatDock::CycleSendPlatform()
{
	if (availablePlatforms.isEmpty())
		return;
	QList<int> order = {int(Twitch), int(YouTube), int(Kick)};
	int idx = order.indexOf(sendPlatform);
	for (int step = 1; step <= order.size(); ++step) {
		const int next = order[(idx + step) % order.size()];
		if (availablePlatforms.contains(next) && !accounts[next].accessToken.isEmpty()) {
			sendPlatform = next;
			break;
		}
		if (availablePlatforms.contains(next)) {
			sendPlatform = next;
			break;
		}
	}
	UpdateSendStatus();
}

void ChatDock::UpdateSendStatus()
{
	sendPlatformIcon->setPixmap(PlatformIcon(Platform(sendPlatform)).pixmap(22, 22));
	sendStatus->setText(Tr("ChatDockSendingTo").arg(PlatformLabel(Platform(sendPlatform))));
}

void ChatDock::LoadAccount(Platform platform)
{
	Account &acc = accounts[platform];
	acc = {};
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(platform)) : nullptr;
	if (one) {
		acc.clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
		acc.clientSecret = QString::fromUtf8(obs_data_get_string(one, "client_secret"));
		acc.accessToken = QString::fromUtf8(obs_data_get_string(one, "access_token"));
		acc.refreshToken = QString::fromUtf8(obs_data_get_string(one, "refresh_token"));
		acc.expiresAt = obs_data_get_int(one, "expires_at");
		acc.userId = QString::fromUtf8(obs_data_get_string(one, "user_id"));
		acc.userName = QString::fromUtf8(obs_data_get_string(one, "user_name"));
		acc.login = acc.userName.toLower();
	}
	obs_data_release(one);
	obs_data_release(root);
}

void ChatDock::EnsureToken(Platform platform, std::function<void(bool)> done)
{
	Account &acc = accounts[platform];
	if (acc.accessToken.isEmpty()) {
		done(false);
		return;
	}
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	if (acc.expiresAt <= 0 || acc.expiresAt > now + 60) {
		done(true);
		return;
	}
	if (acc.refreshToken.isEmpty() || acc.clientId.isEmpty()) {
		done(false);
		return;
	}
	QUrl url;
	QUrlQuery query;
	query.addQueryItem("grant_type", "refresh_token");
	query.addQueryItem("refresh_token", acc.refreshToken);
	query.addQueryItem("client_id", acc.clientId);
	if (platform == Twitch) {
		url = QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"));
	} else if (platform == YouTube) {
		url = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
		query.addQueryItem("client_secret", acc.clientSecret);
	} else {
		url = QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
		query.addQueryItem("client_secret", acc.clientSecret);
	}
	Http(url.toString(), {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/x-www-form-urlencoded")}},
	     query.query(QUrl::FullyEncoded).toUtf8(), "POST",
	     [this, platform, done](int status, const QByteArray &body, const QString &) {
		     Account &acc = accounts[platform];
		     const QJsonObject json = QJsonDocument::fromJson(body).object();
		     if (status < 200 || status >= 300 || !json.contains("access_token")) {
			     done(false);
			     return;
		     }
		     acc.accessToken = json.value("access_token").toString();
		     if (json.contains("refresh_token"))
			     acc.refreshToken = json.value("refresh_token").toString();
		     const int expiresIn = json.value("expires_in").toInt(0);
		     acc.expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
		     if (current_profile_config) {
			     obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
			     obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(platform)) : nullptr;
			     if (one) {
				     obs_data_set_string(one, "access_token", acc.accessToken.toUtf8().constData());
				     obs_data_set_string(one, "refresh_token", acc.refreshToken.toUtf8().constData());
				     obs_data_set_int(one, "expires_at", acc.expiresAt);
				     obs_data_set_obj(root, PlatformKey(platform), one);
				     obs_data_set_obj(current_profile_config, "stream_accounts", root);
			     }
			     obs_data_release(one);
			     obs_data_release(root);
		     }
		     done(true);
	     });
}

void ChatDock::Http(const QString &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body,
		    const char *verb, std::function<void(int, const QByteArray &, const QString &)> done)
{
	const QByteArray urlBytes = url.toUtf8();
	const QByteArray verbBytes(verb);
	const QByteArray payload = body;
	const auto hdrs = headers;
	RunAsync(
		this,
		[urlBytes, verbBytes, payload, hdrs] {
			HttpResult result;
			CURL *curl = curl_easy_init();
			if (!curl) {
				result.error = QStringLiteral("curl_easy_init failed");
				return result;
			}
			curl_slist *list = nullptr;
			for (const auto &header : hdrs) {
				const QByteArray line = header.first + ": " + header.second;
				list = curl_slist_append(list, line.constData());
			}
			curl_easy_setopt(curl, CURLOPT_URL, urlBytes.constData());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
			if (verbBytes != "GET") {
				curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verbBytes.constData());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.constData());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
			}
			result = CurlPerform(curl);
			curl_slist_free_all(list);
			curl_easy_cleanup(curl);
			return result;
		},
		std::move(done));
}

void ChatDock::StartConnections()
{
	StopConnections();
	if (availablePlatforms.contains(Twitch) && !accounts[Twitch].accessToken.isEmpty())
		ConnectTwitchIrc();
	if (availablePlatforms.contains(YouTube) && !accounts[YouTube].accessToken.isEmpty())
		StartYouTubeChat();
	if (availablePlatforms.contains(Kick) && !accounts[Kick].accessToken.isEmpty())
		ResolveKickChatroom();
	viewerTimer->start();
	RefreshViewers();
}

void ChatDock::StopConnections()
{
	viewerTimer->stop();
	youtubeTimer->stop();
	kickReconnectTimer->stop();
	DisconnectTwitchIrc();
	DisconnectKickChat();
	ytLiveChatId.clear();
	ytVideoId.clear();
	ytNextPage.clear();
}

void ChatDock::RefreshViewers()
{
	if (availablePlatforms.contains(Twitch))
		FetchTwitchViewers();
	if (availablePlatforms.contains(YouTube))
		FetchYouTubeViewers();
	if (availablePlatforms.contains(Kick))
		FetchKickViewers();
}

void ChatDock::SetViewerCount(Platform platform, int count, bool live)
{
	if (!viewerLabels[platform])
		return;
	if (!live)
		viewerLabels[platform]->setText(QStringLiteral("—"));
	else
		viewerLabels[platform]->setText(QString::number(count));
}

void ChatDock::FetchTwitchViewers()
{
	EnsureToken(Twitch, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Twitch];
		if (acc.userId.isEmpty() || acc.clientId.isEmpty())
			return;
		const QString url = QStringLiteral("https://api.twitch.tv/helix/streams?user_id=") + acc.userId;
		Http(url,
		     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
		      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     SetViewerCount(Twitch, 0, false);
				     return;
			     }
			     const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
			     if (data.isEmpty()) {
				     SetViewerCount(Twitch, 0, false);
				     return;
			     }
			     SetViewerCount(Twitch, data.at(0).toObject().value("viewer_count").toInt(), true);
		     });
	});
}

void ChatDock::FetchYouTubeViewers()
{
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok)
			return;
		if (ytVideoId.isEmpty()) {
			SetViewerCount(YouTube, 0, false);
			return;
		}
		const Account &acc = accounts[YouTube];
		const QString url = QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=") +
				    ytVideoId;
		Http(url, {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {},
		     "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     SetViewerCount(YouTube, 0, false);
				     return;
			     }
			     const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
			     if (items.isEmpty()) {
				     SetViewerCount(YouTube, 0, false);
				     return;
			     }
			     const QString viewers =
				     items.at(0).toObject().value("liveStreamingDetails").toObject().value("concurrentViewers").toString();
			     if (viewers.isEmpty())
				     SetViewerCount(YouTube, 0, false);
			     else
				     SetViewerCount(YouTube, viewers.toInt(), true);
		     });
	});
}

void ChatDock::FetchKickViewers()
{
	EnsureToken(Kick, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Kick];
		if (acc.userId.isEmpty())
			return;
		QUrl url(QStringLiteral("https://api.kick.com/public/v1/users/livestreams"));
		QUrlQuery query;
		query.addQueryItem("user_id", acc.userId);
		url.setQuery(query);
		Http(url.toString(),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     SetViewerCount(Kick, 0, false);
				     return;
			     }
			     const QJsonObject live = FirstDataObject(body);
			     if (live.isEmpty()) {
				     SetViewerCount(Kick, 0, false);
				     return;
			     }
			     const int count = live.value("viewer_count").toInt(live.value("viewers").toInt());
			     SetViewerCount(Kick, count, true);
		     });
	});
}

void ChatDock::ConnectTwitchIrc()
{
	DisconnectTwitchIrc();
	EnsureToken(Twitch, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Twitch];
		Http(QStringLiteral("https://api.twitch.tv/helix/users"),
		     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
		      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
			     if (data.isEmpty())
				     return;
			     const QJsonObject user = data.at(0).toObject();
			     twitchLogin = user.value("login").toString().toLower();
			     accounts[Twitch].userId = user.value("id").toString();
			     accounts[Twitch].login = twitchLogin;
			     accounts[Twitch].userName = user.value("display_name").toString(twitchLogin);

			     twitchIrc = new QSslSocket(this);
			     twitchJoined = false;
			     twitchBuffer.clear();
			     connect(twitchIrc, &QSslSocket::encrypted, this, &ChatDock::OnTwitchReady);
			     connect(twitchIrc, &QSslSocket::readyRead, this, &ChatDock::OnTwitchData);
			     connect(twitchIrc, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
				     QTimer::singleShot(3000, this, &ChatDock::ConnectTwitchIrc);
			     });
			     twitchIrc->connectToHostEncrypted(QStringLiteral("irc.chat.twitch.tv"), 6697);
		     });
	});
}

void ChatDock::DisconnectTwitchIrc()
{
	if (!twitchIrc)
		return;
	twitchIrc->disconnectFromHost();
	twitchIrc->deleteLater();
	twitchIrc = nullptr;
	twitchJoined = false;
	twitchBuffer.clear();
}

void ChatDock::OnTwitchReady()
{
	if (!twitchIrc)
		return;
	const Account &acc = accounts[Twitch];
	const QByteArray nick = twitchLogin.toUtf8();
	twitchIrc->write("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
	twitchIrc->write("PASS oauth:" + acc.accessToken.toUtf8() + "\r\n");
	twitchIrc->write("NICK " + nick + "\r\n");
}

void ChatDock::OnTwitchData()
{
	if (!twitchIrc)
		return;
	twitchBuffer.append(twitchIrc->readAll());
	while (true) {
		const int idx = twitchBuffer.indexOf("\r\n");
		if (idx < 0)
			break;
		const QString line = QString::fromUtf8(twitchBuffer.left(idx));
		twitchBuffer.remove(0, idx + 2);
		HandleTwitchLine(line);
	}
}

void ChatDock::HandleTwitchLine(const QString &line)
{
	if (line.startsWith(QStringLiteral("PING "))) {
		if (twitchIrc)
			twitchIrc->write("PONG :tmi.twitch.tv\r\n");
		return;
	}
	if (line.contains(QStringLiteral(" 001 ")) && !twitchJoined && !twitchLogin.isEmpty()) {
		twitchJoined = true;
		twitchIrc->write("JOIN #" + twitchLogin.toUtf8() + "\r\n");
		return;
	}
	if (!line.contains(QStringLiteral("PRIVMSG")))
		return;

	QString tags;
	QString rest = line;
	if (rest.startsWith(QLatin1Char('@'))) {
		const int sp = rest.indexOf(QLatin1Char(' '));
		tags = rest.mid(1, sp - 1);
		rest = rest.mid(sp + 1);
	}
	const int excl = rest.indexOf(QLatin1Char('!'));
	const int priv = rest.indexOf(QStringLiteral("PRIVMSG"));
	const int colon = rest.indexOf(QStringLiteral(" :"), priv);
	if (colon < 0)
		return;
	ChatMessage msg;
	msg.platform = Twitch;
	msg.text = rest.mid(colon + 2);
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.user = excl > 0 ? rest.left(excl).section(QLatin1Char(':'), -1) : QStringLiteral("twitch");
	msg.color = QStringLiteral("#9146FF");
	for (const QString &part : tags.split(QLatin1Char(';'))) {
		const QString key = part.section(QLatin1Char('='), 0, 0);
		const QString val = part.section(QLatin1Char('='), 1);
		if (key == QLatin1String("display-name") && !val.isEmpty())
			msg.user = val;
		else if (key == QLatin1String("color") && !val.isEmpty())
			msg.color = val;
		else if (key == QLatin1String("id"))
			msg.id = val;
	}
	if (msg.id.isEmpty())
		msg.id = QStringLiteral("tw-%1-%2").arg(msg.ts).arg(msg.user);
	AppendMessage(msg);
}

void ChatDock::SendTwitchMessage(const QString &text)
{
	if (!twitchIrc || !twitchJoined || twitchLogin.isEmpty())
		return;
	twitchIrc->write("PRIVMSG #" + twitchLogin.toUtf8() + " :" + text.toUtf8() + "\r\n");
	ChatMessage msg;
	msg.platform = Twitch;
	msg.user = accounts[Twitch].userName.isEmpty() ? twitchLogin : accounts[Twitch].userName;
	msg.color = QStringLiteral("#9146FF");
	msg.text = text;
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.id = QStringLiteral("local-tw-%1").arg(msg.ts);
	AppendMessage(msg);
}

void ChatDock::StartYouTubeChat()
{
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[YouTube];
		auto tryStatus = [this, acc](const QString &status, std::function<void()> next) {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
			QUrlQuery query;
			query.addQueryItem("part", "snippet,contentDetails,status");
			query.addQueryItem("broadcastStatus", status);
			query.addQueryItem("broadcastType", "all");
			query.addQueryItem("maxResults", "5");
			url.setQuery(query);
			Http(url.toString(),
			     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {},
			     "GET", [this, next](int code, const QByteArray &body, const QString &) {
				     if (code >= 200 && code < 300) {
					     const QJsonArray items =
						     QJsonDocument::fromJson(body).object().value("items").toArray();
					     if (!items.isEmpty()) {
						     const QJsonObject item = items.at(0).toObject();
						     ytVideoId = item.value("id").toString();
						     ytLiveChatId = item.value("snippet").toObject().value("activeLiveChatId").toString();
						     if (ytLiveChatId.isEmpty()) {
							     /* Resolve via videos.list */
							     EnsureToken(YouTube, [this](bool tok) {
								     if (!tok || ytVideoId.isEmpty())
									     return;
								     const Account &a = accounts[YouTube];
								     Http(QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=") +
										  ytVideoId,
									  {{QByteArrayLiteral("Authorization"),
									    QByteArrayLiteral("Bearer ") + a.accessToken.toUtf8()}},
									  {}, "GET",
									  [this](int st, const QByteArray &payload, const QString &) {
										  if (st >= 200 && st < 300) {
											  const QJsonArray vids =
												  QJsonDocument::fromJson(payload)
													  .object()
													  .value("items")
													  .toArray();
											  if (!vids.isEmpty())
												  ytLiveChatId =
													  vids.at(0)
														  .toObject()
														  .value("liveStreamingDetails")
														  .toObject()
														  .value("activeLiveChatId")
														  .toString();
										  }
										  if (!ytLiveChatId.isEmpty())
											  PollYouTubeChat();
									  });
							     });
						     } else {
							     PollYouTubeChat();
						     }
						     return;
					     }
				     }
				     next();
			     });
		};
		tryStatus(QStringLiteral("active"), [this, tryStatus] {
			tryStatus(QStringLiteral("upcoming"), [] {});
		});
	});
}

void ChatDock::PollYouTubeChat()
{
	if (ytLiveChatId.isEmpty())
		return;
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok || ytLiveChatId.isEmpty())
			return;
		const Account &acc = accounts[YouTube];
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChatMessages"));
		QUrlQuery query;
		query.addQueryItem("liveChatId", ytLiveChatId);
		query.addQueryItem("part", "snippet,authorDetails");
		if (!ytNextPage.isEmpty())
			query.addQueryItem("pageToken", ytNextPage);
		url.setQuery(query);
		Http(url.toString(),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {}, "GET",
		     [this](int status, const QByteArray &body, const QString &) {
			     int interval = 5000;
			     if (status >= 200 && status < 300) {
				     const QJsonObject root = QJsonDocument::fromJson(body).object();
				     ytNextPage = root.value("nextPageToken").toString();
				     interval = qMax(2000, root.value("pollingIntervalMillis").toInt(5000));
				     const QJsonArray items = root.value("items").toArray();
				     for (const QJsonValue &row : items) {
					     const QJsonObject obj = row.toObject();
					     const QJsonObject snip = obj.value("snippet").toObject();
					     if (snip.value("type").toString() != QLatin1String("textMessageEvent"))
						     continue;
					     ChatMessage msg;
					     msg.platform = YouTube;
					     msg.id = obj.value("id").toString();
					     msg.user = obj.value("authorDetails").toObject().value("displayName").toString();
					     msg.color = QStringLiteral("#FF5555");
					     msg.text = snip.value("textMessageDetails").toObject().value("messageText").toString();
					     msg.ts = QDateTime::currentMSecsSinceEpoch();
					     AppendMessage(msg);
				     }
			     }
			     youtubeTimer->start(interval);
		     });
	});
}

void ChatDock::SendYouTubeMessage(const QString &text)
{
	if (ytLiveChatId.isEmpty())
		return;
	EnsureToken(YouTube, [this, text](bool ok) {
		if (!ok || ytLiveChatId.isEmpty())
			return;
		const Account &acc = accounts[YouTube];
		QJsonObject snip;
		snip.insert("liveChatId", ytLiveChatId);
		snip.insert("type", "textMessageEvent");
		QJsonObject details;
		details.insert("messageText", text);
		snip.insert("textMessageDetails", details);
		QJsonObject body;
		body.insert("snippet", snip);
		Http(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChatMessages?part=snippet"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
		     [this, text](int status, const QByteArray &, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     ChatMessage msg;
			     msg.platform = YouTube;
			     msg.user = accounts[YouTube].userName.isEmpty() ? QStringLiteral("You") : accounts[YouTube].userName;
			     msg.color = QStringLiteral("#FF5555");
			     msg.text = text;
			     msg.ts = QDateTime::currentMSecsSinceEpoch();
			     msg.id = QStringLiteral("local-yt-%1").arg(msg.ts);
			     AppendMessage(msg);
		     });
	});
}

void ChatDock::ResolveKickChatroom()
{
	EnsureToken(Kick, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Kick];
		Http(QStringLiteral("https://api.kick.com/public/v1/channels"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     const QJsonObject channel = FirstDataObject(body);
			     kickSlug = channel.value("slug").toString();
			     if (kickSlug.isEmpty())
				     kickSlug = accounts[Kick].userName;
			     if (accounts[Kick].userId.isEmpty())
				     accounts[Kick].userId = JsonId(channel.value("broadcaster_user_id").isUndefined()
									     ? channel.value("user_id")
									     : channel.value("broadcaster_user_id"));
			     if (kickSlug.isEmpty())
				     return;
			     Http(QStringLiteral("https://kick.com/api/v2/channels/") +
					  QString::fromUtf8(QUrl::toPercentEncoding(kickSlug)),
				  {{QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}}, {}, "GET",
				  [this](int code, const QByteArray &payload, const QString &) {
					  if (code >= 200 && code < 300) {
						  const QJsonObject root = QJsonDocument::fromJson(payload).object();
						  kickChatroomId = JsonId(root.value("chatroom").toObject().value("id"));
						  if (kickChatroomId.isEmpty())
							  kickChatroomId = JsonId(root.value("id"));
					  }
					  ConnectKickChat();
				  });
		     });
	});
}

void ChatDock::ConnectKickChat()
{
	DisconnectKickChat();
	if (kickChatroomId.isEmpty())
		return;
	kickWs = new QSslSocket(this);
	kickBuffer.clear();
	kickWsReady = false;
	kickSubscribed = false;
	connect(kickWs, &QSslSocket::encrypted, this, &ChatDock::OnKickEncrypted);
	connect(kickWs, &QSslSocket::readyRead, this, &ChatDock::OnKickData);
	connect(kickWs, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
		if (!kickReconnectTimer->isActive())
			kickReconnectTimer->start(4000);
	});
	connect(kickWs, &QAbstractSocket::disconnected, this, [this] {
		kickWsReady = false;
		if (!kickReconnectTimer->isActive())
			kickReconnectTimer->start(4000);
	});
	kickWs->connectToHostEncrypted(QStringLiteral("ws-us2.pusher.com"), 443);
}

void ChatDock::DisconnectKickChat()
{
	if (!kickWs)
		return;
	kickWs->disconnectFromHost();
	kickWs->deleteLater();
	kickWs = nullptr;
	kickBuffer.clear();
	kickWsReady = false;
	kickSubscribed = false;
}

void ChatDock::OnKickEncrypted()
{
	if (!kickWs)
		return;
	const QByteArray key = QByteArrayLiteral("32cbd69e4b950bf97679");
	const QByteArray path =
		"/app/" + key + "?protocol=7&client=js&version=7.4.0&flash=false";
	const QByteArray handshake =
		"GET " + path +
		" HTTP/1.1\r\n"
		"Host: ws-us2.pusher.com\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n";
	kickWs->write(handshake);
}

void ChatDock::OnKickData()
{
	if (!kickWs)
		return;
	kickBuffer.append(kickWs->readAll());
	if (!kickWsReady) {
		const int hdrEnd = kickBuffer.indexOf("\r\n\r\n");
		if (hdrEnd < 0)
			return;
		const QByteArray header = kickBuffer.left(hdrEnd);
		kickBuffer.remove(0, hdrEnd + 4);
		if (!header.contains("101")) {
			DisconnectKickChat();
			if (!kickReconnectTimer->isActive())
				kickReconnectTimer->start(8000);
			return;
		}
		kickWsReady = true;
		const QJsonObject sub{
			{QStringLiteral("event"), QStringLiteral("pusher:subscribe")},
			{QStringLiteral("data"),
			 QJsonObject{{QStringLiteral("auth"), QString()},
				     {QStringLiteral("channel"),
				      QStringLiteral("chatrooms.%1.v2").arg(kickChatroomId)}}}};
		SendKickWsText(QString::fromUtf8(QJsonDocument(sub).toJson(QJsonDocument::Compact)));
		kickSubscribed = true;
	}

	while (kickBuffer.size() >= 2) {
		const quint8 b0 = static_cast<quint8>(kickBuffer.at(0));
		const quint8 b1 = static_cast<quint8>(kickBuffer.at(1));
		const bool fin = (b0 & 0x80) != 0;
		const quint8 opcode = b0 & 0x0f;
		const bool masked = (b1 & 0x80) != 0;
		quint64 payloadLen = b1 & 0x7f;
		int headerLen = 2;
		if (payloadLen == 126) {
			if (kickBuffer.size() < 4)
				return;
			payloadLen = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(kickBuffer.constData() + 2));
			headerLen = 4;
		} else if (payloadLen == 127) {
			if (kickBuffer.size() < 10)
				return;
			payloadLen = qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(kickBuffer.constData() + 2));
			headerLen = 10;
		}
		const int maskLen = masked ? 4 : 0;
		if (static_cast<quint64>(kickBuffer.size()) < headerLen + maskLen + payloadLen)
			return;
		QByteArray payload = kickBuffer.mid(headerLen + maskLen, static_cast<int>(payloadLen));
		if (masked) {
			const QByteArray mask = kickBuffer.mid(headerLen, 4);
			for (int i = 0; i < payload.size(); ++i)
				payload[i] = char(payload.at(i) ^ mask.at(i % 4));
		}
		kickBuffer.remove(0, headerLen + maskLen + static_cast<int>(payloadLen));
		if (!fin)
			continue;
		if (opcode == 0x8) {
			DisconnectKickChat();
			return;
		}
		if (opcode == 0x9) {
			/* pong */
			QByteArray frame;
			frame.append(char(0x8a));
			frame.append(char(0x80 | payload.size()));
			const QByteArray mask = QByteArray::fromHex("01020304");
			frame.append(mask);
			for (int i = 0; i < payload.size(); ++i)
				frame.append(char(payload.at(i) ^ mask.at(i % 4)));
			kickWs->write(frame);
			continue;
		}
		if (opcode == 0x1)
			HandleKickFrame(payload);
	}
}

void ChatDock::SendKickWsText(const QString &text)
{
	if (!kickWs || !kickWsReady)
		return;
	const QByteArray payload = text.toUtf8();
	QByteArray frame;
	frame.append(char(0x81));
	if (payload.size() < 126) {
		frame.append(char(0x80 | payload.size()));
	} else {
		frame.append(char(0x80 | 126));
		quint16 len = qToBigEndian<quint16>(static_cast<quint16>(payload.size()));
		frame.append(reinterpret_cast<const char *>(&len), 2);
	}
	const QByteArray mask = QByteArray::fromHex("0a0b0c0d");
	frame.append(mask);
	for (int i = 0; i < payload.size(); ++i)
		frame.append(char(payload.at(i) ^ mask.at(i % 4)));
	kickWs->write(frame);
}

void ChatDock::HandleKickFrame(const QByteArray &payload)
{
	OnKickWsMessage(QString::fromUtf8(payload));
}

void ChatDock::OnKickWsMessage(const QString &message)
{
	const QJsonObject root = QJsonDocument::fromJson(message.toUtf8()).object();
	const QString event = root.value("event").toString();
	if (event == QLatin1String("pusher:ping")) {
		SendKickWsText(QStringLiteral("{\"event\":\"pusher:pong\",\"data\":{}}"));
		return;
	}
	if (!event.contains(QStringLiteral("ChatMessage")))
		return;
	QJsonObject data = root.value("data").toObject();
	if (data.isEmpty()) {
		const QString raw = root.value("data").toString();
		data = QJsonDocument::fromJson(raw.toUtf8()).object();
	}
	const QJsonObject sender = data.value("sender").toObject();
	ChatMessage msg;
	msg.platform = Kick;
	msg.id = JsonId(data.value("id"));
	msg.user = sender.value("username").toString(sender.value("slug").toString());
	msg.color = sender.value("identity").toObject().value("color").toString(QStringLiteral("#53FC18"));
	msg.text = data.value("content").toString();
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	if (msg.id.isEmpty())
		msg.id = QStringLiteral("kick-%1-%2").arg(msg.ts).arg(msg.user);
	AppendMessage(msg);
}

void ChatDock::SendKickMessage(const QString &text)
{
	EnsureToken(Kick, [this, text](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Kick];
		QJsonObject body;
		body.insert("content", text);
		body.insert("type", "user");
		if (!acc.userId.isEmpty())
			body.insert("broadcaster_user_id", acc.userId.toLongLong());
		Http(QStringLiteral("https://api.kick.com/public/v1/chat"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
		     [this, text](int status, const QByteArray &, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     ChatMessage msg;
			     msg.platform = Kick;
			     msg.user = accounts[Kick].userName.isEmpty() ? QStringLiteral("You") : accounts[Kick].userName;
			     msg.color = QStringLiteral("#53FC18");
			     msg.text = text;
			     msg.ts = QDateTime::currentMSecsSinceEpoch();
			     msg.id = QStringLiteral("local-kick-%1").arg(msg.ts);
			     AppendMessage(msg);
		     });
	});
}

void ChatDock::AppendMessage(const ChatMessage &msg)
{
	if (msg.text.isEmpty())
		return;
	if (!msg.id.isEmpty()) {
		if (seenMessageIds.contains(msg.id))
			return;
		seenMessageIds.insert(msg.id);
		if (seenMessageIds.size() > 2000) {
			seenMessageIds.clear();
		}
	}

	const QString iconPath = PlatformIcon(msg.platform).name().isEmpty()
					 ? QString()
					 : QString();
	Q_UNUSED(iconPath);
	const QString color = msg.color.isEmpty() ? PlatformAccent(msg.platform).name() : msg.color;
	const QString html =
		QStringLiteral(
			"<table cellspacing='0' cellpadding='0'><tr>"
			"<td style='padding-right:6px; vertical-align:top;'>"
			"<img src='%1' width='16' height='16'/></td>"
			"<td style='vertical-align:top;'>"
			"<span style='color:%2; font-weight:600;'>%3</span>"
			"<span style='color:#ddd;'>: %4</span></td></tr></table>")
			.arg(msg.platform == YouTube   ? QStringLiteral(":/aitum/media/youtube.png")
			     : msg.platform == Kick    ? QStringLiteral(":/aitum/media/kick.png")
						       : QStringLiteral(":/aitum/media/twitch.png"),
			     color, EscapeHtml(msg.user), Linkify(EscapeHtml(msg.text)));

	auto *item = new QListWidgetItem(messageList);
	auto *label = new QLabel(html);
	label->setTextFormat(Qt::RichText);
	label->setWordWrap(true);
	label->setOpenExternalLinks(true);
	label->setStyleSheet(QStringLiteral("background: transparent; color: #eee;"));
	label->setTextInteractionFlags(Qt::TextBrowserInteraction);
	const int width = qMax(120, messageList->viewport()->width() - 16);
	label->setFixedWidth(width);
	item->setSizeHint(label->sizeHint() + QSize(0, 4));
	messageList->addItem(item);
	messageList->setItemWidget(item, label);

	while (messageList->count() > maxMessages) {
		auto *old = messageList->takeItem(0);
		delete old;
	}
	messageList->scrollToBottom();
}

void ChatDock::SendChatMessage()
{
	const QString text = messageEdit->text().trimmed();
	if (text.isEmpty())
		return;
	messageEdit->clear();
	switch (sendPlatform) {
	case YouTube:
		SendYouTubeMessage(text);
		break;
	case Kick:
		SendKickMessage(text);
		break;
	case Twitch:
	default:
		SendTwitchMessage(text);
		break;
	}
}
